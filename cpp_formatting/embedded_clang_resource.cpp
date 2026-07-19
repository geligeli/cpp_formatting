#include "cpp_formatting/embedded_clang_resource.h"

#include <zlib.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

extern "C" const unsigned char kClangIncludeHeadersTarGz[];
extern "C" const size_t kClangIncludeHeadersTarGzSize;

namespace {

namespace fs = std::filesystem;

auto fnv1aHash(const unsigned char* data, size_t len) -> uint64_t {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (size_t i = 0; i < len; ++i) {
    h ^= data[i];
    h *= 0x100000001b3ULL;
  }
  return h;
}

auto getCacheRoot() -> fs::path {
  if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) return xdg;
  if (const char* home = std::getenv("HOME"); home && *home)
    return fs::path(home) / ".cache";
  std::error_code ec;
  if (fs::path tmp = fs::temp_directory_path(ec); !ec) return tmp;
  return ".";
}

// Decompresses a gzip stream held fully in memory.  Returns an empty vector
// on failure.
auto gunzip(const unsigned char* data, size_t len)
    -> std::vector<unsigned char> {
  z_stream zs{};
  zs.next_in = const_cast<Bytef*>(data);
  zs.avail_in = static_cast<uInt>(len);
  // 15 window bits + 16 to expect a gzip (rather than zlib) header.
  if (inflateInit2(&zs, 15 + 16) != Z_OK) return {};

  std::vector<unsigned char> out;
  std::array<unsigned char, 1 << 16> chunk;
  int ret = Z_OK;
  while (ret == Z_OK) {
    zs.next_out = chunk.data();
    zs.avail_out = static_cast<uInt>(chunk.size());
    ret = inflate(&zs, Z_NO_FLUSH);
    if (ret != Z_OK && ret != Z_STREAM_END) {
      inflateEnd(&zs);
      return {};
    }
    out.insert(out.end(), chunk.data(),
               chunk.data() + (chunk.size() - zs.avail_out));
  }
  inflateEnd(&zs);
  return ret == Z_STREAM_END ? out : std::vector<unsigned char>{};
}

// Parses an octal numeric field from a tar header.
auto parseOctal(const char* field, size_t len) -> uint64_t {
  uint64_t value = 0;
  size_t i = 0;
  while (i < len && (field[i] == ' ' || field[i] == '\0')) ++i;
  for (; i < len && field[i] >= '0' && field[i] <= '7'; ++i)
    value = value * 8 + static_cast<uint64_t>(field[i] - '0');
  return value;
}

// Length of a NUL-terminated field with a fixed maximum, without strnlen
// (which is POSIX-only).
auto boundedStrlen(const char* field, size_t maxLen) -> size_t {
  const void* nul = std::memchr(field, '\0', maxLen);
  return nul ? static_cast<const char*>(nul) - field : maxLen;
}

// Rejects tar entry names that could escape the extraction directory.
auto isSafeEntryName(const std::string& name) -> bool {
  if (name.empty() || fs::path(name).is_absolute()) return false;
  for (const auto& component : fs::path(name)) {
    if (component == "..") return false;
  }
  return true;
}

// Extracts an uncompressed tar archive held in memory into `destDir`,
// creating regular files and directories only.  Returns false on failure.
auto extractTar(const std::vector<unsigned char>& tar, const fs::path& destDir)
    -> bool {
  constexpr size_t kBlockSize = 512;
  size_t offset = 0;
  std::string longName;  // GNU 'L' extension: name for the next entry.

  while (offset + kBlockSize <= tar.size()) {
    const char* header = reinterpret_cast<const char*>(tar.data()) + offset;
    if (header[0] == '\0') break;  // End-of-archive zero block.

    const uint64_t size = parseOctal(header + 124, 12);
    const char typeflag = header[156];
    std::string name = longName.empty()
                           ? std::string(header, boundedStrlen(header, 100))
                           : longName;
    longName.clear();
    offset += kBlockSize;

    // Entries must not claim more data than the archive holds.
    const uint64_t dataBlocks = (size + kBlockSize - 1) / kBlockSize;
    if (offset + dataBlocks * kBlockSize > tar.size()) return false;
    const char* data = reinterpret_cast<const char*>(tar.data()) + offset;

    if (typeflag == 'L') {  // GNU long name: applies to the next header.
      longName.assign(data, boundedStrlen(data, static_cast<size_t>(size)));
    } else if (typeflag == '0' || typeflag == '\0') {  // Regular file.
      if (!isSafeEntryName(name)) return false;
      const fs::path outPath = destDir / name;
      std::error_code ec;
      fs::create_directories(outPath.parent_path(), ec);
      if (ec) return false;
      std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
      if (!out) return false;
      out.write(data, static_cast<std::streamsize>(size));
      if (!out) return false;
    } else if (typeflag == '5') {  // Directory.
      if (!isSafeEntryName(name)) return false;
      std::error_code ec;
      fs::create_directories(destDir / name, ec);
      if (ec) return false;
    }
    // All other entry types (pax headers, links, ...) are skipped; their
    // payload is still stepped over below.

    offset += dataBlocks * kBlockSize;
  }
  return true;
}

// Creates and returns a unique sibling directory of `finalDir`, or an empty
// path on failure.  Portable replacement for mkdtemp().
auto makeTempDir(const fs::path& finalDir) -> fs::path {
  const auto seed = static_cast<unsigned long long>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  const std::string base = finalDir.filename().string();
  for (unsigned long long i = 0; i < 100; ++i) {
    fs::path candidate = finalDir.parent_path() /
                         (base + "." + std::to_string(seed + i) + ".tmp");
    std::error_code ec;
    if (fs::create_directory(candidate, ec)) return candidate;
    if (ec && ec != std::errc::file_exists) return {};
  }
  return {};
}

void removeAll(const fs::path& path) {
  std::error_code ec;
  fs::remove_all(path, ec);
}

}  // namespace

auto ensureClangResourceDir() -> std::string {
  char hashBuf[32];
  std::snprintf(hashBuf, sizeof(hashBuf), "%016llx",
                static_cast<unsigned long long>(fnv1aHash(
                    kClangIncludeHeadersTarGz, kClangIncludeHeadersTarGzSize)));
  const fs::path base = getCacheRoot() / "cpp_formatting";
  const fs::path cacheDir = base / ("clang_resource_" + std::string(hashBuf));

  std::error_code ec;
  if (fs::is_directory(cacheDir / "include", ec)) return cacheDir.string();

  if (!fs::create_directories(base, ec) && ec) return {};

  // Extract into a sibling temp directory then rename atomically.  Concurrent
  // invocations may both extract; the loser's rename fails harmlessly because
  // the target already exists, and we fall through to using it.
  const fs::path tmpDir = makeTempDir(cacheDir);
  if (tmpDir.empty()) return {};

  const std::vector<unsigned char> tar =
      gunzip(kClangIncludeHeadersTarGz, kClangIncludeHeadersTarGzSize);
  if (tar.empty() || !extractTar(tar, tmpDir)) {
    removeAll(tmpDir);
    return {};
  }

  fs::rename(tmpDir, cacheDir, ec);
  if (ec) {
    removeAll(tmpDir);
    if (!fs::is_directory(cacheDir / "include", ec)) return {};
  }

  return cacheDir.string();
}
