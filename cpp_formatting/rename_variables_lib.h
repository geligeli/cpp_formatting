#ifndef CPP_FORMATTING_RENAME_VARIABLES_LIB_H_
#define CPP_FORMATTING_RENAME_VARIABLES_LIB_H_

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "clang/Basic/Diagnostic.h"
#include "clang/Tooling/Tooling.h"
#include "cpp_formatting/lint_lib.h"
#include "cpp_formatting/output_mode.h"
#include "cpp_formatting/rename_state.h"
#include "cpp_formatting/tu_driver.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

namespace clang {
class Preprocessor;
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
// FileSet lives in tu_driver.h: it is what an action may rewrite, which the
// driver and every pass share.
using ::FileSet;

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

  // Type and namespace scopes
  Type,       ///< Classes, structs, unions, enums (nested ones included),
              ///< class templates, typedefs and alias declarations.  A type
              ///< the standard library reads by name (value_type, iterator,
              ///< type, ...) is never renamed; see isProtocolTypeName.
  Namespace,  ///< Named namespaces (inline ones included) and namespace
              ///< aliases.  The closing `}  // namespace x` comment is
              ///< rewritten along with the declaration.
};

// True when one declaration can match both scopes -- the fine-grained scopes
// are subsets of the broad ones, so a static data member is matched by
// `member`, `static_member` and `const_member` alike.  Two rules that both
// match it each rewrite the same bytes, and the second rewrite lands on text
// the first already replaced: `MaxCount` under `member: snake_case` plus
// `static_member: kConstant` comes out as `kMaxCountt`.  That is silent
// corruption, not a conflict, so a caller configuring several rules must
// reject the pair rather than run it.
bool scopesCanMatchSameDecl(VariableScope a, VariableScope b);

// True when declarations of the two scopes can share a DeclContext, i.e. a
// name one of them takes is a name the other cannot also have.  Data members
// and member functions share a class; the three global scopes share a
// namespace; locals share neither.  Rules whose scopes share a context must
// use styles that cannot produce the same name -- see namingStylesCanCollide()
// and the note on collides() in AGENTS.md.
bool scopesShareADeclContext(VariableScope a, VariableScope b);

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

/// Reports renames that were skipped to avoid a collision.  Always prints a
/// one-line summary; with \p Verbose also prints every site.  Duplicates (the
/// same declaration seen from several translation units) are collapsed.
void reportRenameConflicts(const RenameConflicts& Conflicts, bool Verbose,
                           llvm::raw_ostream& OS);

/// The TU driver's client for one rename rule: builds the per-TU action that
/// renames variables into a TUSlot, and buffers all in-place writes until
/// flush() is called.
///
/// Usage:
///   auto F = RenameAllMemberVariables(cb, OutputMode::InPlace, files);
///   int rc = runTranslationUnits(DB, sources, adjuster, opts, *F);
///   F->flush();   // write all changed files to disk atomically
///   return rc;
class RenameActionFactory : public TUSlotClient {
 public:
  RenameActionFactory(VariableRenameCallback CB, VariableScope Scope,
                      OutputMode Mode, FileSet CollectFrom);

  // TUSlotClient
  auto ruleCount() const -> size_t override { return 1; }
  auto createAction(TUSlot& Slot)
      -> std::unique_ptr<clang::FrontendAction> override;
  auto shared() -> CrossTUState& override { return Shared; }
  /// False in Emit mode, whose records carry their own vetoes for aggregation
  /// to act on, so a veto needs no second pass.
  auto rerunNeededOnVeto() const -> bool override {
    return Mode != OutputMode::Emit;
  }
  /// False in Emit mode: every TU records independently and aggregation
  /// merges, which also makes the records identical for any thread count.
  auto crossTUSeeding() const -> bool override {
    return Mode != OutputMode::Emit;
  }
  void finish(std::vector<TUSlot>& Slots) override;

  /// Attach a lint report.  When set (Lint mode), every rewrite also records
  /// a diagnostic tagged with \p RuleId.  Not owned.
  void setLintReport(LintReport* Report, std::string RuleId) {
    this->Report = Report;
    this->RuleId = std::move(RuleId);
  }

  /// The rewrites buffered during the run.  Read after runTranslationUnits()
  /// and before flush(); in Lint mode this is how the main obtains the
  /// rewritten content for diff output.
  auto rewrites() const -> const PendingRewrites& { return Pending; }

  /// Write all buffered in-place rewrites to disk (or print them, in DryRun
  /// mode).  Must be called once after the run completes.
  void flush();

  /// In Emit mode, writes this run's edit records plus the dependent-token
  /// resolution sidecar (built from the cross-TU map) as JSON to \p OS.  Call
  /// after the run completes.
  void emitEdits(llvm::raw_ostream& OS);

