#include "mini/geometry.h"

int main() {
  const mini::Rect a(3, 4);
  const mini::Rect b(5, 6);
  return mini::total_area(a, b) == 42 ? 0 : 1;
}
