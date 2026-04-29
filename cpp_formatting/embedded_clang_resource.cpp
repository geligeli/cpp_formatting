#include "cpp_formatting/embedded_clang_resource.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

extern "C" const unsigned char kClangIncludeHeadersTarGz[];
extern "C" const size_t kClangIncludeHeadersTarGzSize;

namespace {

auto fnv1aHash(const unsigned char* data, size_t len) -> uint64_t {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (size_t i = 0; i < len; ++i) {
    h ^= data[i];
    h *= 0x100000001b3ULL;
  }
  return h;
}

auto getCacheRoot() -> std::string {
  if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) return xdg;
  if (const char* home = std::getenv("HOME"); home && *home)
    return std::string(home) + "/.cache";
  return "/tmp";
}

auto dirExists(const std::string& path) -> bool {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

auto mkdirP(const std::string& path) -> bool {
  if (path.empty() || dirExists(path)) return true;
  auto pos = path.find_last_of('/');
  if (pos != std::string::npos && pos > 0) {
    if (!mkdirP(path.substr(0, pos))) return false;
  }
  return ::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

auto shellQuote(const std::string& s) -> std::string {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'')
      out += "'\\''";
    else
      out += c;
  }
  out += "'";
  return out;
}

}  // namespace

auto ensureClangResourceDir() -> std::string {
  char hashBuf[32];
  std::snprintf(hashBuf, sizeof(hashBuf), "%016lx",
                static_cast<unsigned long>(fnv1aHash(
                    kClangIncludeHeadersTarGz, kClangIncludeHeadersTarGzSize)));
  std::string base = getCacheRoot() + "/cpp_formatting";
  std::string cacheDir = base + "/clang_resource_" + hashBuf;

  if (dirExists(cacheDir + "/include")) return cacheDir;

  if (!mkdirP(base)) return {};

  // Extract into a sibling temp directory then rename atomically.  Concurrent
  // invocations may both extract; the loser's rename fails harmlessly because
  // the target already exists, and we fall through to using it.
  char tmpTemplate[1024];
  std::snprintf(tmpTemplate, sizeof(tmpTemplate), "%s/clang_resource_%s.XXXXXX",
                base.c_str(), hashBuf);
  if (::mkdtemp(tmpTemplate) == nullptr) return {};
  std::string tmpDir = tmpTemplate;

  std::string tarPath = tmpDir + "/headers.tar.gz";
  {
    std::ofstream out(tarPath, std::ios::binary | std::ios::trunc);
    if (!out) {
      std::system(("rm -rf " + shellQuote(tmpDir)).c_str());
      return {};
    }
    out.write(reinterpret_cast<const char*>(kClangIncludeHeadersTarGz),
              static_cast<std::streamsize>(kClangIncludeHeadersTarGzSize));
  }

  std::string cmd =
      "tar -xzf " + shellQuote(tarPath) + " -C " + shellQuote(tmpDir);
  if (std::system(cmd.c_str()) != 0) {
    std::system(("rm -rf " + shellQuote(tmpDir)).c_str());
    return {};
  }
  std::remove(tarPath.c_str());

  if (::rename(tmpDir.c_str(), cacheDir.c_str()) != 0) {
    std::system(("rm -rf " + shellQuote(tmpDir)).c_str());
    if (!dirExists(cacheDir + "/include")) return {};
  }

  return cacheDir;
}