  /// Renames that were skipped because the new name was already taken in the
  /// same scope, or because a reference to them could not be rewritten.
  auto conflicts() const -> const RenameConflicts& { return Conflicts; }

  /// Declarations vetoed during the run because some reference to them
  /// cannot be rewritten.
  auto vetoes() const -> const RenameVetoes& { return Shared.Vetoes; }

 private:
  VariableRenameCallback CB;
  VariableScope Scope;
  OutputMode Mode;
  FileSet CollectFrom;
  PendingRewrites Pending;    // merged by finish()
  EditReport Edits;           // merged by finish(); Emit mode
  RenameConflicts Conflicts;  // merged by finish()
  // Vetoes and the cross-TU dependent-token resolution map, exchanged between
  // TUs by the driver; see tu_driver.h.
  CrossTUState Shared;
  LintReport* Report = nullptr;
  std::string RuleId;
};

// ---------------------------------------------------------------------------
// Per-TU rename helper
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Macro arguments the preprocessor consumed
// ---------------------------------------------------------------------------

/// The macro-argument tokens that never reached the parser.  A function-like
/// macro that only ever pastes a parameter (`FLAGS_##name`), stringizes it
/// (`#name`) or does not use it at all emits no token of its own for that
/// argument, so no AST node can refer to the identifier at its spelling -- and
/// a rename of a declaration that merely shares the spelling leaves nothing
/// behind there.  absl's ABSL_FLAG(std::string, docker_image, ...) next to a
/// struct member `docker_image` is the case: the flag's name is only ever
/// pasted and stringized, and the spelling audit used to decline the member.
/// Keyed by (file, offset) of the argument token as written.  Filled during
/// parsing by the PPCallbacks watchConsumedMacroArgs() installs.
struct ConsumedMacroArgs {
  /// Argument tokens no expansion ever emitted as themselves.
  std::set<std::pair<clang::FileID, unsigned>> Tokens;
  /// Argument tokens some expansion pasted with `##` -- at any nesting depth,
  /// so an argument forwarded from MOCK_METHOD down to GMOCK_MOCKER_ counts.
  /// A declaration spelled at one of these is never renamed: the paste forms
  /// other identifiers from its spelling, and a use written the same way
  /// (`ON_CALL(m, DoThis())`) forms them again from the *old* spelling.
  std::set<std::pair<clang::FileID, unsigned>> Pasted;
};

/// Installs the callbacks that fill \p Out; call before the TU is parsed.
/// \p Out must outlive the Preprocessor's use of it.
void watchConsumedMacroArgs(clang::Preprocessor& PP, ConsumedMacroArgs& Out);

/// Runs one rename rule (collect declarations, then apply renames) on an
/// already-parsed translation unit, writing edits into \p RW.  Used by
/// cpp_format to run several rules in a single ClangTool pass.
/// \p DepRes, when non-null, enables cross-TU renaming of template-dependent
/// member tokens: this TU's instantiations record resolutions into it, and its
/// dependent tokens are rewritten from the resolutions it was seeded with
/// (the TU driver merges every TU's map and seeds later TUs from the result).
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
/// \p RenamedNames, when non-null, receives the old spelling of every
/// declaration this TU set out to rename; the TU driver intersects it with the
/// dependent tokens no TU resolved to decline those names (see nameVetoKey).
/// \p PP, when non-null, lets the scan pass read macro definitions, to decline
/// a declaration spelled as an argument of a macro that pastes with `##`.
/// \p Consumed, when non-null, tells the spelling audit which macro-argument
/// tokens the preprocessor consumed without ever emitting (see
/// ConsumedMacroArgs); an identifier there is not a reference and does not
/// decline anything.
/// \p AllScopes lists every rule's scope when several rules run over one AST
/// (empty means just \p Scope): the dependent-token recorder uses it to tell
/// "another rule renames this member" from "nobody does".
void runRenameRuleOnAST(clang::ASTContext& Ctx, clang::Rewriter& RW,
                        const VariableRenameCallback& CB, VariableScope Scope,
                        const FileSet& CollectFrom,
                        LintReport* Report = nullptr,
                        llvm::StringRef RuleId = "",
                        DependentResolutions* DepRes = nullptr,
                        EditReport* Edits = nullptr,
                        RenameConflicts* Conflicts = nullptr,
                        RenameVetoes* Vetoes = nullptr,
                        std::set<std::string>* RenamedNames = nullptr,
                        clang::Preprocessor* PP = nullptr,
                        const ConsumedMacroArgs* Consumed = nullptr);

