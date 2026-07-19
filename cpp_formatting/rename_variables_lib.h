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
#include "cpp_formatting/lint_lib.h"
#include "cpp_formatting/output_mode.h"
#include "llvm/ADT/StringRef.h"

namespace clang {
class ASTConsumer;
class ASTContext;
class CompilerInstance;
class Rewriter;
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

  // Function scopes
  Method,  ///< Member functions, static and non-static (CXXMethodDecl).
           ///< Constructors, destructors, conversion functions, and overloaded
           ///< operators are never renamed.
};

// ---------------------------------------------------------------------------
// Pending rewrites — buffered in-place writes
// ---------------------------------------------------------------------------

// `PendingRewrites` (path -> fully rewritten content) is defined in
// lint_lib.h.  It is populated by RenameActionFactory during ClangTool::run()
// and written to disk atomically by RenameActionFactory::flush() after the
// run completes.
//
// Buffering is what makes multi-file in-place renaming correct: every TU
// compiles against the original on-disk source, so no TU ever sees partially
// renamed headers from a previous TU.

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

  /// Attach a lint report.  When set (Lint mode), every rewrite also records
  /// a diagnostic tagged with \p RuleId.  Not owned.
  void setLintReport(LintReport* Report, std::string RuleId) {
    this->Report = Report;
    this->RuleId = std::move(RuleId);
  }

  /// The rewrites buffered during ClangTool::run().  Read after run() and
  /// before flush(); in Lint mode this is how the main obtains the rewritten
  /// content for diff output.
  auto rewrites() const -> const PendingRewrites& { return Pending; }

  /// Write all buffered in-place rewrites to disk.  Must be called once after
  /// ClangTool::run() completes.  No-op in DryRun mode.
  void flush();

 private:
  VariableRenameCallback CB;
  VariableScope Scope;
  OutputMode Mode;
  FileSet CollectFrom;
  PendingRewrites Pending;
  LintReport* Report = nullptr;
  std::string RuleId;
};

// ---------------------------------------------------------------------------
// Per-TU rename helper
// ---------------------------------------------------------------------------

/// Runs one rename rule (collect declarations, then apply renames) on an
/// already-parsed translation unit, writing edits into \p RW.  Used by
/// cpp_format to run several rules in a single ClangTool pass.
void runRenameRuleOnAST(clang::ASTContext& Ctx, clang::Rewriter& RW,
                        const VariableRenameCallback& CB, VariableScope Scope,
                        const FileSet& CollectFrom,
                        LintReport* Report = nullptr,
                        llvm::StringRef RuleId = "");

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

/// Returns a RenameActionFactory that renames member functions (static and
/// non-static).  Constructors, destructors, conversion functions, and
/// overloaded operators are never renamed.  A virtual function is renamed
/// together with its entire override hierarchy; if any member of the
/// hierarchy is declared outside \p CollectFrom, the rename is skipped so
/// `override` checking is never broken.
auto RenameAllMemberFunctions(VariableRenameCallback CB,
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
