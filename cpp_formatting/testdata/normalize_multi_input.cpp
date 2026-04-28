#include "normalize_multi_input.h"

int Rect::m_count = 0;

void setup(Rect& r, int w, int h) {
  // Dot and arrow member access.
  r.m_width = w;
  r.m_height = h;
  ++Rect::m_count;

  // Pointer-to-member formation: DeclRefExpr referring to a FieldDecl.
  int Rect::* pw = &Rect::m_width;
  r.*pw = w;

  // Lambda body: member access inside a closure must be renamed.
  auto area = [&r]() { return r.m_width * r.m_height; };
  (void)area;

  // Local variable with the same spelling as a member — must NOT be renamed
  // when scope=member.
  int m_width = r.m_width;
  (void)m_width;
}
