#include "code_browser/http_server.h"

#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "code_browser/api.h"
#include "code_browser/file_cache.h"
#include "code_browser/index_db.h"
#include "code_browser/index_schema.h"
#include "code_browser/repo.h"
#include "code_browser/static_assets.h"
#include "cpp_formatting/cpp_index_merge.h"
#include "cpp_formatting/index.pb.h"

namespace code_browser {
namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace fs = std::filesystem;
using tcp = net::ip::tcp;

struct Reply {
  int status = 0;
  std::string body;
  http::response<http::string_body> res;
};

// A blocking client on its own io_context; one request per connection
// unless `stream` is given, in which case the connection is reused.
auto Request(uint16_t port, const std::string& target,
             http::verb method = http::verb::get,
             std::vector<std::pair<std::string, std::string>> headers = {},
             beast::tcp_stream* reuse = nullptr) -> Reply {
  net::io_context ioc;
  std::unique_ptr<beast::tcp_stream> owned;
  beast::tcp_stream* stream = reuse;
  if (!stream) {
    owned = std::make_unique<beast::tcp_stream>(ioc);
    owned->connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), port));
    stream = owned.get();
  }
  http::request<http::empty_body> req{method, target, 11};
  req.set(http::field::host, "localhost");
  for (const auto& [k, v] : headers) req.set(k, v);
  if (!reuse) req.keep_alive(false);
  http::write(*stream, req);
  beast::flat_buffer buffer;
  http::response_parser<http::string_body> parser;
  if (method == http::verb::head) parser.skip(true);
  http::read(*stream, buffer, parser);
  Reply r;
  r.res = parser.release();
  r.status = r.res.result_int();
  r.body = r.res.body();
  return r;
}

class HttpServerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() /
           ("code_browser_http_test_" +
            std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
            "_" + std::to_string(counter_++));
    fs::remove_all(dir_);
    fs::create_directories(dir_);
    std::ofstream(dir_ / "a.h") << "int a;\n";
    cpp_index::IndexUnit u;
    u.set_producer("test");
    cpp_index::File* f = u.add_files();
    f->set_path("a.h");
    f->set_kind(cpp_index::SOURCE);
    cpp_index::Symbol* s = u.add_symbols();
    s->set_usr("c:@a");
    s->set_name("a");
    s->set_qualified_name("a");
    s->set_kind(cpp_index::VARIABLE);
    cpp_index::Occurrence* o = u.add_occurrences();
    o->set_file(0);
    o->set_begin(4);
    o->set_end(5);
    o->set_symbol(0);
    o->set_roles(cpp_index::DEFINITION | cpp_index::DECLARATION);
    normalizeUnit(u);
    std::string error;
    db_ = IndexDb::FromIndex(buildIndex(u), ImportOptions{"t.pb"}, &error);
    ASSERT_TRUE(db_) << error;
    RepoOptions ropts;
    ropts.root = dir_;
    repo_ = std::make_unique<Repo>(*Repo::Open(ropts, &error));
    files_ = std::make_unique<FileCache>(1 << 20);
    api_ = std::make_unique<ApiHandler>(*db_, *repo_, *files_);
    assets_ = std::make_unique<StaticAssets>(StaticAssets::FromEmbedded());
  }
  void TearDown() override { fs::remove_all(dir_); }

  auto Serve(ServerOptions opts = {}) -> std::unique_ptr<HttpServer> {
    opts.port = 0;
    opts.log_requests = false;
    if (opts.threads == 0) opts.threads = 4;
    auto server = std::make_unique<HttpServer>(opts, *api_, *assets_);
    std::string error;
    EXPECT_TRUE(server->Start(&error)) << error;
    EXPECT_NE(server->bound_port(), 0);
    return server;
  }

  fs::path dir_;
  std::unique_ptr<IndexDb> db_;
  std::unique_ptr<Repo> repo_;
  std::unique_ptr<FileCache> files_;
  std::unique_ptr<ApiHandler> api_;
  std::unique_ptr<StaticAssets> assets_;
  static inline int counter_ = 0;
};

