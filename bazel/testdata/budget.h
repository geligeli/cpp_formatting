#ifndef BAZEL_TESTDATA_BUDGET_H_
#define BAZEL_TESTDATA_BUDGET_H_

// Fixture for the aspect's header-only branch.  //bazel/testdata:budget has no
// translation unit of its own, so it gets no emit action: nothing parses this
// file on its own behalf, and its members are renamed by the TUs that include
// it (demo.cpp and demo_main.cpp, which reach it through --owned-files).  It
// still writes an empty manifest, which is what keeps a manifest from an older
// build -- one that named a record file the current build does not produce --
// from outliving the action that wrote it.
struct Budget {
  int remaining_;

  int Remaining() const { return remaining_; }
};

#endif  // BAZEL_TESTDATA_BUDGET_H_
