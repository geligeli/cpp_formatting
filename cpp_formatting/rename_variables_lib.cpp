#include "cpp_formatting/rename_variables_lib.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/FileEntry.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/Tooling.h"

using namespace clang;
using namespace clang::tooling;

namespace {

// ---------------------------------------------------------------------------
// Scope classification
// ---------------------------------------------------------------------------

// Only ordinary named member functions can be renamed: constructors and
// destructors take the class name, conversion functions and overloaded
// operators have no identifier of their own that renameToStyle could rewrite
// without breaking the syntax.
static bool isRenamableMethod(const CXXMethodDecl* MD) {
  if (isa<CXXConstructorDecl, CXXDestructorDecl, CXXConversionDecl>(MD))
    return false;
  return MD->getOverloadedOperator() == OO_None;
}

bool matchesScope(const NamedDecl* D, VariableScope Scope) {
  switch (Scope) {
    case VariableScope::Member:
      if (isa<FieldDecl>(D)) return true;
      if (const auto* VD = dyn_cast<VarDecl>(D))
        return VD->isStaticDataMember();
      return false;
    case VariableScope::Local: {
      const auto* VD = dyn_cast<VarDecl>(D);
      return VD && VD->isLocalVarDeclOrParm();
    }
    case VariableScope::Global: {
      const auto* VD = dyn_cast<VarDecl>(D);
      return VD && !VD->isLocalVarDeclOrParm() && !VD->isStaticDataMember();
    }
    case VariableScope::StaticMember: {
      const auto* VD = dyn_cast<VarDecl>(D);
      return VD && VD->isStaticDataMember();
    }
    case VariableScope::ConstMember: {
      const auto* VD = dyn_cast<VarDecl>(D);
      return VD && VD->isStaticDataMember() &&
             (VD->isConstexpr() || VD->getType().isConstQualified());
    }
    case VariableScope::StaticGlobal: {
      const auto* VD = dyn_cast<VarDecl>(D);
      if (!VD || VD->isLocalVarDeclOrParm() || VD->isStaticDataMember())
        return false;
      return VD->getStorageClass() == SC_Static;
    }
    case VariableScope::ConstGlobal: {
      const auto* VD = dyn_cast<VarDecl>(D);
      if (!VD || VD->isLocalVarDeclOrParm() || VD->isStaticDataMember())
        return false;
      return VD->isConstexpr() || VD->getType().isConstQualified();
    }
    case VariableScope::Method: {
      const auto* MD = dyn_cast<CXXMethodDecl>(D);
      return MD && isRenamableMethod(MD);
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Template instantiation helpers
// ---------------------------------------------------------------------------

// The record \p RD was instantiated from, or null if it is not an
// instantiation.  Instantiated records come in two shapes and only the first is
// a ClassTemplateSpecializationDecl:
//
//   Outer<int>            -- a specialization; its pattern is the primary
//                            template's CXXRecordDecl.
//   Outer<int>::Inner     -- a class *nested* in a class template is an
//                            ordinary CXXRecordDecl whose pattern comes from
//                            getInstantiatedFromMemberClass().
//
// Missing the second shape meant members of a nested class never mapped back to
// the pattern the rename map is keyed by: the declaration was renamed (it is
// collected from the pattern) while every use in an instantiation was skipped,
// and dependent uses were vetoed for binding to a member "not being renamed".
static const CXXRecordDecl* instantiationPattern(const CXXRecordDecl* RD) {
  if (const auto* Spec = dyn_cast<ClassTemplateSpecializationDecl>(RD)) {
    // An explicit specialization is not an instantiation: its members are
    // written out, and are their own pattern.  Mapping them anywhere would be
    // matching two unrelated member lists by position.
    const TemplateSpecializationKind Kind =
        Spec->getTemplateSpecializationKind();
    if (Kind == TSK_Undeclared || Kind == TSK_ExplicitSpecialization)
      return nullptr;
    // An instantiation of a *partial* specialization must map back to that
    // specialization's own members, not the primary template's: they are
    // different classes with different member lists, so matching by index
    // across them rebinds a use to an unrelated member.
    auto From = Spec->getSpecializedTemplateOrPartial();
    if (const auto* Partial =
            dyn_cast<ClassTemplatePartialSpecializationDecl*>(From))
      return Partial;
    return cast<ClassTemplateDecl*>(From)->getTemplatedDecl();
  }
  return RD->getInstantiatedFromMemberClass();
}

static const FieldDecl* primaryTemplateMember(const FieldDecl* FD) {
  const auto* RD = dyn_cast_or_null<CXXRecordDecl>(FD->getParent());
  if (!RD) return FD;
  const CXXRecordDecl* Pattern = instantiationPattern(RD);
  if (!Pattern) return FD;
  // Fields have no back-pointer to the declaration they were instantiated
  // from, so match by position; recursing walks a chain of nested
  // instantiations up to the outermost pattern.
  unsigned Idx = FD->getFieldIndex();
  unsigned I = 0;
  for (const FieldDecl* PF : Pattern->fields()) {
    if (I++ == Idx) return primaryTemplateMember(PF);
  }
  return FD;
}

static const VarDecl* primaryTemplateStaticMember(const VarDecl* VD) {
  while (const VarDecl* P = VD->getInstantiatedFromStaticDataMember()) VD = P;
  return VD;
}

// Maps a method of a class-template specialization (or an instantiated member
// function template) back to the method of the primary template.
static const CXXMethodDecl* primaryTemplateMethod(const CXXMethodDecl* MD) {
  while (const auto* P = dyn_cast_or_null<CXXMethodDecl>(
             MD->getInstantiatedFromMemberFunction()))
    MD = P;
  return MD;
}

// Collects \p MD and, transitively, every virtual function it overrides.  The
// whole family must be renamed together — renaming only some overrides would
// break `override` checking.
static void collectOverrideFamily(const CXXMethodDecl* MD,
                                  std::vector<const CXXMethodDecl*>& Out) {
  MD = primaryTemplateMethod(MD);
  Out.push_back(MD);
  for (const CXXMethodDecl* O : MD->overridden_methods())
    collectOverrideFamily(O, Out);
}

// ---------------------------------------------------------------------------
// File-set predicate
// ---------------------------------------------------------------------------

static bool shouldCollect(SourceLocation Loc, SourceManager& SM,
                          const FileSet& CollectFrom) {
  if (Loc.isInvalid()) return false;
  // Resolve through macro expansions to the file the name is actually spelled
  // in.  A declaration written as a macro *argument* is spelled in an ordinary
  // file and renames like any other.  One spelled in a macro *body* is
  // collected here too and then vetoed by the scan pass, so it is reported as a
  // skipped rename rather than silently ignored.  A name formed by token
  // pasting has no file at all (Clang's scratch buffer), and falls out here:
  // scratch space is never the main file and never in the set.
  const FileID FID = SM.getFileID(SM.getSpellingLoc(Loc));
  const bool IsMain = FID == SM.getMainFileID();
  if (CollectFrom.empty()) return IsMain;
  const FileEntry* FE = SM.getFileEntryForID(FID);
  if (!FE) return IsMain;
  llvm::StringRef RealPath = FE->tryGetRealPathName();
  if (RealPath.empty()) return IsMain;
  return CollectFrom.count(RealPath.str()) > 0;
}

// ---------------------------------------------------------------------------
// Stable location keys
// ---------------------------------------------------------------------------

// Returns a (file identifier, byte offset) key for the place \p Loc is
// *spelled*.  The identifier is the file's real path, which is stable across
// translation units and across processes -- so a token recorded while compiling
// a.cpp matches the same token when the header is compiled as its own TU, and a
// declaration one Bazel action vetoes matches the records another emitted for
// it.  For in-memory buffers with no real path (the unit-test helper) it falls
// back to the presumed file name, which is enough for the single-TU case.
static std::pair<std::string, unsigned> locKey(SourceLocation Loc,
                                               SourceManager& SM) {
  SourceLocation Spelling = SM.getSpellingLoc(Loc);
  std::pair<FileID, unsigned> Decomposed = SM.getDecomposedLoc(Spelling);
  std::string Path;
  if (const FileEntry* FE = SM.getFileEntryForID(Decomposed.first)) {
    llvm::StringRef RealPath = FE->tryGetRealPathName();
    if (!RealPath.empty()) Path = RealPath.str();
  }
  if (Path.empty()) {
    PresumedLoc PLoc = SM.getPresumedLoc(Spelling);
    Path = PLoc.isValid() ? PLoc.getFilename() : "<main>";
  }
  return std::make_pair(std::move(Path), Decomposed.second);
}

// The location to actually rewrite for a (possibly macro-expanded) token, or an
// invalid location when no byte range of any file holds the name.
//
// getFileLoc() walks macro-argument levels down to the spelling and macro-body
// levels up to the expansion, so the two agree exactly when the token was typed
// by the user -- directly, or as a macro argument forwarded through any number
// of macros, where the spelling is the call site and rewriting it is simply
// correct.  They disagree for a token spelled in a macro body (one location
// shared by every expansion, which need not agree on what it names) and for a
// ##-pasted token (whose only spelling is Clang's scratch buffer).
static SourceLocation rewriteLocFor(SourceLocation Loc, SourceManager& SM) {
  if (Loc.isInvalid()) return SourceLocation();
  if (!Loc.isMacroID()) return Loc;
  SourceLocation Spelling = SM.getSpellingLoc(Loc);
  if (SM.getFileLoc(Loc) != Spelling) return SourceLocation();
  return Spelling;
}

// locKey restricted to files the tool owns -- the key space used for cross-TU
// dependent-token resolution, where a token is only tracked if we may rewrite
// the file it lives in.
static std::optional<std::pair<std::string, unsigned>> ownedKey(
    SourceLocation Loc, SourceManager& SM, const FileSet& CollectFrom) {
  if (Loc.isInvalid()) return std::nullopt;
  const FileEntry* FE =
      SM.getFileEntryForID(SM.getFileID(SM.getSpellingLoc(Loc)));
  llvm::StringRef RealPath = FE ? FE->tryGetRealPathName() : llvm::StringRef();
  const bool Owned =
      CollectFrom.empty()
          ? SM.getFileID(SM.getSpellingLoc(Loc)) == SM.getMainFileID()
          : (!RealPath.empty() && CollectFrom.count(RealPath.str()) > 0);
  if (!Owned) return std::nullopt;
  return locKey(Loc, SM);
}

// ---------------------------------------------------------------------------
// Veto key for a rename
// ---------------------------------------------------------------------------

// The key under which a declaration's rename is vetoed, and under which its
// edit records name their owner.  For a virtual function it is the base-most
// declaration of the override hierarchy, so that a veto discovered on *any*
// override suppresses the whole family -- including in records emitted by
// another process, which never sees which part of the hierarchy this TU had in
// view.  collectOverrideFamily() walks upwards only, so every override's
// family contains the same roots and all of them agree on this key.
static std::pair<std::string, unsigned> renameOwnerKey(const Decl* D,
                                                       SourceManager& SM) {
  if (const auto* MD = dyn_cast<CXXMethodDecl>(D)) {
    std::vector<const CXXMethodDecl*> Family;
    collectOverrideFamily(MD, Family);
    std::optional<std::pair<std::string, unsigned>> Root;
    for (const CXXMethodDecl* M : Family) {
      if (M->size_overridden_methods() != 0) continue;
      std::pair<std::string, unsigned> K = locKey(M->getLocation(), SM);
      if (!Root || K < *Root) Root = std::move(K);
    }
    if (Root) return *Root;
  }
  return locKey(D->getLocation(), SM);
}

// Records a veto against renaming \p Key.  Returns true if this is the first
// veto for that declaration, so the caller can report it once.
static bool addRenameVeto(RenameVetoes* Vetoes, const Decl* Key,
                          SourceManager& SM, llvm::StringRef OldName,
                          const std::string& Reason) {
  if (!Vetoes) return false;
  std::pair<std::string, unsigned> Owner = renameOwnerKey(Key, SM);
  std::string File = relativizeToCwd(Owner.first);
  return Vetoes
      ->try_emplace(std::make_pair(File, Owner.second),
                    RenameVeto{File, Owner.second, OldName.str(), Reason})
      .second;
}

// ---------------------------------------------------------------------------
// Pass 1: collect the rename map
// ---------------------------------------------------------------------------

using RenameMap = std::unordered_map<const Decl*, std::string>;

class CollectRenamesVisitor
    : public RecursiveASTVisitor<CollectRenamesVisitor> {
 public:
  CollectRenamesVisitor(SourceManager& SM, const VariableRenameCallback& CB,
                        VariableScope Scope, RenameMap& Renames,
                        const FileSet& CollectFrom,
                        RenameConflicts* Conflicts = nullptr,
                        RenameVetoes* Vetoes = nullptr)
      : SM(SM),
        CB(CB),
        Scope(Scope),
        Renames(Renames),
        CollectFrom(CollectFrom),
        Conflicts(Conflicts),
        Vetoes(Vetoes) {}

  bool VisitFieldDecl(FieldDecl* D) {
    collect(D);
    return true;
  }
  bool VisitVarDecl(VarDecl* D) {
    collect(D);
    return true;
  }
  bool VisitCXXMethodDecl(CXXMethodDecl* D) {
    collectMethod(D);
    return true;
  }

 private:
  void collect(NamedDecl* D) {
    if (D->isImplicit() || !matchesScope(D, Scope)) return;
    if (!shouldCollect(D->getLocation(), SM, CollectFrom)) return;
    const Decl* Key = D->getCanonicalDecl();
    if (!Visited.insert(Key).second) return;
    std::string NewName;
    if (!CB(D->getName(), NewName) || NewName == D->getName().str()) return;
    if (unusableNewName(D, Key, NewName) || vetoed(D, Key, NewName) ||
        collides(D, NewName))
      return;
    Renames[Key] = std::move(NewName);
  }

  void collectMethod(const CXXMethodDecl* D) {
    D = primaryTemplateMethod(D);
    if (D->isImplicit() || !matchesScope(D, Scope)) return;
    if (!shouldCollect(D->getLocation(), SM, CollectFrom)) return;
    const Decl* Key = D->getCanonicalDecl();
    if (!Visited.insert(Key).second) return;
    // All-or-nothing across the override hierarchy: if any overridden base
    // function is declared outside the collected files, renaming here would
    // leave the hierarchy inconsistent, so skip the rename entirely.
    std::vector<const CXXMethodDecl*> Family;
    collectOverrideFamily(D, Family);
    for (const CXXMethodDecl* M : Family)
      if (!shouldCollect(M->getLocation(), SM, CollectFrom)) return;
    std::string NewName;
    if (!CB(D->getName(), NewName) || NewName == D->getName().str()) return;
    // One check for the whole family: renameOwnerKey() keys a virtual function
    // by the base-most declaration of its hierarchy, so a veto recorded against
    // any override is found from here.
    if (unusableNewName(D, Key, NewName) || vetoed(D, Key, NewName)) return;
    // One check for the whole family: a clash anywhere in the hierarchy means
    // the rename cannot be applied consistently, so none of it is.
    for (const CXXMethodDecl* M : Family)
      if (collides(M, NewName)) return;
    for (const CXXMethodDecl* M : Family)
      Renames[M->getCanonicalDecl()] = NewName;
  }

  // True when the new name cannot name the declaration no matter what else is
  // in scope: it is a keyword, or a macro is defined with that name.  Both slip
  // past collides(), which only asks whether the name is *taken* in this
  // DeclContext -- googletest renames `char_` to `char` and `errno_` to
  // `errno`, giving `char char;` and an assignment to glibc's errno macro.
  bool unusableNewName(const NamedDecl* D, const Decl* Key,
                       llvm::StringRef NewName) {
    ASTContext& Ctx = D->getASTContext();
    const IdentifierInfo& II = Ctx.Idents.get(NewName);
    if (II.isKeyword(Ctx.getLangOpts())) {
      record(D, NewName, "'" + NewName.str() + "' is a keyword");
      return true;
    }
    if (II.hasMacroDefinition() || II.hadMacroDefinition()) {
      const std::string Reason =
          "'" + NewName.str() + "' is defined as a macro";
      // Whether a macro is visible depends on what this TU included, so a plain
      // skip would let a TU that never sees the definition rename the
      // declaration this one refuses to.  A veto is global; a skip is not.
      addRenameVeto(Vetoes, Key, SM, D->getName(), Reason);
      record(D, NewName, Reason);
      return true;
    }
    return false;
  }

  // True when an earlier translation unit found a reference to this
  // declaration that cannot be rewritten (see RenameVetoes).  The rename is
  // then skipped everywhere and reported, rather than applied here and left
  // dangling at the reference.
  bool vetoed(const NamedDecl* D, const Decl* Key, llvm::StringRef NewName) {
    if (!Vetoes || Vetoes->empty()) return false;
    std::pair<std::string, unsigned> Owner = renameOwnerKey(Key, SM);
    auto It = Vetoes->find({relativizeToCwd(Owner.first), Owner.second});
    if (It == Vetoes->end()) return false;
    record(D, NewName, It->second.Reason);
    return true;
  }

  // True when NewName is already taken in D's own scope, in which case the
  // rename is skipped entirely (declaration and uses) and recorded.  Renaming
  // into an occupied name is not a formatting change: at best it fails to
  // compile, at worst it silently rebinds uses to the other entity.
  bool collides(const NamedDecl* D, llvm::StringRef NewName) {
    const DeclContext* DC = D->getDeclContext();
    if (!DC) return false;
    DC = DC->getPrimaryContext();
    const Decl* Key = D->getCanonicalDecl();

    // Something of that name is already declared in the *same* scope.  Only
    // the immediate context is consulted: shadowing an inherited member or an
    // outer-scope name is legal C++ and not this tool's business.
    ASTContext& Ctx = D->getASTContext();
    DeclarationName DN(&Ctx.Idents.get(NewName));
    for (const NamedDecl* ND : DC->lookup(DN)) {
      if (ND->getCanonicalDecl() == Key || ND->isImplicit()) continue;
      if (overloadsCleanly(Ctx, D, ND)) continue;
      record(D, NewName,
             ("existing " + std::string(ND->getDeclKindName()) + " '" +
              NewName.str() + "'"));
      return true;
    }

    // Two declarations in the same scope renaming to the same new name: the
    // first one through keeps it, the second is skipped -- unless they are
    // functions that would form a legal overload set.
    auto [It, Inserted] = Claimed.try_emplace({DC, NewName.str()}, D);
    if (!Inserted && It->second->getCanonicalDecl() != Key &&
        !overloadsCleanly(Ctx, D, It->second)) {
      record(D, NewName, "another declaration in the same scope renames to it");
      return true;
    }
    return false;
  }

  // Two functions may share a name in one scope -- that is an overload set, and
  // renaming one onto another's name is a supported outcome.  Identical
  // signatures are not: that is a redeclaration.  Anything else (a field and a
  // method, two fields) cannot share a name at all.
  static bool overloadsCleanly(ASTContext& Ctx, const NamedDecl* A,
                               const NamedDecl* B) {
    const auto* FA = dyn_cast<FunctionDecl>(A);
    const auto* FB = dyn_cast<FunctionDecl>(B);
    if (!FA || !FB) return false;
    return !Ctx.hasSameType(FA->getType(), FB->getType());
  }

  void record(const NamedDecl* D, llvm::StringRef NewName,
              const std::string& Reason) {
    if (!Conflicts) return;
    const PresumedLoc PL = SM.getPresumedLoc(D->getLocation());
    Conflicts->push_back(RenameConflict{
        PL.isValid() ? PL.getFilename() : "", PL.isValid() ? PL.getLine() : 0,
        PL.isValid() ? PL.getColumn() : 0, D->getName().str(), NewName.str(),
        Reason});
  }

  SourceManager& SM;
  const VariableRenameCallback& CB;
  VariableScope Scope;
  RenameMap& Renames;
  const FileSet& CollectFrom;
  RenameConflicts* Conflicts = nullptr;
  // Vetoes carried over from an earlier TU or an earlier pass of a re-run,
  // plus a place to record a macro clash found here for the other TUs.
  RenameVetoes* Vetoes = nullptr;
  std::unordered_set<const Decl*> Visited;
  // (scope, new name) -> the declaration that claimed it first.
  std::map<std::pair<const DeclContext*, std::string>, const NamedDecl*>
      Claimed;
};

// ---------------------------------------------------------------------------
// Dependent member tokens (cross-TU resolution)
// ---------------------------------------------------------------------------

// The canonical rename-map key for the member a MemberExpr resolves to, or null
// if the member is not one of the kinds we rename.  Mirrors the dispatch in
// ApplyRenamesVisitor::VisitMemberExpr.
static const Decl* memberExprKey(const MemberExpr* E) {
  if (const auto* FD = dyn_cast<FieldDecl>(E->getMemberDecl()))
    return primaryTemplateMember(FD);
  if (const auto* VD = dyn_cast<VarDecl>(E->getMemberDecl()))
    return primaryTemplateStaticMember(VD)->getCanonicalDecl();
  if (const auto* MD = dyn_cast<CXXMethodDecl>(E->getMemberDecl()))
    return primaryTemplateMethod(MD)->getCanonicalDecl();
  return nullptr;
}

// The canonical rename-map key for what a DeclRefExpr refers to, or null if it
// is not one of the kinds we rename.  Mirrors the dispatch in
// ApplyRenamesVisitor::VisitDeclRefExpr.
static const Decl* declRefKey(const DeclRefExpr* E) {
  const ValueDecl* D = E->getDecl();
  if (!D->getDeclName().isIdentifier()) return nullptr;
  if (const auto* VD = dyn_cast<VarDecl>(D))
    return VD->isStaticDataMember()
               ? primaryTemplateStaticMember(VD)->getCanonicalDecl()
               : VD->getCanonicalDecl();
  if (const auto* FD = dyn_cast<FieldDecl>(D)) return primaryTemplateMember(FD);
  if (const auto* MD = dyn_cast<CXXMethodDecl>(D))
    return primaryTemplateMethod(MD)->getCanonicalDecl();
  return nullptr;
}

// Pass A (cheap, no instantiations): find the locations of template-dependent
// member tokens (`x.val` where x is dependent) that live in files we own.  If
// none exist we can skip the expensive instantiation walk of Pass B entirely.
class DependentTokenCollector
    : public RecursiveASTVisitor<DependentTokenCollector> {
 public:
  DependentTokenCollector(SourceManager& SM, const FileSet& CollectFrom,
                          std::set<std::pair<std::string, unsigned>>& Locs)
      : SM(SM), CollectFrom(CollectFrom), Locs(Locs) {}

  bool VisitCXXDependentScopeMemberExpr(CXXDependentScopeMemberExpr* E) {
    SourceLocation Loc = E->getMemberLoc();
    // A dependent token written as a macro *argument* is spelled at the call
    // site, so it is an ordinary candidate -- `EXPECT_FALSE(this->table_)` in a
    // TYPED_TEST is the common shape.  ownedKey() keys by the spelling, so the
    // recorder and the applier agree on the same location.  Only a token with
    // no spelling of its own (macro body, ## paste) is skipped.
    if (!rewriteLocFor(Loc, SM).isValid()) return true;
    if (auto Key = ownedKey(Loc, SM, CollectFrom)) Locs.insert(*Key);
    return true;
  }

  // `Helper<T>::member` -- a qualified *name* whose lookup is deferred to
  // instantiation, not a member access on an object.  Same problem and same
  // treatment as `x.member`: the token is spelled here, and which entity it
  // names is only learned from an instantiation, usually in another TU.
  bool VisitDependentScopeDeclRefExpr(DependentScopeDeclRefExpr* E) {
    SourceLocation Loc = E->getLocation();
    if (!rewriteLocFor(Loc, SM).isValid()) return true;
    if (auto Key = ownedKey(Loc, SM, CollectFrom)) Locs.insert(*Key);
    return true;
  }

 private:
  SourceManager& SM;
  const FileSet& CollectFrom;
  std::set<std::pair<std::string, unsigned>>& Locs;
};

// Pass B (walks template instantiations): for every resolved member access that
// lands on a known dependent-token location, record the new name it resolves
// to.  A binding to a member that is not being renamed vetoes the location, so
// a template instantiated with a type outside the FileSet is never left with a
// dangling member access.
class RecordDependentResolutionsVisitor
    : public RecursiveASTVisitor<RecordDependentResolutionsVisitor> {
 public:
  RecordDependentResolutionsVisitor(
      SourceManager& SM, const RenameMap& Renames, const FileSet& CollectFrom,
      const std::set<std::pair<std::string, unsigned>>& DependentLocs,
      DependentResolutions& DepRes)
      : SM(SM),
        Renames(Renames),
        CollectFrom(CollectFrom),
        DependentLocs(DependentLocs),
        DepRes(DepRes) {}

  bool shouldVisitTemplateInstantiations() const { return true; }

  bool VisitMemberExpr(MemberExpr* E) {
    SourceLocation Loc = E->getMemberLoc();
    if (!rewriteLocFor(Loc, SM).isValid()) return true;
    auto Key = ownedKey(Loc, SM, CollectFrom);
    if (!Key || DependentLocs.find(*Key) == DependentLocs.end()) return true;
    const Decl* MemberKey = memberExprKey(E);
    if (!MemberKey) return true;
    const NamedDecl* Member = E->getMemberDecl();
    if (!Member->getDeclName().isIdentifier()) return true;
    llvm::StringRef Old = Member->getName();
    auto It = Renames.find(MemberKey);
    if (It != Renames.end()) {
      std::pair<std::string, unsigned> Owner = renameOwnerKey(MemberKey, SM);
      recordResolution(DepRes, *Key, It->second, Old, Old.size(),
                       {relativizeToCwd(Owner.first), Owner.second});
    } else {
      vetoResolution(DepRes, *Key);
    }
    return true;
  }

  // The resolved form of a qualified dependent name: in an instantiation
  // `Helper<T>::member` becomes an ordinary DeclRefExpr whose location still
  // points at the token in the pattern.
  bool VisitDeclRefExpr(DeclRefExpr* E) {
    SourceLocation Loc = E->getLocation();
    if (!rewriteLocFor(Loc, SM).isValid()) return true;
    auto Key = ownedKey(Loc, SM, CollectFrom);
    if (!Key || DependentLocs.find(*Key) == DependentLocs.end()) return true;
    const Decl* RefKey = declRefKey(E);
    if (!RefKey) return true;
    llvm::StringRef Old = E->getDecl()->getName();
    auto It = Renames.find(RefKey);
    if (It != Renames.end()) {
      std::pair<std::string, unsigned> Owner = renameOwnerKey(RefKey, SM);
      recordResolution(DepRes, *Key, It->second, Old, Old.size(),
                       {relativizeToCwd(Owner.first), Owner.second});
    } else {
      vetoResolution(DepRes, *Key);
    }
    return true;
  }

 private:
  SourceManager& SM;
  const RenameMap& Renames;
  const FileSet& CollectFrom;
  const std::set<std::pair<std::string, unsigned>>& DependentLocs;
  DependentResolutions& DepRes;
};

// ---------------------------------------------------------------------------
// Pass 2: apply renames at every declaration and use site
//
// Runs twice over each TU.  The Scan pass rewrites nothing; it only looks for
// references the Rewriter cannot express -- a token spelled in a macro body, or
// one synthesized by token pasting -- and vetoes the declaration they belong
// to.  The Rewrite pass then applies whatever survived.  Scanning first is what
// makes a rename all-or-nothing within a TU: no edit is buffered until every
// reference in that TU has been seen.
// ---------------------------------------------------------------------------

enum class ApplyMode { Scan, Rewrite };

// The names a function declares: parameters and local variables (ParmVarDecl is
// a VarDecl, so one case covers both).  A rename onto one of these would be
// captured by it at every use inside that function.
class LocalNameCollector : public RecursiveASTVisitor<LocalNameCollector> {
 public:
  explicit LocalNameCollector(std::set<std::string>& Out) : Out(Out) {}

  bool VisitVarDecl(VarDecl* D) {
    if (D->getDeclName().isIdentifier() && !D->getName().empty())
      Out.insert(D->getName().str());
    return true;
  }

 private:
  std::set<std::string>& Out;
};

class ApplyRenamesVisitor : public RecursiveASTVisitor<ApplyRenamesVisitor> {
 public:
  ApplyRenamesVisitor(Rewriter& RW, SourceManager& SM, const RenameMap& Renames,
                      const FileSet& CollectFrom, DependentResolutions* DepRes,
                      LintReport* Report, std::string RuleId, EditReport* Edits,
                      ApplyMode Mode = ApplyMode::Rewrite,
                      RenameVetoes* Vetoes = nullptr,
                      RenameConflicts* Conflicts = nullptr)
      : RW(RW),
        SM(SM),
        Renames(Renames),
        CollectFrom(CollectFrom),
        DepRes(DepRes),
        Report(Report),
        RuleId(std::move(RuleId)),
        Edits(Edits),
        Mode(Mode),
        Vetoes(Vetoes),
        Conflicts(Conflicts) {}

  // While scanning, every function pushes the names it declares (parameters and
  // locals), so scan() can tell whether a rename would be captured by one.  The
  // whole function is collected up front rather than tracked as the walk
  // descends: that over-approximates -- a local in a sibling block does not
  // really shadow -- and over-vetoing is the safe direction here, since the
  // failure it prevents is silent.
  bool TraverseDecl(Decl* D) {
    using Base = RecursiveASTVisitor<ApplyRenamesVisitor>;
    auto* FD = dyn_cast_or_null<FunctionDecl>(D);
    if (Mode != ApplyMode::Scan || !FD) return Base::TraverseDecl(D);
    LocalNames.emplace_back();
    LocalNameCollector(LocalNames.back()).TraverseDecl(FD);
    const bool Result = Base::TraverseDecl(D);
    LocalNames.pop_back();
    return Result;
  }

  // Only while scanning.  A dependent token spelled in a macro body has no
  // MemberExpr in the pattern -- which member it names is known only in an
  // instantiation -- so without this the member would be renamed at its
  // declaration and the macro left spelling the old name.  The rewrite pass
  // must never walk instantiations: their locations point back into the
  // pattern, which is rewritten once through the pattern's own nodes.
  bool shouldVisitTemplateInstantiations() const {
    return Mode == ApplyMode::Scan;
  }

  auto rewriteLoc(SourceLocation Loc) const -> SourceLocation {
    return rewriteLocFor(Loc, SM);
  }

  // Each file is rewritten by exactly one TU: its own.
  auto owns(SourceLocation Loc) const -> bool {
    SourceLocation RL = rewriteLoc(Loc);
    return RL.isValid() && SM.getFileID(RL) == SM.getMainFileID();
  }

  // Applies one rewrite and, in Lint mode, records the matching diagnostic; in
  // Emit mode, appends a structured edit record instead of (only) rewriting.
  void renameAt(SourceLocation Loc, const Decl* Key, StringRef OldName,
                const std::string& NewName) {
    Loc = rewriteLoc(Loc);
    if (Loc.isInvalid()) return;
    // One spelled token can arrive through several AST nodes when a macro
    // expands the same argument more than once (`#define TWICE(x) ((x)+(x))`).
    if (!Rewritten.insert(SM.getDecomposedLoc(Loc)).second) return;
    RW.ReplaceText(Loc, OldName.size(), NewName);
    if (Report) {
      PresumedLoc PLoc = SM.getPresumedLoc(Loc);
      Report->add({PLoc.isValid() ? PLoc.getFilename() : "", PLoc.getLine(),
                   PLoc.getColumn(), RuleId,
                   "'" + OldName.str() + "' should be '" + NewName + "'"});
    }
    if (Edits) {
      std::pair<std::string, unsigned> K = locKey(Loc, SM);
      std::pair<std::string, unsigned> Owner = ownerKey(Key);
      Edits->Edits.push_back({relativizeToCwd(K.first), K.second,
                              static_cast<unsigned>(OldName.size()),
                              OldName.str(), NewName,
                              relativizeToCwd(Owner.first), Owner.second});
    }
  }

  bool VisitFieldDecl(FieldDecl* D) {
    handle(D->getLocation(), D->getCanonicalDecl(), D->getName());
    return true;
  }

  bool VisitVarDecl(VarDecl* D) {
    handle(D->getLocation(),
           D->isStaticDataMember()
               ? primaryTemplateStaticMember(D)->getCanonicalDecl()
               : D->getCanonicalDecl(),
           D->getName());
    return true;
  }

  bool VisitCXXMethodDecl(CXXMethodDecl* D) {
    // Constructors, destructors, conversion functions and operators have no
    // plain identifier; getName() asserts on those.  They are never renamed
    // (isRenamableMethod), so they are never in the map either.
    if (!D->getDeclName().isIdentifier()) return true;
    handle(D->getLocation(), primaryTemplateMethod(D)->getCanonicalDecl(),
           D->getName());
    return true;
  }

  bool VisitDeclRefExpr(DeclRefExpr* E) {
    const Decl* Key = nullptr;
    StringRef OldName;
    if (const auto* VD = dyn_cast<VarDecl>(E->getDecl())) {
      Key = VD->isStaticDataMember()
                ? primaryTemplateStaticMember(VD)->getCanonicalDecl()
                : VD->getCanonicalDecl();
      OldName = VD->getName();
    } else if (const auto* FD = dyn_cast<FieldDecl>(E->getDecl())) {
      Key = primaryTemplateMember(FD);
      OldName = FD->getName();
    } else if (const auto* MD = dyn_cast<CXXMethodDecl>(E->getDecl())) {
      // Unqualified calls to static member functions and pointers to member
      // functions (e.g. `S::count`, `&S::get`).  Coroutine desugaring can
      // leave references to methods without a simple identifier (conversion
      // operators, lambda `operator()`); getName() asserts on those.
      if (!MD->getDeclName().isIdentifier()) return true;
      Key = primaryTemplateMethod(MD)->getCanonicalDecl();
      OldName = MD->getName();
    }
    handle(E->getLocation(), Key, OldName,
           /*Unqualified=*/E->getQualifier() == nullptr);
    return true;
  }

  bool VisitMemberExpr(MemberExpr* E) {
    const Decl* Key = nullptr;
    if (const auto* FD = dyn_cast<FieldDecl>(E->getMemberDecl()))
      Key = primaryTemplateMember(FD);
    else if (const auto* VD = dyn_cast<VarDecl>(E->getMemberDecl()))
      Key = primaryTemplateStaticMember(VD)->getCanonicalDecl();
    else if (const auto* MD = dyn_cast<CXXMethodDecl>(E->getMemberDecl()))
      // Member function calls: `obj.get()`, `ptr->get()`, implicit `this`.
      Key = primaryTemplateMethod(MD)->getCanonicalDecl();
    if (!Key || !E->getMemberDecl()->getDeclName().isIdentifier()) return true;
    // `m` alone is an implicit `this->m`, and unqualified lookup finds it -- a
    // local of the new name would capture it.  Anything with a base or a
    // qualifier written out (`this->m`, `obj.m`, `Base::m`) cannot be captured.
    const auto* This = dyn_cast<CXXThisExpr>(E->getBase()->IgnoreImpCasts());
    const bool Unqualified =
        This != nullptr && This->isImplicit() && E->getQualifier() == nullptr;
    handle(E->getMemberLoc(), Key, E->getMemberDecl()->getName(), Unqualified);
    return true;
  }

  // Constructor mem-initializers (e.g. `S() : val_(0)`).  These are not visited
  // via VisitMemberExpr because CXXCtorInitializer is not a Stmt/Decl.
  bool TraverseConstructorInitializer(CXXCtorInitializer* Init) {
    if (Init && Init->isAnyMemberInitializer()) {
      if (const FieldDecl* FD = Init->getAnyMember())
        handle(Init->getMemberLocation(), primaryTemplateMember(FD),
               FD->getName());
    }
    return RecursiveASTVisitor<
        ApplyRenamesVisitor>::TraverseConstructorInitializer(Init);
  }

  // Designated initializers (e.g. `S s{.val_ = 0}`).  The field name lives in
  // the designator of a DesignatedInitExpr, not in a MemberExpr.
  bool VisitDesignatedInitExpr(DesignatedInitExpr* E) {
    for (const DesignatedInitExpr::Designator& D : E->designators()) {
      if (!D.isFieldDesignator()) continue;
      if (const FieldDecl* FD = D.getFieldDecl())
        handle(D.getFieldLoc(), primaryTemplateMember(FD), FD->getName());
    }
    return true;
  }

  // Template-dependent member access (e.g. `x.val` where x is a template
  // parameter).  The member is unresolved in this TU, so we rewrite it from the
  // cross-TU resolution recorded by the TUs that instantiate the template.
  // Only the token in its own main file is rewritten (as for every other site),
  // which -- combined with headers-last ordering -- keeps each file written
  // once.  There is no declaration to veto here: which member the token names
  // is exactly what this TU does not know, and DependentResolutions carries its
  // own veto for that.
  bool VisitCXXDependentScopeMemberExpr(CXXDependentScopeMemberExpr* E) {
    if (Mode == ApplyMode::Scan) return true;
    // Emit mode defers dependent-token edits to the aggregation phase (which
    // has the full cross-TU resolution picture); here we only serialize the
    // resolutions this TU observed.
    if (!DepRes || Edits) return true;
    SourceLocation Loc = E->getMemberLoc();
    if (!owns(Loc)) return true;
    auto Key = ownedKey(Loc, SM, CollectFrom);
    if (!Key) return true;
    auto It = DepRes->find(*Key);
    if (It == DepRes->end() || It->second.Vetoed || !It->second.HasName)
      return true;
    const IdentifierInfo* II = E->getMember().getAsIdentifierInfo();
    if (!II) return true;
    renameAt(Loc, nullptr, II->getName(), It->second.NewName);
    return true;
  }

  // The qualified-name counterpart (`Helper<T>::member`).  Same deal: rewritten
  // from the resolution the instantiating TUs recorded, in its own main file.
  bool VisitDependentScopeDeclRefExpr(DependentScopeDeclRefExpr* E) {
    if (Mode == ApplyMode::Scan) return true;
    if (!DepRes || Edits) return true;
    SourceLocation Loc = E->getLocation();
    if (!owns(Loc)) return true;
    auto Key = ownedKey(Loc, SM, CollectFrom);
    if (!Key) return true;
    auto It = DepRes->find(*Key);
    if (It == DepRes->end() || It->second.Vetoed || !It->second.HasName)
      return true;
    const IdentifierInfo* II = E->getDeclName().getAsIdentifierInfo();
    if (!II) return true;
    renameAt(Loc, nullptr, II->getName(), It->second.NewName);
    return true;
  }

 private:
  // One reference site, in whichever mode this pass is running.
  // \p Unqualified says the reference finds the member by unqualified name
  // lookup, so a local of the new name would capture it.  False for anything
  // written with an explicit qualifier or base (`this->m`, `obj.m`, `S::m`),
  // for declarations, and for the member name in a mem-initializer or a field
  // designator -- those are looked up in the class, never in the local scope.
  void handle(SourceLocation Loc, const Decl* Key, StringRef OldName,
              bool Unqualified = false) {
    if (!Key || Loc.isInvalid()) return;
    auto It = Renames.find(Key);
    if (It == Renames.end()) return;
    if (Mode == ApplyMode::Scan) {
      scan(Loc, Key, OldName, It->second, Unqualified);
      return;
    }
    if (!owns(Loc)) return;
    renameAt(Loc, Key, OldName, It->second);
  }

  // Two ways a reference can make a rename unsafe.
  void scan(SourceLocation Loc, const Decl* Key, StringRef OldName,
            const std::string& NewName, bool Unqualified) {
    if (!Vetoes) return;
    // (a) No rewritable spelling, so the reference would keep the old name.
    // This deliberately ignores file ownership: a macro in a header we do not
    // own that names one of our members is just as fatal, and just as
    // unrewritable.
    if (!rewriteLoc(Loc).isValid()) {
      SourceLocation Spelling = SM.getSpellingLoc(Loc);
      std::string Reason;
      if (!SM.getFileEntryForID(SM.getFileID(Spelling))) {
        Reason = "the name is formed by token pasting";
      } else {
        PresumedLoc PLoc = SM.getPresumedLoc(Spelling);
        Reason = "referenced from a macro body";
        if (PLoc.isValid())
          Reason += " at " + relativizeToCwd(PLoc.getFilename()) + ":" +
                    std::to_string(PLoc.getLine());
      }
      veto(Key, OldName, NewName, Reason);
      return;
    }
    // (b) The token can be rewritten, but would no longer name the member: it
    // is found by unqualified lookup and a local or parameter of the new name
    // is in scope here, so the rewrite silently rebinds the use.  googletest's
    // OnCallSpec::action_ -> action, inside WillByDefault(const Action<F>&
    // action), turns `action_ = action` into the self-assignment `action =
    // action`.  Every enclosing function is checked, not just the innermost,
    // since a lambda body can name either.
    if (!Unqualified) return;
    for (const std::set<std::string>& Frame : LocalNames) {
      if (Frame.find(NewName) == Frame.end()) continue;
      veto(Key, OldName, NewName,
           "'" + NewName + "' is a local or parameter where it is used");
      return;
    }
  }

  void veto(const Decl* Key, StringRef OldName, const std::string& NewName,
            const std::string& Reason) {
    if (!addRenameVeto(Vetoes, Key, SM, OldName, Reason)) return;
    if (!Conflicts) return;
    const PresumedLoc PL =
        SM.getPresumedLoc(cast<NamedDecl>(Key)->getLocation());
    Conflicts->push_back(RenameConflict{
        PL.isValid() ? PL.getFilename() : "", PL.isValid() ? PL.getLine() : 0,
        PL.isValid() ? PL.getColumn() : 0, OldName.str(), NewName, Reason});
  }

  auto ownerKey(const Decl* Key) -> std::pair<std::string, unsigned> {
    if (!Key) return {};
    auto It = OwnerKeys.find(Key);
    if (It != OwnerKeys.end()) return It->second;
    return OwnerKeys.emplace(Key, renameOwnerKey(Key, SM)).first->second;
  }

  Rewriter& RW;
  SourceManager& SM;
  const RenameMap& Renames;
  const FileSet& CollectFrom;
  DependentResolutions* DepRes;  // null when the feature is disabled
  LintReport* Report;            // null outside Lint mode
  std::string RuleId;
  EditReport* Edits;  // non-null in Emit mode
  ApplyMode Mode;
  RenameVetoes* Vetoes;        // non-null when all-or-nothing is enabled
  RenameConflicts* Conflicts;  // where a veto is reported
  std::set<std::pair<FileID, unsigned>> Rewritten;
  std::map<const Decl*, std::pair<std::string, unsigned>> OwnerKeys;
  // One frame per enclosing function, innermost last.  Scan mode only.
  std::vector<std::set<std::string>> LocalNames;
};

// ---------------------------------------------------------------------------
// Debug tracer — visits every reference to a renamed variable and logs it.
// Used by OutputMode::Debug so users can see, per TU, exactly which sites the
// tool detected and which would actually be rewritten (main-file + non-macro).
// ---------------------------------------------------------------------------

class DebugTraceVisitor : public RecursiveASTVisitor<DebugTraceVisitor> {
 public:
  DebugTraceVisitor(SourceManager& SM, const RenameMap& Renames,
                    llvm::raw_ostream& Out)
      : SM(SM), Renames(Renames), Out(Out) {}

  bool VisitFieldDecl(FieldDecl* D) {
    auto It = Renames.find(D->getCanonicalDecl());
    if (It != Renames.end())
      log("FieldDecl ", D->getLocation(), D->getName(), It->second);
    return true;
  }
  bool VisitVarDecl(VarDecl* D) {
    const Decl* Key = D->isStaticDataMember()
                          ? primaryTemplateStaticMember(D)->getCanonicalDecl()
                          : D->getCanonicalDecl();
    auto It = Renames.find(Key);
    if (It != Renames.end())
      log("VarDecl   ", D->getLocation(), D->getName(), It->second);
    return true;
  }
  bool VisitCXXMethodDecl(CXXMethodDecl* D) {
    auto It = Renames.find(primaryTemplateMethod(D)->getCanonicalDecl());
    if (It != Renames.end())
      log("MethodDecl", D->getLocation(), D->getName(), It->second);
    return true;
  }
  bool VisitDeclRefExpr(DeclRefExpr* E) {
    const Decl* Key = nullptr;
    StringRef OldName;
    if (const auto* VD = dyn_cast<VarDecl>(E->getDecl())) {
      Key = VD->isStaticDataMember()
                ? primaryTemplateStaticMember(VD)->getCanonicalDecl()
                : VD->getCanonicalDecl();
      OldName = VD->getName();
    } else if (const auto* FD = dyn_cast<FieldDecl>(E->getDecl())) {
      Key = primaryTemplateMember(FD);
      OldName = FD->getName();
    } else if (const auto* MD = dyn_cast<CXXMethodDecl>(E->getDecl())) {
      if (!MD->getDeclName().isIdentifier()) return true;
      Key = primaryTemplateMethod(MD)->getCanonicalDecl();
      OldName = MD->getName();
    }
    if (!Key) return true;
    auto It = Renames.find(Key);
    if (It != Renames.end())
      log("DeclRef   ", E->getLocation(), OldName, It->second);
    return true;
  }
  bool VisitMemberExpr(MemberExpr* E) {
    const Decl* Key = nullptr;
    StringRef OldName;
    if (const auto* FD = dyn_cast<FieldDecl>(E->getMemberDecl())) {
      Key = primaryTemplateMember(FD);
      OldName = FD->getName();
    } else if (const auto* VD = dyn_cast<VarDecl>(E->getMemberDecl())) {
      Key = primaryTemplateStaticMember(VD)->getCanonicalDecl();
      OldName = VD->getName();
    } else if (const auto* MD = dyn_cast<CXXMethodDecl>(E->getMemberDecl())) {
      if (!MD->getDeclName().isIdentifier()) return true;
      Key = primaryTemplateMethod(MD)->getCanonicalDecl();
      OldName = MD->getName();
    }
    if (!Key) return true;
    auto It = Renames.find(Key);
    if (It != Renames.end())
      log("MemberExpr", E->getMemberLoc(), OldName, It->second);
    return true;
  }

  bool TraverseConstructorInitializer(CXXCtorInitializer* Init) {
    if (Init && Init->isAnyMemberInitializer()) {
      if (const FieldDecl* FD = Init->getAnyMember()) {
        auto It = Renames.find(primaryTemplateMember(FD));
        if (It != Renames.end())
          log("CtorInit  ", Init->getMemberLocation(), FD->getName(),
              It->second);
      }
    }
    return RecursiveASTVisitor<
        DebugTraceVisitor>::TraverseConstructorInitializer(Init);
  }

  bool VisitDesignatedInitExpr(DesignatedInitExpr* E) {
    for (const DesignatedInitExpr::Designator& D : E->designators()) {
      if (!D.isFieldDesignator()) continue;
      const FieldDecl* FD = D.getFieldDecl();
      if (!FD) continue;
      auto It = Renames.find(primaryTemplateMember(FD));
      if (It != Renames.end())
        log("DesignInit", D.getFieldLoc(), FD->getName(), It->second);
    }
    return true;
  }

 private:
  void log(StringRef Kind, SourceLocation Loc, StringRef OldName,
           const std::string& NewName) {
    auto PLoc = SM.getPresumedLoc(Loc);
    const bool isMacro = Loc.isMacroID();
    // Where the applier would rewrite: the token itself, or -- for a macro
    // argument -- the call site that spells it.  Invalid means the name has no
    // spelling of its own (macro body, or ## paste), which vetoes the rename
    // for every site, not just this one.
    const SourceLocation RL = rewriteLocFor(Loc, SM);
    const bool inMain = RL.isValid() && SM.getFileID(RL) == SM.getMainFileID();
    const char* file = PLoc.isValid() ? PLoc.getFilename() : "<invalid>";
    unsigned line = PLoc.isValid() ? PLoc.getLine() : 0;
    unsigned col = PLoc.isValid() ? PLoc.getColumn() : 0;
    Out << "    " << Kind << "  " << OldName << " -> " << NewName << "  at "
        << file << ":" << line << ":" << col
        << "  [main=" << (inMain ? "Y" : "N")
        << " macro=" << (isMacro ? "Y" : "N") << "]";
    if (!RL.isValid())
      Out << "  VETOES_RENAME";
    else if (inMain)
      Out << "  WILL_RENAME";
    Out << "\n";
  }

  SourceManager& SM;
  const RenameMap& Renames;
  llvm::raw_ostream& Out;
};

// ---------------------------------------------------------------------------
// ASTConsumer
// ---------------------------------------------------------------------------

class RenameVariablesConsumer : public ASTConsumer {
 public:
  RenameVariablesConsumer(Rewriter& RW, VariableRenameCallback CB,
                          VariableScope Scope, FileSet CollectFrom,
                          OutputMode Mode = OutputMode::DryRun,
                          LintReport* Report = nullptr, std::string RuleId = "",
                          DependentResolutions* DepRes = nullptr,
                          EditReport* Edits = nullptr,
                          RenameConflicts* Conflicts = nullptr,
                          RenameVetoes* Vetoes = nullptr)
      : RW(RW),
        CB(std::move(CB)),
        Scope(Scope),
        CollectFrom(std::move(CollectFrom)),
        Mode(Mode),
        Report(Report),
        RuleId(std::move(RuleId)),
        DepRes(DepRes),
        Edits(Edits),
        Conflicts(Conflicts),
        Vetoes(Vetoes) {}

  void HandleTranslationUnit(ASTContext& Ctx) override {
    SourceManager& SM = Ctx.getSourceManager();

    if (Mode == OutputMode::Debug) {
      RenameMap Renames;
      CollectRenamesVisitor Collector(SM, CB, Scope, Renames, CollectFrom);
      Collector.TraverseDecl(Ctx.getTranslationUnitDecl());
      // LLVM 18 removed FileEntry::getName(); a file may be reachable under
      // several names, so the name now lives on FileEntryRef.
      OptionalFileEntryRef FE = SM.getFileEntryRefForID(SM.getMainFileID());
      llvm::errs()
          << "============================================================\n";
      llvm::errs() << "TU: " << (FE ? FE->getName() : "<unknown>") << "\n";
      llvm::errs() << "Identified " << Renames.size() << " rename(s):\n";
      for (const auto& [DeclKey, NewName] : Renames) {
        const auto* ND = cast<NamedDecl>(DeclKey);
        auto PLoc = SM.getPresumedLoc(ND->getLocation());
        llvm::errs() << "  \"" << ND->getName() << "\" -> \"" << NewName
                     << "\"  (decl at "
                     << (PLoc.isValid() ? PLoc.getFilename() : "<invalid>")
                     << ":" << (PLoc.isValid() ? PLoc.getLine() : 0) << ")\n";
      }
      if (!Renames.empty()) {
        llvm::errs() << "Reference sites in this TU's AST:\n";
        DebugTraceVisitor Tracer(SM, Renames, llvm::errs());
        Tracer.TraverseDecl(Ctx.getTranslationUnitDecl());
      }
      return;  // no rewrites in Debug mode
    }

    runRenameRuleOnAST(Ctx, RW, CB, Scope, CollectFrom, Report, RuleId, DepRes,
                       Edits, Conflicts, Vetoes);
  }

 private:
  Rewriter& RW;
  VariableRenameCallback CB;
  VariableScope Scope;
  FileSet CollectFrom;
  OutputMode Mode;
  LintReport* Report;  // null outside Lint mode
  std::string RuleId;
  DependentResolutions* DepRes;  // null when the feature is disabled
  EditReport* Edits;             // non-null in Emit mode
  RenameConflicts* Conflicts;    // non-null when collecting
  RenameVetoes* Vetoes;          // non-null when all-or-nothing is enabled
};

// ---------------------------------------------------------------------------
// RenameVariablesAction  (internal — not in the public header)
//
// In InPlace mode writes are NOT flushed to disk immediately; instead the
// modified content is stored in *Pending so that RenameActionFactory::flush()
// can write every file atomically after ClangTool::run() completes.  This
// ensures that every TU compiles against the original on-disk source,
// regardless of how many files share headers.
// ---------------------------------------------------------------------------

class RenameVariablesAction : public ASTFrontendAction {
 public:
  RenameVariablesAction(VariableRenameCallback CB, VariableScope Scope,
                        OutputMode Mode, const FileSet& CollectFrom,
                        PendingRewrites* Pending, LintReport* Report,
                        std::string RuleId, DependentResolutions* DepRes,
                        EditReport* Edits, RenameConflicts* Conflicts,
                        RenameVetoes* Vetoes)
      : CB(std::move(CB)),
        Scope(Scope),
        Mode(Mode),
        CollectFrom(CollectFrom),
        Pending(Pending),
        Report(Report),
        RuleId(std::move(RuleId)),
        DepRes(DepRes),
        Edits(Edits),
        Conflicts(Conflicts),
        Vetoes(Vetoes) {}

  void EndSourceFileAction() override {
    SourceManager& SM = TheRewriter.getSourceMgr();
    FileID MainFID = SM.getMainFileID();

    // Buffer the main file's content only if it actually has edits.
    // Applies to both DryRun and InPlace: DryRun outputs via flush().
    for (auto It = TheRewriter.buffer_begin(); It != TheRewriter.buffer_end();
         ++It) {
      if (It->first != MainFID) continue;
      const FileEntry* FE = SM.getFileEntryForID(MainFID);
      if (!FE) break;
      std::string Path = FE->tryGetRealPathName().str();
      if (Path.empty()) break;
      std::string Content;
      llvm::raw_string_ostream OS(Content);
      It->second.write(OS);
      (*Pending)[std::move(Path)] = std::move(Content);
      break;
    }
  }

  auto CreateASTConsumer(CompilerInstance& CI, StringRef)
      -> std::unique_ptr<ASTConsumer> override {
    TheRewriter.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
    return std::make_unique<RenameVariablesConsumer>(
        TheRewriter, CB, Scope, CollectFrom, Mode, Report, RuleId, DepRes,
        Edits, Conflicts, Vetoes);
  }

 private:
  VariableRenameCallback CB;
  VariableScope Scope;
  OutputMode Mode;
  const FileSet& CollectFrom;
  PendingRewrites* Pending;
  LintReport* Report;
  std::string RuleId;
  DependentResolutions* DepRes;
  EditReport* Edits;
  RenameConflicts* Conflicts;
  RenameVetoes* Vetoes;
  Rewriter TheRewriter;
};

// ---------------------------------------------------------------------------
// CaptureAction — used by the test helper (single in-memory TU, no disk I/O)
// ---------------------------------------------------------------------------

class CaptureAction : public ASTFrontendAction {
 public:
  CaptureAction(VariableRenameCallback CB, VariableScope Scope,
                std::string& Output)
      : CB(std::move(CB)), Scope(Scope), Output(Output) {}

  void EndSourceFileAction() override {
    llvm::raw_string_ostream OS(Output);
    TheRewriter.getEditBuffer(TheRewriter.getSourceMgr().getMainFileID())
        .write(OS);
  }

  auto CreateASTConsumer(CompilerInstance& CI, StringRef)
      -> std::unique_ptr<ASTConsumer> override {
    TheRewriter.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
    return std::make_unique<RenameVariablesConsumer>(
        TheRewriter, CB, Scope, FileSet{}, OutputMode::DryRun,
        /*Report=*/nullptr, /*RuleId=*/"", &DepRes, /*Edits=*/nullptr,
        /*Conflicts=*/nullptr, &Vetoes);
  }

 private:
  VariableRenameCallback CB;
  VariableScope Scope;
  Rewriter TheRewriter;
  std::string& Output;
  // Single in-memory TU: instantiations and their dependent tokens are in the
  // same file, so intra-TU resolution needs a place to record them.
  DependentResolutions DepRes;
  // Likewise for vetoes: with one TU the scan pass always runs before the
  // rewrite pass, so no re-run is needed to make them order-independent.
  RenameVetoes Vetoes;
};

}  // namespace

// ---------------------------------------------------------------------------
// reportRenameConflicts (public)
// ---------------------------------------------------------------------------

void reportRenameConflicts(const RenameConflicts& Conflicts, bool Verbose,
                           llvm::raw_ostream& OS) {
  if (Conflicts.empty()) return;
  // One declaration is seen once per TU that includes it, so collapse.
  std::set<std::tuple<std::string, unsigned, unsigned, std::string>> Seen;
  for (const RenameConflict& C : Conflicts) {
    if (!Seen.emplace(C.File, C.Line, C.Column, C.NewName).second) continue;
    if (Verbose)
      OS << relativizeToCwd(C.File) << ":" << C.Line << ":" << C.Column
         << ": skipped rename '" << C.OldName << "' -> '" << C.NewName
         << "': " << C.Reason << "\n";
  }
  OS << Seen.size()
     << " rename(s) skipped (name collision, or a reference that cannot be "
        "rewritten)"
     << (Verbose ? "" : "; pass --report-rename-conflicts for the sites")
     << "\n";
}

// ---------------------------------------------------------------------------
// runRenameRuleOnAST (public)
// ---------------------------------------------------------------------------

void runRenameRuleOnAST(ASTContext& Ctx, Rewriter& RW,
                        const VariableRenameCallback& CB, VariableScope Scope,
                        const FileSet& CollectFrom, LintReport* Report,
                        llvm::StringRef RuleId, DependentResolutions* DepRes,
                        EditReport* Edits, RenameConflicts* Conflicts,
                        RenameVetoes* Vetoes) {
  SourceManager& SM = Ctx.getSourceManager();
  Decl* TU = Ctx.getTranslationUnitDecl();

  RenameMap Renames;
  CollectRenamesVisitor Collector(SM, CB, Scope, Renames, CollectFrom,
                                  Conflicts, Vetoes);
  Collector.TraverseDecl(TU);

  // Scan for references this TU cannot rewrite — a name spelled in a macro
  // body, or formed by token pasting — and drop those renames before anything
  // is applied.  Renaming is all-or-nothing: half a rename does not compile.
  // Declarations vetoed by an *earlier* TU never made it into Renames above;
  // one vetoed here may already have been renamed by an earlier TU, which is
  // why the drivers re-run the whole tool once when any veto was recorded.
  if (Vetoes && !Renames.empty()) {
    const size_t Before = Vetoes->size();
    ApplyRenamesVisitor Scanner(RW, SM, Renames, CollectFrom, DepRes, Report,
                                RuleId.str(), Edits, ApplyMode::Scan, Vetoes,
                                Conflicts);
    Scanner.TraverseDecl(TU);
    if (Vetoes->size() != Before) {
      for (auto It = Renames.begin(); It != Renames.end();) {
        std::pair<std::string, unsigned> Owner = renameOwnerKey(It->first, SM);
        if (Vetoes->count({relativizeToCwd(Owner.first), Owner.second}) > 0)
          It = Renames.erase(It);
        else
          ++It;
      }
    }
  }

  // Template-dependent member tokens (e.g. `x.val` where x is a template
  // parameter) spelled in files we own.  Pass A is cheap (no instantiations)
  // and gates the rest: absent such tokens the whole feature is a no-op.
  std::set<std::pair<std::string, unsigned>> DependentLocs;
  if (DepRes) {
    DependentTokenCollector Collect(SM, CollectFrom, DependentLocs);
    Collect.TraverseDecl(TU);
  }

  // Record what this TU's instantiations resolve those tokens to.  Needs a
  // rename map, and only pays for the instantiation walk when there is a token
  // to resolve.
  if (DepRes && !Renames.empty() && !DependentLocs.empty()) {
    RecordDependentResolutionsVisitor Recorder(SM, Renames, CollectFrom,
                                               DependentLocs, *DepRes);
    Recorder.TraverseDecl(TU);
  }

  // Apply.  Besides its own declarations/uses (Renames), this TU may spell a
  // dependent token that an earlier TU already resolved — the header that
  // defines a template is typically processed after, and declares nothing to
  // rename itself — so run the apply pass whenever either has work.
  const bool ApplyDependent =
      DepRes && std::any_of(DependentLocs.begin(), DependentLocs.end(),
                            [&](const auto& L) {
                              auto It = DepRes->find(L);
                              return It != DepRes->end() &&
                                     It->second.HasName && !It->second.Vetoed;
                            });
  if (Renames.empty() && !ApplyDependent) return;
  ApplyRenamesVisitor Applier(RW, SM, Renames, CollectFrom, DepRes, Report,
                              RuleId.str(), Edits);
  Applier.TraverseDecl(TU);
}

// ---------------------------------------------------------------------------
// RenameActionFactory (public)
// ---------------------------------------------------------------------------

RenameActionFactory::RenameActionFactory(VariableRenameCallback CB,
                                         VariableScope Scope, OutputMode Mode,
                                         FileSet CollectFrom)
    : CB(std::move(CB)),
      Scope(Scope),
      Mode(Mode),
      CollectFrom(std::move(CollectFrom)) {}

auto RenameActionFactory::createAction(TUSlot& Slot)
    -> std::unique_ptr<clang::FrontendAction> {
  // Runs on a worker thread: only the tool's immutable configuration and the
  // slot's own members are touched.  A null Report stays null -- a non-null
  // pointer is what switches the visitors' diagnostic recording on.
  return std::make_unique<RenameVariablesAction>(
      CB, Scope, Mode, CollectFrom, &Slot.Pending,
      Report ? &Slot.Report : nullptr, RuleId, &Slot.DepRes[0],
      Mode == OutputMode::Emit ? &Slot.Edits : nullptr, &Slot.Conflicts,
      &Slot.Vetoes);
}

void RenameActionFactory::finish(std::vector<TUSlot>& Slots) {
  for (TUSlot& S : Slots) {
    for (auto& [Path, Content] : S.Pending) Pending[Path] = std::move(Content);
    Edits.Edits.insert(Edits.Edits.end(),
                       std::make_move_iterator(S.Edits.Edits.begin()),
                       std::make_move_iterator(S.Edits.Edits.end()));
    Conflicts.insert(Conflicts.end(),
                     std::make_move_iterator(S.Conflicts.begin()),
                     std::make_move_iterator(S.Conflicts.end()));
    if (Report)
      for (const LintDiagnostic& D : S.Report.diagnostics()) Report->add(D);
  }
}

void RenameActionFactory::emitEdits(llvm::raw_ostream& OS) {
  // Promote the cross-TU dependent-token resolutions to sidecar records; the
  // aggregation phase resolves them across all TUs before turning survivors
  // into edits.  Keys are relativized to cwd so they match the edit records and
  // are stable across sandboxes.
  for (const DependentResolutions& Map : Shared.DepResPerRule)
    for (const auto& [Key, R] : Map) {
      if (!R.HasName && !R.Vetoed) continue;
      Edits.Resolutions.push_back({relativizeToCwd(Key.first), Key.second,
                                   R.Length, R.OldName, R.NewName, R.Vetoed,
                                   R.OwnerFile, R.OwnerOffset});
    }
  for (const auto& [Key, V] : Shared.Vetoes) Edits.Vetoes.push_back(V);
  Edits.emitJSON(OS);
}

void RenameActionFactory::flush() {
  if (Mode == OutputMode::Debug || Mode == OutputMode::Lint ||
      Mode == OutputMode::Emit) {
    Pending.clear();
    return;
  }
  if (Mode == OutputMode::DryRun) {
    bool multiFile = Pending.size() > 1;
    for (const auto& [Path, Content] : Pending) {
      if (multiFile) llvm::outs() << "=== " << Path << " ===\n";
      llvm::outs() << Content;
    }
    Pending.clear();
    return;
  }
  for (const auto& [Path, Content] : Pending) {
    std::ofstream Out(Path, std::ios::trunc | std::ios::binary);
    Out << Content;
  }
  Pending.clear();
}

// ---------------------------------------------------------------------------
// Convenience factories
// ---------------------------------------------------------------------------

auto RenameAllMemberVariables(VariableRenameCallback CB, OutputMode Mode,
                              FileSet CollectFrom)
    -> std::unique_ptr<RenameActionFactory> {
  return std::make_unique<RenameActionFactory>(
      std::move(CB), VariableScope::Member, Mode, std::move(CollectFrom));
}

auto RenameAllLocalVariables(VariableRenameCallback CB, OutputMode Mode,
                             FileSet CollectFrom)
    -> std::unique_ptr<RenameActionFactory> {
  return std::make_unique<RenameActionFactory>(
      std::move(CB), VariableScope::Local, Mode, std::move(CollectFrom));
}

auto RenameAllGlobalVariables(VariableRenameCallback CB, OutputMode Mode,
                              FileSet CollectFrom)
    -> std::unique_ptr<RenameActionFactory> {
  return std::make_unique<RenameActionFactory>(
      std::move(CB), VariableScope::Global, Mode, std::move(CollectFrom));
}

auto RenameAllStaticMemberVariables(VariableRenameCallback CB, OutputMode Mode,
                                    FileSet CollectFrom)
    -> std::unique_ptr<RenameActionFactory> {
  return std::make_unique<RenameActionFactory>(
      std::move(CB), VariableScope::StaticMember, Mode, std::move(CollectFrom));
}

auto RenameAllConstMemberVariables(VariableRenameCallback CB, OutputMode Mode,
                                   FileSet CollectFrom)
    -> std::unique_ptr<RenameActionFactory> {
  return std::make_unique<RenameActionFactory>(
      std::move(CB), VariableScope::ConstMember, Mode, std::move(CollectFrom));
}

auto RenameAllStaticGlobalVariables(VariableRenameCallback CB, OutputMode Mode,
                                    FileSet CollectFrom)
    -> std::unique_ptr<RenameActionFactory> {
  return std::make_unique<RenameActionFactory>(
      std::move(CB), VariableScope::StaticGlobal, Mode, std::move(CollectFrom));
}

auto RenameAllConstGlobalVariables(VariableRenameCallback CB, OutputMode Mode,
                                   FileSet CollectFrom)
    -> std::unique_ptr<RenameActionFactory> {
  return std::make_unique<RenameActionFactory>(
      std::move(CB), VariableScope::ConstGlobal, Mode, std::move(CollectFrom));
}

auto RenameAllMemberFunctions(VariableRenameCallback CB, OutputMode Mode,
                              FileSet CollectFrom)
    -> std::unique_ptr<RenameActionFactory> {
  return std::make_unique<RenameActionFactory>(
      std::move(CB), VariableScope::Method, Mode, std::move(CollectFrom));
}

// ---------------------------------------------------------------------------
// Source ordering helper
// ---------------------------------------------------------------------------

auto orderSourcesForRename(const std::vector<std::string>& SourcePaths)
    -> std::vector<std::string> {
  std::vector<std::string> sources, headers;
  for (const auto& P : SourcePaths)
    (isHeaderSource(P) ? headers : sources).push_back(P);
  sources.insert(sources.end(), headers.begin(), headers.end());
  return sources;
}

// ---------------------------------------------------------------------------
// Test helper
// ---------------------------------------------------------------------------

auto rewriteVariableNames(llvm::StringRef Code, VariableRenameCallback CB,
                          VariableScope Scope,
                          const std::vector<std::string>& Args) -> std::string {
  std::string Output;
  bool Ok = runToolOnCodeWithArgs(
      std::make_unique<CaptureAction>(std::move(CB), Scope, Output), Code,
      Args);
  if (!Ok || Output.empty()) return Code.str();
  return Output;
}
