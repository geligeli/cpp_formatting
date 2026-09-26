#include "code_browser/api.h"

#include <algorithm>
#include <charconv>
#include <ctime>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <system_error>

#include "code_browser/api.pb.h"
#include "cpp_formatting/cpp_index_merge.h"
#include "cpp_formatting/index.pb.h"
#include "google/protobuf/json/json.h"

namespace code_browser {

namespace {

namespace api = code_browser::api;

constexpr uint32_t kDefaultRefLimit = 500;
constexpr uint32_t kMaxRefLimit = 5000;
constexpr size_t kDefaultSearchLimit = 20;
constexpr size_t kMaxSearchLimit = 200;
constexpr size_t kMaxLocations = 50;  // definitions/declarations in SymbolInfo
constexpr uint32_t kDefaultTextLimit = 200;  // lines
constexpr uint32_t kMaxTextLimit = 2000;

auto Quote(std::string_view etag) -> std::string {
  return "\"" + std::string(etag) + "\"";
}

// If-None-Match may carry several tags, weak ones, or `*`.
auto MatchesEtag(std::string_view header, std::string_view quoted) -> bool {
  if (quoted.empty() || header.empty()) return false;
  size_t pos = 0;
  while (pos <= header.size()) {
    size_t comma = header.find(',', pos);
    if (comma == std::string_view::npos) comma = header.size();
    std::string_view tag = header.substr(pos, comma - pos);
    while (!tag.empty() && tag.front() == ' ') tag.remove_prefix(1);
    while (!tag.empty() && tag.back() == ' ') tag.remove_suffix(1);
    if (tag.rfind("W/", 0) == 0) tag.remove_prefix(2);
    if (tag == "*" || tag == quoted) return true;
    pos = comma + 1;
  }
  return false;
}

auto HttpDate(int64_t ns) -> std::string {
  const std::time_t secs = static_cast<std::time_t>(ns / 1000000000LL);
  std::tm tm{};
  gmtime_r(&secs, &tm);
  char buf[64];
  std::strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S GMT", &tm);
  return buf;
}

auto ParseUint(std::string_view s, uint32_t& out) -> bool {
  if (s.empty()) return false;
  uint32_t v = 0;
  const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc() || ptr != s.data() + s.size()) return false;
  out = v;
  return true;
}

auto ParseInt(std::string_view s, int32_t& out) -> bool {
  if (s.empty()) return false;
  int32_t v = 0;
  const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc() || ptr != s.data() + s.size()) return false;
  out = v;
  return true;
}

class Params {
 public:
  explicit Params(std::string_view query) : pairs_(ParseQuery(query)) {}
  auto Get(std::string_view key) const -> std::optional<std::string> {
    for (const auto& [k, v] : pairs_)
      if (k == key) return v;
    return std::nullopt;
  }

 private:
  std::vector<std::pair<std::string, std::string>> pairs_;
};

auto Json(const google::protobuf::Message& m, int status = 200) -> ApiResponse {
  ApiResponse r;
  r.status = status;
  r.body = ToJson(m);
  return r;
}

// Adds the ETag and answers 304 when the client already has it.
auto WithEtag(ApiResponse r, std::string_view etag, const ApiRequest& request)
    -> ApiResponse {
  r.etag = Quote(etag);
  if (MatchesEtag(request.if_none_match, r.etag)) {
    r.status = 304;
    r.body.clear();
  }
  return r;
}

}  // namespace

auto PercentDecode(std::string_view s) -> std::optional<std::string> {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '%') {
      if (i + 2 >= s.size()) return std::nullopt;
      const auto hex = [](char h) -> int {
        if (h >= '0' && h <= '9') return h - '0';
        if (h >= 'a' && h <= 'f') return h - 'a' + 10;
        if (h >= 'A' && h <= 'F') return h - 'A' + 10;
        return -1;
      };
      const int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
      if (hi < 0 || lo < 0) return std::nullopt;
      const char decoded = static_cast<char>(hi * 16 + lo);
      if (decoded == '\0') return std::nullopt;
      out += decoded;
      i += 2;
    } else if (c == '+') {
      out += ' ';
    } else {
      out += c;
    }
  }
  return out;
}

auto ParseQuery(std::string_view query)
    -> std::vector<std::pair<std::string, std::string>> {
  std::vector<std::pair<std::string, std::string>> out;
  size_t pos = 0;
  while (pos < query.size()) {
    size_t amp = query.find('&', pos);
    if (amp == std::string_view::npos) amp = query.size();
    const std::string_view pair = query.substr(pos, amp - pos);
    pos = amp + 1;
    if (pair.empty()) continue;
    const size_t eq = pair.find('=');
    const std::optional<std::string> key = PercentDecode(pair.substr(0, eq));
    const std::optional<std::string> value =
        eq == std::string_view::npos ? std::string()
                                     : PercentDecode(pair.substr(eq + 1));
    if (key && value) out.emplace_back(*key, *value);
  }
  return out;
}

