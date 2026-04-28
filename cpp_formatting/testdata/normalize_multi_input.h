#pragma once

struct Rect {
  int m_width;
  int m_height;
  static int m_count;

  // Implicit-this member access inside a method — must be renamed.
  int area() const { return m_width * m_height; }
};