TEST_F(HttpServerTest, ServesApiAndStatic) {
  const std::unique_ptr<HttpServer> server = Serve();
  const uint16_t port = server->bound_port();
  Reply r = Request(port, "/api/repo");
  EXPECT_EQ(r.status, 200);
  EXPECT_EQ(r.res[http::field::content_type],
            "application/json; charset=utf-8");
  EXPECT_NE(r.body.find("\"files\""), std::string::npos);
  EXPECT_EQ(r.res[http::field::server], "code_browser");

  r = Request(port, "/");
  EXPECT_EQ(r.status, 200);
  EXPECT_EQ(r.res[http::field::content_type], "text/html; charset=utf-8");
  EXPECT_NE(r.body.find("<html"), std::string::npos);
  const std::string etag(r.res[http::field::etag]);
  ASSERT_FALSE(etag.empty());
  r = Request(port, "/app.js");
  EXPECT_EQ(r.status, 200);
  r = Request(port, "/", http::verb::get, {{"If-None-Match", etag}});
  EXPECT_EQ(r.status, 304);
  EXPECT_TRUE(r.body.empty());

  EXPECT_EQ(Request(port, "/nope").status, 404);
  EXPECT_EQ(Request(port, "/api/nope").status, 404);
  EXPECT_EQ(Request(port, "/api/file?path=a.h").body, "int a;\n");
  EXPECT_EQ(Request(port, "/api/file?path=%2e%2e/x").status, 400);
  EXPECT_EQ(Request(port, "/api/repo", http::verb::post).status, 405);
  EXPECT_EQ(Request(port, "/", http::verb::post).status, 405);
  EXPECT_EQ(Request(port, "/" + std::string(9000, 'x')).status, 414);
}

TEST_F(HttpServerTest, HeadHasLengthAndNoBody) {
  const std::unique_ptr<HttpServer> server = Serve();
  const Reply r =
      Request(server->bound_port(), "/api/file?path=a.h", http::verb::head);
  EXPECT_EQ(r.status, 200);
  EXPECT_EQ(r.res[http::field::content_length], "7");
  EXPECT_TRUE(r.body.empty());
}

TEST_F(HttpServerTest, KeepAliveServesSeveralRequestsOnOneConnection) {
  const std::unique_ptr<HttpServer> server = Serve();
  net::io_context ioc;
  beast::tcp_stream stream(ioc);
  stream.connect(
      tcp::endpoint(net::ip::make_address("127.0.0.1"), server->bound_port()));
  for (int i = 0; i < 3; ++i) {
    const Reply r = Request(0, "/api/repo", http::verb::get, {}, &stream);
    EXPECT_EQ(r.status, 200);
    EXPECT_TRUE(r.res.keep_alive());
  }
}

TEST_F(HttpServerTest, ManyConcurrentClients) {
  const std::unique_ptr<HttpServer> server = Serve();
  const uint16_t port = server->bound_port();
  std::vector<std::thread> clients;
  std::atomic<int> ok{0};
  for (int t = 0; t < 32; ++t)
    clients.emplace_back([&] {
      for (int i = 0; i < 20; ++i)
        if (Request(port, i % 2 ? "/api/repo" : "/api/annotations?path=a.h")
                .status == 200)
          ++ok;
    });
  for (std::thread& t : clients) t.join();
  EXPECT_EQ(ok.load(), 32 * 20);
}

TEST_F(HttpServerTest, IdleConnectionsAreClosed) {
  ServerOptions opts;
  opts.idle_timeout = std::chrono::seconds(1);
  const std::unique_ptr<HttpServer> server = Serve(opts);
  net::io_context ioc;
  tcp::socket socket(ioc);
  socket.connect(
      tcp::endpoint(net::ip::make_address("127.0.0.1"), server->bound_port()));
  // Say nothing; the server hangs up after the idle timeout.
  char byte = 0;
  beast::error_code ec;
  const auto started = std::chrono::steady_clock::now();
  socket.read_some(net::buffer(&byte, 1), ec);
  EXPECT_TRUE(ec);  // eof
  EXPECT_LT(std::chrono::steady_clock::now() - started,
            std::chrono::seconds(10));
}

TEST_F(HttpServerTest, StopReturnsWithAnIdleClientAttached) {
  std::unique_ptr<HttpServer> server = Serve();
  net::io_context ioc;
  tcp::socket socket(ioc);
  socket.connect(
      tcp::endpoint(net::ip::make_address("127.0.0.1"), server->bound_port()));
  const auto started = std::chrono::steady_clock::now();
  server->Stop();
  server->Stop();  // idempotent
  EXPECT_LT(std::chrono::steady_clock::now() - started,
            std::chrono::seconds(10));
}

TEST_F(HttpServerTest, SecondBindOnTheSamePortFails) {
  const std::unique_ptr<HttpServer> server = Serve();
  ServerOptions opts;
  opts.port = server->bound_port();
  opts.threads = 1;
  opts.log_requests = false;
  HttpServer second(opts, *api_, *assets_);
  std::string error;
  EXPECT_FALSE(second.Start(&error));
  EXPECT_FALSE(error.empty());
}

}  // namespace
}  // namespace code_browser
