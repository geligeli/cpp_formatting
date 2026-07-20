#ifndef CPP_FORMATTING_CPP_FORMAT_LIB_H_
#define CPP_FORMATTING_CPP_FORMAT_LIB_H_

#include <memory>
#include <string>
#include <vector>

#include "clang/Tooling/Tooling.h"
#include "cpp_formatting/lint_lib.h"
#include "cpp_formatting/output_mode.h"
#include "cpp_formatting/rename_variables_lib.h"

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
/// plus (optionally) the trailing-return-type rewrite in a single pass over
/// each translation unit, sharing one Rewriter.  This avoids re-parsing every
/// TU once per pass: parses per TU drop from 1 + #rules to 1.
///
/// Like RenameActionFactory, rewritten content is buffered in PendingRewrites
/// and committed by flush() after ClangTool::run() returns, so every TU
/// compiles against the original on-disk source.
class CppFormatActionFactory : public clang::tooling::FrontendActionFactory {
 public:
  CppFormatActionFactory(std::vector<NormalizeRule> Rules,
                         bool TrailingReturnTypes, std::string TrailingRuleId,
                         OutputMode Mode, FileSet CollectFrom);

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

 private:
  std::vector<NormalizeRule> Rules;
  bool TrailingReturnTypes;
  std::string TrailingRuleId;
  OutputMode Mode;
  FileSet CollectFrom;
  PendingRewrites Pending;
  EditReport Edits;  // populated in Emit mode (all rules + trailing-return)
  // One cross-TU dependent-token resolution map per rule (see
  // DependentResolutions).  Persists for the whole ClangTool::run() so a header
  // TU can consume resolutions recorded by earlier .cpp TUs.  Kept per-rule so
  // a token resolved by one rule is never re-applied by another.
  std::vector<DependentResolutions> DepResPerRule;
  LintReport* Report = nullptr;
};

#endif  // CPP_FORMATTING_CPP_FORMAT_LIB_H_
