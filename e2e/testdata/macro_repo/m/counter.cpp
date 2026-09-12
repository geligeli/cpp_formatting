#include "m/counter.h"

namespace m {

// An ordinary (non-macro) use: this one is renamed.
int total(const Counter& c) { return c.itemCount; }

}  // namespace m
