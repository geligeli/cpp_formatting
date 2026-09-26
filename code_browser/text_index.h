// Full-text search over the checkout: every text file under the root,
// concatenated into one corpus, with a suffix array over it, in one file on
// disk that the server maps read-only.  A query is an exact byte string (an
// n-gram of any length); its matches are one contiguous range of the suffix
// array, found by binary search.
//
// What is walked: every regular file under the root, except
//   - symlinks (so `bazel-out`, `bazel-bin`, `bazel-<workspace>` and anything
//     reached through them -- external/ included -- are never followed),
//   - `bazel-*`, `bazel-out` and `external` at the root even when they are
//     real directories,
//   - dot-files and dot-directories (.git, .cache, ...),
//   - files larger than `max_file_bytes`,
//   - files the caller excludes (the index, the database, this file),
//   - and, once read, binary files (any NUL byte).
//
// The file (<index>.fts), little-endian, every section 8-byte aligned:
//   Header        magic, version, section offsets
//   names         the manifest's paths, back to back
//   manifest      one ManifestRow per walked file, by path; a binary file has
//                 a row too (flagged), so that a restart can tell nothing
//                 changed from a stat of each file alone
//   corpus        each indexed file's bytes followed by one NUL.  No file
//                 holds a NUL, so no match crosses into the next file.
//   suffix array  int32_t per corpus byte that is not a separator, sorted by
//                 the ASCII-lowercased corpus.  Case-insensitive matches are
//                 one range; case-sensitive ones are that range filtered
//                 against the original bytes.  Folding ASCII only keeps
//                 every offset where it was.
#ifndef CODE_BROWSER_TEXT_INDEX_H_
#define CODE_BROWSER_TEXT_INDEX_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace code_browser {

struct TextCandidate {
  std::string path;  // relative to the root, '/'-separated
  uint64_t size = 0;
  int64_t mtime_ns = 0;
  friend auto operator==(const TextCandidate&, const TextCandidate&)
      -> bool = default;
};

struct TextIndexOptions {
  uint64_t max_file_bytes = 4 << 20;
  // Absolute paths never indexed, nor anything whose path starts with one
  // (`index.pb.fts` also keeps out `index.pb.fts.tmp`).
  std::vector<std::filesystem::path> exclude;
};

// The files to index, by path.  Stats each one; reads nothing.
auto ListTextCandidates(const std::filesystem::path& root,
                        const TextIndexOptions& options, std::string* error)
    -> std::optional<std::vector<TextCandidate>>;

struct TextBuildStats {
  bool rebuilt = false;
  uint64_t files = 0;    // indexed
  uint64_t skipped = 0;  // binary or unreadable
  uint64_t corpus_bytes = 0;
  int64_t elapsed_ms = 0;
};

// Reads the candidates and writes the index to `out` (through a temporary
// file and a rename).
auto BuildTextIndex(const std::filesystem::path& root,
                    const std::vector<TextCandidate>& candidates,
                    const std::filesystem::path& out, TextBuildStats* stats,
                    std::string* error) -> bool;

struct TextSearchOptions {
  bool case_sensitive = true;
  size_t offset = 0;   // in lines
  size_t limit = 200;  // lines
};

struct TextSpan {
  uint32_t begin = 0;  // bytes into TextLineHit::text
  uint32_t end = 0;
};

// One line with at least one match.
struct TextLineHit {
  uint32_t line = 0;    // 1-based
  uint32_t column = 0;  // 1-based byte column of the first match
  // The line, clipped around its first match when long; `text_offset` is the
  // byte column (0-based) it starts at.  Bytes that are not UTF-8 are '?'.
  std::string text;
  uint32_t text_offset = 0;
  bool clipped_end = false;
  std::vector<TextSpan> spans;  // the matches, merged where they overlap
};

struct TextFileHits {
  std::string path;
  std::vector<TextLineHit> lines;
};

struct TextSearchResult {
  uint64_t total_matches = 0;
  uint64_t total_lines = 0;
  uint64_t files_matched = 0;
  // More than kMaxTextMatches matched: the counts and lines cover a subset.
  bool truncated = false;
  size_t next_offset = 0;  // 0: this was the last page
  std::vector<TextFileHits> files;
};

// How many matches a search looks at.
inline constexpr size_t kMaxTextMatches = 100000;
inline constexpr size_t kMaxTextQuery = 1024;

// Why `query` cannot be searched for, or nullopt.  A query is one line.
auto InvalidTextQuery(std::string_view query) -> std::optional<std::string>;

class TextIndex {
 public:
  // Maps the file and validates its sections and manifest -- not the suffix
  // array, which is only read as searched (and bounds-checked there).
  static auto Open(const std::filesystem::path& path, std::string* error)
      -> std::unique_ptr<TextIndex>;
  ~TextIndex();
  TextIndex(const TextIndex&) = delete;
  auto operator=(const TextIndex&) -> TextIndex& = delete;

  // Whether the index was built from exactly these files, as they are now.
  auto IsCurrent(const std::vector<TextCandidate>& candidates) const -> bool;

  // `query` must pass InvalidTextQuery().  Thread-safe: nothing changes after
  // Open().
  auto Search(std::string_view query, const TextSearchOptions& options) const
      -> TextSearchResult;

  auto path() const -> const std::string& { return path_; }
  auto etag() const -> const std::string& { return etag_; }
  auto files() const -> uint64_t { return files_.size(); }
  auto skipped() const -> uint64_t { return manifest_size_ - files_.size(); }
  auto corpus_bytes() const -> uint64_t { return corpus_.size(); }
  auto file_bytes() const -> uint64_t { return map_size_; }
  auto built_at() const -> std::string;  // RFC 3339 UTC

 private:
  TextIndex() = default;

  struct IndexedFile {
    std::string_view path;
    uint64_t begin = 0;  // in the corpus
    uint64_t size = 0;
  };

  std::string path_;
  std::string etag_;
  const char* map_ = nullptr;
  size_t map_size_ = 0;
  uint64_t manifest_size_ = 0;
  int64_t built_at_ns_ = 0;
  const void* manifest_ = nullptr;
  std::string_view names_;
  std::string_view corpus_;
  const int32_t* sa_ = nullptr;
  size_t sa_size_ = 0;
  std::vector<IndexedFile> files_;  // by begin, which is by path
};

// The index at `path` if it matches the checkout, else a fresh one built
// there.  `stats` says which (rebuilt) and, if built, what it took.
auto OpenOrBuildTextIndex(const std::filesystem::path& root,
                          const std::filesystem::path& path,
                          const TextIndexOptions& options,
                          TextBuildStats* stats, std::string* error)
    -> std::unique_ptr<TextIndex>;

}  // namespace code_browser

#endif  // CODE_BROWSER_TEXT_INDEX_H_
