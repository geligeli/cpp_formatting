#pragma once

#include <memory>
#include <string>

#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "llvm/ADT/StringRef.h"

namespace clang {
class ASTConsumer;
class CompilerInstance;
}  // namespace clang

// ---------------------------------------------------------------------------
// Output mode for TrailingReturnTypesAction
// ---------------------------------------------------------------------------

enum class OutputMode {
  DryRun,   ///< Print rewritten source to stdout (default).
  InPlace,  ///< Overwrite the input file on disk.
};

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

 private:
  clang::Rewriter& Rewrite;
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
  explicit TrailingReturnTypesAction(OutputMode Mode);

  void EndSourceFileAction() override;

  std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
      clang::CompilerInstance& CI, llvm::StringRef File) override;

 private:
  OutputMode Mode;
  clang::Rewriter TheRewriter;
  TrailingReturnCallback Callback;  ///< must be declared after TheRewriter
  clang::ast_matchers::MatchFinder Finder;
};

// ---------------------------------------------------------------------------
// Test helper
// ---------------------------------------------------------------------------

/// Parses \p Code as C++17, applies the trailing-return-type rewrite, and
/// returns the transformed source.  Returns the original string if the tool
/// fails to parse the input.
std::string rewriteToTrailingReturnTypes(llvm::StringRef Code,
                                         const std::vector<std::string> &Args = {"-std=c++17", "-xc++"});
