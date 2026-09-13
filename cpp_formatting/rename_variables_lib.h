#ifndef CPP_FORMATTING_RENAME_VARIABLES_LIB_H_
#define CPP_FORMATTING_RENAME_VARIABLES_LIB_H_

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "clang/Basic/Diagnostic.h"
#include "clang/Tooling/Tooling.h"
#include "cpp_formatting/lint_lib.h"
#include "cpp_formatting/output_mode.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

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
// Cross-TU resolution of template-dependent member tokens
// ---------------------------------------------------------------------------
//
// A member access through a template parameter — e.g. `x.val` in
//   auto set_val(auto& x) { x.val = 12; }
// — is a *dependent* expression: the member it names is only known once the
// template is instantiated, and the instantiation typically lives in a
// different translation unit than the header that spells the token.  The
// per-main-file Rewriter model therefore cannot rename such a token on its own:
// the header's own TU never instantiates the template, and the instantiating
// .cpp's TU may not rewrite the header.
//
// To bridge the two, every TU records — keyed by the token's (real path, byte
// offset) — the new name each observed instantiation resolves the token to.
// When the header is later processed as its own main file, the token is
// rewritten using that agreed-upon name.  If instantiations disagree, or any
// instantiation binds the token to a member that is not being renamed (e.g. a
// type outside the FileSet), the entry is vetoed and the token is left
// untouched.  Because header sources are ordered last (see
// orderSourcesForRename), all instantiating .cpp TUs are processed before the
// header TU that consumes their resolutions.
struct DependentResolution {
  std::string NewName;   ///< the agreed-upon new name
  bool HasName = false;  ///< at least one instantiation resolved a new name
  bool Vetoed = false;   ///< conflicting / out-of-scope binding: do not rewrite
  std::string OldName;   ///< spelled member identifier (for emitting a record)
  unsigned Length = 0;   ///< length of the spelled token
  /// The member the token resolves to, keyed as in RenameVetoes, so that a
  /// veto of that member also drops this token in aggregation.
  std::string OwnerFile;
  unsigned OwnerOffset = 0;
};

/// (real path — or, for in-memory buffers, a presumed file name — and byte
/// offset within that file) -> resolution.
using DependentResolutions =
    std::map<std::pair<std::string, unsigned>, DependentResolution>;

// ---------------------------------------------------------------------------
// Renames vetoed by a reference the tool cannot rewrite
// ---------------------------------------------------------------------------
//
// A reference spelled inside a macro *body* — `#define BUMP(c) ((c).count +=
// 1)` — or synthesized by token pasting has no byte range in any source file
// that holds the name: the body token is one location shared by every
// expansion, and a pasted token exists only in Clang's scratch buffer. Renaming
// the declaration would leave such a reference spelling the old name, which is
// a build break rather than a formatting change.
//
// Renaming is therefore all-or-nothing: a single unrewritable reference vetoes
// the declaration, and it keeps its old name everywhere.  The veto is keyed by
// the declaration's (real path, byte offset) — stable across TUs *and* across
// processes, which is what lets one Bazel action's veto suppress the edits
// another action already emitted for the same declaration (see EditRecord's
// owner fields in lint_lib.h).
//
// Within one ClangTool::run() the map is threaded through every TU by the
// factories.  A veto found in a TU processed *after* one that already renamed
// the declaration cannot undo that TU's buffered rewrite, so the drivers
// re-run the tool once when the first pass discovered any veto (see
// runWithVetoRerun); nothing is written to disk until flush(), so discarding
// the first pass costs nothing.  Emit mode skips that re-run: its output is
// records carrying these vetoes, and aggregation does the dropping.
//
// A macro *argument* is not affected: `FWD(c.count)` spells `count` at the call
// site, so it is rewritten normally (see ApplyRenamesVisitor::rewriteLoc). That
// holds for template-dependent tokens too -- `EXPECT_FALSE(this->count_)` in a
// class template -- since DependentResolutions is keyed by the spelling.
using RenameVetoes = std::map<std::pair<std::string, unsigned>, RenameVeto>;

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
/// A rename that was *not* applied because the new name is already taken in
/// the same scope.  Renaming anyway would either be a redeclaration error or,
/// worse, silently rebind existing uses to a different entity -- so the
/// declaration and all its uses are left alone and the site is reported.
struct RenameConflict {
  std::string File;
  unsigned Line = 0;
  unsigned Column = 0;
  std::string OldName;
  std::string NewName;
  std::string Reason;  ///< what the new name would have clashed with
};
using RenameConflicts = std::vector<RenameConflict>;

