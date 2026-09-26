// Line coverage from an LCOV tracefile (`bazel coverage --combined_report=lcov`
// writes one to bazel-out/_coverage/_coverage_report.dat), keyed by the same
// paths as the index, for the overlay on the source.
//
// The parser is strict: a record it does not know, a count that is not a
// number (a negative one included -- that is a coverage bug, not data), or a
// record outside SF..end_of_record is an error naming the line, not a
// silently wrong overlay.  Function records are read past; LF/LH/BRF/BRH are
// recomputed from the DA/BRDA data rather than trusted, so that two records
// of one file (one per test binary, when a report was not merged) add up.
//
// Loaded once at startup and never changed, so every accessor is thread-safe.
#ifndef CODE_BROWSER_COVERAGE_H_
#define CODE_BROWSER_COVERAGE_H_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace code_browser {

struct CoverageTotals {
  uint32_t files = 0;
  uint32_t lines_found = 0;  // lines with a DA record
  uint32_t lines_hit = 0;    // ... executed at least once
  uint32_t branches_found = 0;
  uint32_t branches_hit = 0;  // taken at least once
  void Add(const CoverageTotals& other);
};

struct LineCoverage {
  uint32_t line = 0;  // 1-based
  uint64_t hits = 0;  // saturates rather than wraps
  uint32_t branches = 0;
  uint32_t branches_taken = 0;
};

struct FileCoverage {
  std::string path;                 // as the index spells it
  std::vector<LineCoverage> lines;  // ascending, one per instrumented line
  CoverageTotals totals;            // files = 1
};

struct CoverageOptions {
  // Absolute prefixes cut off an SF path: the root and the exec root, each
  // canonical and as given.  An SF path under none of them stays absolute.
  std::vector<std::filesystem::path> strip_prefixes;
};

// An SF path as the index spells it: `/proc/self/cwd/` and `./` dropped, a
// strip prefix removed, and, failing that, whatever follows a sandbox's
// `/execroot/<workspace>/`.
auto NormalizeCoveragePath(std::string_view sf, const CoverageOptions& options)
    -> std::string;

class Coverage {
 public:
  static auto Load(const std::filesystem::path& path,
                   const CoverageOptions& options, std::string* error)
      -> std::unique_ptr<Coverage>;
  // `name` is what errors call the text.  The ETag is derived from the text.
  static auto Parse(std::string_view text, std::string_view name,
                    const CoverageOptions& options, std::string* error)
      -> std::unique_ptr<Coverage>;

  // The file's record, or null.
  auto File(std::string_view path) const -> const FileCoverage*;
  // Everything under a directory, as the index names directories ("" is the
  // root, "/" the parent of absolute paths), or null when nothing is.
  auto Dir(std::string_view dir) const -> const CoverageTotals*;

  auto files() const -> const std::vector<FileCoverage>& { return files_; }
  auto totals() const -> const CoverageTotals& { return totals_; }
  auto path() const -> const std::string& { return path_; }
  auto etag() const -> const std::string& { return etag_; }
  // When the tracefile was written (0 for parsed text): a source file changed
  // after it may have its hit counts on the wrong lines.
  auto mtime_ns() const -> int64_t { return mtime_ns_; }
  auto collected_at() const -> std::string;  // RFC 3339 UTC, "" when unknown

 private:
  Coverage() = default;

  std::string path_;
  std::string etag_;
  int64_t mtime_ns_ = 0;
  std::vector<FileCoverage> files_;  // by path
  std::map<std::string, CoverageTotals, std::less<>> dirs_;
  CoverageTotals totals_;
};

}  // namespace code_browser

#endif  // CODE_BROWSER_COVERAGE_H_
