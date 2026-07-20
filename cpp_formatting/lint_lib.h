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
struct EditRecord {
  std::string File;
  unsigned Offset = 0;
  unsigned Length = 0;
  std::string Old;  ///< original bytes (verification / debugging)
  std::string New;  ///< replacement bytes
};

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
};

// One invocation's output: ordinary edits plus the dependent-token sidecar.
struct EditReport {
  std::vector<EditRecord> Edits;
  std::vector<ResolutionRecord> Resolutions;

  auto empty() const -> bool { return Edits.empty() && Resolutions.empty(); }

  // Serializes as a JSON object {"edits":[...],"resolutions":[...]}.
  void emitJSON(llvm::raw_ostream& OS) const;
};

// Parses a JSON edit report produced by EditReport::emitJSON, APPENDING to
// \p Out.  Returns false on malformed input.
auto parseEditReport(llvm::StringRef Json, EditReport& Out) -> bool;

// Resolves the union of dependent-token resolution records (agree -> edit; veto
// or disagreement -> dropped, promoting survivors to edits) and merges all
// edits per file (dedup byte-identical; distinct overlap -> a message appended
// to \p Conflicts and that file omitted).  Performs no file I/O — the merged,
// sorted, non-overlapping edits for each file are returned in \p MergedByFile.
// Returns true when every file merged without conflict.
auto mergeEditReports(
    const std::vector<EditReport>& Reports,
    std::map<std::string, std::vector<EditRecord>>& MergedByFile,
    std::vector<std::string>& Conflicts) -> bool;

// Like mergeEditReports, but also reads each touched file's original content
// via
// \p ReadFile and writes the rewritten content per file into \p Out.
auto aggregateEdits(
    const std::vector<EditReport>& Reports,
    const std::function<std::optional<std::string>(llvm::StringRef)>& ReadFile,
    std::map<std::string, std::string>& Out,
    std::vector<std::string>& Conflicts) -> bool;

#endif  // CPP_FORMATTING_LINT_LIB_H_
