// The SQLite form of a cpp_index::Index, and the importer that writes it.
//
// The index is queried far more often than it is produced, and a monorepo's
// index does not fit comfortably in memory as protobuf messages, so the
// browser reads it from SQLite: one import per index file, then indexed point
// queries through the page cache.  Ids equal the proto's own indexes (files
// and symbols) so a row can be compared against `cpp_format --dump-index`;
// occurrences are numbered from 1 in the order the index lists them.
#ifndef CODE_BROWSER_INDEX_SCHEMA_H_
#define CODE_BROWSER_INDEX_SCHEMA_H_

#include <cstdint>
#include <string>

struct sqlite3;

namespace cpp_index {
class Index;
}

namespace code_browser {

// Bumped whenever the tables change; a database with another version is
// refused by the reader and re-imported by the server.
inline constexpr int kSchemaVersion = 1;

// What the database remembers about the index it came from.
struct ImportOptions {
  std::string source_path;
  uint64_t source_size = 0;
  int64_t source_mtime_ns = 0;  // nanoseconds since the epoch, 0 if unknown
};

struct ImportStats {
  uint32_t files = 0;
  uint32_t symbols = 0;
  uint64_t occurrences = 0;
  uint32_t unresolved = 0;
  uint64_t symbol_relations = 0;
  uint64_t occurrence_relations = 0;
  uint64_t trigrams = 0;
  uint32_t elapsed_ms = 0;
  std::string etag;
};

// Fills `size` and `mtime` from the file at `source_path`.  False if it
// cannot be stat'ed.
auto FillSourceInfo(ImportOptions& opts) -> bool;

// Creates the schema in an empty, open database and writes the whole index
// in one transaction.  Returns "" on success, else a message; the database
// is left in an unspecified state on failure.
auto ImportIndex(const cpp_index::Index& index, sqlite3* db,
                 const ImportOptions& opts, ImportStats* stats) -> std::string;

// Creates the database file at `db_path` (which must not exist), imports,
// closes.  A half-written file is removed on failure.
auto ImportIndexToFile(const cpp_index::Index& index,
                       const std::string& db_path, const ImportOptions& opts,
                       ImportStats* stats) -> std::string;

// The directory a path is listed under and its own name, the way the
// importer records them: "a/b/c.h" -> ("a/b", "c.h"), "README" -> ("",
// "README"), "/usr/include/x.h" -> ("/usr/include", "x.h").
auto SplitPath(std::string_view path)
    -> std::pair<std::string_view, std::string_view>;

}  // namespace code_browser

#endif  // CODE_BROWSER_INDEX_SCHEMA_H_
