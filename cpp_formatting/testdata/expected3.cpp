#include <cstddef>
#include <ostream>

auto byteLength(const char* s) -> std::size_t {
    std::size_t n = 0;
    while (s[n]) ++n;
    return n;
}

auto print(std::ostream& os, const char* s) -> std::ostream& {
    return os << s;
}
