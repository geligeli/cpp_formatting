#include "bazel/testdata/impl_deps/meter.h"

#include "gauge.h"

int ReadMeter() {
  Gauge gauge;
  gauge.level_ = 7;
  return gauge.level_;
}
