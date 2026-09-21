// Read-only queries over an imported index (see index_schema.h).
//
// One IndexDb serves every thread of the server: each query borrows a
// connection from a small pool (SQLite connections must not be shared between
// threads at once), runs a prepared statement, and returns plain structs.
// Nothing here knows about HTTP or JSON.
#ifndef CODE_BROWSER_INDEX_DB_H_
#define CODE_BROWSER_INDEX_DB_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "code_browser/index_schema.h"
#include "cpp_formatting/index.pb.h"

namespace code_browser {

struct FileRow {
  int32_t id = -1;
  std::string path;
  cpp_index::FileKind kind = cpp_index::FILE_KIND_UNSPECIFIED;
};

struct DirEntry {
  std::string name;
  std::string path;
  bool is_dir = false;
  int32_t file_id = -1;  // files only
  cpp_index::FileKind kind = cpp_index::FILE_KIND_UNSPECIFIED;
};

struct OccRow {
  int64_t id = 0;
  int32_t file = -1;
  uint32_t begin = 0;
  uint32_t end = 0;
  int32_t symbol = -1;
  uint32_t roles = 0;
  cpp_index::MacroContext macro = cpp_index::NOT_IN_MACRO;
};

struct SymbolRow {
  int32_t id = -1;
  std::string usr;
  std::string name;
  std::string qualified_name;
  cpp_index::SymbolKind kind = cpp_index::SYMBOL_KIND_UNSPECIFIED;
  cpp_index::SymbolSubKind sub_kind = cpp_index::SUB_KIND_NONE;
  cpp_index::Language language = cpp_index::LANGUAGE_UNSPECIFIED;
  uint32_t properties = 0;
  std::string type;
  bool has_canonical = false;
  int32_t canonical_file = -1;
  uint32_t canonical_begin = 0;
  uint32_t canonical_end = 0;
  int64_t definition_occ = 0;  // 0: none
};

struct RelationRow {
  cpp_index::RelationKind kind = cpp_index::RELATION_UNSPECIFIED;
  int32_t symbol = -1;  // the other end
};

struct UnresolvedRow {
  uint32_t begin = 0;
  uint32_t end = 0;
  std::string name;
};

// Filters and paging for a symbol's occurrences.
struct RefQuery {
  uint32_t role_mask = 0;     // 0: any role; else at least one of these bits
  uint32_t exclude_mask = 0;  // none of these bits
  std::optional<int32_t> file;
  uint32_t offset = 0;
  uint32_t limit = 500;
};

struct SearchOptions {
  size_t limit = 20;
  bool include_locals = false;  // parameters and LOCAL-property symbols
  std::optional<cpp_index::SymbolKind> kind;
};

struct SearchHit {
  SymbolRow symbol;
  float score = 0;
};

struct SearchResult {
  std::vector<SearchHit> hits;
  bool truncated = false;
};

struct DbStats {
  uint32_t files = 0;
  uint32_t symbols = 0;
  uint64_t occurrences = 0;
  uint32_t unresolved = 0;
  uint64_t db_bytes = 0;
  int64_t source_mtime_ns = 0;  // the index file's mtime at import
  std::string imported_at;
  std::string source_path;
  std::string producer;
};

class IndexDb {
 public:
  // Opens an existing database read-only.  Refuses another schema version.
  static auto Open(const std::string& path, std::string* error)
      -> std::unique_ptr<IndexDb>;
  // Imports `index` into a private in-memory database (tests, and one-shot
  // tools).
  static auto FromIndex(const cpp_index::Index& index,
                        const ImportOptions& opts, std::string* error)
      -> std::unique_ptr<IndexDb>;
  ~IndexDb();
  IndexDb(const IndexDb&) = delete;
  auto operator=(const IndexDb&) -> IndexDb& = delete;

  auto path() const -> const std::string& { return path_; }
  auto etag() const -> const std::string& { return etag_; }
  auto stats() const -> const DbStats& { return stats_; }

  // Files and directories.  A directory is named by its path ("" is the
  // root; "/" holds the absolute SYSTEM paths).
  auto FileIdOf(std::string_view path) const -> std::optional<int32_t>;
  auto File(int32_t id) const -> std::optional<FileRow>;
  // Every file with this last path component, by path.  What an `#include`
  // names is resolved against these: the index records no include edges.
  auto FilesNamed(std::string_view name) const -> std::vector<FileRow>;
  auto ListDir(std::string_view dir) const
      -> std::optional<std::vector<DirEntry>>;  // nullopt: no such directory

  // Occurrences.
  auto FileOccurrences(int32_t file) const -> std::vector<OccRow>;
  // Every occurrence whose [begin, end) contains `offset`, sorted.
  auto OccurrencesAt(int32_t file, uint32_t offset) const
      -> std::vector<OccRow>;
  auto Occurrence(int64_t id) const -> std::optional<OccRow>;
  auto OccurrenceRelations(int64_t occ) const -> std::vector<RelationRow>;
  auto Unresolved(int32_t file) const -> std::vector<UnresolvedRow>;

  // Symbols.
  auto Symbol(int32_t id) const -> std::optional<SymbolRow>;
  auto SymbolByUsr(std::string_view usr) const -> std::optional<SymbolRow>;
  // Sorted by (file, begin); `q.offset`/`q.limit` page it.
  auto SymbolOccurrences(int32_t symbol, const RefQuery& q) const
      -> std::vector<OccRow>;
  auto CountSymbolOccurrences(int32_t symbol, const RefQuery& q) const
      -> uint32_t;
  // How many files the symbol occurs in.
  auto CountSymbolFiles(int32_t symbol) const -> uint32_t;
  // The symbol's own relations (reverse=false: kind, target) or the
  // relations naming it (reverse=true: kind, source) -- members of a class,
  // classes derived from it, functions overriding it.
  auto Related(int32_t symbol, bool reverse) const -> std::vector<RelationRow>;
  // Name search: a prefix of the name, a substring of it (3+ characters), or
  // a `::`-qualified spelling matched against qualified_name.  Ranked: exact
  // name, then prefix, then substring; shorter qualified names first.
  auto Search(std::string_view query, const SearchOptions& opts) const
      -> SearchResult;

 private:
  struct Connection;
  class Lease;

  // Defined in the .cpp like the destructor: a defaulted constructor has to be
  // able to destroy the members it already built, and Connection is incomplete
  // here.
  IndexDb();
  auto Init(std::string* error) -> bool;
  auto Acquire() const -> Lease;

  std::string path_;  // the file, or the in-memory URI
  int open_flags_ = 0;
  std::string etag_;
  DbStats stats_;
  // FromIndex(): the connection that keeps the in-memory database alive.
  std::unique_ptr<Connection> anchor_;
  mutable std::mutex pool_mutex_;
  mutable std::vector<std::unique_ptr<Connection>> pool_;
};

}  // namespace code_browser

#endif  // CODE_BROWSER_INDEX_DB_H_
