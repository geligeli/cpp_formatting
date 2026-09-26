#include "code_browser/text_index.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <bit>
#include <cerrno>
#include <chrono>
#include <compare>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <system_error>
#include <utility>

#include "code_browser/suffix_array.h"

namespace code_browser {

namespace fs = std::filesystem;

namespace {

static_assert(std::endian::native == std::endian::little,
              "the .fts format is little-endian and mapped as is");

constexpr char kMagic[8] = {'C', 'B', 'F', 'T', 'S', '\0', '\0', '\0'};
constexpr uint32_t kVersion = 1;

struct Header {
  char magic[8];
  uint32_t version;
  uint32_t reserved;
  int64_t built_at_ns;
  uint64_t manifest_count;
  uint64_t names_offset;
  uint64_t names_size;
  uint64_t manifest_offset;
  uint64_t corpus_offset;
  uint64_t corpus_size;
  uint64_t sa_offset;
  uint64_t sa_count;
};

constexpr uint32_t kIndexed = 1;  // in the corpus (else binary / unreadable)

struct ManifestRow {
  uint64_t name_offset;
  uint32_t name_size;
  uint32_t flags;
  uint64_t size;
  int64_t mtime_ns;
  uint64_t corpus_begin;  // kIndexed only
};

// A line longer than this is shown clipped around its first match.
constexpr uint64_t kMaxLineBytes = 400;
constexpr uint64_t kContextBefore = 120;

auto Align8(uint64_t n) -> uint64_t { return (n + 7) & ~uint64_t{7}; }

auto ToNs(fs::file_time_type t) -> int64_t {
  const auto sys = std::chrono::file_clock::to_sys(t);
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             sys.time_since_epoch())
      .count();
}

auto Fold(char c) -> uint8_t {
  const auto u = static_cast<uint8_t>(c);
  return u >= 'A' && u <= 'Z' ? static_cast<uint8_t>(u + ('a' - 'A')) : u;
}

auto HasAsciiLetter(std::string_view s) -> bool {
  return std::any_of(s.begin(), s.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
  });
}

auto IsContinuation(char c) -> bool {
  return (static_cast<uint8_t>(c) & 0xc0) == 0x80;
}

// `s` with every byte that is not part of a well-formed UTF-8 sequence
// replaced by '?': the length stays, so byte offsets into it stay valid.
auto SanitizeUtf8(std::string_view s) -> std::string {
  std::string out(s);
  size_t i = 0;
  while (i < s.size()) {
    const auto c = static_cast<uint8_t>(s[i]);
    size_t len = 0;
    uint8_t lo = 0x80, hi = 0xbf;  // bounds of the second byte
    if (c < 0x80) {
      ++i;
      continue;
    } else if (c >= 0xc2 && c <= 0xdf) {
      len = 2;
    } else if (c >= 0xe0 && c <= 0xef) {
      len = 3;
      if (c == 0xe0) lo = 0xa0;
      if (c == 0xed) hi = 0x9f;
    } else if (c >= 0xf0 && c <= 0xf4) {
      len = 4;
      if (c == 0xf0) lo = 0x90;
      if (c == 0xf4) hi = 0x8f;
    }
    bool ok = len != 0 && i + len <= s.size();
    for (size_t k = 1; ok && k < len; ++k) {
      const auto b = static_cast<uint8_t>(s[i + k]);
      ok = k == 1 ? b >= lo && b <= hi : IsContinuation(s[i + k]);
    }
    if (ok) {
      i += len;
    } else {
      out[i] = '?';
      ++i;
    }
  }
  return out;
}

auto ReadWholeFile(const fs::path& p) -> std::optional<std::string> {
  std::ifstream in(p, std::ios::binary);
  if (!in) return std::nullopt;
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size < 0) return std::nullopt;
  in.seekg(0);
  std::string data(static_cast<size_t>(size), '\0');
  if (size > 0 && !in.read(data.data(), size)) return std::nullopt;
  // Grown since the stat: whatever is there now.
  if (in.peek() != std::ifstream::traits_type::eof()) {
    std::string rest((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    data += rest;
  }
  return data;
}

}  // namespace

// ---------------------------------------------------------------------------
// Walking the checkout
// ---------------------------------------------------------------------------

