#include "cpp_formatting/rename_variables_lib.h"

#include <fstream>
#include <unordered_map>
#include <unordered_set>

#include "clang/AST/DeclTemplate.h"
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

  bool VisitFieldDecl(FieldDecl* D) {
    if (!SM.isWrittenInMainFile(D->getLocation())) return true;
    auto It = Renames.find(D->getCanonicalDecl());
    if (It != Renames.end())
      renameAt(RW, D->getLocation(), D->getName(), It->second);
    return true;
  }

  bool VisitVarDecl(VarDecl* D) {
    if (!SM.isWrittenInMainFile(D->getLocation())) return true;
    const Decl* Key = D->isStaticDataMember()
                          ? primaryTemplateStaticMember(D)->getCanonicalDecl()
                          : D->getCanonicalDecl();
    auto It = Renames.find(Key);
    if (It != Renames.end())
      renameAt(RW, D->getLocation(), D->getName(), It->second);
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
    }
    if (!Key) return true;
    auto It = Renames.find(Key);
    if (It != Renames.end())
      renameAt(RW, E->getLocation(), OldName, It->second);
    return true;
  }

  bool VisitMemberExpr(MemberExpr* E) {
    if (!SM.isWrittenInMainFile(E->getMemberLoc())) return true;
    const Decl* Key = nullptr;
    if (const auto* FD = dyn_cast<FieldDecl>(E->getMemberDecl()))
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

  // Constructor mem-initializers (e.g. `S() : val_(0)`).  These are not visited
  // via VisitMemberExpr because CXXCtorInitializer is not a Stmt/Decl.
  bool TraverseConstructorInitializer(CXXCtorInitializer* Init) {
    if (Init && Init->isAnyMemberInitializer()) {
      SourceLocation Loc = Init->getMemberLocation();
      if (SM.isWrittenInMainFile(Loc)) {
        const FieldDecl* FD = Init->getAnyMember();
        if (FD) {
          auto It = Renames.find(primaryTemplateMember(FD));
          if (It != Renames.end()) renameAt(RW, Loc, FD->getName(), It->second);
        }
      }
    }
    return RecursiveASTVisitor<
        ApplyRenamesVisitor>::TraverseConstructorInitializer(Init);
  }

 private:
  Rewriter& RW;
  SourceManager& SM;
  const RenameMap& Renames;
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
                          OutputMode Mode = OutputMode::DryRun)
      : RW(RW),
        CB(std::move(CB)),
        Scope(Scope),
        CollectFrom(std::move(CollectFrom)),
        Mode(Mode) {}

  void HandleTranslationUnit(ASTContext& Ctx) override {
    SourceManager& SM = Ctx.getSourceManager();
    RenameMap Renames;
    CollectRenamesVisitor Collector(SM, CB, Scope, Renames, CollectFrom);
    Collector.TraverseDecl(Ctx.getTranslationUnitDecl());

    if (Mode == OutputMode::Debug) {
      const FileEntry* FE = SM.getFileEntryForID(SM.getMainFileID());
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

    if (Renames.empty()) return;
    ApplyRenamesVisitor Applier(RW, SM, Renames);
    Applier.TraverseDecl(Ctx.getTranslationUnitDecl());
  }

 private:
  Rewriter& RW;
  VariableRenameCallback CB;
  VariableScope Scope;
  FileSet CollectFrom;
  OutputMode Mode;
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
                        PendingRewrites* Pending)
      : CB(std::move(CB)),
        Scope(Scope),
        Mode(Mode),
        CollectFrom(CollectFrom),
        Pending(Pending) {}

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
                                                     CollectFrom, Mode);
  }

 private:
  VariableRenameCallback CB;
  VariableScope Scope;
  OutputMode Mode;
  const FileSet& CollectFrom;
  PendingRewrites* Pending;
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
  return std::make_unique<RenameVariablesAction>(CB, Scope, Mode, CollectFrom,
                                                 &Pending);
}

void RenameActionFactory::flush() {
  if (Mode == OutputMode::Debug) {
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
