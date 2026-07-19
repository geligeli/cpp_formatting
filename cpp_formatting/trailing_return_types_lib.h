#ifndef CPP_FORMATTING_TRAILING_RETURN_TYPES_LIB_H_
#define CPP_FORMATTING_TRAILING_RETURN_TYPES_LIB_H_

#include <memory>
#include <string>

#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/Tooling.h"
#include "cpp_formatting/lint_lib.h"
#include "cpp_formatting/output_mode.h"
#include "llvm/ADT/StringRef.h"

namespace clang {
class ASTConsumer;
class CompilerInstance;
}  // namespace clang

// ---------------------------------------------------------------------------
// TrailingReturnCallback
// ---------------------------------------------------------------------------

/// Rewrites each matched function declaration to use a trailing return type
/// (e.g. `int foo()` -> `auto foo() -> int`).
class TrailingReturnCallback
    : public clang::ast_matchers::MatchFinder::MatchCallback {
 public:
  explicit TrailingReturnCallback(clang::Rewriter& Rewrite);
  void run(
      const clang::ast_matchers::MatchFinder::MatchResult& Result) override;

  /// Attach a lint report.  When set, every rewrite also records a
  /// diagnostic tagged with \p RuleId.  Not owned.
  void setLintReport(LintReport* Report, std::string RuleId) {
    this->Report = Report;
    this->RuleId = std::move(RuleId);
  }

 private:
  clang::Rewriter& Rewrite;
  LintReport* Report = nullptr;  // null outside Lint mode
  std::string RuleId;
};

// ---------------------------------------------------------------------------
// Shared matcher registration
// ---------------------------------------------------------------------------

/// Registers the trailing-return-type matchers on \p Finder, forwarding
/// matches to \p Callback.  \p Callback must outlive \p Finder.
/// This is the single authoritative place for the matcher predicate.
void registerTrailingReturnMatchers(clang::ast_matchers::MatchFinder& Finder,
                                    TrailingReturnCallback& Callback);

// ---------------------------------------------------------------------------
// TrailingReturnTypesAction
// ---------------------------------------------------------------------------

/// Frontend action that rewrites all eligible function declarations in a
/// source file to use trailing return types.
class TrailingReturnTypesAction : public clang::ASTFrontendAction {
 public:
  /// \p Pending and \p Report are used in Lint mode only: rewritten content
  /// is buffered into \p Pending (instead of being printed or written to
  /// disk) and every rewrite records a diagnostic in \p Report.
  explicit TrailingReturnTypesAction(OutputMode Mode,
                                     PendingRewrites* Pending = nullptr,
                                     LintReport* Report = nullptr,
                                     std::string RuleId = "");

  void EndSourceFileAction() override;

  auto CreateASTConsumer(clang::CompilerInstance& CI, llvm::StringRef File)
      -> std::unique_ptr<clang::ASTConsumer> override;

 private:
  OutputMode Mode;
  PendingRewrites* Pending;
  clang::Rewriter TheRewriter;
  TrailingReturnCallback Callback;  ///< must be declared after TheRewriter
  clang::ast_matchers::MatchFinder Finder;
};

// ---------------------------------------------------------------------------
// TrailingReturnActionFactory
// ---------------------------------------------------------------------------

/// Factory for ClangTool::run().  Mirrors RenameActionFactory: buffers
/// rewritten content per file and (in Lint mode) collects diagnostics.
class TrailingReturnActionFactory
    : public clang::tooling::FrontendActionFactory {
 public:
  explicit TrailingReturnActionFactory(OutputMode Mode) : Mode(Mode) {}

  void setLintReport(LintReport* Report, std::string RuleId) {
    this->Report = Report;
    this->RuleId = std::move(RuleId);
  }

  auto create() -> std::unique_ptr<clang::FrontendAction> override {
    return std::make_unique<TrailingReturnTypesAction>(Mode, &Pending, Report,
                                                       RuleId);
  }

  auto rewrites() const -> const PendingRewrites& { return Pending; }

 private:
  OutputMode Mode;
  PendingRewrites Pending;
  LintReport* Report = nullptr;
  std::string RuleId;
};

// ---------------------------------------------------------------------------
// Test helper
// ---------------------------------------------------------------------------

/// Parses \p Code as C++17, applies the trailing-return-type rewrite, and
/// returns the transformed source.  Returns the original string if the tool
/// fails to parse the input.
auto rewriteToTrailingReturnTypes(llvm::StringRef Code,
                                  const std::vector<std::string>& Args = {
                                      "-std=c++17", "-xc++"}) -> std::string;

#endif  // CPP_FORMATTING_TRAILING_RETURN_TYPES_LIB_H_
