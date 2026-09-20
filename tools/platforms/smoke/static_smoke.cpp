// What a fully static link has to get right, in one process: the startup files
// (it runs at all), TLS set up without a dynamic loader, threads, libm, C++
// exceptions unwinding through libunwind, iostreams' static initialisers -- and
// that it *is* static.  Built for //tools/platforms:linux_*_glibc_static by the
// transition in :for_platform.bzl; see the BUILD file.
#include <sys/auxv.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static thread_local int tls_value = 7;

int main(int argc, char**) {
  // The kernel reports where it mapped the program interpreter; a static
  // executable has none.
  if (getauxval(AT_BASE) != 0) {
    std::fprintf(stderr, "FAIL: a dynamic loader is mapped (AT_BASE != 0)\n");
    return 1;
  }

  std::atomic<int> sum{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([&sum, i] {
      tls_value += i;  // each thread starts from its own 7
      sum += tls_value;
    });
  }
  for (std::thread& t : threads) t.join();
  if (sum != 4 * 7 + 0 + 1 + 2 + 3) {
    std::fprintf(stderr, "FAIL: TLS/threads: sum=%d\n", sum.load());
    return 1;
  }

  bool caught = false;
  try {
    throw std::runtime_error("unwound");
  } catch (const std::exception& e) {
    caught = std::string(e.what()) == "unwound";
  }
  if (!caught) {
    std::fprintf(stderr, "FAIL: exception not caught\n");
    return 1;
  }

  const double root = std::sqrt(static_cast<double>(argc) + 1.0);  // argc == 1
  if (std::fabs(root * root - 2.0) > 1e-12) {
    std::fprintf(stderr, "FAIL: libm: sqrt(2)=%f\n", root);
    return 1;
  }

  std::cout << "PASS" << std::endl;
  return 0;
}
