#include "cpp_formatting/rename_variables_lib.h"

#include <algorithm>
#include <fstream>
#include <optional>
#include <set>
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

static const FieldDecl* primaryTemplateMember(const FieldDecl* FD) {
  const auto* RD = dyn_cast_or_null<CXXRecordDecl>(FD->getParent());
  if (!RD) return FD;
  const auto* Spec = dyn_cast<ClassTemplateSpecializationDecl>(RD);
  if (!Spec) return FD;
  CXXRecordDecl* Primary = Spec->getSpecializedTemplate()->getTemplatedDecl();
  unsigned Idx = FD->getFieldIndex();
  unsigned I = 0;
  for (FieldDecl* PF : Primary->fields()) {
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
  if (CollectFrom.empty()) return SM.isWrittenInMainFile(Loc);
  if (Loc.isInvalid()) return false;
  const FileEntry* FE = SM.getFileEntryForID(SM.getFileID(Loc));
  if (!FE) return SM.isWrittenInMainFile(Loc);
  llvm::StringRef RealPath = FE->tryGetRealPathName();
  if (RealPath.empty()) return SM.isWrittenInMainFile(Loc);
  return CollectFrom.count(RealPath.str()) > 0;
}

// ---------------------------------------------------------------------------
// Owned-file key for cross-TU dependent-token resolution
// ---------------------------------------------------------------------------

// Returns a (file identifier, byte offset) key for \p Loc when it points into a
// file the tool owns, or nullopt otherwise.  The identifier is the file's real
// path, which is stable across translation units (so a token recorded while
// compiling a.cpp matches the same token when the header is compiled as its own
// TU).  For in-memory buffers with no real path (the unit-test helper) it falls
// back to the presumed file name, which is enough for the single-TU case where
// ownership is decided by isWrittenInMainFile.
static std::optional<std::pair<std::string, unsigned>> ownedKey(
    SourceLocation Loc, SourceManager& SM, const FileSet& CollectFrom) {
  if (Loc.isInvalid()) return std::nullopt;
  SourceLocation Spelling = SM.getSpellingLoc(Loc);
  std::pair<FileID, unsigned> Decomposed = SM.getDecomposedLoc(Spelling);
  const FileEntry* FE = SM.getFileEntryForID(Decomposed.first);
  std::string Path;
  if (FE) {
    llvm::StringRef RealPath = FE->tryGetRealPathName();
    if (!RealPath.empty()) Path = RealPath.str();
  }
  const bool Owned = CollectFrom.empty()
                         ? SM.isWrittenInMainFile(Loc)
                         : (!Path.empty() && CollectFrom.count(Path) > 0);
  if (!Owned) return std::nullopt;
  if (Path.empty()) {
    PresumedLoc PLoc = SM.getPresumedLoc(Spelling);
    Path = PLoc.isValid() ? PLoc.getFilename() : "<main>";
  }
  return std::make_pair(std::move(Path), Decomposed.second);
}

// ---------------------------------------------------------------------------
// Dependent-token resolution map helpers
// ---------------------------------------------------------------------------

static void recordResolution(DependentResolutions& DepRes,
                             const std::pair<std::string, unsigned>& Key,
                             const std::string& NewName,
                             llvm::StringRef OldName, unsigned Length) {
  DependentResolution& R = DepRes[Key];
  if (R.Vetoed) return;
  if (R.HasName && R.NewName != NewName) {
    R.Vetoed = true;  // instantiations disagree — leave the token alone.
    return;
  }
  R.NewName = NewName;
  R.HasName = true;
  R.OldName = OldName.str();
  R.Length = Length;
}

static void vetoResolution(DependentResolutions& DepRes,
                           const std::pair<std::string, unsigned>& Key) {
  DepRes[Key].Vetoed = true;
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
                        const FileSet& CollectFrom)
      : SM(SM),
        CB(CB),
        Scope(Scope),
        Renames(Renames),
        CollectFrom(CollectFrom) {}

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
    if (CB(D->getName(), NewName) && NewName != D->getName().str())
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
    if (CB(D->getName(), NewName) && NewName != D->getName().str())
      for (const CXXMethodDecl* M : Family)
        Renames[M->getCanonicalDecl()] = NewName;
  }

  SourceManager& SM;
  const VariableRenameCallback& CB;
  VariableScope Scope;
  RenameMap& Renames;
  const FileSet& CollectFrom;
  std::unordered_set<const Decl*> Visited;
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
    if (Loc.isInvalid() || Loc.isMacroID()) return true;
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
    if (Loc.isInvalid() || Loc.isMacroID()) return true;
    auto Key = ownedKey(Loc, SM, CollectFrom);
    if (!Key || DependentLocs.find(*Key) == DependentLocs.end()) return true;
    const Decl* MemberKey = memberExprKey(E);
    if (!MemberKey) return true;
    const NamedDecl* Member = E->getMemberDecl();
    if (!Member->getDeclName().isIdentifier()) return true;
    llvm::StringRef Old = Member->getName();
    auto It = Renames.find(MemberKey);
    if (It != Renames.end())
      recordResolution(DepRes, *Key, It->second, Old, Old.size());
    else
      vetoResolution(DepRes, *Key);
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
// ---------------------------------------------------------------------------

