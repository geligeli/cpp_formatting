// The checkout being browsed: where a path from the index lives on disk,
// whether a request may name it, and what HEAD points at.
#ifndef CODE_BROWSER_REPO_H_
#define CODE_BROWSER_REPO_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "cpp_formatting/index.pb.h"

namespace code_browser {

struct RepoOptions {
  std::filesystem::path root;  // the git checkout (the index's cwd)
  // Where bazel-out/... and external/... resolve.  Discovered from the
  // root's bazel-out convenience symlink when not given.
  std::optional<std::filesystem::path> exec_root;
  bool serve_system_files = true;  // absolute paths the index calls SYSTEM
};

struct GitHead {
  std::string commit;  // 40 hex chars, or "" when not a git checkout
  std::string ref;     // "refs/heads/main", or "" when detached
};

struct FileBytes {
  std::string data;
  int64_t mtime_ns = 0;
  uint64_t size = 0;
};

class Repo {
 public:
  // Canonicalises the root and discovers the exec root.  Fails if the root
  // is not a directory.
  static auto Open(RepoOptions opts, std::string* error) -> std::optional<Repo>;

  auto root() const -> const std::filesystem::path& { return root_; }
  auto exec_root() const -> const std::optional<std::filesystem::path>& {
    return exec_root_;
  }
  // Read on every call (two small files); no libgit2, no subprocess.
  auto ReadHead() const -> GitHead;

  // The path a request may name: no `..`, no empty segment, no backslash,
  // no control byte; `./` prefixes dropped; a leading `/` kept (SYSTEM
  // files).  `allow_empty` accepts "" (the root directory).
  static auto NormalizeRequestPath(std::string_view path,
                                   bool allow_empty = false)
      -> std::optional<std::string>;

  // Where an index path lives: SOURCE (and anything not indexed) under the
  // root, provided its canonical form stays inside it; GENERATED/EXTERNAL
  // under the exec root; SYSTEM as written.  nullopt when it cannot be
  // served from this checkout.
  auto Resolve(std::string_view index_path, cpp_index::FileKind kind) const
      -> std::optional<std::filesystem::path>;

  auto Read(const std::filesystem::path& file) const
      -> std::optional<FileBytes>;
  // mtime (ns since the epoch) and size, without reading the file.
  auto Stat(const std::filesystem::path& file) const
      -> std::optional<std::pair<int64_t, uint64_t>>;

 private:
  Repo() = default;
  std::filesystem::path root_;
  std::optional<std::filesystem::path> exec_root_;
  bool serve_system_files_ = true;
};

}  // namespace code_browser

#endif  // CODE_BROWSER_REPO_H_
