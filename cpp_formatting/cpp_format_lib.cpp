#include "cpp_formatting/cpp_format_lib.h"

#include <fstream>
#include <iterator>
#include <utility>

#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Basic/FileEntry.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "cpp_formatting/const_placement_lib.h"
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
                    std::optional<ConstStyle> ConstPlacement,
                    std::optional<ReturnTypeStyle> ReturnStyle,
                    const std::string& ReturnRuleId, const FileSet& CollectFrom,
                    LintReport* Report,
                    std::vector<DependentResolutions>* DepResPerRule,
                    EditReport* Edits, RenameConflicts* Conflicts,
                    RenameVetoes* Vetoes, std::set<std::string>* RenamedNames,
                    Preprocessor* PP)
      : RW(RW),
        Rules(Rules),
        ConstPlacement(ConstPlacement),
        ReturnStyle(ReturnStyle),
        ReturnRuleId(ReturnRuleId),
        CollectFrom(CollectFrom),
        Report(Report),
        DepResPerRule(DepResPerRule),
        Edits(Edits),
        Conflicts(Conflicts),
        Vetoes(Vetoes),
        RenamedNames(RenamedNames),
        PP(PP) {}

  void HandleTranslationUnit(ASTContext& Ctx) override {
    for (size_t I = 0; I < Rules.size(); ++I)
      runRenameRuleOnAST(Ctx, RW, Rules[I].CB, Rules[I].Scope, CollectFrom,
                         Report, Rules[I].RuleId,
                         DepResPerRule ? &(*DepResPerRule)[I] : nullptr, Edits,
                         Conflicts, Vetoes, RenamedNames, PP);

    // Between the renames and the return-type pass.  It has to run after the
    // renames because it shares their Rewriter and reads nothing they wrote;
    // it has to run *before* the return-type pass because that pass lifts the
    // whole return type as text (Rewriter::getRewrittenText), so a qualifier
    // moved first rides along into `-> type` instead of being clobbered by the
    // wholesale `auto` replacement.  In Emit mode the same ordering lets
    // runToTrailing() subsume these records the way it subsumes rename ones.
    if (ConstPlacement)
      runConstPlacementOnAST(Ctx, RW, *ConstPlacement, Report,
                             constStyleRuleId(*ConstPlacement), Edits);

    if (ReturnStyle) {
      // MatchFinder::matchAST runs the matchers on the already-parsed AST —
      // no extra parse.  Shares the Rewriter with the rename rules, so it runs
      // last and can subsume rename edits that landed inside a return type.
      ast_matchers::MatchFinder Finder;
      TrailingReturnCallback Callback(RW, *ReturnStyle);
      if (Report) Callback.setLintReport(Report, ReturnRuleId);
      if (Edits) Callback.setEmitReport(Edits);
      registerTrailingReturnMatchers(Finder, Callback, *ReturnStyle);
      Finder.matchAST(Ctx);
    }
  }

 private:
  Rewriter& RW;
  const std::vector<NormalizeRule>& Rules;
  std::optional<ConstStyle> ConstPlacement;
  std::optional<ReturnTypeStyle> ReturnStyle;
  const std::string& ReturnRuleId;
  const FileSet& CollectFrom;
  LintReport* Report;  // null outside Lint mode
  std::vector<DependentResolutions>* DepResPerRule;
  EditReport* Edits;  // non-null in Emit mode
  RenameConflicts* Conflicts;
  // One veto set for all rules: a reference that cannot be rewritten is
  // unrewritable whatever new name a rule would have given it.
  RenameVetoes* Vetoes;
  std::set<std::string>* RenamedNames;
  Preprocessor* PP;
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
                  std::optional<ConstStyle> ConstPlacement,
                  std::optional<ReturnTypeStyle> ReturnStyle,
                  const std::string& ReturnRuleId, OutputMode Mode,
                  const FileSet& CollectFrom, PendingRewrites* Pending,
                  LintReport* Report,
                  std::vector<DependentResolutions>* DepResPerRule,
                  EditReport* Edits, RenameConflicts* Conflicts,
                  RenameVetoes* Vetoes, std::set<std::string>* RenamedNames)
      : Rules(Rules),
        ConstPlacement(ConstPlacement),
        ReturnStyle(ReturnStyle),
        ReturnRuleId(ReturnRuleId),
        Mode(Mode),
        CollectFrom(CollectFrom),
        Pending(Pending),
        Report(Report),
        DepResPerRule(DepResPerRule),
        Edits(Edits),
        Conflicts(Conflicts),
        Vetoes(Vetoes),
        RenamedNames(RenamedNames) {}

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
        TheRewriter, Rules, ConstPlacement, ReturnStyle, ReturnRuleId,
        CollectFrom, Report, DepResPerRule, Edits, Conflicts, Vetoes,
        RenamedNames, &CI.getPreprocessor());
  }

 private:
  const std::vector<NormalizeRule>& Rules;
  std::optional<ConstStyle> ConstPlacement;
  std::optional<ReturnTypeStyle> ReturnStyle;
  const std::string& ReturnRuleId;
  OutputMode Mode;
  const FileSet& CollectFrom;
  PendingRewrites* Pending;
  LintReport* Report;
  std::vector<DependentResolutions>* DepResPerRule;
  EditReport* Edits;
  RenameConflicts* Conflicts;
  // One veto set for all rules: a reference that cannot be rewritten is
  // unrewritable whatever new name a rule would have given it.
  RenameVetoes* Vetoes;
  std::set<std::string>* RenamedNames;
  Rewriter TheRewriter;
};

}  // namespace

