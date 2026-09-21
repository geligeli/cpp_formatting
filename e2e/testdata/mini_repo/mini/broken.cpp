// Does not compile, on purpose; see BUILD.bazel.
#include "mini/no_such_header.h"

int broken_entry() { return NoSuchFunction(); }
