auto add(int a, int b) -> int { return a + b; }

auto scale(double x) -> double { return x * 2.0; }

auto sentinel() -> const int* {
  static int val = -1;
  return &val;
}

void reset(int& x) { x = 0; }

auto alreadyTrailing() -> bool { return true; }

auto deduced() { return 42; }
