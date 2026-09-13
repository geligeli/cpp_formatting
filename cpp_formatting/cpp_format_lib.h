#ifndef CPP_FORMATTING_CPP_FORMAT_LIB_H_
#define CPP_FORMATTING_CPP_FORMAT_LIB_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cpp_formatting/lint_lib.h"
#include "cpp_formatting/output_mode.h"
#include "cpp_formatting/rename_variables_lib.h"
#include "cpp_formatting/trailing_return_types_lib.h"
#include "cpp_formatting/tu_driver.h"

// ---------------------------------------------------------------------------
// NormalizeRule
// ---------------------------------------------------------------------------

/// One normalize_variables rule to apply: a scope, the rename callback that
/// computes new names, and the lint rule id used to tag diagnostics.
struct NormalizeRule {
  VariableScope Scope;
  VariableRenameCallback CB;
  std::string RuleId;  ///< e.g. "normalize_variables/member/snake_case"
};

// ---------------------------------------------------------------------------
// CppFormatActionFactory
// ---------------------------------------------------------------------------

/// The TU driver's client for cpp_format: builds, per translation unit, one
/// action that applies every normalize_variables rule plus (optionally) the
/// return-type rewrite in a single pass over the AST, sharing one Rewriter.
/// This avoids re-parsing every TU once per pass: parses per TU drop from
/// 1 + #rules to 1.
///
/// Like RenameActionFactory, each TU writes into its own TUSlot; finish()
/// merges the slots in source order and flush() commits the rewritten content
/// afterwards, so every TU compiles against the original on-disk source.
class CppFormatActionFactory : public TUSlotClient {
 public:
  /// \p ReturnStyle selects the return-type pass; nullopt skips it entirely.
  CppFormatActionFactory(std::vector<NormalizeRule> Rules,
                         std::optional<ReturnTypeStyle> ReturnStyle,
                         std::string ReturnRuleId, OutputMode Mode,
                         FileSet CollectFrom);

  // TUSlotClient
  auto ruleCount() const -> size_t override { return Rules.size(); }
  auto createAction(TUSlot& Slot)
      -> std::unique_ptr<clang::FrontendAction> override;
  auto shared() -> CrossTUState& override { return Shared; }
  /// False in Emit mode, whose records carry their own vetoes for aggregation
  /// to act on, so a veto needs no second pass.
  auto rerunNeededOnVeto() const -> bool override {
    return Mode != OutputMode::Emit;
  }
  /// False in Emit mode: every TU records independently and aggregation
  /// merges, which also makes the records identical for any thread count.
  auto crossTUSeeding() const -> bool override {
    return Mode != OutputMode::Emit;
  }
  void finish(std::vector<TUSlot>& Slots) override;

  /// Attach a lint report shared by all rules.  Not owned.
  void setLintReport(LintReport* Report) { this->Report = Report; }

  /// The rewrites buffered during the run.  In Lint mode this is how the main
  /// obtains the rewritten content for diff output.
  auto rewrites() const -> const PendingRewrites& { return Pending; }

  /// Commit buffered rewrites: print them (DryRun) or write them to disk
  /// atomically (InPlace).  No-op in Lint/Emit mode.
  void flush();

  /// In Emit mode, writes this run's edit records (every rule's renames plus
  /// the trailing-return rewrites) and the dependent-token resolution sidecar
  /// as JSON to \p OS.  Call after the run completes.
  void emitEdits(llvm::raw_ostream& OS);

  /// Renames skipped because the new name was already taken in the same
  /// scope, or because a reference to them could not be rewritten.
  auto conflicts() const -> const RenameConflicts& { return Conflicts; }

  /// Declarations vetoed during the run because some reference to them
  /// cannot be rewritten.
  auto vetoes() const -> const RenameVetoes& { return Shared.Vetoes; }

 private:
  std::vector<NormalizeRule> Rules;
  std::optional<ReturnTypeStyle> ReturnStyle;
  std::string ReturnRuleId;
  OutputMode Mode;
  FileSet CollectFrom;
  PendingRewrites Pending;    // merged by finish()
  EditReport Edits;           // merged by finish(); Emit mode
  RenameConflicts Conflicts;  // merged by finish()
  // Vetoes plus one cross-TU dependent-token resolution map per rule (kept
  // per rule so a token resolved by one rule is never re-applied by another).
  // Exchanged between TUs by the driver; see tu_driver.h.
  CrossTUState Shared;
  LintReport* Report = nullptr;
};

#endif  // CPP_FORMATTING_CPP_FORMAT_LIB_H_
