#include "m/counter.h"

namespace m {

// An ordinary use of both members: otherCount is renamed here, itemCount is
// left alone together with its declaration.
int total(const Counter& c) { return c.itemCount + c.otherCount; }

}  // namespace m
