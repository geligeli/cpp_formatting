#include "m/counter.h"

int main() {
  m::Counter c{0, 0};
  BUMP(c);  // the expansion that vetoes renaming Counter::itemCount
  return m::total(c) == 1 ? 0 : 1;
}
