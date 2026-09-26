#include "code_browser/coverage.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <compare>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace code_browser {

namespace {

namespace fs = std::filesystem;

// Every record kind an LCOV tracefile may hold (geninfo(1)).
constexpr std::string_view kKnownKeys[] = {
    "TN",  "VER", "SF", "FN", "FNDA", "FNF", "FNH", "FNL",
    "FNA", "DA",  "LF", "LH", "BRDA", "BRF", "BRH", "MCDC",
};

auto Fnv1a(std::string_view s) -> uint64_t {
  uint64_t h = 1469598103934665603ULL;
  for (const unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  return h;
}

auto SaturatingAdd(uint64_t a, uint64_t b) -> uint64_t {
  return a > std::numeric_limits<uint64_t>::max() - b
             ? std::numeric_limits<uint64_t>::max()
             : a + b;
}

template <typename T>
auto ParseNumber(std::string_view s, T& out) -> bool {
  if (s.empty()) return false;
  const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
  return ec == std::errc() && ptr == s.data() + s.size();
}

auto Split(std::string_view s, char sep) -> std::vector<std::string_view> {
  std::vector<std::string_view> out;
  size_t pos = 0;
  while (true) {
    const size_t next = s.find(sep, pos);
    out.push_back(s.substr(pos, next - pos));
    if (next == std::string_view::npos) return out;
    pos = next + 1;
  }
}

// "a/b/c" -> "a/b"; "a" -> ""; "/usr" -> "/"; "/" -> "".
auto ParentDir(std::string_view path) -> std::string_view {
  if (path == "/") return "";
  const size_t slash = path.rfind('/');
  if (slash == std::string_view::npos) return "";
  if (slash == 0) return "/";
  return path.substr(0, slash);
}

auto StartsWithDir(std::string_view path, std::string_view dir) -> bool {
  return path.size() > dir.size() && path.substr(0, dir.size()) == dir &&
         path[dir.size()] == '/';
}

// `./` segments and repeated slashes out; `..` is left (and then matches no
// indexed file, which is the honest outcome).
auto CleanSegments(std::string_view path) -> std::string {
  const bool absolute = !path.empty() && path.front() == '/';
  std::string out = absolute ? "/" : "";
  for (const std::string_view seg : Split(path, '/')) {
    if (seg.empty() || seg == ".") continue;
    if (!out.empty() && out.back() != '/') out += '/';
    out += seg;
  }
  return out;
}

struct Accumulator {
  std::map<uint32_t, uint64_t> hits;  // DA, by line
  // BRDA, by (line, the branch's identity: the fields between the line and
  // the count, e.g. "0,1" or "e0,1").
  std::map<std::pair<uint32_t, std::string>, uint64_t> branches;
};

auto Build(std::map<std::string, Accumulator>& accumulated)
    -> std::vector<FileCoverage> {
  std::vector<FileCoverage> files;
  files.reserve(accumulated.size());
  for (auto& [path, acc] : accumulated) {
    FileCoverage f;
    f.path = path;
    f.totals.files = 1;
    for (const auto& [line, hits] : acc.hits) {
      f.lines.push_back(LineCoverage{line, hits, 0, 0});
      ++f.totals.lines_found;
      if (hits > 0) ++f.totals.lines_hit;
    }
    for (const auto& [key, taken] : acc.branches) {
      ++f.totals.branches_found;
      if (taken > 0) ++f.totals.branches_hit;
      // A branch on a line with no DA record counts in the totals only.
      const auto it = std::lower_bound(
          f.lines.begin(), f.lines.end(), key.first,
          [](const LineCoverage& l, uint32_t line) { return l.line < line; });
      if (it == f.lines.end() || it->line != key.first) continue;
      ++it->branches;
      if (taken > 0) ++it->branches_taken;
    }
    files.push_back(std::move(f));
  }
  return files;  // by path: the map was
}

}  // namespace

void CoverageTotals::Add(const CoverageTotals& other) {
  files += other.files;
  lines_found += other.lines_found;
  lines_hit += other.lines_hit;
  branches_found += other.branches_found;
  branches_hit += other.branches_hit;
}

auto NormalizeCoveragePath(std::string_view sf, const CoverageOptions& options)
    -> std::string {
  constexpr std::string_view kCwd = "/proc/self/cwd/";
  if (sf.substr(0, kCwd.size()) == kCwd) sf.remove_prefix(kCwd.size());
  std::string path = CleanSegments(sf);
  if (path.empty() || path.front() != '/') return path;
  for (const fs::path& prefix : options.strip_prefixes) {
    const std::string p = CleanSegments(prefix.generic_string());
    if (p.empty() || p == "/") continue;
    if (StartsWithDir(path, p)) return path.substr(p.size() + 1);
  }
  // A path inside some action's sandbox: .../execroot/<workspace>/pkg/f.cc.
  constexpr std::string_view kExecroot = "/execroot/";
  if (const size_t at = path.rfind(kExecroot); at != std::string::npos) {
    const size_t ws = at + kExecroot.size();
    const size_t slash = path.find('/', ws);
    if (slash != std::string::npos && slash + 1 < path.size())
      return path.substr(slash + 1);
  }
  return path;
}

auto Coverage::Parse(std::string_view text, std::string_view name,
                     const CoverageOptions& options, std::string* error)
    -> std::unique_ptr<Coverage> {
  std::map<std::string, Accumulator> accumulated;
  Accumulator* current = nullptr;
  size_t lineno = 0;
  size_t record_line = 0;  // the current record's SF
  const auto fail = [&](const std::string& what) -> std::unique_ptr<Coverage> {
    *error = std::string(name) + ":" + std::to_string(lineno) + ": " + what;
    return nullptr;
  };
  size_t pos = 0;
  while (pos < text.size()) {
    size_t eol = text.find('\n', pos);
    if (eol == std::string_view::npos) eol = text.size();
    std::string_view line = text.substr(pos, eol - pos);
    pos = eol + 1;
    ++lineno;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.find_first_not_of(" \t") == std::string_view::npos) continue;
    if (line == "end_of_record") {
      if (current == nullptr) return fail("end_of_record outside a record");
      current = nullptr;
      continue;
    }
    const size_t colon = line.find(':');
    const std::string_view key =
        line.substr(0, colon == std::string_view::npos ? line.size() : colon);
    if (colon == std::string_view::npos ||
        std::find(std::begin(kKnownKeys), std::end(kKnownKeys), key) ==
            std::end(kKnownKeys))
      return fail("not an LCOV record: '" + std::string(line) + "'");
    const std::string_view value = line.substr(colon + 1);
    if (key == "TN" || key == "VER") continue;  // test name, format version
    if (key == "SF") {
      if (current != nullptr) return fail("SF inside a record");
      const std::string path = NormalizeCoveragePath(value, options);
      if (path.empty()) return fail("SF with no path");
      current = &accumulated[path];
      record_line = lineno;
      continue;
    }
    if (current == nullptr) return fail(std::string(key) + " outside a record");
    if (key == "DA") {
      // DA:<line>,<count>[,<checksum>]
      const std::vector<std::string_view> f = Split(value, ',');
      uint32_t ln = 0;
      uint64_t count = 0;
      if (f.size() < 2 || f.size() > 3 || !ParseNumber(f[0], ln) ||
          !ParseNumber(f[1], count))
        return fail("DA: expected <line>,<count>, got '" + std::string(line) +
                    "'");
      if (ln == 0) continue;  // what some tools write for a whole function
      uint64_t& hits = current->hits[ln];
      hits = SaturatingAdd(hits, count);
    } else if (key == "BRDA") {
      // BRDA:<line>,[e]<block>,<branch>,<taken>; `-` for never evaluated.
      const std::vector<std::string_view> f = Split(value, ',');
      uint32_t ln = 0;
      uint64_t taken = 0;
      if (f.size() < 4 || !ParseNumber(f[0], ln) ||
          (f.back() != "-" && !ParseNumber(f.back(), taken)))
        return fail("BRDA: expected <line>,<block>,<branch>,<taken>, got '" +
                    std::string(line) + "'");
      const std::string id(value.substr(
          f[0].size() + 1, value.size() - f[0].size() - f.back().size() - 2));
      if (ln == 0) continue;
      uint64_t& t = current->branches[{ln, id}];
      t = SaturatingAdd(t, taken);
    }
    // FN, FNDA, FNF, FNH, FNL, FNA, LF, LH, BRF, BRH, MCDC: summaries this
    // reader recomputes, or functions it does not show.
  }
  if (current != nullptr) {
    lineno = record_line;
    return fail("record has no end_of_record");
  }

