#include "normalize_method_input.h"

Shape* Shape::createDefault() { return new Circle(); }

double totalArea(Shape& a, Shape& b) { return a.getArea() + b.getArea(); }
