#include "normalize_multi_input.h"

int Rect::count = 0;

void setup(Rect& r, int w, int h) {
  // Dot and arrow member access.
  r.width = w;
  r.height = h;
  ++Rect::count;

  // Pointer-to-member formation: DeclRefExpr referring to a FieldDecl.
  int Rect::* pw = &Rect::width;
  r.*pw = w;

  // Lambda body: member access inside a closure must be renamed.
  auto area = [&r]() { return r.width * r.height; };
  (void)area;

  // Local variable with the same spelling as a member — must NOT be renamed
  // when scope=member.
  int m_width = r.width;
  (void)m_width;
}
