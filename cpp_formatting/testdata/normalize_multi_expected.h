#pragma once

struct Rect {
  int width;
  int height;
  static int count;

  // Implicit-this member access inside a method — must be renamed.
  int area() const { return width * height; }
};
