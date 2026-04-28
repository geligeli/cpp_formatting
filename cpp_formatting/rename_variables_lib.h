#ifndef CPP_FORMATTING_RENAME_VARIABLES_LIB_H_
#define CPP_FORMATTING_RENAME_VARIABLES_LIB_H_

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/Tooling.h"
#include "cpp_formatting/output_mode.h"
#include "llvm/ADT/StringRef.h"

namespace clang {
class ASTConsumer;
class CompilerInstance;
}  // namespace clang

// ---------------------------------------------------------------------------
// File set
// ---------------------------------------------------------------------------

// Absolute real paths of source files that the tool "owns".  When non-empty,
// variable declarations are collected from any file whose real path is in this
// set, not just the main file being compiled.  This lets the tool rename uses
// in a .cpp even when the declaration lives in a header that is also in the
// set. An empty FileSet falls back to the original behaviour (main-file-only
// collection).
using FileSet = std::unordered_set<std::string>;

// ---------------------------------------------------------------------------
// Rename callback
// ---------------------------------------------------------------------------

/// Called once per canonical variable declaration.
/// Populate \p new_name and return \c true to rename; return \c false to leave
/// the variable unchanged.
using VariableRenameCallback =
    std::function<bool(std::string_view, std::string&)>;

// ---------------------------------------------------------------------------
// Variable scope
// ---------------------------------------------------------------------------

enum class VariableScope {
  Member,  ///< Non-static member variables (FieldDecl) and static data members.
  Local,   ///< Local variables and function parameters.
  Global,  ///< File- and namespace-scope variables (non-member, non-local).
};

// ---------------------------------------------------------------------------
// RenameVariablesAction
// ---------------------------------------------------------------------------

/// Frontend action that renames variables within the given \p Scope.
/// The callback is invoked once per canonical declaration; all declaration and
/// use sites (DeclRefExpr, MemberExpr) in the main file are rewritten.
class RenameVariablesAction : public clang::ASTFrontendAction {
 public:
  RenameVariablesAction(VariableRenameCallback CB, VariableScope Scope,
                        OutputMode Mode, FileSet CollectFrom = {});

  void EndSourceFileAction() override;

  auto CreateASTConsumer(clang::CompilerInstance& CI, llvm::StringRef File)
      -> std::unique_ptr<clang::ASTConsumer> override;

 private:
  VariableRenameCallback CB;
  VariableScope Scope;
  OutputMode Mode;
  FileSet CollectFrom;
  clang::Rewriter TheRewriter;
};

// ---------------------------------------------------------------------------
// Convenience factories
// ---------------------------------------------------------------------------

/// Returns a FrontendActionFactory suitable for \c ClangTool::run() that
/// renames non-static member variables and static data members.
auto RenameAllMemberVariables(VariableRenameCallback CB,
                              OutputMode Mode = OutputMode::DryRun,
                              FileSet CollectFrom = {})
    -> std::unique_ptr<clang::tooling::FrontendActionFactory>;

/// Returns a FrontendActionFactory that renames local variables and parameters.
auto RenameAllLocalVariables(VariableRenameCallback CB,
                             OutputMode Mode = OutputMode::DryRun,
                             FileSet CollectFrom = {})
    -> std::unique_ptr<clang::tooling::FrontendActionFactory>;

/// Returns a FrontendActionFactory that renames file- and namespace-scope
/// (global) variables.
auto RenameAllGlobalVariables(VariableRenameCallback CB,
                              OutputMode Mode = OutputMode::DryRun,
                              FileSet CollectFrom = {})
    -> std::unique_ptr<clang::tooling::FrontendActionFactory>;

// ---------------------------------------------------------------------------
// Test helper
// ---------------------------------------------------------------------------

/// Parses \p Code as C++17, applies the variable rename, and returns the
/// transformed source.  Returns the original string if the tool fails.
auto rewriteVariableNames(llvm::StringRef Code, VariableRenameCallback CB,
                          VariableScope Scope,
                          const std::vector<std::string>& Args = {
                              "-std=c++17", "-xc++"}) -> std::string;

#endif  // CPP_FORMATTING_RENAME_VARIABLES_LIB_H_
