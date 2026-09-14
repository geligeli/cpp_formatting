#ifndef NAMING_REPORT_H_
#define NAMING_REPORT_H_

#include <vector>

#include "naming/shapes.h"

namespace GeoKit {

struct report_line {
  rect_data rect;
  shape_kind kind = kRect;
  pixel_count area = 0;
};

using report = std::vector<report_line>;

report Summarize(const std::vector<rect_data>& rects);

}  // namespace GeoKit

#endif  // NAMING_REPORT_H_
