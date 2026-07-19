#pragma once

struct Shape {
  virtual ~Shape() = default;
  virtual double get_area() const = 0;
  static Shape* create_default();
};

struct Circle : Shape {
  double get_area() const override { return 3.14 * m_radius * m_radius; }
  double m_radius = 1.0;
};