auto ListTextCandidates(const fs::path& root, const TextIndexOptions& options,
                        std::string* error)
    -> std::optional<std::vector<TextCandidate>> {
  std::vector<std::string> excluded;
  for (const fs::path& p : options.exclude) {
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(p, ec);
    excluded.push_back((ec ? fs::absolute(p) : canonical).string());
  }
  std::vector<TextCandidate> out;
  std::error_code ec;
  fs::recursive_directory_iterator it(
      root, fs::directory_options::skip_permission_denied, ec);
  if (ec) {
    *error = "cannot walk " + root.string() + ": " + ec.message();
    return std::nullopt;
  }
  for (const fs::recursive_directory_iterator end; it != end;
       it.increment(ec)) {
    if (ec) {
      *error = "cannot walk " + root.string() + ": " + ec.message();
      return std::nullopt;
    }
    const fs::path& p = it->path();
    const std::string name = p.filename().string();
    std::error_code sec;
    const fs::file_status st = it->symlink_status(sec);
    if (sec) continue;
    bool skip = fs::is_symlink(st) || name.empty() || name[0] == '.' ||
                (it.depth() == 0 &&
                 (name == "external" || name.rfind("bazel-", 0) == 0));
    for (const std::string& x : excluded)
      if (!skip && p.native().rfind(x, 0) == 0) skip = true;
    if (skip) {
      if (fs::is_directory(st)) it.disable_recursion_pending();
      continue;
    }
    if (!fs::is_regular_file(st)) continue;
    const uint64_t size = it->file_size(sec);
    if (sec || size > options.max_file_bytes) continue;
    const fs::file_time_type mtime = it->last_write_time(sec);
    if (sec) continue;
    out.push_back(
        {p.lexically_relative(root).generic_string(), size, ToNs(mtime)});
  }
  std::sort(out.begin(), out.end(),
            [](const TextCandidate& a, const TextCandidate& b) {
              return a.path < b.path;
            });
  return out;
}

// ---------------------------------------------------------------------------
// Building
// ---------------------------------------------------------------------------

auto BuildTextIndex(const fs::path& root,
                    const std::vector<TextCandidate>& candidates,
                    const fs::path& out, TextBuildStats* stats,
                    std::string* error) -> bool {
  const auto start = std::chrono::steady_clock::now();
  std::string corpus;
  std::vector<ManifestRow> rows(candidates.size());
  std::string names;
  uint64_t indexed = 0;
  for (size_t i = 0; i < candidates.size(); ++i) {
    const TextCandidate& c = candidates[i];
    ManifestRow& row = rows[i];
    row = {};
    row.name_offset = names.size();
    row.name_size = static_cast<uint32_t>(c.path.size());
    row.size = c.size;
    row.mtime_ns = c.mtime_ns;
    names += c.path;
    const std::optional<std::string> data = ReadWholeFile(root / c.path);
    if (!data || data->find('\0') != std::string::npos) continue;
    if (corpus.size() + data->size() + 1 > kMaxSuffixArrayText) {
      *error = "the text of the checkout exceeds " +
               std::to_string(kMaxSuffixArrayText >> 20) +
               " MiB; lower --text-max-file-kb or turn --text-search off";
      return false;
    }
    row.flags = kIndexed;
    row.corpus_begin = corpus.size();
    corpus += *data;
    corpus += '\0';
    ++indexed;
  }

  Header h{};
  std::memcpy(h.magic, kMagic, sizeof kMagic);
  h.version = kVersion;
  h.built_at_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  h.manifest_count = rows.size();
  h.names_offset = Align8(sizeof(Header));
  h.names_size = names.size();
  h.manifest_offset = Align8(h.names_offset + h.names_size);
  h.corpus_offset =
      Align8(h.manifest_offset + rows.size() * sizeof(ManifestRow));
  h.corpus_size = corpus.size();
  h.sa_offset = Align8(h.corpus_offset + h.corpus_size);
  h.sa_count = corpus.size() - indexed;

  // The corpus goes out as read, then is folded in place for the suffix
  // array: the text once and the array are all the memory this takes.
  const std::string tmp = out.string() + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) {
      *error = "cannot write " + tmp;
      return false;
    }
    uint64_t pos = 0;
    const auto put = [&](uint64_t offset, const void* data, size_t size) {
      static constexpr char kZeros[8] = {};
      f.write(kZeros, static_cast<std::streamsize>(offset - pos));
      f.write(static_cast<const char*>(data),
              static_cast<std::streamsize>(size));
      pos = offset + size;
    };
    put(0, &h, sizeof h);
    put(h.names_offset, names.data(), names.size());
    put(h.manifest_offset, rows.data(), rows.size() * sizeof(ManifestRow));
    put(h.corpus_offset, corpus.data(), corpus.size());
    for (char& c : corpus) c = static_cast<char>(Fold(c));
    std::vector<int32_t> sa = BuildSuffixArray(corpus);
    corpus = {};
    if (sa.size() != h.corpus_size) {
      *error = "cannot build the suffix array";
      std::remove(tmp.c_str());
      return false;
    }
    // The separators sort first: one NUL per indexed file, and no other.
    put(h.sa_offset, sa.data() + indexed, h.sa_count * sizeof(int32_t));
    f.close();
    if (!f) {
      *error = "cannot write " + tmp;
      std::remove(tmp.c_str());
      return false;
    }
  }
  std::error_code ec;
  fs::rename(tmp, out, ec);
  if (ec) {
    *error = "cannot replace " + out.string() + ": " + ec.message();
    std::remove(tmp.c_str());
    return false;
  }
  if (stats) {
    stats->rebuilt = true;
    stats->files = indexed;
    stats->skipped = candidates.size() - indexed;
    stats->corpus_bytes = h.corpus_size;
    stats->elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
  }
  return true;
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