class ApplyRenamesVisitor : public RecursiveASTVisitor<ApplyRenamesVisitor> {
 public:
  ApplyRenamesVisitor(Rewriter& RW, SourceManager& SM, const RenameMap& Renames,
                      const FileSet& CollectFrom, DependentResolutions* DepRes,
                      LintReport* Report, std::string RuleId, EditReport* Edits)
      : RW(RW),
        SM(SM),
        Renames(Renames),
        CollectFrom(CollectFrom),
        DepRes(DepRes),
        Report(Report),
        RuleId(std::move(RuleId)),
        Edits(Edits) {}

  // Applies one rewrite and, in Lint mode, records the matching diagnostic; in
  // Emit mode, appends a structured edit record instead of (only) rewriting.
  // All call sites already filter to main-file, non-macro locations, so the
  // recorded location always points at the rewritten token.
  void renameAt(SourceLocation Loc, StringRef OldName,
                const std::string& NewName) {
    if (Loc.isInvalid() || Loc.isMacroID()) return;
    RW.ReplaceText(Loc, OldName.size(), NewName);
    if (Report) {
      PresumedLoc PLoc = SM.getPresumedLoc(Loc);
      Report->add({PLoc.isValid() ? PLoc.getFilename() : "", PLoc.getLine(),
                   PLoc.getColumn(), RuleId,
                   "'" + OldName.str() + "' should be '" + NewName + "'"});
    }
    if (Edits) {
      SourceLocation Spell = SM.getSpellingLoc(Loc);
      std::pair<FileID, unsigned> D = SM.getDecomposedLoc(Spell);
      std::string Path;
      if (const FileEntry* FE = SM.getFileEntryForID(D.first)) {
        llvm::StringRef RP = FE->tryGetRealPathName();
        if (!RP.empty()) Path = RP.str();
      }
      if (Path.empty()) {
        PresumedLoc PL = SM.getPresumedLoc(Spell);
        if (PL.isValid()) Path = PL.getFilename();
      }
      Edits->Edits.push_back({relativizeToCwd(Path), D.second,
                              static_cast<unsigned>(OldName.size()),
                              OldName.str(), NewName});
    }
  }

  bool VisitFieldDecl(FieldDecl* D) {
    if (!SM.isWrittenInMainFile(D->getLocation())) return true;
    auto It = Renames.find(D->getCanonicalDecl());
    if (It != Renames.end())
      renameAt(D->getLocation(), D->getName(), It->second);
    return true;
  }

  bool VisitVarDecl(VarDecl* D) {
    if (!SM.isWrittenInMainFile(D->getLocation())) return true;
    const Decl* Key = D->isStaticDataMember()
                          ? primaryTemplateStaticMember(D)->getCanonicalDecl()
                          : D->getCanonicalDecl();
    auto It = Renames.find(Key);
    if (It != Renames.end())
      renameAt(D->getLocation(), D->getName(), It->second);
    return true;
  }

  bool VisitCXXMethodDecl(CXXMethodDecl* D) {
    if (!SM.isWrittenInMainFile(D->getLocation())) return true;
    const Decl* Key = primaryTemplateMethod(D)->getCanonicalDecl();
    auto It = Renames.find(Key);
    if (It != Renames.end())
      renameAt(D->getLocation(), D->getName(), It->second);
    return true;
  }

