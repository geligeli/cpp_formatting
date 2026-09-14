#ifndef CPP_FORMATTING_LINT_LIB_H_
#define CPP_FORMATTING_LINT_LIB_H_

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

// ---------------------------------------------------------------------------
// Pending rewrites — buffered file contents
// ---------------------------------------------------------------------------

// Maps real absolute file paths to their fully rewritten content.  Populated
// during ClangTool::run(); in InPlace mode written to disk by flush(), in
// Lint mode consumed by the main to emit a unified diff.
using PendingRewrites = std::map<std::string, std::string>;

// ---------------------------------------------------------------------------
// Lint diagnostics
// ---------------------------------------------------------------------------

// A single lint violation: the location of a would-be rewrite plus the rule
// that produced it.
struct LintDiagnostic {
  std::string File;
  unsigned Line = 0;
  unsigned Column = 0;
  std::string RuleId;
  std::string Message;
};

// Collects lint diagnostics across translation units and emits them as plain
// text or SARIF.
class LintReport {
 public:
  void add(LintDiagnostic Diag);

  // Discards every diagnostic recorded so far.  Used when a translation unit
  // is run again (see tu_driver.h), so the second pass does not append to the
  // first pass's findings.
  void clear() { Diagnostics.clear(); }

  auto empty() const -> bool { return Diagnostics.empty(); }
  auto size() const -> std::size_t { return Diagnostics.size(); }
  auto diagnostics() const -> const std::vector<LintDiagnostic>& {
    return Diagnostics;
  }

  // Emits one `file:line:col: warning: message [rule-id]` line per
  // diagnostic, sorted by file/line/column.
  void emitText(llvm::raw_ostream& OS) const;

  // Emits a SARIF 2.1.0 log with a single run.  File paths are relativized
  // to the current working directory when they sit underneath it, so code
  // scanning uploaders (e.g. GitHub) can match them to repository files.
  void emitSARIF(llvm::raw_ostream& OS, llvm::StringRef ToolName) const;

 private:
  std::vector<LintDiagnostic> Diagnostics;
};

// Returns \p Path relativized to the current working directory when it sits
// underneath it; otherwise returns \p Path unchanged.
auto relativizeToCwd(llvm::StringRef Path) -> std::string;

// ---------------------------------------------------------------------------
// Unified diff
// ---------------------------------------------------------------------------

// Emits \p Modified as a git-apply-able unified diff against \p Original
// (`--- a/<path>` / `+++ b/<path>` headers, 3 lines of context, adjacent
// hunks merged, "\ No newline at end of file" markers).  Emits nothing when
// the two inputs are identical.
void emitUnifiedDiff(llvm::StringRef Original, llvm::StringRef Modified,
                     llvm::StringRef Path, llvm::raw_ostream& OS);

// Emits lint results to stdout in the requested \p Format ("text", "sarif",
// or "diff") and returns the process exit code: 0 when no violations were
// found, 1 otherwise.  For "diff", each file's original content is read from
// disk and diffed against the buffered rewritten content in \p Rewrites.
auto emitLintResults(const LintReport& Report, const PendingRewrites& Rewrites,
                     llvm::StringRef Format, llvm::StringRef ToolName) -> int;

// ---------------------------------------------------------------------------
// Structured edits (Bazel per-TU emit + aggregation)
// ---------------------------------------------------------------------------
//
// For a Bazel-parallelised reformat, each target's action emits the edits it
// found as *records* against the original file content — never a rendered diff,
// which cannot be merged (hunk offsets don't compose).  A separate aggregation
// phase unions all reports, resolves template-dependent tokens across TUs, and
// merges the edits per file (dedup identical; overlapping-distinct is a
// conflict).  File keys are the source's real path — under Bazel that resolves
// to the absolute workspace path (source symlinks), which is stable across
// actions; aggregate_edits uses absolute keys directly and joins relative keys
// (from a plain --emit-edits run) with its --root.

