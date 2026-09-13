#ifndef CPP_FORMATTING_CPP_FORMAT_LIB_H_
#define CPP_FORMATTING_CPP_FORMAT_LIB_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "clang/Tooling/Tooling.h"
#include "cpp_formatting/lint_lib.h"
#include "cpp_formatting/output_mode.h"
#include "cpp_formatting/rename_variables_lib.h"
#include "cpp_formatting/trailing_return_types_lib.h"

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

/// Factory for ClangTool::run() that applies every normalize_variables rule
/// plus (optionally) the return-type rewrite in a single pass over
/// each translation unit, sharing one Rewriter.  This avoids re-parsing every
/// TU once per pass: parses per TU drop from 1 + #rules to 1.
///
/// Like RenameActionFactory, rewritten content is buffered in PendingRewrites
/// and committed by flush() after ClangTool::run() returns, so every TU
/// compiles against the original on-disk source.
class CppFormatActionFactory : public clang::tooling::FrontendActionFactory {
 public:
  /// \p ReturnStyle selects the return-type pass; nullopt skips it entirely.
  CppFormatActionFactory(std::vector<NormalizeRule> Rules,
                         std::optional<ReturnTypeStyle> ReturnStyle,
                         std::string ReturnRuleId, OutputMode Mode,
                         FileSet CollectFrom);

  auto create() -> std::unique_ptr<clang::FrontendAction> override;

  /// Attach a lint report shared by all rules.  Not owned.
  void setLintReport(LintReport* Report) { this->Report = Report; }

  /// The rewrites buffered during ClangTool::run().  In Lint mode this is how
  /// the main obtains the rewritten content for diff output.
  auto rewrites() const -> const PendingRewrites& { return Pending; }

  /// Commit buffered rewrites: print them (DryRun) or write them to disk
  /// atomically (InPlace).  No-op in Lint/Emit mode.
  void flush();

  /// In Emit mode, writes this run's edit records (every rule's renames plus
  /// the trailing-return rewrites) and the dependent-token resolution sidecar
  /// as JSON to \p OS.  Call after ClangTool::run() completes.
  void emitEdits(llvm::raw_ostream& OS);

  /// Renames skipped because the new name was already taken in the same
  /// scope, or because a reference to them could not be rewritten.
  auto conflicts() const -> const RenameConflicts& { return Conflicts; }

  /// Declarations vetoed during ClangTool::run() because some reference to
  /// them cannot be rewritten.  Non-empty means an earlier TU may have renamed
  /// a declaration a later one vetoed: discard the buffered output with
  /// resetForRerun() and run the tool again.
  auto vetoes() const -> const RenameVetoes& { return Vetoes; }

  /// Drops everything buffered by a previous ClangTool::run() while keeping
  /// the accumulated vetoes, so a second run applies only the renames that
  /// survived.  Nothing has reached disk at this point (flush() has not run).
  void resetForRerun();

  /// False in Emit mode, whose records carry their own vetoes for aggregation
  /// to act on, so a veto needs no second pass.  See runWithVetoRerun().
  auto rerunNeededOnVeto() const -> bool { return Mode != OutputMode::Emit; }

 private:
  std::vector<NormalizeRule> Rules;
  std::optional<ReturnTypeStyle> ReturnStyle;
  std::string ReturnRuleId;
  OutputMode Mode;
  FileSet CollectFrom;
  PendingRewrites Pending;
  EditReport Edits;  // populated in Emit mode (all rules + trailing-return)
  RenameConflicts Conflicts;  // renames skipped because the name was taken
  // Declarations that must not be renamed because a reference to them is not
  // rewritable; shared by all rules and kept across a re-run.
  RenameVetoes Vetoes;
  // One cross-TU dependent-token resolution map per rule (see
  // DependentResolutions).  Persists for the whole ClangTool::run() so a header
  // TU can consume resolutions recorded by earlier .cpp TUs.  Kept per-rule so
  // a token resolved by one rule is never re-applied by another.
  std::vector<DependentResolutions> DepResPerRule;
  LintReport* Report = nullptr;
};

#endif  // CPP_FORMATTING_CPP_FORMAT_LIB_H_
