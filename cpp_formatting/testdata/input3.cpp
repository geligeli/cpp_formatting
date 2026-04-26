#include <cstddef>
#include <ostream>

std::size_t byteLength(const char* s) {
  std::size_t n = 0;
  while (s[n]) ++n;
  return n;
}

std::ostream& print(std::ostream& os, const char* s) { return os << s; }
