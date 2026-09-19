#include "code_browser/http_server.h"

#include <atomic>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <cstdio>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace code_browser {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace {

constexpr auto kAsTuple = net::as_tuple(net::use_awaitable);
constexpr size_t kHeaderLimit = 16 * 1024;
constexpr size_t kBodyLimit = 64 * 1024;  // a POST is refused, not read forever

// What a request became, before it is put on the wire.
struct Reply {
  int status = 200;
  std::string content_type;
  std::string body;
  std::string etag;
  std::string cache_control;
  std::vector<std::pair<std::string, std::string>> headers;
};

auto Route(const ApiHandler& api, const StaticAssets& assets,
           const http::request<http::string_body>& req, size_t max_target_bytes)
    -> Reply {
  const std::string_view target = req.target();
  if (target.size() > max_target_bytes) {
    const ApiResponse r = ErrorResponse(414, "request target too long");
    return {r.status, r.content_type, r.body, "", r.cache_control, {}};
  }
  const size_t q = target.find('?');
  const std::string_view raw_path = target.substr(0, q);
  const std::string_view query =
      q == std::string_view::npos ? std::string_view() : target.substr(q + 1);
  const std::optional<std::string> path = PercentDecode(raw_path);
  const std::string method(req.method_string());
  const bool read_like =
      req.method() == http::verb::get || req.method() == http::verb::head;
  if (!path) {
    const ApiResponse r = ErrorResponse(400, "malformed request target");
    return {r.status, r.content_type, r.body, "", r.cache_control, {}};
  }
  const std::string if_none_match(req[http::field::if_none_match]);
  if (path->rfind("/api/", 0) == 0) {
    ApiResponse r =
        api.Handle({method, *path, std::string(query), if_none_match});
    return {
        r.status,          std::move(r.content_type),  std::move(r.body),
        std::move(r.etag), std::move(r.cache_control), std::move(r.headers)};
  }
  if (!read_like) {
    const ApiResponse r = ErrorResponse(405, "method not allowed");
    return {r.status, r.content_type,  r.body,
            "",       r.cache_control, {{"Allow", "GET, HEAD"}}};
  }
  const std::optional<StaticAsset> asset = assets.Get(*path);
  if (!asset) {
    return {404, "text/plain; charset=utf-8", "not found\n", "", "no-store",
            {}};
  }
  Reply reply{200,         asset->content_type,  asset->body,
              asset->etag, asset->cache_control, {}};
  if (!asset->etag.empty() &&
      if_none_match.find(asset->etag) != std::string::npos) {
    reply.status = 304;
    reply.body.clear();
  }
  return reply;
}

}  // namespace

struct HttpServer::Impl {
  Impl(ServerOptions o, const ApiHandler& api, const StaticAssets& assets)
      : options(std::move(o)),
        api(api),
        assets(assets),
        ioc(static_cast<int>(ThreadCount(options))),
        acceptor(ioc),
        signals(ioc),
        drain_timer(ioc) {}

  static auto ThreadCount(const ServerOptions& o) -> unsigned {
    if (o.threads != 0) return o.threads;
    const unsigned hw = std::thread::hardware_concurrency();
    return hw == 0 ? 4 : hw;
  }

  auto Session(tcp::socket socket) -> net::awaitable<void> {
    ++active_sessions;
    try {
      co_await SessionBody(std::move(socket));
    } catch (const std::exception& e) {
      std::fprintf(stderr, "code_browser: session error: %s\n", e.what());
    }
    --active_sessions;
  }

