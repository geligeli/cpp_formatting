#include "demo.h"

int demo() {
  Widget w;
  w.item_count_ = 3;
  // Reaches a member of the header-only //bazel/testdata:budget, which has no
  // action of its own: this TU is where budget.h is parsed and rewritten.
  Budget b;
  b.remaining_ = 1;
  return total_of(w) + b.Remaining();
}
