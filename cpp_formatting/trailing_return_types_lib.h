#ifndef CPP_FORMATTING_TRAILING_RETURN_TYPES_LIB_H_
#define CPP_FORMATTING_TRAILING_RETURN_TYPES_LIB_H_

#include <memory>
#include <string>

#include "clang/AST/Decl.h"
#include "clang/AST/TypeLoc.h"
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
// ReturnTypeStyle
// ---------------------------------------------------------------------------

/// Which way the return type is moved.  The two directions are mutually
/// exclusive: running both over one TU would fight over the same declarations.
enum class ReturnTypeStyle {
  Trailing,  ///< `int foo()`          -> `auto foo() -> int`
  Leading,   ///< `auto foo() -> int`  -> `int foo()`
};

// ---------------------------------------------------------------------------
// TrailingReturnCallback
// ---------------------------------------------------------------------------

/// Rewrites each matched function declaration to the configured return-type
/// style: `int foo()` -> `auto foo() -> int` (Trailing), or the reverse
/// (Leading).
///
/// The Leading direction rejects far more than it rewrites — see
/// `runToLeading()` in the .cpp for the guards and why each one is needed.
class TrailingReturnCallback
    : public clang::ast_matchers::MatchFinder::MatchCallback {
 public:
  explicit TrailingReturnCallback(
      clang::Rewriter& Rewrite,
      ReturnTypeStyle Style = ReturnTypeStyle::Trailing);
  void run(
      const clang::ast_matchers::MatchFinder::MatchResult& Result) override;

  /// Attach a lint report.  When set, every rewrite also records a
  /// diagnostic tagged with \p RuleId.  Not owned.
  void setLintReport(LintReport* Report, std::string RuleId) {
    this->Report = Report;
    this->RuleId = std::move(RuleId);
  }

  /// Attach an edit report (Emit mode).  When set, each rewrite is appended as
  /// structured edit records instead of only mutating the Rewriter, and any
  /// rename edits already recorded inside the moved return type are dropped
  /// (their text is carried into the "-> type" insertion).  Not owned.
  void setEmitReport(EditReport* Edits) { this->Emit = Edits; }

 private:
  /// Rewrites `int foo()` to `auto foo() -> int`.
  void runToTrailing(const clang::FunctionDecl& Func, clang::SourceManager& SM,
                     clang::FunctionTypeLoc FTL);
  /// Rewrites `auto foo() -> int` back to `int foo()`.
  void runToLeading(const clang::FunctionDecl& Func, clang::SourceManager& SM,
                    clang::FunctionTypeLoc FTL);

  clang::Rewriter& Rewrite;
  ReturnTypeStyle Style;
  LintReport* Report = nullptr;  // null outside Lint mode
  std::string RuleId;
  EditReport* Emit = nullptr;  // non-null in Emit mode
};

// ---------------------------------------------------------------------------
// Shared matcher registration
// ---------------------------------------------------------------------------

/// Registers the return-type matchers for \p Style on \p Finder, forwarding
/// matches to \p Callback.  \p Callback must outlive \p Finder.
/// This is the single authoritative place for the matcher predicates.
void registerTrailingReturnMatchers(
    clang::ast_matchers::MatchFinder& Finder, TrailingReturnCallback& Callback,
    ReturnTypeStyle Style = ReturnTypeStyle::Trailing);

// ---------------------------------------------------------------------------
// TrailingReturnTypesAction
// ---------------------------------------------------------------------------

/// Frontend action that rewrites all eligible function declarations in a
/// source file to the configured return-type style.
class TrailingReturnTypesAction : public clang::ASTFrontendAction {
 public:
  /// \p Pending and \p Report are used in Lint mode only: rewritten content
  /// is buffered into \p Pending (instead of being printed or written to
  /// disk) and every rewrite records a diagnostic in \p Report.
  explicit TrailingReturnTypesAction(
      OutputMode Mode, PendingRewrites* Pending = nullptr,
      LintReport* Report = nullptr, std::string RuleId = "",
      ReturnTypeStyle Style = ReturnTypeStyle::Trailing);

  void EndSourceFileAction() override;

  auto CreateASTConsumer(clang::CompilerInstance& CI, llvm::StringRef File)
      -> std::unique_ptr<clang::ASTConsumer> override;

 private:
  OutputMode Mode;
  PendingRewrites* Pending;
  ReturnTypeStyle Style;
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
  explicit TrailingReturnActionFactory(
      OutputMode Mode, ReturnTypeStyle Style = ReturnTypeStyle::Trailing)
      : Mode(Mode), Style(Style) {}

  void setLintReport(LintReport* Report, std::string RuleId) {
    this->Report = Report;
    this->RuleId = std::move(RuleId);
  }

  auto create() -> std::unique_ptr<clang::FrontendAction> override {
    return std::make_unique<TrailingReturnTypesAction>(Mode, &Pending, Report,
                                                       RuleId, Style);
  }

  auto rewrites() const -> const PendingRewrites& { return Pending; }

 private:
  OutputMode Mode;
  ReturnTypeStyle Style;
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

/// The reverse: rewrites `auto foo() -> int` back to `int foo()`.  Returns the
/// original string if the tool fails to parse the input (and, as for every
/// guard-rejected declaration, if there is nothing safe to move).
auto rewriteToLeadingReturnTypes(llvm::StringRef Code,
                                 const std::vector<std::string>& Args = {
                                     "-std=c++17", "-xc++"}) -> std::string;

#endif  // CPP_FORMATTING_TRAILING_RETURN_TYPES_LIB_H_
