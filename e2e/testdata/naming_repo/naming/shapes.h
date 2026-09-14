#ifndef NAMING_SHAPES_H_
#define NAMING_SHAPES_H_

#include <vector>

// A namespace, types and constants written against the Google style on
// purpose: the `type` and `namespace` rules have to move every spelling of
// them, across the three targets, and leave the STL protocol names alone.
namespace GeoKit {

constexpr int max_width = 4096;
const int kMaxShapes = 64;

struct rect_data {
  struct dims {
    int w = 0;
    int h = 0;
  };
  dims size;
  int width_px = 0;
  int height_px = 0;
};

enum shape_kind { kRect, kSquare };

typedef int pixel_count;
using pixel_list = std::vector<pixel_count>;

// `value_type` and `iterator` are what the standard library reads: they stay.
template <class T>
struct box_of {
  typedef T value_type;
  struct iterator {};
  T item;
};

// A dependent type name, resolved from the instantiations in shapes.cpp and
// shapes_test.cpp (which own neither this header's token nor each other).
template <class R>
typename R::dims dims_of(const R& r) {
  return r.size;
}

class area_calculator {
 public:
  area_calculator();
  ~area_calculator();
  pixel_count Area(const rect_data& r) const;
  shape_kind Classify(const rect_data& r) const;
  static box_of<rect_data> Boxed(const rect_data& r);
  int Calls() const { return calls_; }

 private:
  int calls_ = 0;
};

namespace Detail {
pixel_count widest(const pixel_list& widths);
}  // namespace Detail

}  // namespace GeoKit

#endif  // NAMING_SHAPES_H_
