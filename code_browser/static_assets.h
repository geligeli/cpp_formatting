// The browser's pages: three files, embedded in the binary at build time, or
// read from a directory on every request while developing them.
#ifndef CODE_BROWSER_STATIC_ASSETS_H_
#define CODE_BROWSER_STATIC_ASSETS_H_

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace code_browser {

struct StaticAsset {
  std::string content_type;
  std::string body;
  std::string etag;           // quoted; "" in directory mode
  std::string cache_control;  // "max-age=0, must-revalidate" or "no-store"
};

class StaticAssets {
 public:
  static auto FromEmbedded() -> StaticAssets;
  static auto FromDirectory(std::filesystem::path dir) -> StaticAssets;

  // "/", "/index.html", "/app.js", "/app.css"; nullopt for anything else.
  auto Get(std::string_view request_path) const -> std::optional<StaticAsset>;
  auto directory() const -> const std::optional<std::filesystem::path>& {
    return dir_;
  }

 private:
  struct Embedded {
    std::string content_type;
    std::string_view body;
    std::string etag;
  };
  std::optional<std::filesystem::path> dir_;
  Embedded index_html_, app_js_, app_css_;
};

}  // namespace code_browser

#endif  // CODE_BROWSER_STATIC_ASSETS_H_
