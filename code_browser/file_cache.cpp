#include "code_browser/file_cache.h"

#include <algorithm>
#include <optional>

namespace code_browser {

auto CachedFile::LineOf(uint32_t offset) const
    -> std::pair<uint32_t, uint32_t> {
  if (line_starts.empty()) return {1, 1};
  auto it = std::upper_bound(line_starts.begin(), line_starts.end(), offset);
  const size_t line = static_cast<size_t>(it - line_starts.begin());  // >= 1
  const uint32_t start = line_starts[line - 1];
  return {static_cast<uint32_t>(line), offset - start + 1};
}

auto CachedFile::LineText(uint32_t line) const -> std::string_view {
  if (line == 0 || line > line_starts.size()) return {};
  const uint32_t start = line_starts[line - 1];
  uint32_t end = line < line_starts.size() ? line_starts[line]
                                           : static_cast<uint32_t>(data.size());
  while (end > start && (data[end - 1] == '\n' || data[end - 1] == '\r')) --end;
  return std::string_view(data).substr(start, end - start);
}

auto FileCache::Get(const Repo& repo, const std::filesystem::path& resolved)
    -> std::shared_ptr<const CachedFile> {
  const std::string key = resolved.string();
  const auto stat = repo.Stat(resolved);
  if (!stat) return nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(key);
    if (it != entries_.end()) {
      if (it->second.file->mtime_ns == stat->first &&
          it->second.file->size == stat->second) {
        lru_.splice(lru_.begin(), lru_, it->second.lru);
        return it->second.file;
      }
      bytes_ -= it->second.file->size;
      lru_.erase(it->second.lru);
      entries_.erase(it);
    }
  }
  std::optional<FileBytes> bytes = repo.Read(resolved);
  if (!bytes) return nullptr;
  auto file = std::make_shared<CachedFile>();
  file->size = bytes->size;
  file->mtime_ns = bytes->mtime_ns;
  file->data = std::move(bytes->data);
  file->etag =
      std::to_string(file->mtime_ns) + "-" + std::to_string(file->size);
  file->line_starts.push_back(0);
  for (size_t i = 0; i < file->data.size(); ++i)
    if (file->data[i] == '\n' && i + 1 < file->data.size())
      file->line_starts.push_back(static_cast<uint32_t>(i + 1));
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // A concurrent reader may have inserted the same key meanwhile; the
    // newer entry replaces it.
    auto it = entries_.find(key);
    if (it != entries_.end()) {
      bytes_ -= it->second.file->size;
      lru_.erase(it->second.lru);
      entries_.erase(it);
    }
    lru_.push_front(key);
    entries_[key] = Entry{file, lru_.begin()};
    bytes_ += file->size;
    while (bytes_ > max_bytes_ && lru_.size() > 1) {
      const std::string& victim = lru_.back();
      auto v = entries_.find(victim);
      bytes_ -= v->second.file->size;
      entries_.erase(v);
      lru_.pop_back();
    }
  }
  return file;
}

auto FileCache::bytes() const -> size_t {
  std::lock_guard<std::mutex> lock(mutex_);
  return bytes_;
}

}  // namespace code_browser