/// One rule of a multi-rule run (see runRenameRulesOnAST).
struct RenameRuleSpec {
  const VariableRenameCallback* CB;
  VariableScope Scope;
  std::string RuleId;
  DependentResolutions* DepRes;  ///< this rule's map, or null
};

/// Runs several rules over one AST as a unit: every rule collects its
/// candidates first, then the collisions are resolved with all of them in
/// view (a new name blocked only by a declaration another rule renames away
/// is free; two candidates for one name in one scope go to the first rule in
/// the list), then every rule's scan pass runs, then the dependent-token
/// recorders, then the rewrites -- in rule order at each step.  A rename
/// accepted because another vacates its name is recorded as depending on it,
/// so a later veto of the mover takes the dependent down with it (in this
/// pass, and through the edit records in Emit mode).  runRenameRuleOnAST is
/// this with one rule.
void runRenameRulesOnAST(clang::ASTContext& Ctx, clang::Rewriter& RW,
                         llvm::ArrayRef<RenameRuleSpec> Rules,
                         const FileSet& CollectFrom,
                         LintReport* Report = nullptr,
                         EditReport* Edits = nullptr,
                         RenameConflicts* Conflicts = nullptr,
                         RenameVetoes* Vetoes = nullptr,
                         std::set<std::string>* RenamedNames = nullptr,
                         clang::Preprocessor* PP = nullptr,
                         const ConsumedMacroArgs* Consumed = nullptr);

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
auto RenameAllTypes(VariableRenameCallback CB, OutputMode Mode,
                    FileSet CollectFrom)
    -> std::unique_ptr<RenameActionFactory>;
auto RenameAllNamespaces(VariableRenameCallback CB, OutputMode Mode,
                         FileSet CollectFrom)
    -> std::unique_ptr<RenameActionFactory>;
auto RenameAllMemberFunctions(VariableRenameCallback CB,
                              OutputMode Mode = OutputMode::DryRun,
                              FileSet CollectFrom = {})
    -> std::unique_ptr<RenameActionFactory>;

// ---------------------------------------------------------------------------
// Dependent tokens (shared with the symbol index)
// ---------------------------------------------------------------------------

/// A template-dependent token with a spelling of its own: a member access
/// through a template parameter (`t.m`, `t.f(x)`), a qualified name whose
/// lookup is deferred to instantiation (`Helper<T>::k`), or an unqualified
/// call with dependent arguments (`f(t)`).  \p Loc is the token as written
/// (for a macro argument, the expansion location; the spelling is at the call
/// site); a token spelled in a macro body or formed by `##` has no spelling of
/// its own and is not reported.
struct DependentToken {
  clang::SourceLocation Loc;
  std::string Name;
};

/// Every dependent token in the TU's pattern code, in any file.
auto collectDependentTokens(clang::ASTContext& Ctx)
    -> std::vector<DependentToken>;

/// Walks the TU's template instantiations and, for every resolved reference
/// whose *spelling* location \p IsToken accepts, calls \p OnBinding with that
/// spelling and the declaration it resolved to -- mapped back to the template
/// pattern when it is an instantiated member or function, so the binding
/// names the declaration as written.  Nothing is called for a token no
/// instantiation in this TU resolves.
void forEachDependentBinding(
    clang::ASTContext& Ctx,
    llvm::function_ref<bool(clang::SourceLocation)> IsToken,
    llvm::function_ref<void(clang::SourceLocation, const clang::Decl*)>
        OnBinding);

// ---------------------------------------------------------------------------
// Source ordering helper
// ---------------------------------------------------------------------------

/// Returns \p SourcePaths reordered so that header files (isHeaderSource)
/// come after all non-header files, preserving relative order within each
/// group.  This is the order the TU driver processes sources in (its two
/// phases); exposed for callers that need the same order.
auto orderSourcesForRename(const std::vector<std::string>& SourcePaths)
    -> std::vector<std::string>;

// ---------------------------------------------------------------------------
// Test helper
// ---------------------------------------------------------------------------

/// Parses \p Code as C++17, applies the variable rename, and returns the
/// transformed source.  Returns the original string if the tool fails.
auto rewriteVariableNames(
    llvm::StringRef Code, VariableRenameCallback CB, VariableScope Scope,
    const std::vector<std::string>& Args = {"-std=c++17", "-xc++"},
    const std::vector<std::pair<std::string, std::string>>& VirtualFiles = {})
    -> std::string;

/// Test helper for several rules at once (in-memory TU, rules in order).
auto rewriteWithRules(
    llvm::StringRef Code,
    const std::vector<std::pair<VariableScope, VariableRenameCallback>>& Rules,
    const std::vector<std::string>& Args = {"-std=c++17", "-xc++"})
    -> std::string;

#endif  // CPP_FORMATTING_RENAME_VARIABLES_LIB_H_
