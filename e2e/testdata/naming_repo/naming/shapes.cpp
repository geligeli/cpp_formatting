#include "naming/shapes.h"

namespace GeoKit {

area_calculator::area_calculator() = default;
area_calculator::~area_calculator() = default;

pixel_count area_calculator::Area(const rect_data& r) const {
  const rect_data::dims d = dims_of(r);
  return d.w * d.h + r.width_px * r.height_px;
}

shape_kind area_calculator::Classify(const rect_data& r) const {
  return r.width_px == r.height_px ? kSquare : kRect;
}

box_of<rect_data> area_calculator::Boxed(const rect_data& r) {
  box_of<rect_data> b;
  b.item = r;
  return b;
}

namespace Detail {
pixel_count widest(const pixel_list& widths) {
  pixel_count best = 0;
  for (pixel_count w : widths)
    if (w > best && w <= max_width) best = w;
  return best;
}
}  // namespace Detail

}  // namespace GeoKit