auto ParseRoles(std::string_view spec) -> std::optional<uint32_t> {
  if (spec.empty()) return 0;
  uint32_t number = 0;
  if (ParseUint(spec, number)) return number;
  uint32_t mask = 0;
  size_t pos = 0;
  while (pos <= spec.size()) {
    size_t bar = spec.find('|', pos);
    if (bar == std::string_view::npos) bar = spec.size();
    const std::string name(spec.substr(pos, bar - pos));
    cpp_index::Role value = cpp_index::ROLE_NONE;
    if (name.empty() || !cpp_index::Role_Parse(name, &value) ||
        value == cpp_index::ROLE_NONE)
      return std::nullopt;
    mask |= static_cast<uint32_t>(value);
    pos = bar + 1;
  }
  return mask;
}

auto ToJson(const google::protobuf::Message& message) -> std::string {
  google::protobuf::json::PrintOptions opts;
  opts.preserve_proto_field_names = true;
  std::string out;
  if (!google::protobuf::json::MessageToJsonString(message, &out, opts).ok())
    return "{}";
  return out;
}

auto ErrorResponse(int status, std::string_view message) -> ApiResponse {
  api::Error e;
  e.set_status(status);
  e.set_message(std::string(message));
  ApiResponse r = Json(e, status);
  r.cache_control = "no-store";
  return r;
}

// ---------------------------------------------------------------------------
// Building blocks shared by the routes
// ---------------------------------------------------------------------------

namespace {

// Resolves index files to their bytes once per request.
class FileResolver {
 public:
  FileResolver(const IndexDb& db, const Repo& repo, FileCache& files)
      : db_(db), repo_(repo), files_(files) {}

  struct Entry {
    FileRow row;
    std::shared_ptr<const CachedFile> bytes;  // null when not available
  };

  auto Get(int32_t file_id) -> const Entry* {
    auto it = entries_.find(file_id);
    if (it != entries_.end()) return &it->second;
    const std::optional<FileRow> row = db_.File(file_id);
    if (!row) return nullptr;
    Entry e;
    e.row = *row;
    if (const std::optional<std::filesystem::path> p =
            repo_.Resolve(row->path, row->kind))
      e.bytes = files_.Get(repo_, *p);
    return &entries_.emplace(file_id, std::move(e)).first->second;
  }

  void Fill(api::Location* loc, int32_t file_id, uint32_t begin, uint32_t end) {
    loc->set_file_id(file_id);
    loc->set_begin(begin);
    loc->set_end(end);
    const Entry* e = Get(file_id);
    if (!e) return;
    loc->set_path(e->row.path);
    if (e->bytes) {
      const auto [line, column] = e->bytes->LineOf(begin);
      loc->set_line(line);
      loc->set_column(column);
    }
  }

