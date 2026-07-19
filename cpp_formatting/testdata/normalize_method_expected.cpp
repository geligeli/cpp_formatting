#include "normalize_method_input.h"

Shape* Shape::create_default() { return new Circle(); }

double totalArea(Shape& a, Shape& b) { return a.get_area() + b.get_area(); }
