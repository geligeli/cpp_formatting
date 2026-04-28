#ifndef CPP_FORMATTING_RENAME_VARIABLES_LIB_H_
#define CPP_FORMATTING_RENAME_VARIABLES_LIB_H_

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

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
  // Broad scopes
  Member,  ///< Non-static member variables (FieldDecl) and static data members.
  Local,   ///< Local variables and function parameters.
  Global,  ///< File- and namespace-scope variables (non-member, non-local).

  // Fine-grained member scopes
  StaticMember,  ///< Static data members only (VarDecl::isStaticDataMember).
  ConstMember,   ///< Static data members that are const or constexpr.

  // Fine-grained global scopes
  StaticGlobal,  ///< File/namespace-scope vars declared with the static
                 ///< keyword.
  ConstGlobal,   ///< File/namespace-scope vars that are const or constexpr.
};

// ---------------------------------------------------------------------------
// Pending rewrites — buffered in-place writes
// ---------------------------------------------------------------------------

// Maps real absolute file paths to their fully rewritten content.  Populated
// by RenameActionFactory during ClangTool::run(); written to disk atomically
// by RenameActionFactory::flush() after the run completes.
//
// Buffering is what makes multi-file in-place renaming correct: every TU
// compiles against the original on-disk source, so no TU ever sees partially
// renamed headers from a previous TU.
using PendingRewrites = std::map<std::string, std::string>;

// ---------------------------------------------------------------------------
// RenameActionFactory
// ---------------------------------------------------------------------------

/// Factory for ClangTool::run() that renames variables and buffers all
/// in-place writes until flush() is called.
///
/// Usage:
///   auto F = RenameAllMemberVariables(cb, OutputMode::InPlace, files);
///   int rc = Tool.run(F.get());
///   F->flush();   // write all changed files to disk atomically
///   return rc;
class RenameActionFactory : public clang::tooling::FrontendActionFactory {
 public:
  RenameActionFactory(VariableRenameCallback CB, VariableScope Scope,
                      OutputMode Mode, FileSet CollectFrom);

  auto create() -> std::unique_ptr<clang::FrontendAction> override;

  /// Write all buffered in-place rewrites to disk.  Must be called once after
  /// ClangTool::run() completes.  No-op in DryRun mode.
  void flush();

 private:
  VariableRenameCallback CB;
  VariableScope Scope;
  OutputMode Mode;
  FileSet CollectFrom;
  PendingRewrites Pending;
};

// ---------------------------------------------------------------------------
// Convenience factories
// ---------------------------------------------------------------------------

/// Returns a RenameActionFactory that renames non-static member variables and
/// static data members.
auto RenameAllMemberVariables(VariableRenameCallback CB,
                              OutputMode Mode = OutputMode::DryRun,
                              FileSet CollectFrom = {})
    -> std::unique_ptr<RenameActionFactory>;

/// Returns a RenameActionFactory that renames local variables and parameters.
auto RenameAllLocalVariables(VariableRenameCallback CB,
                             OutputMode Mode = OutputMode::DryRun,
                             FileSet CollectFrom = {})
    -> std::unique_ptr<RenameActionFactory>;

/// Returns a RenameActionFactory that renames file- and namespace-scope
/// (global) variables.
auto RenameAllGlobalVariables(VariableRenameCallback CB,
                              OutputMode Mode = OutputMode::DryRun,
                              FileSet CollectFrom = {})
    -> std::unique_ptr<RenameActionFactory>;

/// Returns a RenameActionFactory that renames static data members only
/// (excludes non-static field members).
auto RenameAllStaticMemberVariables(VariableRenameCallback CB,
                                    OutputMode Mode = OutputMode::DryRun,
                                    FileSet CollectFrom = {})
    -> std::unique_ptr<RenameActionFactory>;

/// Returns a RenameActionFactory that renames static data members that are
/// declared const or constexpr.
auto RenameAllConstMemberVariables(VariableRenameCallback CB,
                                   OutputMode Mode = OutputMode::DryRun,
                                   FileSet CollectFrom = {})
    -> std::unique_ptr<RenameActionFactory>;

/// Returns a RenameActionFactory that renames file- and namespace-scope
/// variables declared with the static keyword (internal linkage).
auto RenameAllStaticGlobalVariables(VariableRenameCallback CB,
                                    OutputMode Mode = OutputMode::DryRun,
                                    FileSet CollectFrom = {})
    -> std::unique_ptr<RenameActionFactory>;

/// Returns a RenameActionFactory that renames file- and namespace-scope
/// variables that are const or constexpr.
auto RenameAllConstGlobalVariables(VariableRenameCallback CB,
                                   OutputMode Mode = OutputMode::DryRun,
                                   FileSet CollectFrom = {})
    -> std::unique_ptr<RenameActionFactory>;

// ---------------------------------------------------------------------------
// Source ordering helper
// ---------------------------------------------------------------------------

/// Returns \p SourcePaths reordered so that header files (.h/.hh/.hpp/.hxx)
/// come after all non-header files, preserving relative order within each
/// group.
///
/// Combined with PendingRewrites buffering this ensures that every TU sees
/// the original on-disk source regardless of how many headers are in the list.
auto orderSourcesForRename(const std::vector<std::string>& SourcePaths)
    -> std::vector<std::string>;

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