/// Reports renames that were skipped to avoid a collision.  Always prints a
/// one-line summary; with \p Verbose also prints every site.  Duplicates (the
/// same declaration seen from several translation units) are collapsed.
void reportRenameConflicts(const RenameConflicts& Conflicts, bool Verbose,
                           llvm::raw_ostream& OS);

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

  /// In Emit mode, writes this run's edit records plus the dependent-token
  /// resolution sidecar (built from the cross-TU map) as JSON to \p OS.  Call
  /// after ClangTool::run() completes.
  void emitEdits(llvm::raw_ostream& OS);

  /// Renames that were skipped because the new name was already taken in the
  /// same scope, or because a reference to them could not be rewritten.
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
  VariableRenameCallback CB;
  VariableScope Scope;
  OutputMode Mode;
  FileSet CollectFrom;
  PendingRewrites Pending;
  EditReport Edits;           // populated in Emit mode
  RenameConflicts Conflicts;  // renames skipped because the name was taken
  // Template-dependent member tokens resolved across TUs; persists for the
  // whole ClangTool::run() so a header TU can consume resolutions recorded by
  // earlier .cpp TUs.  See DependentResolutions above.
  DependentResolutions DepRes;
  // Declarations that must not be renamed because a reference to them is not
  // rewritable; persists across a re-run.  See RenameVetoes above.
  RenameVetoes Vetoes;
  LintReport* Report = nullptr;
  std::string RuleId;
};

// ---------------------------------------------------------------------------
// Driving a run that may veto renames
// ---------------------------------------------------------------------------

/// Runs \p Factory over \p Tool and, when the pass vetoed any rename, discards
/// everything it buffered and runs it again with those vetoes known up front.
/// Returns the last run's exit code.  Call flush()/emitEdits() afterwards, as
/// for a plain ClangTool::run().
///
/// A veto discovered while processing one TU can invalidate a rename an earlier
/// TU already buffered, and a Rewriter's edits cannot be taken back.  Nothing
/// has reached disk before flush(), so re-running is the cheap way to make the
/// result order-independent — and it costs nothing at all in the common case,
/// where no macro names anything being renamed.
///
/// Usually one extra pass suffices, since the second renames a subset of the
/// first and so can discover no new veto.  The exception is a name freed up by
/// a veto and claimed by another declaration (see CollectRenamesVisitor's
/// collides()), which can veto in turn; the loop runs to a fixpoint under a
/// small cap.
///
/// Emit mode needs none of this and says so via rerunNeededOnVeto(): its output
/// is records, not a Rewriter buffer, and aggregation already drops every edit
/// whose owner any report vetoed — including edits and vetoes from this same
/// run.  Skipping the re-run there is what keeps the Bazel path, where each
/// target is its own process, from paying for a second parse.
template <typename FactoryT>
auto runWithVetoRerun(clang::tooling::ClangTool& Tool, FactoryT& Factory)
    -> int {
  constexpr int MaxPasses = 4;
  int rc = Tool.run(&Factory);
  size_t Vetoed = Factory.vetoes().size();
  if (Vetoed == 0 || !Factory.rerunNeededOnVeto()) return rc;
  llvm::errs() << "re-running: " << Vetoed
               << " rename(s) vetoed by a reference that cannot be rewritten\n";
  // The first pass already reported any parse diagnostics; don't repeat them.
  clang::IgnoringDiagConsumer Silent;
  Tool.setDiagnosticConsumer(&Silent);
  for (int Pass = 1; Pass < MaxPasses; ++Pass) {
    Factory.resetForRerun();
    rc = Tool.run(&Factory);
    if (Factory.vetoes().size() == Vetoed) break;
    Vetoed = Factory.vetoes().size();
  }
  Tool.setDiagnosticConsumer(nullptr);
  return rc;
}

// ---------------------------------------------------------------------------
// Per-TU rename helper
// ---------------------------------------------------------------------------

/// Runs one rename rule (collect declarations, then apply renames) on an
/// already-parsed translation unit, writing edits into \p RW.  Used by
/// cpp_format to run several rules in a single ClangTool pass.
/// \p DepRes, when non-null, enables cross-TU renaming of template-dependent
/// member tokens: this TU's instantiations record resolutions into it, and its
/// dependent tokens are rewritten from resolutions recorded by earlier TUs.
/// The same DependentResolutions instance must be passed for every TU of one
/// ClangTool::run() (the factories hold one and thread its address through).
///
/// \p Edits, when non-null, switches to emit mode: ordinary decl/use renames
/// are appended as edit records instead of being consumed for output, and
/// template-dependent tokens are NOT applied (their resolutions are serialized
/// separately for the aggregation phase).
/// \p Vetoes, when non-null, enables all-or-nothing renaming: a scan pass over
/// this TU records every declaration that has a reference the tool cannot
/// rewrite (see RenameVetoes), those declarations are dropped from this TU's
/// rename set before anything is applied, and declarations vetoed by an earlier
/// TU are never collected in the first place.
void runRenameRuleOnAST(clang::ASTContext& Ctx, clang::Rewriter& RW,
                        const VariableRenameCallback& CB, VariableScope Scope,
                        const FileSet& CollectFrom,
                        LintReport* Report = nullptr,
                        llvm::StringRef RuleId = "",
                        DependentResolutions* DepRes = nullptr,
                        EditReport* Edits = nullptr,
                        RenameConflicts* Conflicts = nullptr,
                        RenameVetoes* Vetoes = nullptr);

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
