#pragma once

struct Shape {
  virtual ~Shape() = default;
  virtual double getArea() const = 0;
  static Shape* createDefault();
};

struct Circle : Shape {
  double getArea() const override { return 3.14 * m_radius * m_radius; }
  double m_radius = 1.0;
};
