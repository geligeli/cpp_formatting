#ifndef MINI_GEOMETRY_H_
#define MINI_GEOMETRY_H_

#include "mini/shapes.h"

namespace mini {

int total_area(const Rect& a, const Rect& b);
int wider_of(const Rect& a, const Rect& b);

}  // namespace mini

#endif  // MINI_GEOMETRY_H_
