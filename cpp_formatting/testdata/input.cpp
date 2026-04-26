int add(int a, int b) { return a + b; }

double scale(double x) { return x * 2.0; }

const int* sentinel() {
  static int val = -1;
  return &val;
}

void reset(int& x) { x = 0; }

auto alreadyTrailing() -> bool { return true; }

auto deduced() { return 42; }