// One text replacement at a byte range of a file's ORIGINAL content.
//
// A rename edit also names the declaration it belongs to (OwnerFile,
// OwnerOffset — the real path and byte offset of that declaration's name
// token, stable across TUs and processes).  Aggregation drops every edit whose
// owner some other invocation vetoed, which is what keeps a rename all-or-
// nothing across targets: the library's action renames the declaration while
// the binary's action discovers a reference it cannot rewrite.  Edits that are
// not renames (trailing-return rewrites) leave OwnerFile empty and are never
// vetoed.
struct EditRecord {
  std::string File;
  unsigned Offset = 0;
  unsigned Length = 0;
  std::string Old;        ///< original bytes (verification / debugging)
  std::string New;        ///< replacement bytes
  std::string OwnerFile;  ///< declaration this rename belongs to, or ""
  unsigned OwnerOffset = 0;
};

// A declaration that must not be renamed because some reference to it cannot be
// rewritten — it is spelled inside a macro body, or synthesized by token
// pasting, so no byte range of any source file holds the name.  Renaming the
// declaration anyway would leave that reference spelling the old name, i.e.
// break the build.  Keyed like EditRecord's owner: (real path, byte offset) of
// the declaration's name token.
struct RenameVeto {
  std::string File;
  unsigned Offset = 0;
  std::string Name;    ///< the declaration's spelled name (for reporting)
  std::string Reason;  ///< why the reference could not be rewritten
};

// A rename that was *not* applied, and why.  Either the new name was already
// taken in the same scope, or some reference to the declaration cannot be
// rewritten (spelled in a macro body, token-pasted, in a file no invocation
// rewrites, ...).  Renaming anyway would fail to compile, or silently rebind
// existing uses -- so the declaration and all its uses are left alone and the
// site is reported.  Carried in the edit records so that an aggregating run,
// whose per-file emit actions each reported only what they saw, can report the
// whole repository's skips in one place.
struct RenameSkip {
  std::string File;  ///< the declaration's file, or "" when the skip is keyed
                     ///< by name alone (no single declaration to point at)
  unsigned Line = 0;
  unsigned Column = 0;
  std::string OldName;
  std::string NewName;  ///< the name it would have got, or "" when unknown
  std::string Reason;
};

// Emits one `file:line:col: skipped rename 'old' -> 'new': reason` line per
// distinct skip when \p Verbose, and a one-line count either way.  A
// declaration is seen once per translation unit that includes it, so the
// sites are deduplicated.  Emits nothing at all when \p Skips is empty.
void reportRenameSkips(const std::vector<RenameSkip>& Skips, bool Verbose,
                       llvm::raw_ostream& OS);

// A template-dependent member token resolved from an instantiation, pending
// cross-TU resolution in aggregation.  Agreeing records become an edit; a veto
// or a disagreement drops the token.
struct ResolutionRecord {
  std::string File;
  unsigned Offset = 0;
  unsigned Length = 0;
  std::string Old;
  std::string New;
  bool Veto = false;
  std::string OwnerFile;  ///< member this token resolves to, or ""
  unsigned OwnerOffset = 0;
  /// Which rename rule produced this.  One dependent token is looked at by
  /// every rule, and the rules disagree by construction: the rule that owns
  /// the member it resolves to records a name, and every other rule records a
  /// veto because it resolves to a member *that rule* is not renaming.  Merging
  /// is veto-absorbing, so pooling the rules under one (file, offset) key lets
  /// one rule's veto cancel another's rename -- the declaration is renamed and
  /// the dependent use is left spelling the old name.  The index keeps them
  /// apart, exactly as CrossTUState::DepResPerRule does in a direct run.
  unsigned Rule = 0;
};

// One invocation's output: ordinary edits plus the dependent-token sidecar.
/// A rename that was accepted only because another rename vacates its new
/// name (the field `count` to `count_` is what frees `count` for the method
/// `Count()`): the mover is named by its owner key and old spelling.  If any
/// report vetoes the mover -- or declines its name outright -- the dependent
/// keeps its name too, and \p Site says which rename that was.
struct RenameDependency {
  std::string OwnerFile;
  unsigned OwnerOffset = 0;
  std::string OnFile;
  unsigned OnOffset = 0;
  std::string OnName;
  RenameSkip Site;  ///< the dependent rename, for the skip report
};

