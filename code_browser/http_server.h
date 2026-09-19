// The HTTP front: Boost.Beast over a multi-threaded Asio io_context, one
// coroutine per connection.  Knows how to move bytes and headers; the API
// layer decides what they mean.  Beast and Asio appear only in the .cpp.
#ifndef CODE_BROWSER_HTTP_SERVER_H_
#define CODE_BROWSER_HTTP_SERVER_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "code_browser/api.h"
#include "code_browser/static_assets.h"

namespace code_browser {

struct ServerOptions {
  std::string address = "127.0.0.1";
  uint16_t port = 8080;                   // 0: any free port; see bound_port()
  unsigned threads = 0;                   // 0: one per hardware thread
  std::chrono::seconds idle_timeout{30};  // per read/write, and keep-alive idle
  std::chrono::seconds drain_timeout{5};  // on Stop(): in-flight responses
  size_t max_target_bytes = 8192;
  bool log_requests = true;  // one line per request on stderr
};

class HttpServer {
 public:
  HttpServer(ServerOptions options, const ApiHandler& api,
             const StaticAssets& assets);
  ~HttpServer();
  HttpServer(const HttpServer&) = delete;
  auto operator=(const HttpServer&) -> HttpServer& = delete;

  // Binds, listens, starts the threads.  False (with `error`) if the address
  // cannot be bound.
  auto Start(std::string* error) -> bool;
  auto bound_port() const -> uint16_t;
  // Stops accepting, lets in-flight requests finish (up to drain_timeout),
  // stops the io threads and joins them.  Idempotent; safe from any thread
  // that is not an io thread.
  void Stop();
  // SIGINT/SIGTERM initiate the same shutdown; Join() then returns.
  void InstallSignalHandlers();
  // Blocks until the server has stopped.
  void Join();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace code_browser

#endif  // CODE_BROWSER_HTTP_SERVER_H_
