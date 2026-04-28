#include "cpp_formatting/rename_variables_lib.h"

#include <unordered_map>
#include <unordered_set>

#include "clang/AST/DeclTemplate.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/FileEntry.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Tooling/Tooling.h"

using namespace clang;
using namespace clang::tooling;

namespace {

// ---------------------------------------------------------------------------
// Scope classification
// ---------------------------------------------------------------------------

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
  }
  return false;
}

// ---------------------------------------------------------------------------
// Template instantiation helpers
// ---------------------------------------------------------------------------

// Map an instantiated FieldDecl back to the primary-template FieldDecl by
// walking up the class specialization chain and matching by field index.
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

// Walk up the instantiation chain to the primary-template static data member.
static const VarDecl* primaryTemplateStaticMember(const VarDecl* VD) {
  while (const VarDecl* P = VD->getInstantiatedFromStaticDataMember()) VD = P;
  return VD;
}

// ---------------------------------------------------------------------------
// File-set predicate: should we collect declarations from this location?
// ---------------------------------------------------------------------------

// Returns true if declarations at Loc should be entered into the rename map.
// When CollectFrom is empty we replicate the original behaviour (main file
// only). When non-empty we accept any file whose real path is in the set.
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

 private:
  void collect(NamedDecl* D) {
    if (D->isImplicit() || !matchesScope(D, Scope)) return;
    if (!shouldCollect(D->getLocation(), SM, CollectFrom)) return;

    // Use canonical decl as the key so a variable with multiple declarations
    // (e.g. extern + definition) is processed only once.
    const Decl* Key = D->getCanonicalDecl();
    if (!Visited.insert(Key).second) return;

    std::string NewName;
    if (CB(D->getName(), NewName) && NewName != D->getName().str())
      Renames[Key] = std::move(NewName);
  }

  SourceManager& SM;
  const VariableRenameCallback& CB;
  VariableScope Scope;
  RenameMap& Renames;
  const FileSet& CollectFrom;
  std::unordered_set<const Decl*> Visited;
};

// ---------------------------------------------------------------------------
// Pass 2: apply renames at every declaration and use site
// ---------------------------------------------------------------------------

static void renameAt(Rewriter& RW, SourceLocation Loc, StringRef OldName,
                     const std::string& NewName) {
  if (Loc.isValid() && !Loc.isMacroID())
    RW.ReplaceText(Loc, OldName.size(), NewName);
}

class ApplyRenamesVisitor : public RecursiveASTVisitor<ApplyRenamesVisitor> {
 public:
  ApplyRenamesVisitor(Rewriter& RW, SourceManager& SM, const RenameMap& Renames)
      : RW(RW), SM(SM), Renames(Renames) {}

  // FieldDecl declaration site.
  bool VisitFieldDecl(FieldDecl* D) {
    if (!SM.isWrittenInMainFile(D->getLocation())) return true;
    auto It = Renames.find(D->getCanonicalDecl());
    if (It != Renames.end())
      renameAt(RW, D->getLocation(), D->getName(), It->second);
    return true;
  }

  // VarDecl declaration site (local, global, static data member).
  bool VisitVarDecl(VarDecl* D) {
    if (!SM.isWrittenInMainFile(D->getLocation())) return true;
    // For static data members walk up the instantiation chain to the primary
    // template, then take the canonical decl so that the out-of-class
    // definition (a separate VarDecl) maps to the same key as the in-class
    // declaration stored in the rename map.
    const Decl* Key = D->isStaticDataMember()
                          ? primaryTemplateStaticMember(D)->getCanonicalDecl()
                          : D->getCanonicalDecl();
    auto It = Renames.find(Key);
    if (It != Renames.end())
      renameAt(RW, D->getLocation(), D->getName(), It->second);
    return true;
  }

  // References to VarDecl (local/global variables, static data members) and
  // FieldDecl (pointer-to-member formation: &S::field).
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
      // Pointer-to-member formation: &S::m_field
      Key = primaryTemplateMember(FD);
      OldName = FD->getName();
    }

    if (!Key) return true;
    auto It = Renames.find(Key);
    if (It != Renames.end())
      renameAt(RW, E->getLocation(), OldName, It->second);
    return true;
  }

  // Non-static member accesses: obj.field, obj->field, and static member
  // accesses via an instance (obj.StaticMember).
  bool VisitMemberExpr(MemberExpr* E) {
    if (!SM.isWrittenInMainFile(E->getMemberLoc())) return true;
    const Decl* Key = nullptr;
    if (const auto* FD = dyn_cast<FieldDecl>(E->getMemberDecl()))
      // Walk up for template-class members accessed in non-template code.
      Key = primaryTemplateMember(FD);
    else if (const auto* VD = dyn_cast<VarDecl>(E->getMemberDecl()))
      Key = primaryTemplateStaticMember(VD)->getCanonicalDecl();
    if (!Key) return true;
    auto It = Renames.find(Key);
    if (It != Renames.end())
      renameAt(RW, E->getMemberLoc(), E->getMemberDecl()->getName(),
               It->second);
    return true;
  }

 private:
  Rewriter& RW;
  SourceManager& SM;
  const RenameMap& Renames;
};

