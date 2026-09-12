#include <cstdio>

#include "mini/geometry.h"

// A plain cc_test -- no gtest dependency, so the fixture needs nothing from the
// registry beyond rules_cc.
#define CHECK_EQ(expr, want)                                              \
  do {                                                                    \
    const int got_ = (expr);                                              \
    if (got_ != (want)) {                                                 \
      std::printf("%s:%d: %s = %d, want %d\n", __FILE__, __LINE__, #expr, \
                  got_, (want));                                          \
      return 1;                                                           \
    }                                                                     \
  } while (0)

int main() {
  const mini::Rect a(2, 3);
  const mini::Rect unit(1, 1);

  CHECK_EQ(a.area(), 6);
  CHECK_EQ(a.perimeter(), 10);
  CHECK_EQ(mini::width_of(a), 2);
  CHECK_EQ(mini::total_area(a, unit), 7);
  CHECK_EQ(mini::wider_of(a, unit), 2);
  return 0;
}