auto InvalidTextQuery(std::string_view query) -> std::optional<std::string> {
  if (query.empty()) return "empty query";
  if (query.size() > kMaxTextQuery)
    return "a query is at most " + std::to_string(kMaxTextQuery) + " bytes";
  if (query.find_first_of(std::string_view("\0\n\r", 3)) !=
      std::string_view::npos)
    return "a query is one line";
  return std::nullopt;
}

TextIndex::~TextIndex() {
  if (map_ != nullptr) ::munmap(const_cast<char*>(map_), map_size_);
}

auto TextIndex::Open(const fs::path& path, std::string* error)
    -> std::unique_ptr<TextIndex> {
  const auto fail = [&](const std::string& why) {
    *error = path.string() + ": " + why;
    return nullptr;
  };
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return fail(std::strerror(errno));
  struct stat st{};
  if (::fstat(fd, &st) != 0) {
    const int e = errno;
    ::close(fd);
    return fail(std::strerror(e));
  }
  const auto size = static_cast<uint64_t>(st.st_size);
  if (size < sizeof(Header)) {
    ::close(fd);
    return fail("not a text index (too short)");
  }
  void* map = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd);
  if (map == MAP_FAILED) return fail(std::strerror(errno));
  std::unique_ptr<TextIndex> index(new TextIndex());
  index->path_ = path.string();
  index->map_ = static_cast<const char*>(map);
  index->map_size_ = size;

  Header h;
  std::memcpy(&h, map, sizeof h);
  if (std::memcmp(h.magic, kMagic, sizeof kMagic) != 0)
    return fail("not a text index");
  if (h.version != kVersion)
    return fail("text index version " + std::to_string(h.version) +
                ", expected " + std::to_string(kVersion));
  const auto section = [&](uint64_t offset, uint64_t count,
                           uint64_t element) -> bool {
    return offset % 8 == 0 && offset <= size &&
           count <= (size - offset) / element;
  };
  if (!section(h.names_offset, h.names_size, 1) ||
      !section(h.manifest_offset, h.manifest_count, sizeof(ManifestRow)) ||
      !section(h.corpus_offset, h.corpus_size, 1) ||
      !section(h.sa_offset, h.sa_count, sizeof(int32_t)) ||
      h.corpus_size > kMaxSuffixArrayText)
    return fail("truncated or corrupt (sections)");
  const char* base = index->map_;
  index->names_ = std::string_view(base + h.names_offset, h.names_size);
  index->corpus_ = std::string_view(base + h.corpus_offset, h.corpus_size);
  index->manifest_ = base + h.manifest_offset;
  index->manifest_size_ = h.manifest_count;
  index->sa_ = reinterpret_cast<const int32_t*>(base + h.sa_offset);
  index->sa_size_ = h.sa_count;
  index->built_at_ns_ = h.built_at_ns;

  const auto* rows = static_cast<const ManifestRow*>(index->manifest_);
  uint64_t next_begin = 0;
  for (uint64_t i = 0; i < h.manifest_count; ++i) {
    const ManifestRow& r = rows[i];
    if (r.name_offset > h.names_size ||
        r.name_size > h.names_size - r.name_offset)
      return fail("truncated or corrupt (names)");
    if (!(r.flags & kIndexed)) continue;
    // Back to back, each followed by its NUL.  (What was read, which may
    // differ from the stat in the row.)
    const size_t end = r.corpus_begin == next_begin
                           ? index->corpus_.find('\0', r.corpus_begin)
                           : std::string_view::npos;
    if (end == std::string_view::npos)
      return fail("truncated or corrupt (manifest)");
    index->files_.push_back({index->names_.substr(r.name_offset, r.name_size),
                             r.corpus_begin, end - r.corpus_begin});
    next_begin = end + 1;
  }
  if (next_begin != h.corpus_size ||
      h.sa_count != h.corpus_size - index->files_.size())
    return fail("truncated or corrupt (corpus)");
  char etag[64];
  std::snprintf(etag, sizeof etag, "fts-%llx-%llx",
                static_cast<unsigned long long>(h.built_at_ns),
                static_cast<unsigned long long>(h.corpus_size));
  index->etag_ = etag;
  return index;
}

