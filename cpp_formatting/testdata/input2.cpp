struct Counter {
  int count = 0;
  int get() const { return count; }
  Counter& increment() {
    ++count;
    return *this;
  }
  void reset() { count = 0; }
};
