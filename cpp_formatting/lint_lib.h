#ifndef CPP_FORMATTING_LINT_LIB_H_
#define CPP_FORMATTING_LINT_LIB_H_

#include <cstddef>
#include <map>
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

#endif  // CPP_FORMATTING_LINT_LIB_H_