  auto SessionBody(tcp::socket socket) -> net::awaitable<void> {
    beast::tcp_stream stream(std::move(socket));
    std::string remote;
    {
      beast::error_code ec;
      const tcp::endpoint ep = stream.socket().remote_endpoint(ec);
      if (!ec)
        remote = ep.address().to_string() + ":" + std::to_string(ep.port());
    }
    beast::flat_buffer buffer;
    for (;;) {
      stream.expires_after(options.idle_timeout);
      http::request_parser<http::string_body> parser;
      parser.header_limit(kHeaderLimit);
      parser.body_limit(kBodyLimit);
      const auto [read_ec, read_n] =
          co_await http::async_read(stream, buffer, parser, kAsTuple);
      if (read_ec) break;  // end of stream, timeout, or a malformed request
      const http::request<http::string_body>& req = parser.get();
      const auto started = std::chrono::steady_clock::now();

      Reply reply = Route(api, assets, req, options.max_target_bytes);
      const bool keep_alive = req.keep_alive() && !stopping.load();

      http::response<http::string_body> res(
          static_cast<http::status>(reply.status), req.version());
      res.set(http::field::server, "code_browser");
      if (!reply.content_type.empty())
        res.set(http::field::content_type, reply.content_type);
      if (!reply.etag.empty()) res.set(http::field::etag, reply.etag);
      if (!reply.cache_control.empty())
        res.set(http::field::cache_control, reply.cache_control);
      for (const auto& [name, value] : reply.headers) res.set(name, value);
      res.keep_alive(keep_alive);
      const size_t body_bytes = reply.body.size();
      stream.expires_after(options.idle_timeout);
      beast::error_code write_ec;
      if (req.method() == http::verb::head || reply.status == 304) {
        // The headers of the full response, and no body.
        http::response<http::empty_body> head(res.base());
        head.keep_alive(keep_alive);
        if (req.method() == http::verb::head && reply.status != 304)
          head.content_length(body_bytes);
        else
          head.content_length(0);
        auto [ec, n] = co_await http::async_write(stream, head, kAsTuple);
        write_ec = ec;
      } else {
        res.body() = std::move(reply.body);
        res.prepare_payload();
        auto [ec, n] = co_await http::async_write(stream, res, kAsTuple);
        write_ec = ec;
      }
      if (options.log_requests) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count();
        std::fprintf(
            stderr, "%s %s %.*s %d %zu %lldms\n", remote.c_str(),
            std::string(req.method_string()).c_str(),
            static_cast<int>(std::min<size_t>(req.target().size(), 512)),
            req.target().data(), reply.status, body_bytes,
            static_cast<long long>(ms));
      }
      if (write_ec || !keep_alive) break;
    }
    beast::error_code ignored;
    stream.socket().shutdown(tcp::socket::shutdown_send, ignored);
  }

  auto Accept() -> net::awaitable<void> {
    for (;;) {
      auto [ec, socket] = co_await acceptor.async_accept(kAsTuple);
      if (ec) {
        if (stopping.load() || !acceptor.is_open()) co_return;
        continue;  // a transient accept failure; keep serving
      }
      net::co_spawn(ioc, Session(std::move(socket)), net::detached);
    }
  }

  // Runs on an io thread.  Closes the door, then waits for the sessions to
  // finish before stopping the context.
  void InitiateStop() {
    if (stopping.exchange(true)) return;
    beast::error_code ec;
    acceptor.close(ec);
    signals.cancel();
    drain_deadline = std::chrono::steady_clock::now() + options.drain_timeout;
    Drain();
  }

  void Drain() {
    if (active_sessions.load() == 0 ||
        std::chrono::steady_clock::now() >= drain_deadline) {
      ioc.stop();
      return;
    }
    drain_timer.expires_after(std::chrono::milliseconds(20));
    drain_timer.async_wait([this](const beast::error_code& ec) {
      if (!ec) Drain();
    });
  }

  auto OnIoThread() const -> bool {
    const std::thread::id me = std::this_thread::get_id();
    for (const std::thread& t : threads)
      if (t.get_id() == me) return true;
    return false;
  }

  ServerOptions options;
  const ApiHandler& api;
  const StaticAssets& assets;
  net::io_context ioc;
  tcp::acceptor acceptor;
  net::signal_set signals;
  net::steady_timer drain_timer;
  std::vector<std::thread> threads;
  std::atomic<int> active_sessions{0};
  std::atomic<bool> stopping{false};
  std::atomic<bool> started{false};
  std::chrono::steady_clock::time_point drain_deadline;
  std::mutex join_mutex;
};

HttpServer::HttpServer(ServerOptions options, const ApiHandler& api,
                       const StaticAssets& assets)
    : impl_(std::make_unique<Impl>(std::move(options), api, assets)) {}

HttpServer::~HttpServer() { Stop(); }

auto HttpServer::Start(std::string* error) -> bool {
  Impl& s = *impl_;
  beast::error_code ec;
  const auto address = net::ip::make_address(s.options.address, ec);
  if (ec) {
    if (error)
      *error = "bad address " + s.options.address + ": " + ec.message();
    return false;
  }
  const tcp::endpoint endpoint(address, s.options.port);
  s.acceptor.open(endpoint.protocol(), ec);
  if (!ec) s.acceptor.set_option(net::socket_base::reuse_address(true), ec);
  if (!ec) s.acceptor.bind(endpoint, ec);
  if (!ec) s.acceptor.listen(net::socket_base::max_listen_connections, ec);
  if (ec) {
    if (error)
      *error = "cannot listen on " + s.options.address + ":" +
               std::to_string(s.options.port) + ": " + ec.message();
    beast::error_code ignored;
    s.acceptor.close(ignored);
    return false;
  }
  net::co_spawn(s.ioc, s.Accept(), net::detached);
  const unsigned n = Impl::ThreadCount(s.options);
  for (unsigned i = 0; i < n; ++i)
    s.threads.emplace_back([&s] { s.ioc.run(); });
  s.started = true;
  return true;
}

auto HttpServer::bound_port() const -> uint16_t {
  beast::error_code ec;
  const tcp::endpoint ep = impl_->acceptor.local_endpoint(ec);
  return ec ? 0 : ep.port();
}

void HttpServer::InstallSignalHandlers() {
  Impl& s = *impl_;
  s.signals.add(SIGINT);
  s.signals.add(SIGTERM);
  s.signals.async_wait([&s](const beast::error_code& ec, int) {
    if (!ec) s.InitiateStop();
  });
}

void HttpServer::Stop() {
  Impl& s = *impl_;
  if (!s.started.load()) return;
  net::post(s.ioc, [&s] { s.InitiateStop(); });
  if (!s.OnIoThread()) Join();
}

void HttpServer::Join() {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.join_mutex);
  for (std::thread& t : s.threads)
    if (t.joinable()) t.join();
}

}  // namespace code_browser
