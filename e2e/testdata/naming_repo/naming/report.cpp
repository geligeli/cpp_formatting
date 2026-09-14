#include "naming/report.h"

namespace GK = GeoKit;

namespace GeoKit {

report Summarize(const std::vector<rect_data>& rects) {
  report out;
  GK::area_calculator calc;
  for (const rect_data& r : rects) {
    report_line line;
    line.rect = r;
    line.kind = calc.Classify(r);
    line.area = calc.Area(r);
    out.push_back(line);
  }
  return out;
}

}  // namespace GeoKit
