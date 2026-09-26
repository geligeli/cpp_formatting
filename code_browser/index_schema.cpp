#include "code_browser/index_schema.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <initializer_list>
#include <system_error>
#include <unordered_set>
#include <vector>

#include "code_browser/sqlite_util.h"
#include "cpp_formatting/index.pb.h"

namespace code_browser {

namespace {

constexpr const char kTablesSql[] = R"sql(
CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID;
CREATE TABLE files(
  id INTEGER PRIMARY KEY,
  path TEXT NOT NULL UNIQUE,
  kind INTEGER NOT NULL,
  dir TEXT NOT NULL,
  name TEXT NOT NULL,
  test INTEGER NOT NULL);  -- 1: a testonly target's (index.proto, File)
CREATE TABLE dirs(
  path TEXT PRIMARY KEY,
  parent TEXT NOT NULL,
  name TEXT NOT NULL) WITHOUT ROWID;
CREATE TABLE symbols(
  id INTEGER PRIMARY KEY,
  usr TEXT NOT NULL UNIQUE,
  name TEXT NOT NULL,
  name_lower TEXT NOT NULL,
  qualified_name TEXT NOT NULL,
  kind INTEGER NOT NULL,
  sub_kind INTEGER NOT NULL,
  language INTEGER NOT NULL,
  properties INTEGER NOT NULL,
  type TEXT NOT NULL,
  canonical_file INTEGER,
  canonical_begin INTEGER,
  canonical_end INTEGER,
  definition_occ INTEGER);
CREATE TABLE occurrences(
  id INTEGER PRIMARY KEY,
  file INTEGER NOT NULL,
  begin INTEGER NOT NULL,
  end INTEGER NOT NULL,
  symbol INTEGER NOT NULL,
  roles INTEGER NOT NULL,
  macro INTEGER NOT NULL);
CREATE TABLE occurrence_relations(
  occurrence INTEGER NOT NULL,
  kind INTEGER NOT NULL,
  symbol INTEGER NOT NULL);
CREATE TABLE symbol_relations(
  symbol INTEGER NOT NULL,
  kind INTEGER NOT NULL,
  target INTEGER NOT NULL);
CREATE TABLE unresolved(
  file INTEGER NOT NULL,
  begin INTEGER NOT NULL,
  end INTEGER NOT NULL,
  name TEXT NOT NULL);
CREATE TABLE symbol_trigrams(
  tri TEXT NOT NULL,
  symbol INTEGER NOT NULL,
  PRIMARY KEY(tri, symbol)) WITHOUT ROWID;
)sql";

// Created after the bulk inserts: building an index over sorted, finished
// data is several times faster than maintaining it row by row.
constexpr const char kIndexesSql[] = R"sql(
CREATE INDEX files_by_dir ON files(dir, name);
CREATE INDEX dirs_by_parent ON dirs(parent, name);
CREATE INDEX symbols_by_name ON symbols(name_lower);
CREATE INDEX occ_by_file ON occurrences(file, begin, end);
CREATE INDEX occ_by_symbol ON occurrences(symbol, file, begin);
CREATE INDEX occrel_by_occ ON occurrence_relations(occurrence);
CREATE INDEX symrel_by_symbol ON symbol_relations(symbol);
CREATE INDEX symrel_by_target ON symbol_relations(target, kind);
CREATE INDEX unresolved_by_file ON unresolved(file, begin);
)sql";

