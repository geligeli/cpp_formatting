#include "code_browser/index_db.h"

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <compare>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <tuple>
#include <utility>

#include "code_browser/sqlite_util.h"

namespace code_browser {

namespace {

auto Lower(std::string_view s) -> std::string {
  std::string out(s);
  for (char& c : out)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

auto Trim(std::string_view s) -> std::string_view {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
    s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
    s.remove_suffix(1);
  return s;
}

// The two numbers of index.proto that the SQL below spells out.
static_assert(cpp_index::GENERATED_FROM == 100);
static_assert(cpp_index::GENERATES == 8192);

// The statements a connection keeps prepared, by role.
enum Query {
  kFileIdOf,
  kFile,
  kFilesNamed,
  kListDirs,
  kListFiles,
  kDirExists,
  kFileOccurrences,
  kOccurrencesAt,
  kOccurrence,
  kOccurrenceRelations,
  kUnresolved,
  kSymbol,
  kSymbolByUsr,
  kSymbolOccurrences,
  kCountSymbolOccurrences,
  kGeneratedOccurrences,
  kCountGeneratedOccurrences,
  kCountSymbolFiles,
  kRelatedForward,
  kRelatedReverse,
  kSearchPrefix,
  kSearchTrigram,
  kTrigramCount,
  kMeta,
  kNumQueries
};

constexpr const char* kSql[kNumQueries] = {
    /*kFileIdOf*/ "SELECT id FROM files WHERE path = ?",
    /*kFile*/ "SELECT path, kind FROM files WHERE id = ?",
    /*kFilesNamed*/
    "SELECT id, path, kind FROM files WHERE name = ? ORDER BY path",
    /*kListDirs*/ "SELECT path, name FROM dirs WHERE parent = ? ORDER BY name",
    /*kListFiles*/
    "SELECT id, path, name, kind FROM files WHERE dir = ? ORDER BY name",
    /*kDirExists*/ "SELECT 1 FROM dirs WHERE path = ?",
    /*kFileOccurrences*/
    "SELECT id, file, begin, end, symbol, roles, macro FROM occurrences "
    "WHERE file = ? ORDER BY begin, end, symbol, macro",
    /*kOccurrencesAt*/
    "SELECT id, file, begin, end, symbol, roles, macro FROM occurrences "
    "WHERE file = ? AND begin <= ? AND end > ? ORDER BY begin DESC LIMIT 64",
    /*kOccurrence*/
    "SELECT id, file, begin, end, symbol, roles, macro FROM occurrences "
    "WHERE id = ?",
    /*kOccurrenceRelations*/
    "SELECT kind, symbol FROM occurrence_relations WHERE occurrence = ? "
    "ORDER BY kind, symbol",
    /*kUnresolved*/
    "SELECT begin, end, name FROM unresolved WHERE file = ? ORDER BY begin",
    /*kSymbol*/
    "SELECT id, usr, name, qualified_name, kind, sub_kind, language, "
    "properties, type, canonical_file, canonical_begin, canonical_end, "
    "definition_occ FROM symbols WHERE id = ?",
    /*kSymbolByUsr*/
    "SELECT id, usr, name, qualified_name, kind, sub_kind, language, "
    "properties, type, canonical_file, canonical_begin, canonical_end, "
    "definition_occ FROM symbols WHERE usr = ?",
    /*kSymbolOccurrences*/
    "SELECT id, file, begin, end, symbol, roles, macro FROM occurrences "
    "WHERE symbol = ?1 AND (?2 = 0 OR (roles & ?2) != 0) AND (roles & ?3) = 0 "
    "AND (?4 < 0 OR file = ?4) ORDER BY file, begin, end LIMIT ?5 OFFSET ?6",
    /*kCountSymbolOccurrences*/
    "SELECT COUNT(*) FROM occurrences "
    "WHERE symbol = ?1 AND (?2 = 0 OR (roles & ?2) != 0) AND (roles & ?3) = 0 "
    "AND (?4 < 0 OR file = ?4)",
    // The same two over the symbol and everything generated from it
    // (RelationKind.GENERATED_FROM = 100), through symrel_by_target.
    /*kGeneratedOccurrences*/
    "SELECT id, file, begin, end, symbol, roles, macro FROM occurrences "
    "WHERE symbol IN (SELECT ?1 UNION SELECT symbol FROM symbol_relations "
    "WHERE target = ?1 AND kind = 100) "
    "AND (?2 = 0 OR (roles & ?2) != 0) AND (roles & ?3) = 0 "
    "AND (?4 < 0 OR file = ?4) ORDER BY file, begin, end LIMIT ?5 OFFSET ?6",
    /*kCountGeneratedOccurrences*/
    "SELECT COUNT(*) FROM occurrences "
    "WHERE symbol IN (SELECT ?1 UNION SELECT symbol FROM symbol_relations "
    "WHERE target = ?1 AND kind = 100) "
    "AND (?2 = 0 OR (roles & ?2) != 0) AND (roles & ?3) = 0 "
    "AND (?4 < 0 OR file = ?4)",
    /*kCountSymbolFiles*/
    // Role.GENERATES = 8192: an anchor is not an occurrence of the symbol.
    "SELECT COUNT(DISTINCT file) FROM occurrences WHERE symbol = ? "
    "AND (roles & 8192) = 0",
    /*kRelatedForward*/
    "SELECT kind, target FROM symbol_relations WHERE symbol = ? "
    "ORDER BY kind, target",
    /*kRelatedReverse*/
    "SELECT kind, symbol FROM symbol_relations WHERE target = ? "
    "ORDER BY kind, symbol",
    /*kSearchPrefix*/
    "SELECT id, usr, name, qualified_name, kind, sub_kind, language, "
    "properties, type, canonical_file, canonical_begin, canonical_end, "
    "definition_occ FROM symbols WHERE name_lower >= ?1 AND name_lower < ?2 "
    "LIMIT ?3",
    /*kSearchTrigram*/
    "SELECT s.id, s.usr, s.name, s.qualified_name, s.kind, s.sub_kind, "
    "s.language, s.properties, s.type, s.canonical_file, s.canonical_begin, "
    "s.canonical_end, s.definition_occ FROM symbol_trigrams t "
    "JOIN symbols s ON s.id = t.symbol "
    "WHERE t.tri = ?1 AND instr(s.name_lower, ?2) > 0 LIMIT ?3",
    /*kTrigramCount*/ "SELECT COUNT(*) FROM symbol_trigrams WHERE tri = ?",
    /*kMeta*/ "SELECT value FROM meta WHERE key = ?",
};

auto ReadOcc(const Statement& s) -> OccRow {
  OccRow o;
  o.id = s.ColumnInt(0);
  o.file = static_cast<int32_t>(s.ColumnInt(1));
  o.begin = static_cast<uint32_t>(s.ColumnInt(2));
  o.end = static_cast<uint32_t>(s.ColumnInt(3));
  o.symbol = static_cast<int32_t>(s.ColumnInt(4));
  o.roles = static_cast<uint32_t>(s.ColumnInt(5));
  o.macro = static_cast<cpp_index::MacroContext>(s.ColumnInt(6));
  return o;
}

auto ReadSymbol(const Statement& s) -> SymbolRow {
  SymbolRow r;
  r.id = static_cast<int32_t>(s.ColumnInt(0));
  r.usr = std::string(s.ColumnText(1));
  r.name = std::string(s.ColumnText(2));
  r.qualified_name = std::string(s.ColumnText(3));
  r.kind = static_cast<cpp_index::SymbolKind>(s.ColumnInt(4));
  r.sub_kind = static_cast<cpp_index::SymbolSubKind>(s.ColumnInt(5));
  r.language = static_cast<cpp_index::Language>(s.ColumnInt(6));
  r.properties = static_cast<uint32_t>(s.ColumnInt(7));
  r.type = std::string(s.ColumnText(8));
  r.has_canonical = !s.ColumnIsNull(9);
  if (r.has_canonical) {
    r.canonical_file = static_cast<int32_t>(s.ColumnInt(9));
    r.canonical_begin = static_cast<uint32_t>(s.ColumnInt(10));
    r.canonical_end = static_cast<uint32_t>(s.ColumnInt(11));
  }
  r.definition_occ = s.ColumnIsNull(12) ? 0 : s.ColumnInt(12);
  return r;
}

std::atomic<unsigned> memory_db_counter{0};

}  // namespace

// A connection plus its prepared statements.  Prepared lazily, once.
struct IndexDb::Connection {
  Db db;
  Statement statements[kNumQueries];
  bool prepared[kNumQueries] = {};

