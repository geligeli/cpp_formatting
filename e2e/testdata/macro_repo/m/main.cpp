#include "m/counter.h"

int main() {
  m::Counter c{0};
  BUMP(c);  // expands to the stale member name after the rename
  return m::total(c) == 1 ? 0 : 1;
}
