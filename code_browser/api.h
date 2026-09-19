// The JSON API, independent of any HTTP library: a request in, a response
// out.  The HTTP layer only copies fields; the tests call Handle() directly.
//
// Routes (GET and HEAD; everything else is 405):
//   /api/repo                              RepoInfo
//   /api/files?prefix=<dir>                FileList (one level; "" = root)
//   /api/file?path=<p>                     the bytes, text/plain
//   /api/annotations?path=<p>              Annotations
//   /api/symbol/<id>, /api/symbol?usr=<u>  SymbolInfo
//   /api/refs/<id>?role=&exclude=&file=&offset=&limit=   References
//   /api/search?q=&limit=&kind=&locals=1   SearchResults
//   /api/at?path=<p>&offset=<n>            OccurrencesAt
// Errors are an Error message with the matching status.  Responses that
// depend only on the index carry its ETag; a matching If-None-Match gets a
// 304 with no body.
#ifndef CODE_BROWSER_API_H_
#define CODE_BROWSER_API_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "code_browser/file_cache.h"
#include "code_browser/index_db.h"
#include "code_browser/repo.h"
#include "google/protobuf/message.h"

namespace code_browser {

struct ApiRequest {
  std::string method;  // "GET", "HEAD", ...
  std::string path;    // percent-decoded, without the query
  std::string query;   // raw, after the '?'
  std::string if_none_match;
};

struct ApiResponse {
  int status = 200;
  std::string content_type = "application/json; charset=utf-8";
  std::string body;
  std::string etag;  // quoted, or ""
  std::string cache_control = "no-cache";
  std::vector<std::pair<std::string, std::string>> headers;  // extras
};

class ApiHandler {
 public:
  ApiHandler(const IndexDb& db, const Repo& repo, FileCache& files)
      : db_(db), repo_(repo), files_(files) {}

  // Never throws; unknown /api/* paths are 404, other paths too.
  auto Handle(const ApiRequest& request) const -> ApiResponse;

 private:
  auto RepoInfo() const -> ApiResponse;
  auto Files(const ApiRequest&) const -> ApiResponse;
  auto FileBytes(const ApiRequest&) const -> ApiResponse;
  auto Annotations(const ApiRequest&) const -> ApiResponse;
  auto SymbolInfo(const ApiRequest&, std::string_view rest) const
      -> ApiResponse;
  auto References(const ApiRequest&, std::string_view rest) const
      -> ApiResponse;
  auto Search(const ApiRequest&) const -> ApiResponse;
  auto At(const ApiRequest&) const -> ApiResponse;

  const IndexDb& db_;
  const Repo& repo_;
  FileCache& files_;
};

// application/x-www-form-urlencoded-ish: `a=1&b=x%20y&c` -> [(a,1),(b,x y),
// (c,"")]; a bad escape drops the pair.
auto ParseQuery(std::string_view query)
    -> std::vector<std::pair<std::string, std::string>>;
// Percent-decoding; nullopt on a malformed escape or an embedded NUL.
auto PercentDecode(std::string_view s) -> std::optional<std::string>;
// A role set: a number, or `|`-joined names ("DEFINITION|CALL").
auto ParseRoles(std::string_view spec) -> std::optional<uint32_t>;
// proto3 JSON with the field names as declared.
auto ToJson(const google::protobuf::Message& message) -> std::string;
auto ErrorResponse(int status, std::string_view message) -> ApiResponse;

}  // namespace code_browser

#endif  // CODE_BROWSER_API_H_
