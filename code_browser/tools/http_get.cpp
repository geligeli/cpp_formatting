// http_get: a blocking HTTP/1.1 client for the smoke test, so the test needs
// no curl.  Prints the body on stdout; with -I, the status line and headers
// instead.  Exits 22 on a non-2xx status (curl -f), 1 on a connection error.
//
//   http_get [-I] [-H 'Name: value']... http://host:port/path
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

int main(int argc, char** argv) {
  bool head = false;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string url;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-I") {
      head = true;
    } else if (arg == "-H" && i + 1 < argc) {
      const std::string h = argv[++i];
      const size_t colon = h.find(':');
      if (colon == std::string::npos) {
        std::fprintf(stderr, "http_get: bad header %s\n", h.c_str());
        return 1;
      }
      size_t v = colon + 1;
      while (v < h.size() && h[v] == ' ') ++v;
      headers.emplace_back(h.substr(0, colon), h.substr(v));
    } else {
      url = arg;
    }
  }
  if (url.rfind("http://", 0) != 0) {
    std::fprintf(stderr,
                 "usage: http_get [-I] [-H 'k: v'] http://host:port/path\n");
    return 1;
  }
  const std::string rest = url.substr(7);
  const size_t slash = rest.find('/');
  const std::string host_port = rest.substr(0, slash);
  const std::string target =
      slash == std::string::npos ? "/" : rest.substr(slash);
  const size_t colon = host_port.rfind(':');
  const std::string host = host_port.substr(0, colon);
  const std::string port =
      colon == std::string::npos ? "80" : host_port.substr(colon + 1);

  try {
    net::io_context ioc;
    tcp::resolver resolver(ioc);
    beast::tcp_stream stream(ioc);
    stream.connect(resolver.resolve(host, port));
    http::request<http::empty_body> req{
        head ? http::verb::head : http::verb::get, target, 11};
    req.set(http::field::host, host_port);
    req.set(http::field::user_agent, "http_get");
    for (const auto& [k, v] : headers) req.set(k, v);
    http::write(stream, req);
    beast::flat_buffer buffer;
    http::response_parser<http::string_body> parser;
    parser.body_limit(256u << 20);
    if (head) parser.skip(true);
    http::read(stream, buffer, parser);
    const http::response<http::string_body>& res = parser.get();
    beast::error_code ignored;
    stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
    if (head) {
      std::cout << "HTTP/1.1 " << res.result_int() << " " << res.reason()
                << "\n";
      for (const auto& f : res)
        std::cout << f.name_string() << ": " << f.value() << "\n";
    } else {
      std::cout << res.body();
      std::cout.flush();
    }
    std::fprintf(stderr, "http_get: %d\n", res.result_int());
    return res.result_int() / 100 == 2 ? 0 : 22;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "http_get: %s\n", e.what());
    return 1;
  }
}
