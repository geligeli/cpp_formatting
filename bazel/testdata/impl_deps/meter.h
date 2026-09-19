#ifndef BAZEL_TESTDATA_IMPL_DEPS_METER_H_
#define BAZEL_TESTDATA_IMPL_DEPS_METER_H_

// Deliberately does not include gauge.h: Gauge is an implementation detail of
// meter.cpp, which is what makes :gauge an implementation dep.
int ReadMeter();

#endif  // BAZEL_TESTDATA_IMPL_DEPS_METER_H_
