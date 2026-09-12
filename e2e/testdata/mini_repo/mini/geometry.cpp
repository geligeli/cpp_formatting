#include "mini/geometry.h"

namespace mini {

int total_area(const Rect& a, const Rect& b) { return a.area() + b.area(); }

// Instantiates width_of<Rect>, which is what lets the dependent token in
// shapes.h be resolved from this TU.
int wider_of(const Rect& a, const Rect& b) {
  return width_of(a) > width_of(b) ? a.widthPx : b.widthPx;
}

}  // namespace mini