  auto Stmt(Query q) -> Statement& {
    if (!prepared[q]) {
      statements[q].Prepare(db.handle(), kSql[q]);
      prepared[q] = true;
    }
    statements[q].Reset();
    return statements[q];
  }
};

// Borrows a connection for the duration of one query.
class IndexDb::Lease {
 public:
  Lease(const IndexDb& owner, std::unique_ptr<Connection> conn)
      : owner_(owner), conn_(std::move(conn)) {}
  ~Lease() {
    if (!conn_) return;
    std::lock_guard<std::mutex> lock(owner_.pool_mutex_);
    owner_.pool_.push_back(std::move(conn_));
  }
  Lease(const Lease&) = delete;
  auto operator=(const Lease&) -> Lease& = delete;
  auto ok() const -> bool { return conn_ != nullptr; }
  auto operator->() -> Connection* { return conn_.get(); }

 private:
  const IndexDb& owner_;
  std::unique_ptr<Connection> conn_;
};

auto IndexDb::Acquire() const -> Lease {
  {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    if (!pool_.empty()) {
      std::unique_ptr<Connection> c = std::move(pool_.back());
      pool_.pop_back();
      return Lease(*this, std::move(c));
    }
  }
  auto c = std::make_unique<Connection>();
  if (!c->db.Open(path_, open_flags_).empty()) return Lease(*this, nullptr);
  // Every connection reads through the page cache and never writes.
  c->db.Exec("PRAGMA query_only = 1");
  c->db.Exec("PRAGMA mmap_size = 1073741824");
  return Lease(*this, std::move(c));
}

IndexDb::IndexDb() = default;
IndexDb::~IndexDb() = default;

auto IndexDb::Open(const std::string& path, std::string* error)
    -> std::unique_ptr<IndexDb> {
  std::unique_ptr<IndexDb> db(new IndexDb);
  db->path_ = path;
  db->open_flags_ = SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX;
  if (!db->Init(error)) return nullptr;
  std::error_code ec;
  db->stats_.db_bytes = std::filesystem::file_size(path, ec);
  if (ec) db->stats_.db_bytes = 0;
  return db;
}

auto IndexDb::FromIndex(const cpp_index::Index& index,
                        const ImportOptions& opts, std::string* error)
    -> std::unique_ptr<IndexDb> {
  std::unique_ptr<IndexDb> db(new IndexDb);
  // A named in-memory database with a shared cache, so that every pooled
  // connection sees the one the anchor connection imported into.
  db->path_ = "file:code_browser_mem" +
              std::to_string(memory_db_counter.fetch_add(1)) +
              "?mode=memory&cache=shared";
  db->open_flags_ = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                    SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX;
  auto anchor = std::make_unique<Connection>();
  if (std::string e = anchor->db.Open(db->path_, db->open_flags_); !e.empty()) {
    if (error) *error = e;
    return nullptr;
  }
  if (std::string e = ImportIndex(index, anchor->db.handle(), opts, nullptr);
      !e.empty()) {
    if (error) *error = e;
    return nullptr;
  }
  db->anchor_ = std::move(anchor);
  db->open_flags_ =
      SQLITE_OPEN_READONLY | SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX;
  if (!db->Init(error)) return nullptr;
  return db;
}

auto IndexDb::Init(std::string* error) -> bool {
  Lease c = Acquire();
  if (!c.ok()) {
    if (error) *error = "cannot open " + path_;
    return false;
  }
  const auto meta = [&](std::string_view key) -> std::optional<std::string> {
    Statement& s = c->Stmt(kMeta);
    s.BindText(1, key);
    if (!s.Step()) return std::nullopt;
    return std::string(s.ColumnText(0));
  };
  const std::optional<std::string> version = meta("schema_version");
  if (!version) {
    if (error) *error = path_ + ": not a code_browser index database";
    return false;
  }
  if (*version != std::to_string(kSchemaVersion)) {
    if (error)
      *error = path_ + ": schema version " + *version + ", expected " +
               std::to_string(kSchemaVersion);
    return false;
  }
  etag_ = meta("etag").value_or("");
  const auto number = [&](std::string_view key) -> uint64_t {
    const std::optional<std::string> v = meta(key);
    return v ? std::strtoull(v->c_str(), nullptr, 10) : 0;
  };
  stats_.files = static_cast<uint32_t>(number("files"));
  stats_.symbols = static_cast<uint32_t>(number("symbols"));
  stats_.occurrences = number("occurrences");
  stats_.unresolved = static_cast<uint32_t>(number("unresolved"));
  if (const std::optional<std::string> m = meta("source_mtime_ns"))
    stats_.source_mtime_ns = std::strtoll(m->c_str(), nullptr, 10);
  stats_.imported_at = meta("imported_at").value_or("");
  stats_.source_path = meta("source_path").value_or("");
  stats_.producer = meta("producer").value_or("");
  return true;
}

auto IndexDb::FileIdOf(std::string_view path) const -> std::optional<int32_t> {
  Lease c = Acquire();
  if (!c.ok()) return std::nullopt;
  Statement& s = c->Stmt(kFileIdOf);
  s.BindText(1, path);
  if (!s.Step()) return std::nullopt;
  return static_cast<int32_t>(s.ColumnInt(0));
}

auto IndexDb::File(int32_t id) const -> std::optional<FileRow> {
  Lease c = Acquire();
  if (!c.ok()) return std::nullopt;
  Statement& s = c->Stmt(kFile);
  s.BindInt(1, id);
  if (!s.Step()) return std::nullopt;
  FileRow f;
  f.id = id;
  f.path = std::string(s.ColumnText(0));
  f.kind = static_cast<cpp_index::FileKind>(s.ColumnInt(1));
  return f;
}

auto IndexDb::FilesNamed(std::string_view name) const -> std::vector<FileRow> {
  std::vector<FileRow> out;
  Lease c = Acquire();
  if (!c.ok()) return out;
  Statement& s = c->Stmt(kFilesNamed);
  s.BindText(1, name);
  while (s.Step()) {
    FileRow f;
    f.id = static_cast<int32_t>(s.ColumnInt(0));
    f.path = std::string(s.ColumnText(1));
    f.kind = static_cast<cpp_index::FileKind>(s.ColumnInt(2));
    out.push_back(std::move(f));
  }
  return out;
}

auto IndexDb::ListDir(std::string_view dir) const
    -> std::optional<std::vector<DirEntry>> {
  Lease c = Acquire();
  if (!c.ok()) return std::nullopt;
  if (!dir.empty()) {
    Statement& exists = c->Stmt(kDirExists);
    exists.BindText(1, dir);
    if (!exists.Step()) return std::nullopt;
  }
  std::vector<DirEntry> out;
  {
    Statement& s = c->Stmt(kListDirs);
    s.BindText(1, dir);
    while (s.Step()) {
      DirEntry e;
      e.path = std::string(s.ColumnText(0));
      e.name = std::string(s.ColumnText(1));
      e.is_dir = true;
      out.push_back(std::move(e));
    }
  }
  {
    Statement& s = c->Stmt(kListFiles);
    s.BindText(1, dir);
    while (s.Step()) {
      DirEntry e;
      e.file_id = static_cast<int32_t>(s.ColumnInt(0));
      e.path = std::string(s.ColumnText(1));
      e.name = std::string(s.ColumnText(2));
      e.kind = static_cast<cpp_index::FileKind>(s.ColumnInt(3));
      out.push_back(std::move(e));
    }
  }
  return out;
}

auto IndexDb::FileOccurrences(int32_t file) const -> std::vector<OccRow> {
  std::vector<OccRow> out;
  Lease c = Acquire();
  if (!c.ok()) return out;
  Statement& s = c->Stmt(kFileOccurrences);
  s.BindInt(1, file);
  while (s.Step()) out.push_back(ReadOcc(s));
  return out;
}

auto IndexDb::OccurrencesAt(int32_t file, uint32_t offset) const
    -> std::vector<OccRow> {
  std::vector<OccRow> out;
  Lease c = Acquire();
  if (!c.ok()) return out;
  Statement& s = c->Stmt(kOccurrencesAt);
  s.BindInt(1, file);
  s.BindInt(2, offset);
  s.BindInt(3, offset);
  while (s.Step()) out.push_back(ReadOcc(s));
  std::sort(out.begin(), out.end(), [](const OccRow& a, const OccRow& b) {
    return std::tie(a.begin, a.end, a.symbol, a.macro) <
           std::tie(b.begin, b.end, b.symbol, b.macro);
  });
  return out;
}

auto IndexDb::Occurrence(int64_t id) const -> std::optional<OccRow> {
  Lease c = Acquire();
  if (!c.ok()) return std::nullopt;
  Statement& s = c->Stmt(kOccurrence);
  s.BindInt(1, id);
  if (!s.Step()) return std::nullopt;
  return ReadOcc(s);
}

auto IndexDb::OccurrenceRelations(int64_t occ) const
    -> std::vector<RelationRow> {
  std::vector<RelationRow> out;
  Lease c = Acquire();
  if (!c.ok()) return out;
  Statement& s = c->Stmt(kOccurrenceRelations);
  s.BindInt(1, occ);
  while (s.Step())
    out.push_back({static_cast<cpp_index::RelationKind>(s.ColumnInt(0)),
                   static_cast<int32_t>(s.ColumnInt(1))});
  return out;
}

auto IndexDb::Unresolved(int32_t file) const -> std::vector<UnresolvedRow> {
  std::vector<UnresolvedRow> out;
  Lease c = Acquire();
  if (!c.ok()) return out;
  Statement& s = c->Stmt(kUnresolved);
  s.BindInt(1, file);
  while (s.Step())
    out.push_back({static_cast<uint32_t>(s.ColumnInt(0)),
                   static_cast<uint32_t>(s.ColumnInt(1)),
                   std::string(s.ColumnText(2))});
  return out;
}

auto IndexDb::Symbol(int32_t id) const -> std::optional<SymbolRow> {
  Lease c = Acquire();
  if (!c.ok()) return std::nullopt;
  Statement& s = c->Stmt(kSymbol);
  s.BindInt(1, id);
  if (!s.Step()) return std::nullopt;
  return ReadSymbol(s);
}

auto IndexDb::SymbolByUsr(std::string_view usr) const
    -> std::optional<SymbolRow> {
  Lease c = Acquire();
  if (!c.ok()) return std::nullopt;
  Statement& s = c->Stmt(kSymbolByUsr);
  s.BindText(1, usr);
  if (!s.Step()) return std::nullopt;
  return ReadSymbol(s);
}

auto IndexDb::SymbolOccurrences(int32_t symbol, const RefQuery& q) const
    -> std::vector<OccRow> {
  std::vector<OccRow> out;
  Lease c = Acquire();
  if (!c.ok()) return out;
  Statement& s =
      c->Stmt(q.with_generated ? kGeneratedOccurrences : kSymbolOccurrences);
  s.BindInt(1, symbol);
  s.BindInt(2, q.role_mask);
  s.BindInt(3, q.exclude_mask);
  s.BindInt(4, q.file ? *q.file : -1);
  s.BindInt(5, q.limit);
  s.BindInt(6, q.offset);
  while (s.Step()) out.push_back(ReadOcc(s));
  return out;
}

auto IndexDb::CountSymbolOccurrences(int32_t symbol, const RefQuery& q) const
    -> uint32_t {
  Lease c = Acquire();
  if (!c.ok()) return 0;
  Statement& s = c->Stmt(q.with_generated ? kCountGeneratedOccurrences
                                          : kCountSymbolOccurrences);
  s.BindInt(1, symbol);
  s.BindInt(2, q.role_mask);
  s.BindInt(3, q.exclude_mask);
  s.BindInt(4, q.file ? *q.file : -1);
  if (!s.Step()) return 0;
  return static_cast<uint32_t>(s.ColumnInt(0));
}

auto IndexDb::CountSymbolFiles(int32_t symbol) const -> uint32_t {
  Lease c = Acquire();
  if (!c.ok()) return 0;
  Statement& s = c->Stmt(kCountSymbolFiles);
  s.BindInt(1, symbol);
  if (!s.Step()) return 0;
  return static_cast<uint32_t>(s.ColumnInt(0));
}

auto IndexDb::Related(int32_t symbol, bool reverse) const
    -> std::vector<RelationRow> {
  std::vector<RelationRow> out;
  Lease c = Acquire();
  if (!c.ok()) return out;
  Statement& s = c->Stmt(reverse ? kRelatedReverse : kRelatedForward);
  s.BindInt(1, symbol);
  while (s.Step())
    out.push_back({static_cast<cpp_index::RelationKind>(s.ColumnInt(0)),
                   static_cast<int32_t>(s.ColumnInt(1))});
  return out;
}

auto IndexDb::Search(std::string_view query, const SearchOptions& opts) const
    -> SearchResult {
  SearchResult result;
  const std::string q = Lower(Trim(query));
  if (q.empty() || opts.limit == 0) return result;
  // `ns::Name`, or `pkg.Message` as a .proto spells it, matches qualified
  // names; the last component drives the candidate search.
  std::string name_part = q;
  std::string qualified_part;
  const size_t colons = q.rfind("::");
  const size_t dot = q.rfind('.');
  if (colons != std::string::npos || dot != std::string::npos) {
    const bool by_dot = colons == std::string::npos ||
                        (dot != std::string::npos && dot > colons);
    qualified_part = q;
    name_part = q.substr(by_dot ? dot + 1 : colons + 2);
    if (name_part.empty()) return result;
  }
  // More candidates than hits: the ranking below decides which survive.
  const size_t candidate_limit = std::max<size_t>(opts.limit * 50, 2000);

  Lease c = Acquire();
  if (!c.ok()) return result;
  std::vector<SymbolRow> candidates;
  if (name_part.size() >= 3) {
    // The rarest trigram of the name narrows the join the most.
    std::string rarest;
    int64_t rarest_count = -1;
    for (size_t k = 0; k + 3 <= name_part.size(); ++k) {
      const std::string tri = name_part.substr(k, 3);
      Statement& s = c->Stmt(kTrigramCount);
      s.BindText(1, tri);
      const int64_t n = s.Step() ? s.ColumnInt(0) : 0;
      if (rarest_count < 0 || n < rarest_count) {
        rarest_count = n;
        rarest = tri;
      }
    }
    if (rarest_count <= 0) return result;
    Statement& s = c->Stmt(kSearchTrigram);
    s.BindText(1, rarest);
    s.BindText(2, name_part);
    s.BindInt(3, static_cast<int64_t>(candidate_limit + 1));
    while (s.Step()) candidates.push_back(ReadSymbol(s));
  } else {
    Statement& s = c->Stmt(kSearchPrefix);
    s.BindText(1, name_part);
    s.BindText(2, name_part + '\x7f');
    s.BindInt(3, static_cast<int64_t>(candidate_limit + 1));
    while (s.Step()) candidates.push_back(ReadSymbol(s));
  }
  const bool over_candidates = candidates.size() > candidate_limit;
  if (over_candidates) candidates.pop_back();

  std::vector<SearchHit> hits;
  for (SymbolRow& sym : candidates) {
    if (opts.kind && sym.kind != *opts.kind) continue;
    if (!opts.include_locals && (sym.kind == cpp_index::PARAMETER ||
                                 (sym.properties & cpp_index::LOCAL) != 0))
      continue;
    const std::string name = Lower(sym.name);
    float score;
    if (name == name_part)
      score = 3.0f;
    else if (name.compare(0, name_part.size(), name_part) == 0)
      score = 2.0f;
    else
      score = 1.0f;
    if (!qualified_part.empty()) {
      const std::string qualified = Lower(sym.qualified_name);
      if (qualified.find(qualified_part) == std::string::npos) continue;
      if (qualified == qualified_part) score += 1.0f;
    }
    score -= 0.001f * static_cast<float>(sym.qualified_name.size());
    hits.push_back({std::move(sym), score});
  }
  std::stable_sort(hits.begin(), hits.end(),
                   [](const SearchHit& a, const SearchHit& b) {
                     if (a.score != b.score) return a.score > b.score;
                     return a.symbol.qualified_name < b.symbol.qualified_name;
                   });
  result.truncated = over_candidates || hits.size() > opts.limit;
  if (hits.size() > opts.limit) hits.resize(opts.limit);
  result.hits = std::move(hits);
  return result;
}

}  // namespace code_browser