struct EditReport {
  std::vector<EditRecord> Edits;
  std::vector<ResolutionRecord> Resolutions;
  std::vector<RenameVeto> Vetoes;
  /// Renames this invocation declined, for reporting only: aggregation never
  /// acts on them (a skip means no edit was emitted in the first place, and a
  /// veto that must drop *another* invocation's edit travels in Vetoes).
  std::vector<RenameSkip> Skips;
  std::vector<RenameDependency> Dependencies;

  auto empty() const -> bool {
    return Edits.empty() && Resolutions.empty() && Vetoes.empty() &&
           Skips.empty() && Dependencies.empty();
  }

  // Serializes as a JSON object {"edits":[...],"resolutions":[...],
  // "vetoes":[...],"skips":[...],"dependencies":[...]}.
  void emitJSON(llvm::raw_ostream& OS) const;
};

// Parses a JSON edit report produced by EditReport::emitJSON, APPENDING to
// \p Out.  Returns false on malformed input.
auto parseEditReport(llvm::StringRef Json, EditReport& Out) -> bool;

// Drops every edit and resolution whose owning declaration any report vetoed,
// resolves the union of dependent-token resolution records (agree -> edit; veto
// or disagreement -> dropped, promoting survivors to edits) and merges all
// edits per file (dedup byte-identical; distinct overlap -> a message appended
// to \p Conflicts and that file omitted).  Performs no file I/O — the merged,
// sorted, non-overlapping edits for each file are returned in \p MergedByFile.
// Returns true when every file merged without conflict.  When \p Declined is
// non-null, the renames dropped *here* rather than by an invocation -- a
// dependent token no report resolved, whose spelling is therefore declined
// everywhere -- are appended to it, so the caller can report them alongside
// the skips the invocations reported themselves.
auto mergeEditReports(
    const std::vector<EditReport>& Reports,
    std::map<std::string, std::vector<EditRecord>>& MergedByFile,
    std::vector<std::string>& Conflicts,
    std::vector<RenameSkip>* Declined = nullptr) -> bool;

// Like mergeEditReports, but also reads each touched file's original content
// via
// \p ReadFile and writes the rewritten content per file into \p Out.
auto aggregateEdits(
    const std::vector<EditReport>& Reports,
    const std::function<std::optional<std::string>(llvm::StringRef)>& ReadFile,
    std::map<std::string, std::string>& Out,
    std::vector<std::string>& Conflicts,
    std::vector<RenameSkip>* Declined = nullptr) -> bool;

// Appends every non-blank line of \p ListFile -- a newline-separated list of
// record files, as the Bazel rules and cpp_format.sh write it: a repository's
// worth of per-file records does not fit on a command line -- to \p Out.
// Returns false, after printing a diagnostic, when the file cannot be read.
auto appendRecordListFrom(llvm::StringRef ListFile,
                          std::vector<std::string>& Out) -> bool;

// Shared command-line aggregation entry point used by both the standalone
// `aggregate_edits` binary and `cpp_format --aggregate`.  Reads and parses
// every record file named in \p InputPaths, merges them, and then:
//   * Check   -> prints per-file edit counts and exits 1 if any edit would be
//                applied; reads no source files (a hermetic lint gate),
//   * Apply   -> writes the merged content back to each touched file in place,
//   * neither -> prints a git-apply-able unified diff to stdout.
// Record file keys resolve against \p Root — an absolute key is used verbatim,
// a relative key is joined with \p Root (empty \p Root => the current working
// directory).  Diagnostics go to stderr.  Returns the process exit code
// (0 success / clean, 1 edits-present or conflict, 2 unreadable input).
//
// Every mode reports the renames the run declined (see RenameSkip): a one-line
// count always, and one line per site when \p ReportSites.  A skipped rename is
// the one outcome the diff cannot show -- it is precisely a change that is not
// there -- so it is printed even when the aggregation itself is clean.
auto runEditAggregation(const std::vector<std::string>& InputPaths,
                        llvm::StringRef Root, bool Apply, bool Check,
                        bool ReportSites = false) -> int;

#endif  // CPP_FORMATTING_LINT_LIB_H_