auto Fnv1a(std::string_view s, uint64_t h = 1469598103934665603ULL)
    -> uint64_t {
  for (const unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  return h;
}

auto Hex(uint64_t v) -> std::string {
  char buf[17];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
  return buf;
}

auto NowNs() -> int64_t {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

auto Rfc3339(int64_t ns) -> std::string {
  const std::time_t secs = static_cast<std::time_t>(ns / 1000000000LL);
  std::tm tm{};
  gmtime_r(&secs, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

auto Lower(std::string_view s) -> std::string {
  std::string out(s);
  for (char& c : out)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

}  // namespace

auto SplitPath(std::string_view path)
    -> std::pair<std::string_view, std::string_view> {
  const size_t slash = path.rfind('/');
  if (slash == std::string_view::npos) return {"", path};
  if (slash == 0) return {"/", path.substr(1)};
  return {path.substr(0, slash), path.substr(slash + 1)};
}

auto FillSourceInfo(ImportOptions& opts) -> bool {
  std::error_code ec;
  const auto size = std::filesystem::file_size(opts.source_path, ec);
  if (ec) return false;
  const auto mtime = std::filesystem::last_write_time(opts.source_path, ec);
  if (ec) return false;
  opts.source_size = size;
  // file_clock's epoch is not the Unix epoch; go through the system clock.
  const auto sys = std::chrono::file_clock::to_sys(mtime);
  opts.source_mtime_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             sys.time_since_epoch())
                             .count();
  return true;
}

auto ImportIndex(const cpp_index::Index& index, sqlite3* raw,
                 const ImportOptions& opts, ImportStats* stats) -> std::string {
  const auto started = std::chrono::steady_clock::now();
  // The caller owns the connection; everything here goes through the raw
  // handle.
  const auto exec = [&](std::string_view sql) -> std::string {
    char* message = nullptr;
    const std::string owned(sql);
    const int rc = sqlite3_exec(raw, owned.c_str(), nullptr, nullptr, &message);
    std::string out;
    if (rc != SQLITE_OK) out = message ? message : sqlite3_errstr(rc);
    sqlite3_free(message);
    return out;
  };
  for (const char* pragma :
       {"PRAGMA journal_mode=OFF", "PRAGMA synchronous=OFF",
        "PRAGMA temp_store=MEMORY", "PRAGMA cache_size=-262144",
        "PRAGMA locking_mode=EXCLUSIVE"}) {
    if (std::string e = exec(pragma); !e.empty()) return e;
  }
  if (std::string e = exec(kTablesSql); !e.empty()) return e;
  if (std::string e =
          exec("PRAGMA user_version = " + std::to_string(kSchemaVersion));
      !e.empty())
    return e;
  if (std::string e = exec("BEGIN"); !e.empty()) return e;

  std::string error;
  const auto prepare = [&](std::string_view sql) {
    Statement s;
    const std::string e = s.Prepare(raw, sql);
    if (!e.empty() && error.empty()) error = e;
    return s;
  };
  const auto run = [&](Statement& s) {
    s.Step();
    if (!s.error().empty() && error.empty()) error = s.error();
    s.Reset();
  };

  // 1. Occurrence ids and each symbol's definition.  Numbered from 1 in index
  //    order; `definition_occ` is the first DEFINITION, else the first
  //    DECLARATION, of the symbol.
  const int32_t num_symbols = index.symbols_size();
  std::vector<int64_t> definition(num_symbols, 0), declaration(num_symbols, 0);
  {
    int64_t id = 0;
    for (const cpp_index::FileOccurrences& fo : index.per_file())
      for (const cpp_index::Occurrence& o : fo.occurrences()) {
        ++id;
        if (o.symbol() < 0 || o.symbol() >= num_symbols) continue;
        if ((o.roles() & cpp_index::DEFINITION) && definition[o.symbol()] == 0)
          definition[o.symbol()] = id;
        if ((o.roles() & cpp_index::DECLARATION) &&
            declaration[o.symbol()] == 0)
          declaration[o.symbol()] = id;
      }
  }

  // A generated file that only a producer of *another* language's anchors
  // names (index.proto: GENERATES) -- the `.pb.h` of a proto_library no C++
  // in the index includes -- is nothing anybody can open or land in: no
  // symbol is declared there and nothing in it is a use.  It stays out of the
  // tree, and its anchors out of the database.
  std::vector<bool> hidden(static_cast<size_t>(index.files_size()), false);
  {
    std::vector<bool> declared_in(hidden.size(), false);
    for (const cpp_index::Symbol& s : index.symbols())
      if (s.has_canonical() && s.canonical().file() >= 0 &&
          static_cast<size_t>(s.canonical().file()) < declared_in.size())
        declared_in[static_cast<size_t>(s.canonical().file())] = true;
    for (const cpp_index::FileOccurrences& fo : index.per_file()) {
      if (fo.file() < 0 || fo.file() >= index.files_size()) continue;
      const auto f = static_cast<size_t>(fo.file());
      if (index.files(fo.file()).kind() != cpp_index::GENERATED ||
          declared_in[f] || fo.occurrences().empty())
        continue;
      hidden[f] = std::all_of(fo.occurrences().begin(), fo.occurrences().end(),
                              [](const cpp_index::Occurrence& o) {
                                return o.roles() & cpp_index::GENERATES;
                              });
    }
  }

  // 2. Files and the directory tree above them.
  uint32_t files = 0;
  {
    Statement file = prepare(
        "INSERT INTO files(id, path, kind, dir, name, test) "
        "VALUES(?,?,?,?,?,?)");
    Statement dir =
        prepare("INSERT OR IGNORE INTO dirs(path, parent, name) VALUES(?,?,?)");
    std::unordered_set<std::string> seen_dirs;
    for (int32_t i = 0; i < index.files_size() && error.empty(); ++i) {
      if (hidden[static_cast<size_t>(i)]) continue;
      ++files;
      const cpp_index::File& f = index.files(i);
      const auto [d, name] = SplitPath(f.path());
      file.BindInt(1, i);
      file.BindText(2, f.path());
      file.BindInt(3, f.kind());
      file.BindText(4, d);
      file.BindText(5, name);
      file.BindInt(6, std::any_of(f.attributes().begin(), f.attributes().end(),
                                  [](const cpp_index::Attribute& a) {
                                    return a.key() == "testonly";
                                  }));
      run(file);
      // Every ancestor: "a/b/c" -> "a" (parent ""), "a/b" (parent "a");
      // "/usr/x" -> "/" (parent ""), "/usr" (parent "/").
      std::string_view rest = d;
      while (!rest.empty()) {
        if (!seen_dirs.insert(std::string(rest)).second) break;
        const auto [parent, base] = SplitPath(rest);
        dir.BindText(1, rest);
        if (rest == "/") {
          dir.BindText(2, "");
          dir.BindText(3, "/");
        } else {
          dir.BindText(2, parent);
          dir.BindText(3, base);
        }
        run(dir);
        rest = parent;
      }
    }
  }

  // 3. Symbols, their relations, and the trigrams of their names.
  uint64_t symbol_relations = 0, trigrams = 0;
  {
    Statement sym = prepare(
        "INSERT INTO symbols(id, usr, name, name_lower, qualified_name, kind, "
        "sub_kind, language, properties, type, canonical_file, "
        "canonical_begin, "
        "canonical_end, definition_occ) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    Statement rel = prepare(
        "INSERT INTO symbol_relations(symbol, kind, target) VALUES(?,?,?)");
    Statement tri = prepare(
        "INSERT OR IGNORE INTO symbol_trigrams(tri, symbol) VALUES(?,?)");
    for (int32_t i = 0; i < num_symbols && error.empty(); ++i) {
      const cpp_index::Symbol& s = index.symbols(i);
      const std::string lower = Lower(s.name());
      sym.BindInt(1, i);
      sym.BindText(2, s.usr());
      sym.BindText(3, s.name());
      sym.BindText(4, lower);
      sym.BindText(5, s.qualified_name());
      sym.BindInt(6, s.kind());
      sym.BindInt(7, s.sub_kind());
      sym.BindInt(8, s.language());
      sym.BindInt(9, s.properties());
      sym.BindText(10, s.type());
      if (s.has_canonical()) {
        sym.BindInt(11, s.canonical().file());
        sym.BindInt(12, s.canonical().begin());
        sym.BindInt(13, s.canonical().end());
      } else {
        sym.BindNull(11);
        sym.BindNull(12);
        sym.BindNull(13);
      }
      const int64_t def = definition[i] ? definition[i] : declaration[i];
      if (def)
        sym.BindInt(14, def);
      else
        sym.BindNull(14);
      run(sym);
      for (const cpp_index::Relation& r : s.relations()) {
        rel.BindInt(1, i);
        rel.BindInt(2, r.kind());
        rel.BindInt(3, r.symbol());
        run(rel);
        ++symbol_relations;
      }
      for (size_t k = 0; k + 3 <= lower.size(); ++k) {
        tri.BindText(1, std::string_view(lower).substr(k, 3));
        tri.BindInt(2, i);
        run(tri);
        ++trigrams;
      }
    }
  }

  // 4. Occurrences and their relations, in index order (ids from step 1).
  uint64_t occurrences = 0, occurrence_relations = 0;
  {
    Statement occ = prepare(
        "INSERT INTO occurrences(id, file, begin, end, symbol, roles, macro) "
        "VALUES(?,?,?,?,?,?,?)");
    Statement rel = prepare(
        "INSERT INTO occurrence_relations(occurrence, kind, symbol) "
        "VALUES(?,?,?)");
    int64_t id = 0;
    for (const cpp_index::FileOccurrences& fo : index.per_file()) {
      if (!error.empty()) break;
      const bool skip = fo.file() >= 0 && fo.file() < index.files_size() &&
                        hidden[static_cast<size_t>(fo.file())];
      for (const cpp_index::Occurrence& o : fo.occurrences()) {
        ++id;
        if (skip) continue;
        occ.BindInt(1, id);
        occ.BindInt(2, fo.file());
        occ.BindInt(3, o.begin());
        occ.BindInt(4, o.end());
        occ.BindInt(5, o.symbol());
        occ.BindInt(6, o.roles());
        occ.BindInt(7, o.macro());
        run(occ);
        ++occurrences;
        for (const cpp_index::Relation& r : o.relations()) {
          rel.BindInt(1, id);
          rel.BindInt(2, r.kind());
          rel.BindInt(3, r.symbol());
          run(rel);
          ++occurrence_relations;
        }
      }
    }
  }

  // 5. Unresolved dependent tokens.
  {
    Statement unresolved = prepare(
        "INSERT INTO unresolved(file, begin, end, name) VALUES(?,?,?,?)");
    for (const cpp_index::DependentToken& t : index.unresolved()) {
      if (!error.empty()) break;
      unresolved.BindInt(1, t.file());
      unresolved.BindInt(2, t.begin());
      unresolved.BindInt(3, t.end());
      unresolved.BindText(4, t.name());
      run(unresolved);
    }
  }

  // 6. What this database is.
  const int64_t imported_at = NowNs();
  const std::string etag =
      Hex(Fnv1a(opts.source_path + "|" + std::to_string(opts.source_size) +
                "|" + std::to_string(opts.source_mtime_ns) + "|" +
                std::to_string(imported_at)));
  {
    Statement meta = prepare("INSERT INTO meta(key, value) VALUES(?,?)");
    const auto put = [&](std::string_view key, std::string_view value) {
      meta.BindText(1, key);
      meta.BindText(2, value);
      run(meta);
    };
    put("schema_version", std::to_string(kSchemaVersion));
    put("producer", index.producer());
    put("index_schema_version", std::to_string(index.schema_version()));
    put("source_path", opts.source_path);
    put("source_size", std::to_string(opts.source_size));
    put("source_mtime_ns", std::to_string(opts.source_mtime_ns));
    put("imported_at_ns", std::to_string(imported_at));
    put("imported_at", Rfc3339(imported_at));
    put("etag", etag);
    put("files", std::to_string(files));
    put("symbols", std::to_string(num_symbols));
    put("occurrences", std::to_string(occurrences));
    put("unresolved", std::to_string(index.unresolved_size()));
  }
  if (!error.empty()) return error;
  if (std::string e = exec(kIndexesSql); !e.empty()) return e;
  if (std::string e = exec("COMMIT"); !e.empty()) return e;
  if (std::string e = exec("PRAGMA optimize"); !e.empty()) return e;

  if (stats) {
    stats->files = files;
    stats->symbols = static_cast<uint32_t>(num_symbols);
    stats->occurrences = occurrences;
    stats->unresolved = static_cast<uint32_t>(index.unresolved_size());
    stats->symbol_relations = symbol_relations;
    stats->occurrence_relations = occurrence_relations;
    stats->trigrams = trigrams;
    stats->elapsed_ms = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
    stats->etag = etag;
  }
  return "";
}

auto ImportIndexToFile(const cpp_index::Index& index,
                       const std::string& db_path, const ImportOptions& opts,
                       ImportStats* stats) -> std::string {
  std::error_code ec;
  if (std::filesystem::exists(db_path, ec)) return db_path + ": already exists";
  Db db;
  if (std::string e =
          db.Open(db_path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
      !e.empty())
    return db_path + ": " + e;
  std::string e = ImportIndex(index, db.handle(), opts, stats);
  db.Close();
  if (!e.empty()) std::filesystem::remove(db_path, ec);
  return e;
}

}  // namespace code_browser
