#ifndef BAZEL_TESTDATA_DEMO_H_
#define BAZEL_TESTDATA_DEMO_H_

// Fixture for the cpp_format Bazel integration.  Conforms to cpp_format.yaml
// (`member -> snake_case`), so `bazel test //bazel/testdata:format.check`
// passes; rename `item_count` to a camelCase name to see the gate fail and
// `bazel run //bazel/testdata:format.diff` / `:format.fix` react.
struct Widget {
  int item_count;
};

// `w.item_count` is a template-dependent member access: which member it names
// is known only once the template is instantiated (in demo.cpp).  When the
// member is renamed, this token is rewritten from the resolution recorded while
// demo.cpp's TU is emitted — the cross-TU path exercised by the integration.
template <class T>
int total_of(T& w) {
  return w.item_count;
}

#endif  // BAZEL_TESTDATA_DEMO_H_