// ---------------------------------------------------------------------------
// CppFormatActionFactory (public)
// ---------------------------------------------------------------------------

CppFormatActionFactory::CppFormatActionFactory(
    std::vector<NormalizeRule> Rules, std::optional<ConstStyle> ConstPlacement,
    std::optional<ReturnTypeStyle> ReturnStyle, std::string ReturnRuleId,
    OutputMode Mode, FileSet CollectFrom)
    : Rules(std::move(Rules)),
      ConstPlacement(ConstPlacement),
      ReturnStyle(ReturnStyle),
      ReturnRuleId(std::move(ReturnRuleId)),
      Mode(Mode),
      CollectFrom(std::move(CollectFrom)) {}

auto CppFormatActionFactory::createAction(TUSlot& Slot)
    -> std::unique_ptr<clang::FrontendAction> {
  // Runs on a worker thread: only the tool's immutable configuration and the
  // slot's own members are touched.  A null Report stays null -- a non-null
  // pointer is what switches the visitors' diagnostic recording on.
  return std::make_unique<CppFormatAction>(
      Rules, ConstPlacement, ReturnStyle, ReturnRuleId, Mode, CollectFrom,
      &Slot.Pending, Report ? &Slot.Report : nullptr, &Slot.DepRes,
      Mode == OutputMode::Emit ? &Slot.Edits : nullptr, &Slot.Conflicts,
      &Slot.Vetoes, &Slot.RenamedNames);
}

void CppFormatActionFactory::finish(std::vector<TUSlot>& Slots) {
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

void CppFormatActionFactory::emitEdits(llvm::raw_ostream& OS) {
  // Fold every rule's cross-TU dependent-token resolutions into the sidecar;
  // aggregation resolves them across all TUs before turning survivors into
  // edits.  Keys are relativized to cwd to match the edit records and be stable
  // across sandboxes.
  for (unsigned Rule = 0; Rule < Shared.DepResPerRule.size(); ++Rule)
    for (const auto& [Key, R] : Shared.DepResPerRule[Rule]) {
      if (!R.HasName && !R.Vetoed && R.OldName.empty()) continue;
      Edits.Resolutions.push_back({relativizeToCwd(Key.first), Key.second,
                                   R.Length, R.OldName, R.NewName, R.Vetoed,
                                   R.OwnerFile, R.OwnerOffset, Rule});
    }
  for (const auto& [Key, V] : Shared.Vetoes) Edits.Vetoes.push_back(V);
  Edits.emitJSON(OS);
}

void CppFormatActionFactory::flush() {
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
