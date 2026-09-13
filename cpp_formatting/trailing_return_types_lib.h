#ifndef CPP_FORMATTING_TRAILING_RETURN_TYPES_LIB_H_
#define CPP_FORMATTING_TRAILING_RETURN_TYPES_LIB_H_

#include <memory>
#include <string>

#include "clang/AST/Decl.h"
#include "clang/AST/TypeLoc.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "cpp_formatting/lint_lib.h"
#include "cpp_formatting/output_mode.h"
#include "cpp_formatting/tu_driver.h"
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

  /// The files this pass may rewrite (Emit mode).  Without it only the TU's
  /// own main file is rewritten, which is right for a direct run; in Emit mode
  /// a header has no action of its own, so it must be edited by the TUs that
  /// include it.  See isRewritableFile() in tu_driver.h.  Not owned.
  void setOwnedFiles(const FileSet* Owned) { this->Owned = Owned; }

 private:
  /// Rewrites `int foo()` to `auto foo() -> int`.
  void runToTrailing(const clang::FunctionDecl& Func, clang::SourceManager& SM,
                     clang::FunctionTypeLoc FTL);
  /// Rewrites `auto foo() -> int` back to `int foo()`.
  void runToLeading(const clang::FunctionDecl& Func, clang::SourceManager& SM,
                    clang::FunctionTypeLoc FTL);

  clang::Rewriter& Rewrite;
  ReturnTypeStyle Style;
  const FileSet* Owned = nullptr;
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
  /// Rewritten content is buffered into \p Pending -- the whole main file in
  /// DryRun mode, only a main file that has edits otherwise -- and committed by
  /// TrailingReturnActionFactory::flush() after every TU has run.  Nothing is
  /// printed or written from inside a TU.  \p Report, when non-null (Lint
  /// mode), records one diagnostic per rewrite.
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

/// The TU driver's client for the return-type rewrite.  Mirrors
/// RenameActionFactory: buffers rewritten content per file and (in Lint mode)
/// collects diagnostics, and commits the buffered content via flush() once
/// every TU has run.
class TrailingReturnActionFactory : public TUSlotClient {
 public:
  explicit TrailingReturnActionFactory(
      OutputMode Mode, ReturnTypeStyle Style = ReturnTypeStyle::Trailing)
      : Mode(Mode), Style(Style) {}

  void setLintReport(LintReport* Report, std::string RuleId) {
    this->Report = Report;
    this->RuleId = std::move(RuleId);
  }

  // TUSlotClient.  The rewrite has no cross-TU state: nothing is seeded and
  // nothing can veto.
  auto ruleCount() const -> size_t override { return 0; }
  auto createAction(TUSlot& Slot)
      -> std::unique_ptr<clang::FrontendAction> override {
    return std::make_unique<TrailingReturnTypesAction>(
        Mode, &Slot.Pending, Report ? &Slot.Report : nullptr, RuleId, Style);
  }
  auto shared() -> CrossTUState& override { return Shared; }
  auto rerunNeededOnVeto() const -> bool override { return false; }
  auto crossTUSeeding() const -> bool override { return false; }
  void finish(std::vector<TUSlot>& Slots) override;

  auto rewrites() const -> const PendingRewrites& { return Pending; }

  /// Commits the buffered content: prints it (DryRun -- one banner, then each
  /// file, with a `=== path ===` header when there are several), writes it to
  /// disk (InPlace), or drops it (Lint, where the main has already consumed
  /// it).  Call once after every TU has run.
  void flush();

 private:
  OutputMode Mode;
  ReturnTypeStyle Style;
  PendingRewrites Pending;  // merged by finish()
  CrossTUState Shared;      // always empty
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