 private:
  const IndexDb& db_;
  const Repo& repo_;
  FileCache& files_;
  std::map<int32_t, Entry> entries_;
};

// `with_origin`: also what the symbol was generated from, one level up.
void FillSummary(const IndexDb& db, FileResolver& files, const SymbolRow& s,
                 api::SymbolSummary* out, bool with_origin = true) {
  if (with_origin)
    for (const RelationRow& r : db.Related(s.id, /*reverse=*/false)) {
      if (r.kind != cpp_index::GENERATED_FROM) continue;
      if (const std::optional<SymbolRow> origin = db.Symbol(r.symbol))
        FillSummary(db, files, *origin, out->mutable_origin(),
                    /*with_origin=*/false);
      // The anchor that made the link sits on this symbol's declaration.
      if (s.has_canonical)
        for (const OccRow& o :
             db.OccurrencesAt(s.canonical_file, s.canonical_begin))
          if (o.symbol == r.symbol && o.begin == s.canonical_begin &&
              o.end == s.canonical_end && (o.roles & cpp_index::GENERATES) &&
              (o.roles & cpp_index::WRITE))
            out->set_modifies_origin(true);
      break;
    }
  out->set_id(s.id);
  out->set_usr(s.usr);
  out->set_name(s.name);
  out->set_qualified_name(s.qualified_name);
  out->set_kind(s.kind);
  out->set_sub_kind(s.sub_kind);
  out->set_properties(s.properties);
  out->set_type(s.type);
  out->set_language(s.language);
  if (s.definition_occ != 0) {
    if (const std::optional<OccRow> o = db.Occurrence(s.definition_occ)) {
      files.Fill(out->mutable_definition(), o->file, o->begin, o->end);
      return;
    }
  }
  if (s.has_canonical)
    files.Fill(out->mutable_definition(), s.canonical_file, s.canonical_begin,
               s.canonical_end);
}

void FillSpan(const OccRow& o, api::Span* span) {
  span->set_begin(o.begin);
  span->set_end(o.end);
  span->set_symbol(o.symbol);
  span->set_roles(o.roles);
  span->set_macro(o.macro);
}

void FillTotals(const CoverageTotals& t, api::CoverageTotals* out) {
  out->set_files(t.files);
  out->set_lines_found(t.lines_found);
  out->set_lines_hit(t.lines_hit);
  out->set_branches_found(t.branches_found);
  out->set_branches_hit(t.branches_hit);
}

// The `path` parameter, normalised, with what the index knows about it.
struct RequestedFile {
  std::string path;
  std::optional<FileRow> row;  // absent when not indexed
  auto kind() const -> cpp_index::FileKind {
    return row ? row->kind : cpp_index::FILE_KIND_UNSPECIFIED;
  }
};

auto RequestedFileOf(const IndexDb& db, const Params& params,
                     ApiResponse* error) -> std::optional<RequestedFile> {
  const std::optional<std::string> raw = params.Get("path");
  if (!raw) {
    *error = ErrorResponse(400, "missing parameter: path");
    return std::nullopt;
  }
  const std::optional<std::string> path = Repo::NormalizeRequestPath(*raw);
  if (!path) {
    *error = ErrorResponse(400, "invalid path");
    return std::nullopt;
  }
  RequestedFile f;
  f.path = *path;
  if (const std::optional<int32_t> id = db.FileIdOf(*path))
    f.row = db.File(*id);
  return f;
}

// An `#include "x"` / `#include <x>` line.  [begin, end) is the spelling.
struct IncludeDirective {
  uint32_t begin = 0;
  uint32_t end = 0;
  std::string spelling;
  bool angled = false;
  // The spelling is a path from some root and never relative to the file
  // that says it: a .proto's `import`.
  bool rooted = false;
};

// Line by line, with no preprocessor: a directive inside a comment or a
// disabled `#if` is found too (and is as clickable as any other), and one
// whose file is named by a macro is not.
auto ScanIncludes(std::string_view text) -> std::vector<IncludeDirective> {
  std::vector<IncludeDirective> out;
  const auto blank = [](char c) { return c == ' ' || c == '\t'; };
  size_t line = 0;
  while (line < text.size()) {
    size_t eol = text.find('\n', line);
    if (eol == std::string_view::npos) eol = text.size();
    size_t i = line;
    const size_t next = eol + 1;
    line = next;
    while (i < eol && blank(text[i])) ++i;
    if (i == eol || text[i] != '#') continue;
    ++i;
    while (i < eol && blank(text[i])) ++i;
    const std::string_view rest = text.substr(i, eol - i);
    size_t keyword = 0;
    for (const std::string_view k : {"include_next", "include", "import"})
      if (rest.rfind(k, 0) == 0) {
        keyword = k.size();
        break;
      }
    if (keyword == 0) continue;
    i += keyword;
    while (i < eol && blank(text[i])) ++i;
    if (i == eol || (text[i] != '"' && text[i] != '<')) continue;
    const bool angled = text[i] == '<';
    const size_t begin = i + 1;
    const size_t close = text.find(angled ? '>' : '"', begin);
    if (close == std::string_view::npos || close >= eol || close == begin)
      continue;
    IncludeDirective d;
    d.begin = static_cast<uint32_t>(begin);
    d.end = static_cast<uint32_t>(close);
    d.spelling = std::string(text.substr(begin, close - begin));
    d.angled = angled;
    out.push_back(std::move(d));
  }
  return out;
}

// A .proto's imports: `import "a/b.proto";`, `import public ...`, `import
// weak ...`.  As line-based as the scanner above, and for the same reason.
auto ScanProtoImports(std::string_view text) -> std::vector<IncludeDirective> {
  std::vector<IncludeDirective> out;
  const auto blank = [](char c) { return c == ' ' || c == '\t'; };
  size_t line = 0;
  while (line < text.size()) {
    size_t eol = text.find('\n', line);
    if (eol == std::string_view::npos) eol = text.size();
    size_t i = line;
    line = eol + 1;
    while (i < eol && blank(text[i])) ++i;
    const auto word = [&](std::string_view w) {
      if (text.substr(i, eol - i).rfind(w, 0) != 0) return false;
      const size_t after = i + w.size();
      if (after < eol && !blank(text[after]) && text[after] != '"')
        return false;
      i = after;
      while (i < eol && blank(text[i])) ++i;
      return true;
    };
    if (!word("import")) continue;
    if (!word("public")) word("weak");
    if (i == eol || text[i] != '"') continue;
    const size_t begin = i + 1;
    const size_t close = text.find('"', begin);
    if (close == std::string_view::npos || close >= eol || close == begin)
      continue;
    IncludeDirective d;
    d.begin = static_cast<uint32_t>(begin);
    d.end = static_cast<uint32_t>(close);
    d.spelling = std::string(text.substr(begin, close - begin));
    d.rooted = true;
    out.push_back(std::move(d));
  }
  return out;
}

auto EndsWith(std::string_view s, std::string_view suffix) -> bool {
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// The indexed files `d` can name from `includer`, best first: the file next
// to the includer (quoted form), the spelling as a path from the root, then
// the paths ending in it -- first-party ones first, then the fewest
// directories in front of the spelling (`<time.h>` is `.../include/time.h`
// before `.../include/sys/time.h` or a library's `internal/time.h`), then
// the shortest.  No include path is known here, so this is a ranking and not
// a lookup; several candidates are all returned.
auto ResolveInclude(const IndexDb& db, std::string_view includer,
                    const IncludeDirective& d) -> std::vector<FileRow> {
  namespace fs = std::filesystem;
  const std::string spelling =
      fs::path(d.spelling).lexically_normal().generic_string();
  const std::string name = fs::path(spelling).filename().generic_string();
  if (name.empty() || name == "." || name == "..") return {};
  const std::string sibling = (fs::path(includer).parent_path() / d.spelling)
                                  .lexically_normal()
                                  .generic_string();
  const bool suffixable = spelling.rfind("../", 0) != 0 && spelling[0] != '/';
  const auto rank = [&](const FileRow& row) -> int {
    if (!d.angled && !d.rooted && row.path == sibling) return 0;
    if (row.path == spelling) return 1;
    if (!d.rooted && row.path == sibling) return 2;
    if (suffixable && row.path.size() > spelling.size() &&
        row.path.compare(row.path.size() - spelling.size(), spelling.size(),
                         spelling) == 0 &&
        row.path[row.path.size() - spelling.size() - 1] == '/')
      return row.kind == cpp_index::SOURCE ? 3 : 4;
    return -1;
  };
  std::vector<std::pair<int, FileRow>> ranked;
  for (FileRow& row : db.FilesNamed(name))
    if (const int r = rank(row); r >= 0) ranked.emplace_back(r, std::move(row));
  std::stable_sort(ranked.begin(), ranked.end(),
                   [](const auto& a, const auto& b) {
                     if (a.first != b.first) return a.first < b.first;
                     const auto depth = [](const std::string& p) {
                       return std::count(p.begin(), p.end(), '/');
                     };
                     const auto da = depth(a.second.path);
                     const auto db = depth(b.second.path);
                     if (da != db) return da < db;
                     return a.second.path.size() < b.second.path.size();
                   });
  std::vector<FileRow> out;
  for (auto& [r, row] : ranked) out.push_back(std::move(row));
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Routes
// ---------------------------------------------------------------------------

auto ApiHandler::Handle(const ApiRequest& request) const -> ApiResponse {
  if (request.method != "GET" && request.method != "HEAD") {
    ApiResponse r = ErrorResponse(405, "method not allowed");
    r.headers.emplace_back("Allow", "GET, HEAD");
    return r;
  }
  const std::string_view path = request.path;
  if (path == "/api/repo") return RepoInfo();
  if (path == "/api/files") return Files(request);
  if (path == "/api/file") return FileBytes(request);
  if (path == "/api/annotations") return Annotations(request);
  if (path == "/api/includes") return Includes(request);
  if (path == "/api/search") return Search(request);
  if (path == "/api/at") return At(request);
  if (path == "/api/text") return TextSearch(request);
  if (path == "/api/coverage") return CoverageOfFile(request);
  if (path == "/api/symbol") return SymbolInfo(request, "");
  if (path.rfind("/api/symbol/", 0) == 0)
    return SymbolInfo(request, path.substr(12));
  if (path.rfind("/api/refs/", 0) == 0)
    return References(request, path.substr(10));
  return ErrorResponse(404, "no such endpoint");
}

auto ApiHandler::RepoInfo() const -> ApiResponse {
  api::RepoInfo info;
  info.set_root(repo_.root().string());
  if (repo_.exec_root()) info.set_exec_root(repo_.exec_root()->string());
  const GitHead head = repo_.ReadHead();
  info.set_head_commit(head.commit);
  info.set_head_ref(head.ref);
  info.set_db_path(db_.path());
  info.set_index_etag(db_.etag());
  const DbStats& s = db_.stats();
  api::IndexStats* stats = info.mutable_stats();
  stats->set_files(s.files);
  stats->set_symbols(s.symbols);
  stats->set_occurrences(s.occurrences);
  stats->set_unresolved(s.unresolved);
  stats->set_db_bytes(s.db_bytes);
  stats->set_imported_at(s.imported_at);
  stats->set_source_path(s.source_path);
  if (text_ != nullptr) {
    api::TextIndexInfo* t = info.mutable_text_index();
    t->set_path(text_->path());
    t->set_files(text_->files());
    t->set_skipped(text_->skipped());
    t->set_corpus_bytes(text_->corpus_bytes());
    t->set_file_bytes(text_->file_bytes());
    t->set_built_at(text_->built_at());
  }
  if (coverage_ != nullptr) {
    api::CoverageInfo* c = info.mutable_coverage();
    c->set_path(coverage_->path());
    c->set_collected_at(coverage_->collected_at());
    c->set_etag(coverage_->etag());
    FillTotals(coverage_->totals(), c->mutable_totals());
  }
  return Json(info);
}

auto ApiHandler::Files(const ApiRequest& request) const -> ApiResponse {
  const Params params(request.query);
  const std::optional<std::string> prefix = Repo::NormalizeRequestPath(
      params.Get("prefix").value_or(""), /*allow_empty=*/true);
  if (!prefix) return ErrorResponse(400, "invalid prefix");
  const std::optional<std::vector<DirEntry>> entries = db_.ListDir(*prefix);
  if (!entries) return ErrorResponse(404, "no such directory in the index");
  api::FileList list;
  list.set_prefix(*prefix);
  for (const DirEntry& e : *entries) {
    api::TreeEntry* t = list.add_entries();
    t->set_name(e.name);
    t->set_path(e.path);
    t->set_is_dir(e.is_dir);
    if (!e.is_dir) {
      t->set_kind(e.kind);
      t->set_file_id(e.file_id);
      t->set_available(repo_.Resolve(e.path, e.kind).has_value());
    }
    if (coverage_ != nullptr) {
      const CoverageTotals* totals = nullptr;
      if (e.is_dir) {
        totals = coverage_->Dir(e.path);
      } else if (const FileCoverage* fc = coverage_->File(e.path)) {
        totals = &fc->totals;
      }
      if (totals != nullptr) FillTotals(*totals, t->mutable_coverage());
    }
  }
  return Json(list);
}

auto ApiHandler::FileBytes(const ApiRequest& request) const -> ApiResponse {
  ApiResponse error;
  const Params params(request.query);
  const std::optional<RequestedFile> f = RequestedFileOf(db_, params, &error);
  if (!f) return error;
  const std::optional<std::filesystem::path> resolved =
      repo_.Resolve(f->path, f->kind());
  if (!resolved) {
    if (f->row && f->row->kind != cpp_index::SOURCE)
      return ErrorResponse(
          404,
          "not present in the checkout; build the target that generates "
          "it, or point --exec-root at a build that did");
    return ErrorResponse(404, "no such file in the checkout");
  }
  const std::shared_ptr<const CachedFile> bytes = files_.Get(repo_, *resolved);
  if (!bytes) return ErrorResponse(404, "no such file in the checkout");
  ApiResponse r;
  r.content_type = "text/plain; charset=utf-8";
  r.body = bytes->data;
  r.headers.emplace_back("Last-Modified", HttpDate(bytes->mtime_ns));
  if (f->row) {
    r.headers.emplace_back("X-File-Id", std::to_string(f->row->id));
    r.headers.emplace_back("X-File-Kind",
                           cpp_index::FileKind_Name(f->row->kind));
  }
  if (db_.stats().source_mtime_ns != 0 &&
      bytes->mtime_ns > db_.stats().source_mtime_ns)
    r.headers.emplace_back("X-Newer-Than-Index", "1");
  return WithEtag(std::move(r), bytes->etag, request);
}

auto ApiHandler::Annotations(const ApiRequest& request) const -> ApiResponse {
  ApiResponse error;
  const Params params(request.query);
  const std::optional<RequestedFile> f = RequestedFileOf(db_, params, &error);
  if (!f) return error;
  if (!f->row) return ErrorResponse(404, "file not in the index");
  api::Annotations out;
  out.set_path(f->row->path);
  out.set_file_id(f->row->id);
  FileResolver files(db_, repo_, files_);
  std::map<int32_t, bool> seen;
  for (const OccRow& o : db_.FileOccurrences(f->row->id)) {
    FillSpan(o, out.add_spans());
    if (seen.emplace(o.symbol, true).second)
      if (const std::optional<SymbolRow> s = db_.Symbol(o.symbol))
        FillSummary(db_, files, *s, out.add_symbols());
  }
  for (const UnresolvedRow& u : db_.Unresolved(f->row->id)) {
    api::Unresolved* t = out.add_unresolved();
    t->set_begin(u.begin);
    t->set_end(u.end);
    t->set_name(u.name);
  }
  return WithEtag(Json(out), db_.etag(), request);
}

auto ApiHandler::Includes(const ApiRequest& request) const -> ApiResponse {
  ApiResponse error;
  const Params params(request.query);
  const std::optional<RequestedFile> f = RequestedFileOf(db_, params, &error);
  if (!f) return error;
  const std::optional<std::filesystem::path> resolved =
      repo_.Resolve(f->path, f->kind());
  const std::shared_ptr<const CachedFile> bytes =
      resolved ? files_.Get(repo_, *resolved) : nullptr;
  if (!bytes) return ErrorResponse(404, "no such file in the checkout");
  api::Includes out;
  out.set_path(f->path);
  // What a file includes is a matter of its language.
  const std::vector<IncludeDirective> directives =
      EndsWith(f->path, ".proto") ? ScanProtoImports(bytes->data)
                                  : ScanIncludes(bytes->data);
  for (const IncludeDirective& d : directives) {
    api::Include* inc = out.add_includes();
    inc->set_begin(d.begin);
    inc->set_end(d.end);
    inc->set_spelling(d.spelling);
    inc->set_angled(d.angled);
    for (const FileRow& row : ResolveInclude(db_, f->path, d)) {
      api::IncludeTarget* t = inc->add_targets();
      t->set_path(row.path);
      t->set_kind(row.kind);
      t->set_available(repo_.Resolve(row.path, row.kind).has_value());
    }
  }
  // Depends on the file as well as on the index.
  return WithEtag(Json(out), db_.etag() + "-" + bytes->etag, request);
}

auto ApiHandler::SymbolInfo(const ApiRequest& request,
                            std::string_view rest) const -> ApiResponse {
  const Params params(request.query);
  std::optional<SymbolRow> sym;
  if (!rest.empty()) {
    int32_t id = -1;
    if (!ParseInt(rest, id) || id < 0)
      return ErrorResponse(400, "bad symbol id");
    sym = db_.Symbol(id);
  } else if (const std::optional<std::string> usr = params.Get("usr")) {
    sym = db_.SymbolByUsr(*usr);
  } else {
    return ErrorResponse(400, "missing parameter: usr");
  }
  if (!sym) return ErrorResponse(404, "no such symbol");
  FileResolver files(db_, repo_, files_);
  api::SymbolInfo info;
  FillSummary(db_, files, *sym, info.mutable_symbol());
  if (sym->has_canonical)
    files.Fill(info.mutable_canonical(), sym->canonical_file,
               sym->canonical_begin, sym->canonical_end);
  RefQuery defs;
  defs.role_mask = cpp_index::DEFINITION;
  defs.limit = kMaxLocations;
  for (const OccRow& o : db_.SymbolOccurrences(sym->id, defs))
    files.Fill(info.add_definitions(), o.file, o.begin, o.end);
  RefQuery decls;
  decls.role_mask = cpp_index::DECLARATION;
  decls.exclude_mask = cpp_index::DEFINITION;
  decls.limit = kMaxLocations;
  for (const OccRow& o : db_.SymbolOccurrences(sym->id, decls))
    files.Fill(info.add_declarations(), o.file, o.begin, o.end);
  for (const bool reverse : {false, true})
    for (const RelationRow& r : db_.Related(sym->id, reverse)) {
      const std::optional<SymbolRow> other = db_.Symbol(r.symbol);
      if (!other) continue;
      api::Related* rel = info.add_related();
      rel->set_kind(r.kind);
      rel->set_reverse(reverse);
      FillSummary(db_, files, *other, rel->mutable_symbol());
    }
  api::RefCounts* counts = info.mutable_counts();
  RefQuery all;
  all.exclude_mask = cpp_index::GENERATES;
  counts->set_total(db_.CountSymbolOccurrences(sym->id, all));
  counts->set_definitions(db_.CountSymbolOccurrences(sym->id, defs));
  counts->set_declarations(db_.CountSymbolOccurrences(sym->id, decls));
  RefQuery refs;
  refs.role_mask = cpp_index::REFERENCE;
  counts->set_references(db_.CountSymbolOccurrences(sym->id, refs));
  counts->set_files(db_.CountSymbolFiles(sym->id));
  all.with_generated = true;
  counts->set_generated(db_.CountSymbolOccurrences(sym->id, all) -
                        counts->total());
  return WithEtag(Json(info), db_.etag(), request);
}

auto ApiHandler::References(const ApiRequest& request,
                            std::string_view rest) const -> ApiResponse {
  int32_t id = -1;
  if (!ParseInt(rest, id) || id < 0) return ErrorResponse(400, "bad symbol id");
  const std::optional<SymbolRow> sym = db_.Symbol(id);
  if (!sym) return ErrorResponse(404, "no such symbol");
  const Params params(request.query);
  RefQuery q;
  q.limit = kDefaultRefLimit;
  if (const std::optional<std::string> role = params.Get("role")) {
    const std::optional<uint32_t> mask = ParseRoles(*role);
    if (!mask) return ErrorResponse(400, "bad role");
    q.role_mask = *mask;
  }
  if (const std::optional<std::string> exclude = params.Get("exclude")) {
    const std::optional<uint32_t> mask = ParseRoles(*exclude);
    if (!mask) return ErrorResponse(400, "bad exclude");
    q.exclude_mask = *mask;
  }
  if (const std::optional<std::string> file = params.Get("file")) {
    const std::optional<std::string> path = Repo::NormalizeRequestPath(*file);
    const std::optional<int32_t> fid =
        path ? db_.FileIdOf(*path) : std::nullopt;
    if (!fid) return ErrorResponse(404, "file not in the index");
    q.file = *fid;
  }
  if (const std::optional<std::string> offset = params.Get("offset"))
    if (!ParseUint(*offset, q.offset)) return ErrorResponse(400, "bad offset");
  if (const std::optional<std::string> limit = params.Get("limit")) {
    if (!ParseUint(*limit, q.limit) || q.limit == 0)
      return ErrorResponse(400, "bad limit");
    q.limit = std::min(q.limit, kMaxRefLimit);
  }
  if (const std::optional<std::string> expand = params.Get("expand")) {
    if (*expand != "generated") return ErrorResponse(400, "bad expand");
    q.with_generated = true;
  }
  // An anchor marks generated text; it is listed only when asked for by role.
  if (!(q.role_mask & cpp_index::GENERATES))
    q.exclude_mask |= cpp_index::GENERATES;
  api::References out;
  out.set_symbol(id);
  out.set_offset(q.offset);
  out.set_limit(q.limit);
  out.set_total(db_.CountSymbolOccurrences(id, q));
  FileResolver files(db_, repo_, files_);
  if (q.with_generated)
    for (const RelationRow& r : db_.Related(id, /*reverse=*/true)) {
      if (r.kind != cpp_index::GENERATED_FROM) continue;
      // With their origin: that is where `modifies_origin` comes from.
      if (const std::optional<SymbolRow> generated = db_.Symbol(r.symbol))
        FillSummary(db_, files, *generated, out.add_symbols());
    }
  api::FileReferences* group = nullptr;
  uint32_t returned = 0;
  for (const OccRow& o : db_.SymbolOccurrences(id, q)) {
    ++returned;
    if (!group || group->file_id() != o.file) {
      group = out.add_files();
      group->set_file_id(o.file);
      if (const FileResolver::Entry* e = files.Get(o.file)) {
        group->set_path(e->row.path);
        group->set_kind(e->row.kind);
        group->set_test(e->row.test);
      }
    }
    api::Reference* ref = group->add_refs();
    files.Fill(ref->mutable_location(), o.file, o.begin, o.end);
    ref->set_roles(o.roles);
    ref->set_role_names(roleNames(o.roles));
    ref->set_macro(o.macro);
    if (o.symbol != id) ref->set_symbol(o.symbol);
    if (const FileResolver::Entry* e = files.Get(o.file); e && e->bytes)
      ref->set_line_text(
          std::string(e->bytes->LineText(ref->location().line())));
  }
  out.set_truncated(q.offset + returned < out.total());
  return WithEtag(Json(out), db_.etag(), request);
}

auto ApiHandler::Search(const ApiRequest& request) const -> ApiResponse {
  const Params params(request.query);
  const std::optional<std::string> q = params.Get("q");
  if (!q) return ErrorResponse(400, "missing parameter: q");
  SearchOptions opts;
  opts.limit = kDefaultSearchLimit;
  if (const std::optional<std::string> limit = params.Get("limit")) {
    uint32_t n = 0;
    if (!ParseUint(*limit, n) || n == 0) return ErrorResponse(400, "bad limit");
    opts.limit = std::min<size_t>(n, kMaxSearchLimit);
  }
  if (const std::optional<std::string> kind = params.Get("kind")) {
    cpp_index::SymbolKind value = cpp_index::SYMBOL_KIND_UNSPECIFIED;
    if (!cpp_index::SymbolKind_Parse(*kind, &value))
      return ErrorResponse(400, "bad kind");
    opts.kind = value;
  }
  if (const std::optional<std::string> locals = params.Get("locals"))
    opts.include_locals = *locals == "1" || *locals == "true";
  const SearchResult result = db_.Search(*q, opts);
  api::SearchResults out;
  out.set_query(*q);
  out.set_truncated(result.truncated);
  FileResolver files(db_, repo_, files_);
  for (const SearchHit& hit : result.hits) {
    api::SearchHit* h = out.add_hits();
    h->set_score(hit.score);
    FillSummary(db_, files, hit.symbol, h->mutable_symbol());
  }
  return WithEtag(Json(out), db_.etag(), request);
}

auto ApiHandler::At(const ApiRequest& request) const -> ApiResponse {
  ApiResponse error;
  const Params params(request.query);
  const std::optional<RequestedFile> f = RequestedFileOf(db_, params, &error);
  if (!f) return error;
  if (!f->row) return ErrorResponse(404, "file not in the index");
  uint32_t offset = 0;
  const std::optional<std::string> raw = params.Get("offset");
  if (!raw || !ParseUint(*raw, offset)) return ErrorResponse(400, "bad offset");
  api::OccurrencesAt out;
  out.set_path(f->row->path);
  out.set_offset(offset);
  FileResolver files(db_, repo_, files_);
  std::map<int32_t, bool> seen;
  for (const OccRow& o : db_.OccurrencesAt(f->row->id, offset)) {
    FillSpan(o, out.add_spans());
    if (seen.emplace(o.symbol, true).second)
      if (const std::optional<SymbolRow> s = db_.Symbol(o.symbol))
        FillSummary(db_, files, *s, out.add_symbols());
  }
  for (const UnresolvedRow& u : db_.Unresolved(f->row->id))
    if (u.begin <= offset && offset < u.end) {
      api::Unresolved* t = out.add_unresolved();
      t->set_begin(u.begin);
      t->set_end(u.end);
      t->set_name(u.name);
    }
  return WithEtag(Json(out), db_.etag(), request);
}

auto ApiHandler::TextSearch(const ApiRequest& request) const -> ApiResponse {
  const Params params(request.query);
  const std::optional<std::string> q = params.Get("q");
  if (!q) return ErrorResponse(400, "missing parameter: q");
  if (const std::optional<std::string> why = InvalidTextQuery(*q))
    return ErrorResponse(400, *why);
  TextSearchOptions opts;
  opts.limit = kDefaultTextLimit;
  if (const std::optional<std::string> c = params.Get("case")) {
    if (*c == "insensitive")
      opts.case_sensitive = false;
    else if (*c != "sensitive")
      return ErrorResponse(400, "case is `sensitive` or `insensitive`");
  }
  uint32_t n = 0;
  if (const std::optional<std::string> v = params.Get("offset")) {
    if (!ParseUint(*v, n)) return ErrorResponse(400, "bad offset");
    opts.offset = n;
  }
  if (const std::optional<std::string> v = params.Get("limit")) {
    if (!ParseUint(*v, n) || n == 0 || n > kMaxTextLimit)
      return ErrorResponse(
          400, "limit must be in [1, " + std::to_string(kMaxTextLimit) + "]");
    opts.limit = n;
  }
  if (text_ == nullptr)
    return ErrorResponse(503, "full-text search is off (--text-search)");

  const TextSearchResult result = text_->Search(*q, opts);
  api::TextSearchResults out;
  out.set_query(*q);
  out.set_case_sensitive(opts.case_sensitive);
  out.set_total_matches(result.total_matches);
  out.set_total_lines(result.total_lines);
  out.set_files_matched(result.files_matched);
  out.set_truncated(result.truncated);
  out.set_offset(static_cast<uint32_t>(opts.offset));
  out.set_next_offset(static_cast<uint32_t>(result.next_offset));
  for (const TextFileHits& file : result.files) {
    api::TextFile* f = out.add_files();
    f->set_path(file.path);
    f->set_file_id(-1);
    // Whatever the symbol index says about the file: the page greys out what
    // it has no annotations for.
    if (const std::optional<int32_t> id = db_.FileIdOf(file.path)) {
      f->set_file_id(*id);
      if (const std::optional<FileRow> row = db_.File(*id))
        f->set_kind(row->kind);
    }
    for (const TextLineHit& hit : file.lines) {
      api::TextLine* l = f->add_lines();
      l->set_line(hit.line);
      l->set_column(hit.column);
      l->set_text(hit.text);
      l->set_text_offset(hit.text_offset);
      l->set_clipped_end(hit.clipped_end);
      for (const TextSpan& s : hit.spans) {
        api::TextSpan* span = l->add_spans();
        span->set_begin(s.begin);
        span->set_end(s.end);
      }
    }
  }
  return WithEtag(Json(out), text_->etag() + "-" + db_.etag(), request);
}

auto ApiHandler::CoverageOfFile(const ApiRequest& request) const
    -> ApiResponse {
  ApiResponse error;
  const Params params(request.query);
  const std::optional<RequestedFile> f = RequestedFileOf(db_, params, &error);
  if (!f) return error;
  if (coverage_ == nullptr)
    return ErrorResponse(
        503, "no coverage loaded (start the server with --coverage=<lcov>)");
  const FileCoverage* fc = coverage_->File(f->path);
  if (fc == nullptr) return ErrorResponse(404, "no coverage for this file");
  api::FileCoverage out;
  out.set_path(fc->path);
  for (const LineCoverage& l : fc->lines) {
    out.add_lines(l.line);
    out.add_hits(static_cast<uint32_t>(
        std::min<uint64_t>(l.hits, std::numeric_limits<uint32_t>::max())));
    if (l.branches > 0) {
      out.add_branch_lines(l.line);
      out.add_branches(l.branches);
      out.add_branches_taken(l.branches_taken);
    }
  }
  FillTotals(fc->totals, out.mutable_totals());
  // Depends on the file too: edited after the tracefile, its counts may sit
  // on the wrong lines, which the page says.
  std::string file_etag = "-";
  if (const std::optional<std::filesystem::path> resolved =
          repo_.Resolve(f->path, f->kind()))
    if (const auto stat = repo_.Stat(*resolved)) {
      out.set_stale(coverage_->mtime_ns() != 0 &&
                    stat->first > coverage_->mtime_ns());
      file_etag =
          std::to_string(stat->first) + "-" + std::to_string(stat->second);
    }
  return WithEtag(Json(out), coverage_->etag() + "-" + file_etag, request);
}

}  // namespace code_browser
