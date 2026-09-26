#include "demo.h"

#include "demo_testing.h"

// A cc_test (//bazel/testdata:demo_test): its files are test code, which the
// index aspect records as a `testonly` attribute on them and the code browser
// lists after the other uses of a symbol.  Exits 0.
int main() {
  Widget w = MakeWidget(3);
  return total_of(w) == 3 ? 0 : 1;
}