auto TextIndex::IsCurrent(const std::vector<TextCandidate>& candidates) const
    -> bool {
  if (candidates.size() != manifest_size_) return false;
  const auto* rows = static_cast<const ManifestRow*>(manifest_);
  for (size_t i = 0; i < candidates.size(); ++i) {
    const ManifestRow& r = rows[i];
    const TextCandidate& c = candidates[i];
    if (c.size != r.size || c.mtime_ns != r.mtime_ns ||
        c.path != names_.substr(r.name_offset, r.name_size))
      return false;
  }
  return true;
}

auto TextIndex::built_at() const -> std::string {
  const std::time_t secs = static_cast<std::time_t>(built_at_ns_ / 1000000000);
  std::tm tm{};
  gmtime_r(&secs, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

auto TextIndex::Search(std::string_view query,
                       const TextSearchOptions& options) const
    -> TextSearchResult {
  TextSearchResult result;
  if (InvalidTextQuery(query)) return result;
  const size_t m = query.size();
  std::string folded(query);
  for (char& c : folded) c = static_cast<char>(Fold(c));

  // The folded prefix of the suffix at `pos` against the folded query.
  // Entries are not validated when the file is opened (that would read all of
  // it); one out of range compares greater and is never collected.
  const auto valid = [&](int32_t pos) {
    return pos >= 0 && static_cast<size_t>(pos) < corpus_.size();
  };
  const auto compare = [&](int32_t pos) -> int {
    if (!valid(pos)) return 1;
    const char* p = corpus_.data() + pos;
    const size_t avail = corpus_.size() - static_cast<size_t>(pos);
    for (size_t j = 0; j < m; ++j) {
      if (j >= avail) return -1;
      const uint8_t a = Fold(p[j]);
      const auto b = static_cast<uint8_t>(folded[j]);
      if (a != b) return a < b ? -1 : 1;
    }
    return 0;
  };
  const int32_t* const first = sa_;
  const int32_t* const last = sa_ + sa_size_;
  const int32_t* lo = std::partition_point(
      first, last, [&](int32_t pos) { return compare(pos) < 0; });
  const int32_t* hi = std::partition_point(
      lo, last, [&](int32_t pos) { return compare(pos) == 0; });
  const auto range = static_cast<size_t>(hi - lo);

  std::vector<uint64_t> positions;
  const size_t take = std::min(range, kMaxTextMatches);
  result.truncated = range > take;
  positions.reserve(take);
  const bool filter = options.case_sensitive && HasAsciiLetter(query);
  for (const int32_t* p = lo; p != lo + take; ++p)
    if (valid(*p) &&
        (!filter || corpus_.compare(static_cast<size_t>(*p), m, query) == 0))
      positions.push_back(static_cast<uint64_t>(*p));
  std::sort(positions.begin(), positions.end());
  // Case-insensitive, the range is the exact count even when truncated.
  result.total_matches = filter ? positions.size() : range;

  // Group into lines.  Positions ascend, so do files and lines: each file's
  // newlines are counted once.
  struct LineGroup {
    size_t file;
    uint32_t line;
    uint64_t begin, end;  // the line in the corpus, without its newline
    std::vector<std::pair<uint64_t, uint64_t>> spans;
  };
  std::vector<LineGroup> groups;
  size_t file = files_.size();
  uint64_t cursor = 0, line_begin = 0;
  uint32_t line = 0;
  for (const uint64_t pos : positions) {
    if (file == files_.size() ||
        pos >= files_[file].begin + files_[file].size) {
      const auto it = std::upper_bound(
          files_.begin(), files_.end(), pos,
          [](uint64_t p, const IndexedFile& f) { return p < f.begin; });
      file = static_cast<size_t>(it - files_.begin()) - 1;
      cursor = line_begin = files_[file].begin;
      line = 1;
      ++result.files_matched;
    }
    while (const void* nl =
               std::memchr(corpus_.data() + cursor, '\n', pos - cursor)) {
      ++line;
      cursor = line_begin =
          static_cast<uint64_t>(static_cast<const char*>(nl) - corpus_.data()) +
          1;
    }
    cursor = pos;
    if (groups.empty() || groups.back().file != file ||
        groups.back().line != line) {
      const uint64_t file_end = files_[file].begin + files_[file].size;
      const void* nl = std::memchr(corpus_.data() + pos, '\n', file_end - pos);
      uint64_t end = nl ? static_cast<uint64_t>(static_cast<const char*>(nl) -
                                                corpus_.data())
                        : file_end;
      if (end > line_begin && corpus_[end - 1] == '\r') --end;
      groups.push_back({file, line, line_begin, end, {}});
    }
    auto& spans = groups.back().spans;
    if (!spans.empty() && pos <= spans.back().second)
      spans.back().second = std::max(spans.back().second, pos + m);
    else
      spans.emplace_back(pos, pos + m);
  }
  result.total_lines = groups.size();

  const size_t begin = std::min(options.offset, groups.size());
  const size_t end = std::min(groups.size(), begin + options.limit);
  result.next_offset = end < groups.size() ? end : 0;
  for (size_t g = begin; g < end; ++g) {
    const LineGroup& group = groups[g];
    if (result.files.empty() ||
        result.files.back().path != files_[group.file].path)
      result.files.push_back({std::string(files_[group.file].path), {}});
    const uint64_t match = group.spans.front().first;
    uint64_t text_begin = group.begin, text_end = group.end;
    if (group.end - group.begin > kMaxLineBytes) {
      text_begin =
          std::max(group.begin, match > kContextBefore ? match - kContextBefore
                                                       : uint64_t{0});
      while (text_begin < match && IsContinuation(corpus_[text_begin]))
        ++text_begin;
      text_end = std::min(group.end, text_begin + kMaxLineBytes);
      text_end =
          std::max(text_end, std::min(group.end, group.spans.front().second));
      while (text_end < group.end && text_end > match &&
             IsContinuation(corpus_[text_end]))
        --text_end;
    }
    TextLineHit hit;
    hit.line = group.line;
    hit.column = static_cast<uint32_t>(match - group.begin + 1);
    hit.text_offset = static_cast<uint32_t>(text_begin - group.begin);
    hit.clipped_end = text_end < group.end;
    hit.text = SanitizeUtf8(corpus_.substr(text_begin, text_end - text_begin));
    for (const auto& [b, e] : group.spans) {
      const uint64_t sb = std::max(b, text_begin), se = std::min(e, text_end);
      if (sb < se)
        hit.spans.push_back({static_cast<uint32_t>(sb - text_begin),
                             static_cast<uint32_t>(se - text_begin)});
    }
    result.files.back().lines.push_back(std::move(hit));
  }
  return result;
}

auto OpenOrBuildTextIndex(const fs::path& root, const fs::path& path,
                          const TextIndexOptions& options,
                          TextBuildStats* stats, std::string* error)
    -> std::unique_ptr<TextIndex> {
  const std::optional<std::vector<TextCandidate>> candidates =
      ListTextCandidates(root, options, error);
  if (!candidates) return nullptr;
  std::error_code ec;
  if (fs::exists(path, ec)) {
    std::string ignored;
    std::unique_ptr<TextIndex> index = TextIndex::Open(path, &ignored);
    if (index && index->IsCurrent(*candidates)) {
      if (stats) {
        stats->rebuilt = false;
        stats->files = index->files();
        stats->skipped = index->skipped();
        stats->corpus_bytes = index->corpus_bytes();
      }
      return index;
    }
  }
  if (!BuildTextIndex(root, *candidates, path, stats, error)) return nullptr;
  return TextIndex::Open(path, error);
}

}  // namespace code_browser
