#ifndef BAZEL_TESTDATA_DEMO_H_
#define BAZEL_TESTDATA_DEMO_H_

#include "budget.h"

// Fixture for the cpp_format Bazel integration.  Conforms to the repository's
// own cpp_format.yaml (the aspect reads the root module's; data members are
// `trailing_`), so `bazel test //bazel/testdata:format_check_test` passes; drop
// the trailing underscore from `item_count_` to see the gate fail and
// `tools/cpp_format.sh diff //bazel/testdata/...` / `... fix` react.
struct Widget {
  int item_count_;
};

// `w.item_count_` is a template-dependent member access: which member it names
// is known only once the template is instantiated (in demo.cpp).  When the
// member is renamed, this token is rewritten from the resolution recorded while
// demo.cpp's TU is emitted — the cross-TU path exercised by the integration.
template <class T>
int total_of(T& w) {
  return w.item_count_;
}

#endif  // BAZEL_TESTDATA_DEMO_H_
