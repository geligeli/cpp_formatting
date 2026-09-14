#include "naming/report.h"

using namespace GeoKit;

int main() {
  std::vector<rect_data> rects(2);
  rects[0].width_px = 3;
  rects[0].height_px = 4;
  rects[1].width_px = 5;
  rects[1].height_px = 5;
  const report r = Summarize(rects);
  return r.size() == 2 && r[1].kind == kSquare ? 0 : 1;
}