  auto coverage = std::unique_ptr<Coverage>(new Coverage());
  coverage->path_ = std::string(name);
  char hex[17];
  std::snprintf(hex, sizeof hex, "%016llx",
                static_cast<unsigned long long>(Fnv1a(text)));
  coverage->etag_ = std::string("cov-") + hex;
  coverage->files_ = Build(accumulated);
  for (const FileCoverage& f : coverage->files_) {
    coverage->totals_.Add(f.totals);
    std::string_view dir = f.path;
    do {
      dir = ParentDir(dir);
      coverage->dirs_[std::string(dir)].Add(f.totals);
    } while (!dir.empty());
  }
  return coverage;
}

auto Coverage::Load(const fs::path& path, const CoverageOptions& options,
                    std::string* error) -> std::unique_ptr<Coverage> {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    *error = "cannot read " + path.string();
    return nullptr;
  }
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  std::error_code ec;
  const auto mtime = fs::last_write_time(path, ec);
  if (ec) {
    *error = "cannot stat " + path.string() + ": " + ec.message();
    return nullptr;
  }
  std::unique_ptr<Coverage> coverage =
      Parse(text, path.string(), options, error);
  if (!coverage) return nullptr;
  coverage->mtime_ns_ =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::file_clock::to_sys(mtime).time_since_epoch())
          .count();
  coverage->etag_ = "cov-" + std::to_string(coverage->mtime_ns_) + "-" +
                    std::to_string(text.size());
  return coverage;
}

auto Coverage::File(std::string_view path) const -> const FileCoverage* {
  const auto it = std::lower_bound(
      files_.begin(), files_.end(), path,
      [](const FileCoverage& f, std::string_view p) { return f.path < p; });
  return it != files_.end() && it->path == path ? &*it : nullptr;
}

auto Coverage::Dir(std::string_view dir) const -> const CoverageTotals* {
  const auto it = dirs_.find(dir);
  return it != dirs_.end() ? &it->second : nullptr;
}

auto Coverage::collected_at() const -> std::string {
  if (mtime_ns_ == 0) return "";
  const std::time_t secs = static_cast<std::time_t>(mtime_ns_ / 1000000000);
  std::tm tm{};
  gmtime_r(&secs, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

}  // namespace code_browser
