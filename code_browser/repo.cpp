#include "code_browser/repo.h"

#include <chrono>
#include <cstddef>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <system_error>

namespace code_browser {

namespace fs = std::filesystem;

namespace {

auto ReadTextFile(const fs::path& p) -> std::optional<std::string> {
  std::ifstream in(p, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

auto Trim(std::string s) -> std::string {
  while (!s.empty() &&
         (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
    s.pop_back();
  size_t i = 0;
  while (i < s.size() && s[i] == ' ') ++i;
  return s.substr(i);
}

auto IsHex40(std::string_view s) -> bool {
  if (s.size() != 40) return false;
  for (const char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

auto StartsWith(const fs::path& p, const fs::path& prefix) -> bool {
  auto pi = p.begin();
  for (auto it = prefix.begin(); it != prefix.end(); ++it, ++pi) {
    if (it->empty()) continue;  // a trailing slash
    if (pi == p.end() || *pi != *it) return false;
  }
  return true;
}

auto ToNs(fs::file_time_type t) -> int64_t {
  const auto sys = std::chrono::file_clock::to_sys(t);
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             sys.time_since_epoch())
      .count();
}

}  // namespace

auto Repo::Open(RepoOptions opts, std::string* error) -> std::optional<Repo> {
  std::error_code ec;
  const fs::path root = fs::canonical(opts.root, ec);
  if (ec || !fs::is_directory(root, ec)) {
    if (error) *error = opts.root.string() + ": not a directory";
    return std::nullopt;
  }
  Repo r;
  r.root_ = root;
  r.serve_system_files_ = opts.serve_system_files;
  if (opts.exec_root) {
    r.exec_root_ = fs::weakly_canonical(*opts.exec_root, ec);
    if (ec) r.exec_root_ = *opts.exec_root;
  } else {
    // bazel-out -> <output base>/execroot/_main/bazel-out
    const fs::path link = root / "bazel-out";
    if (fs::is_symlink(link, ec)) {
      const fs::path target = fs::read_symlink(link, ec);
      if (!ec && target.filename() == "bazel-out")
        r.exec_root_ = target.parent_path();
    }
  }
  return r;
}

auto Repo::ReadHead() const -> GitHead {
  GitHead head;
  std::error_code ec;
  fs::path gitdir = root_ / ".git";
  if (fs::is_regular_file(gitdir, ec)) {
    // A worktree: ".git" is a file saying where the real directory is.
    const std::optional<std::string> text = ReadTextFile(gitdir);
    if (!text) return head;
    const std::string line = Trim(*text);
    if (line.rfind("gitdir: ", 0) != 0) return head;
    gitdir = fs::path(line.substr(8));
    if (gitdir.is_relative()) gitdir = root_ / gitdir;
  }
  if (!fs::is_directory(gitdir, ec)) return head;
  fs::path commondir = gitdir;
  if (const std::optional<std::string> c = ReadTextFile(gitdir / "commondir")) {
    const fs::path p(Trim(*c));
    commondir = p.is_relative() ? gitdir / p : p;
  }
  const std::optional<std::string> head_text = ReadTextFile(gitdir / "HEAD");
  if (!head_text) return head;
  const std::string line = Trim(*head_text);
  if (line.rfind("ref: ", 0) != 0) {
    if (IsHex40(line)) head.commit = line;
    return head;
  }
  head.ref = line.substr(5);
  for (const fs::path& dir : {gitdir, commondir}) {
    if (const std::optional<std::string> sha = ReadTextFile(dir / head.ref)) {
      const std::string s = Trim(*sha);
      if (IsHex40(s)) {
        head.commit = s;
        return head;
      }
    }
  }
  if (const std::optional<std::string> packed =
          ReadTextFile(commondir / "packed-refs")) {
    std::istringstream in(*packed);
    std::string l;
    while (std::getline(in, l)) {
      if (l.empty() || l[0] == '#' || l[0] == '^') continue;
      const size_t sp = l.find(' ');
      if (sp == std::string::npos) continue;
      if (Trim(l.substr(sp + 1)) == head.ref && IsHex40(l.substr(0, sp))) {
        head.commit = l.substr(0, sp);
        return head;
      }
    }
  }
  return head;
}

auto Repo::NormalizeRequestPath(std::string_view path, bool allow_empty)
    -> std::optional<std::string> {
  for (const unsigned char c : path)
    if (c < 0x20 || c == 0x7f || c == '\\') return std::nullopt;
  std::string out;
  const bool absolute = !path.empty() && path.front() == '/';
  if (absolute) path.remove_prefix(1);
  while (!path.empty()) {
    const size_t slash = path.find('/');
    const std::string_view segment = path.substr(0, slash);
    if (segment.empty() || segment == "..") return std::nullopt;
    if (segment != ".") {
      if (!out.empty()) out += '/';
      out += segment;
    }
    if (slash == std::string_view::npos) break;
    path.remove_prefix(slash + 1);
    if (path.empty()) return std::nullopt;  // a trailing slash names a dir
  }
  if (out.empty() && !allow_empty) return std::nullopt;
  if (absolute) return "/" + out;
  return out;
}

auto Repo::Resolve(std::string_view index_path, cpp_index::FileKind kind) const
    -> std::optional<fs::path> {
  const std::optional<std::string> safe = NormalizeRequestPath(index_path);
  if (!safe) return std::nullopt;
  std::error_code ec;
  if (!safe->empty() && safe->front() == '/') {
    if (!serve_system_files_ || kind != cpp_index::SYSTEM) return std::nullopt;
    const fs::path p(*safe);
    return fs::is_regular_file(p, ec) ? std::optional<fs::path>(p)
                                      : std::nullopt;
  }
  if (kind == cpp_index::GENERATED || kind == cpp_index::EXTERNAL) {
    if (!exec_root_) return std::nullopt;
    const fs::path p = *exec_root_ / *safe;
    return fs::is_regular_file(p, ec) ? std::optional<fs::path>(p)
                                      : std::nullopt;
  }
  // SOURCE, or a path the index does not know: the checkout, and nothing a
  // symlink leads out of it.
  const fs::path p = root_ / *safe;
  const fs::path canonical = fs::weakly_canonical(p, ec);
  if (ec || !StartsWith(canonical, root_)) return std::nullopt;
  return fs::is_regular_file(canonical, ec) ? std::optional<fs::path>(p)
                                            : std::nullopt;
}

auto Repo::Read(const fs::path& file) const -> std::optional<FileBytes> {
  std::error_code ec;
  const auto mtime = fs::last_write_time(file, ec);
  if (ec) return std::nullopt;
  std::optional<std::string> data = ReadTextFile(file);
  if (!data) return std::nullopt;
  FileBytes out;
  out.size = data->size();
  out.data = std::move(*data);
  out.mtime_ns = ToNs(mtime);
  return out;
}

auto Repo::Stat(const fs::path& file) const
    -> std::optional<std::pair<int64_t, uint64_t>> {
  std::error_code ec;
  const auto mtime = fs::last_write_time(file, ec);
  if (ec) return std::nullopt;
  const uint64_t size = fs::file_size(file, ec);
  if (ec) return std::nullopt;
  return std::make_pair(ToNs(mtime), size);
}

}  // namespace code_browser
