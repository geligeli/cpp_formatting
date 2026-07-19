#include "cpp_formatting/cpp_format_lib.h"

#include <fstream>
#include <utility>

#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Basic/FileEntry.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "cpp_formatting/trailing_return_types_lib.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using namespace clang::tooling;

namespace {

// ---------------------------------------------------------------------------
// CppFormatConsumer — runs all rules on one parsed TU
// ---------------------------------------------------------------------------

class CppFormatConsumer : public ASTConsumer {
 public:
  CppFormatConsumer(Rewriter& RW, const std::vector<NormalizeRule>& Rules,
                    bool TrailingReturnTypes, const std::string& TrailingRuleId,
                    const FileSet& CollectFrom, LintReport* Report)
      : RW(RW),
        Rules(Rules),
        TrailingReturnTypes(TrailingReturnTypes),
        TrailingRuleId(TrailingRuleId),
        CollectFrom(CollectFrom),
        Report(Report) {}

  void HandleTranslationUnit(ASTContext& Ctx) override {
    for (const NormalizeRule& Rule : Rules)
      runRenameRuleOnAST(Ctx, RW, Rule.CB, Rule.Scope, CollectFrom, Report,
                         Rule.RuleId);

    if (TrailingReturnTypes) {
      // MatchFinder::matchAST runs the matchers on the already-parsed AST —
      // no extra parse.  Shares the Rewriter with the rename rules.
      ast_matchers::MatchFinder Finder;
      TrailingReturnCallback Callback(RW);
      if (Report) Callback.setLintReport(Report, TrailingRuleId);
      registerTrailingReturnMatchers(Finder, Callback);
      Finder.matchAST(Ctx);
    }
  }

 private:
  Rewriter& RW;
  const std::vector<NormalizeRule>& Rules;
  bool TrailingReturnTypes;
  const std::string& TrailingRuleId;
  const FileSet& CollectFrom;
  LintReport* Report;  // null outside Lint mode
};

// ---------------------------------------------------------------------------
// CppFormatAction
//
// Like RenameVariablesAction, in InPlace mode writes are NOT flushed to disk
// immediately; the modified content is stored in *Pending so that
// CppFormatActionFactory::flush() can write every file atomically after
// ClangTool::run() completes.
// ---------------------------------------------------------------------------

class CppFormatAction : public ASTFrontendAction {
 public:
  CppFormatAction(const std::vector<NormalizeRule>& Rules,
                  bool TrailingReturnTypes, const std::string& TrailingRuleId,
                  OutputMode Mode, const FileSet& CollectFrom,
                  PendingRewrites* Pending, LintReport* Report)
      : Rules(Rules),
        TrailingReturnTypes(TrailingReturnTypes),
        TrailingRuleId(TrailingRuleId),
        Mode(Mode),
        CollectFrom(CollectFrom),
        Pending(Pending),
        Report(Report) {}

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
    return std::make_unique<CppFormatConsumer>(
        TheRewriter, Rules, TrailingReturnTypes, TrailingRuleId, CollectFrom,
        Report);
  }

 private:
  const std::vector<NormalizeRule>& Rules;
  bool TrailingReturnTypes;
  const std::string& TrailingRuleId;
  OutputMode Mode;
  const FileSet& CollectFrom;
  PendingRewrites* Pending;
  LintReport* Report;
  Rewriter TheRewriter;
};

}  // namespace

// ---------------------------------------------------------------------------
// CppFormatActionFactory (public)
// ---------------------------------------------------------------------------

CppFormatActionFactory::CppFormatActionFactory(std::vector<NormalizeRule> Rules,
                                               bool TrailingReturnTypes,
                                               std::string TrailingRuleId,
                                               OutputMode Mode,
                                               FileSet CollectFrom)
    : Rules(std::move(Rules)),
      TrailingReturnTypes(TrailingReturnTypes),
      TrailingRuleId(std::move(TrailingRuleId)),
      Mode(Mode),
      CollectFrom(std::move(CollectFrom)) {}

auto CppFormatActionFactory::create()
    -> std::unique_ptr<clang::FrontendAction> {
  return std::make_unique<CppFormatAction>(Rules, TrailingReturnTypes,
                                           TrailingRuleId, Mode, CollectFrom,
                                           &Pending, Report);
}

void CppFormatActionFactory::flush() {
  if (Mode == OutputMode::Debug || Mode == OutputMode::Lint) {
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