  bool VisitDeclRefExpr(DeclRefExpr* E) {
    if (!SM.isWrittenInMainFile(E->getLocation())) return true;
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
    if (!Key) return true;
    auto It = Renames.find(Key);
    if (It != Renames.end()) renameAt(E->getLocation(), OldName, It->second);
    return true;
  }

  bool VisitMemberExpr(MemberExpr* E) {
    if (!SM.isWrittenInMainFile(E->getMemberLoc())) return true;
    const Decl* Key = nullptr;
    if (const auto* FD = dyn_cast<FieldDecl>(E->getMemberDecl()))
      Key = primaryTemplateMember(FD);
    else if (const auto* VD = dyn_cast<VarDecl>(E->getMemberDecl()))
      Key = primaryTemplateStaticMember(VD)->getCanonicalDecl();
    else if (const auto* MD = dyn_cast<CXXMethodDecl>(E->getMemberDecl()))
      // Member function calls: `obj.get()`, `ptr->get()`, implicit `this`.
      Key = primaryTemplateMethod(MD)->getCanonicalDecl();
    if (!Key) return true;
    auto It = Renames.find(Key);
    if (It != Renames.end())
      renameAt(E->getMemberLoc(), E->getMemberDecl()->getName(), It->second);
    return true;
  }

  // Constructor mem-initializers (e.g. `S() : val_(0)`).  These are not visited
  // via VisitMemberExpr because CXXCtorInitializer is not a Stmt/Decl.
  bool TraverseConstructorInitializer(CXXCtorInitializer* Init) {
    if (Init && Init->isAnyMemberInitializer()) {
      SourceLocation Loc = Init->getMemberLocation();
      if (SM.isWrittenInMainFile(Loc)) {
        const FieldDecl* FD = Init->getAnyMember();
        if (FD) {
          auto It = Renames.find(primaryTemplateMember(FD));
          if (It != Renames.end()) renameAt(Loc, FD->getName(), It->second);
        }
      }
    }
    return RecursiveASTVisitor<
        ApplyRenamesVisitor>::TraverseConstructorInitializer(Init);
  }

  // Designated initializers (e.g. `S s{.val_ = 0}`).  The field name lives in
  // the designator of a DesignatedInitExpr, not in a MemberExpr.
  bool VisitDesignatedInitExpr(DesignatedInitExpr* E) {
    for (const DesignatedInitExpr::Designator& D : E->designators()) {
      if (!D.isFieldDesignator()) continue;
      SourceLocation Loc = D.getFieldLoc();
      if (!SM.isWrittenInMainFile(Loc)) continue;
      const FieldDecl* FD = D.getFieldDecl();
      if (!FD) continue;
      auto It = Renames.find(primaryTemplateMember(FD));
      if (It != Renames.end()) renameAt(Loc, FD->getName(), It->second);
    }
    return true;
  }

  // Template-dependent member access (e.g. `x.val` where x is a template
  // parameter).  The member is unresolved in this TU, so we rewrite it from the
  // cross-TU resolution recorded by the TUs that instantiate the template.
  // Only the token in its own main file is rewritten (as for every other site),
  // which — combined with headers-last ordering — keeps each file written once.
  bool VisitCXXDependentScopeMemberExpr(CXXDependentScopeMemberExpr* E) {
    // Emit mode defers dependent-token edits to the aggregation phase (which
    // has the full cross-TU resolution picture); here we only serialize the
    // resolutions this TU observed.
    if (!DepRes || Edits) return true;
    SourceLocation Loc = E->getMemberLoc();
    if (!SM.isWrittenInMainFile(Loc) || Loc.isInvalid() || Loc.isMacroID())
      return true;
    auto Key = ownedKey(Loc, SM, CollectFrom);
    if (!Key) return true;
    auto It = DepRes->find(*Key);
    if (It == DepRes->end() || It->second.Vetoed || !It->second.HasName)
      return true;
    const IdentifierInfo* II = E->getMember().getAsIdentifierInfo();
    if (!II) return true;
    renameAt(Loc, II->getName(), It->second.NewName);
    return true;
  }

