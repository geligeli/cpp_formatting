struct Counter {
  int count = 0;
  auto get() const -> int { return count; }
  auto increment() -> Counter& {
    ++count;
    return *this;
  }
  void reset() { count = 0; }
};
