#include <cstdlib>
#include <iostream>

#include "naming/report.h"

namespace {

template <class B>
typename B::value_type unbox(const B& b) {
  return b.item;
}

void check(bool ok, const char* what) {
  if (ok) return;
  std::cerr << "FAIL: " << what << "\n";
  std::exit(1);
}

}  // namespace

int main() {
  GeoKit::rect_data r;
  r.width_px = 2;
  r.height_px = 3;
  r.size.w = 1;
  r.size.h = 1;
  GeoKit::area_calculator calc;
  check(calc.Area(r) == 7, "area");
  check(calc.Classify(r) == GeoKit::kRect, "kind");
  check(unbox(GeoKit::area_calculator::Boxed(r)).width_px == 2, "boxed");
  const GeoKit::rect_data::dims d = GeoKit::dims_of(r);
  check(d.w == 1 && d.h == 1, "dims");
  check(GeoKit::Detail::widest({1, 9, 5}) == 9, "widest");
  check(GeoKit::kMaxShapes == 64 && GeoKit::max_width == 4096, "constants");
  std::cout << "OK\n";
  return 0;
}
