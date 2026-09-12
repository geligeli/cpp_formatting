#ifndef MINI_SHAPES_H_
#define MINI_SHAPES_H_

namespace mini {

// camelCase members: `member -> snake_case` renames these, and every use --
// in this target and in the two that depend on it -- has to follow, or the
// rebuild breaks.
class Rect {
 public:
  Rect(int w, int h) : widthPx(w), heightPx(h) {}

  int area() const;
  int perimeter() const { return 2 * (widthPx + heightPx); }

  int widthPx;
  int heightPx;
};

// A template-dependent member access: which member `widthPx` names is not known
// until instantiation, and the instantiations live in the *other* targets' TUs.
// Resolving this is the cross-TU path in DependentResolutions.
template <class T>
int width_of(const T& shape) {
  return shape.widthPx;
}

}  // namespace mini

#endif  // MINI_SHAPES_H_