// ---------------------------------------------------------------------------
// ASTConsumer
// ---------------------------------------------------------------------------

class RenameVariablesConsumer : public ASTConsumer {
 public:
  RenameVariablesConsumer(Rewriter& RW, VariableRenameCallback CB,
                          VariableScope Scope, FileSet CollectFrom)
      : RW(RW),
        CB(std::move(CB)),
        Scope(Scope),
        CollectFrom(std::move(CollectFrom)) {}

  void HandleTranslationUnit(ASTContext& Ctx) override {
    RenameMap Renames;
    CollectRenamesVisitor Collector(Ctx.getSourceManager(), CB, Scope, Renames,
                                    CollectFrom);
    Collector.TraverseDecl(Ctx.getTranslationUnitDecl());
    if (Renames.empty()) return;
    ApplyRenamesVisitor Applier(RW, Ctx.getSourceManager(), Renames);
    Applier.TraverseDecl(Ctx.getTranslationUnitDecl());
  }

 private:
  Rewriter& RW;
  VariableRenameCallback CB;
  VariableScope Scope;
  FileSet CollectFrom;
};

// ---------------------------------------------------------------------------
// Factory for ClangTool::run()
// ---------------------------------------------------------------------------

struct RenameActionFactory : FrontendActionFactory {
  VariableRenameCallback CB;
  VariableScope Scope;
  OutputMode Mode;
  FileSet CollectFrom;

  RenameActionFactory(VariableRenameCallback CB, VariableScope Scope,
                      OutputMode Mode, FileSet CollectFrom)
      : CB(std::move(CB)),
        Scope(Scope),
        Mode(Mode),
        CollectFrom(std::move(CollectFrom)) {}

  auto create() -> std::unique_ptr<FrontendAction> override {
    return std::make_unique<RenameVariablesAction>(CB, Scope, Mode,
                                                   CollectFrom);
  }
};

// ---------------------------------------------------------------------------
// CaptureAction — used by the test helper (always uses main-file collection)
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
    // Empty FileSet → collect from main file only (original unit-test
    // behaviour).
    return std::make_unique<RenameVariablesConsumer>(TheRewriter, CB, Scope,
                                                     FileSet{});
  }

 private:
  VariableRenameCallback CB;
  VariableScope Scope;
  Rewriter TheRewriter;
  std::string& Output;
};

}  // namespace

// ---------------------------------------------------------------------------
// RenameVariablesAction implementation
// ---------------------------------------------------------------------------

RenameVariablesAction::RenameVariablesAction(VariableRenameCallback CB,
                                             VariableScope Scope,
                                             OutputMode Mode,
                                             FileSet CollectFrom)
    : CB(std::move(CB)),
      Scope(Scope),
      Mode(Mode),
      CollectFrom(std::move(CollectFrom)) {}

void RenameVariablesAction::EndSourceFileAction() {
  SourceManager& SM = TheRewriter.getSourceMgr();
  if (Mode == OutputMode::InPlace) {
    TheRewriter.overwriteChangedFiles();
  } else {
    llvm::errs() << "** Rewritten Output (Dry Run): **\n";
    TheRewriter.getEditBuffer(SM.getMainFileID()).write(llvm::outs());
  }
}

auto RenameVariablesAction::CreateASTConsumer(CompilerInstance& CI, StringRef)
    -> std::unique_ptr<ASTConsumer> {
  TheRewriter.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
  return std::make_unique<RenameVariablesConsumer>(TheRewriter, CB, Scope,
                                                   CollectFrom);
}

// ---------------------------------------------------------------------------
// Convenience factories
// ---------------------------------------------------------------------------

auto RenameAllMemberVariables(VariableRenameCallback CB, OutputMode Mode,
                              FileSet CollectFrom)
    -> std::unique_ptr<clang::tooling::FrontendActionFactory> {
  return std::make_unique<RenameActionFactory>(
      std::move(CB), VariableScope::Member, Mode, std::move(CollectFrom));
}

auto RenameAllLocalVariables(VariableRenameCallback CB, OutputMode Mode,
                             FileSet CollectFrom)
    -> std::unique_ptr<clang::tooling::FrontendActionFactory> {
  return std::make_unique<RenameActionFactory>(
      std::move(CB), VariableScope::Local, Mode, std::move(CollectFrom));
}

auto RenameAllGlobalVariables(VariableRenameCallback CB, OutputMode Mode,
                              FileSet CollectFrom)
    -> std::unique_ptr<clang::tooling::FrontendActionFactory> {
  return std::make_unique<RenameActionFactory>(
      std::move(CB), VariableScope::Global, Mode, std::move(CollectFrom));
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
