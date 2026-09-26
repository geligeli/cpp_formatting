#ifndef BAZEL_TESTDATA_DEMO_TESTING_H_
#define BAZEL_TESTDATA_DEMO_TESTING_H_

#include "demo.h"

// A header-only testonly library (//bazel/testdata:demo_testing): it has no
// translation unit, so the index aspect's only output for it is the unit in
// text form that marks this file `testonly` -- the attribute joins the entry
// that demo_test.cpp's action writes for this header.
inline auto MakeWidget(int n) -> Widget {
  Widget w;
  w.item_count_ = n;
  return w;
}

#endif  // BAZEL_TESTDATA_DEMO_TESTING_H_