 private:
  Rewriter& RW;
  SourceManager& SM;
  const RenameMap& Renames;
  const FileSet& CollectFrom;
  DependentResolutions* DepRes;  // null when the feature is disabled
  LintReport* Report;            // null outside Lint mode
  std::string RuleId;
  EditReport* Edits;  // non-null in Emit mode
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
    bool inMain = SM.isWrittenInMainFile(Loc);
    bool isMacro = Loc.isMacroID();
    const char* file = PLoc.isValid() ? PLoc.getFilename() : "<invalid>";
    unsigned line = PLoc.isValid() ? PLoc.getLine() : 0;
    unsigned col = PLoc.isValid() ? PLoc.getColumn() : 0;
    Out << "    " << Kind << "  " << OldName << " -> " << NewName << "  at "
        << file << ":" << line << ":" << col
        << "  [main=" << (inMain ? "Y" : "N")
        << " macro=" << (isMacro ? "Y" : "N") << "]";
    if (inMain && !isMacro) Out << "  WILL_RENAME";
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
                          EditReport* Edits = nullptr)
      : RW(RW),
        CB(std::move(CB)),
        Scope(Scope),
        CollectFrom(std::move(CollectFrom)),
        Mode(Mode),
        Report(Report),
        RuleId(std::move(RuleId)),
        DepRes(DepRes),
        Edits(Edits) {}

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
                       Edits);
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
                        EditReport* Edits)
      : CB(std::move(CB)),
        Scope(Scope),
        Mode(Mode),
        CollectFrom(CollectFrom),
        Pending(Pending),
        Report(Report),
        RuleId(std::move(RuleId)),
        DepRes(DepRes),
        Edits(Edits) {}

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
    return std::make_unique<RenameVariablesConsumer>(TheRewriter, CB, Scope,
                                                     CollectFrom, Mode, Report,
                                                     RuleId, DepRes, Edits);
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
        /*Report=*/nullptr, /*RuleId=*/"", &DepRes);
  }

 private:
  VariableRenameCallback CB;
  VariableScope Scope;
  Rewriter TheRewriter;
  std::string& Output;
  // Single in-memory TU: instantiations and their dependent tokens are in the
  // same file, so intra-TU resolution needs a place to record them.
  DependentResolutions DepRes;
};

}  // namespace

// ---------------------------------------------------------------------------
// runRenameRuleOnAST (public)
// ---------------------------------------------------------------------------

void runRenameRuleOnAST(ASTContext& Ctx, Rewriter& RW,
                        const VariableRenameCallback& CB, VariableScope Scope,
                        const FileSet& CollectFrom, LintReport* Report,
                        llvm::StringRef RuleId, DependentResolutions* DepRes,
                        EditReport* Edits) {
  SourceManager& SM = Ctx.getSourceManager();
  Decl* TU = Ctx.getTranslationUnitDecl();

  RenameMap Renames;
  CollectRenamesVisitor Collector(SM, CB, Scope, Renames, CollectFrom);
  Collector.TraverseDecl(TU);

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

auto RenameActionFactory::create() -> std::unique_ptr<clang::FrontendAction> {
  return std::make_unique<RenameVariablesAction>(
      CB, Scope, Mode, CollectFrom, &Pending, Report, RuleId, &DepRes,
      Mode == OutputMode::Emit ? &Edits : nullptr);
}

void RenameActionFactory::emitEdits(llvm::raw_ostream& OS) {
  // Promote the cross-TU dependent-token resolutions to sidecar records; the
  // aggregation phase resolves them across all TUs before turning survivors
  // into edits.  Keys are relativized to cwd so they match the edit records and
  // are stable across sandboxes.
  for (const auto& [Key, R] : DepRes) {
    if (!R.HasName && !R.Vetoed) continue;
    Edits.Resolutions.push_back({relativizeToCwd(Key.first), Key.second,
                                 R.Length, R.OldName, R.NewName, R.Vetoed});
  }
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
  for (const auto& P : SourcePaths) {
    llvm::StringRef ext = llvm::StringRef(P).rsplit('.').second;
    (ext == "h" || ext == "hh" || ext == "hpp" || ext == "hxx" ? headers
                                                               : sources)
        .push_back(P);
  }
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
