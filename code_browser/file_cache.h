// Source files as the API needs them: bytes, an ETag, and a line table, kept
// in an LRU bounded by bytes.  A cached file is re-read when its size or
// mtime changed (one stat per lookup).  The one mutable structure the server
// shares between threads; it has its own lock.
#ifndef CODE_BROWSER_FILE_CACHE_H_
#define CODE_BROWSER_FILE_CACHE_H_

#include <cstdint>
#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "code_browser/repo.h"

namespace code_browser {

struct CachedFile {
  std::string data;
  int64_t mtime_ns = 0;
  uint64_t size = 0;
  std::string etag;                   // "<mtime_ns>-<size>"
  std::vector<uint32_t> line_starts;  // byte offset of each line; [0] == 0

  // 1-based line and byte column of a byte offset (clamped to the file).
  auto LineOf(uint32_t offset) const -> std::pair<uint32_t, uint32_t>;
  // The text of a 1-based line without its newline; "" past the end.
  auto LineText(uint32_t line) const -> std::string_view;
};

class FileCache {
 public:
  explicit FileCache(size_t max_bytes) : max_bytes_(max_bytes) {}

  // The file at `resolved` (from Repo::Resolve), fresh as of now, or null
  // if it cannot be read.
  auto Get(const Repo& repo, const std::filesystem::path& resolved)
      -> std::shared_ptr<const CachedFile>;

  auto bytes() const -> size_t;

 private:
  struct Entry {
    std::shared_ptr<const CachedFile> file;
    std::list<std::string>::iterator lru;
  };
  size_t max_bytes_;
  mutable std::mutex mutex_;
  std::list<std::string> lru_;  // most recent at the front
  std::unordered_map<std::string, Entry> entries_;
  size_t bytes_ = 0;
};

}  // namespace code_browser

#endif  // CODE_BROWSER_FILE_CACHE_H_
