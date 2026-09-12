#include "demo.h"

// Second target of the demo: a cc_binary that only *uses* Widget::item_count —
// the declaration lives in //bazel/testdata:demo's header.  The aspect parses
// each target's own sources alone, so a rename of the member reaches this use
// site only because the dep's headers are passed as --owned-files.  Without
// that, `demo` would be renamed and this file left behind: a build break.
int main() {
  Widget w;
  w.item_count = 4;
  return total_of(w);
}
