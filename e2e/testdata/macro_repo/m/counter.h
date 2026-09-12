#ifndef M_COUNTER_H_
#define M_COUNTER_H_

namespace m {

struct Counter {
  int itemCount;
};

// The member reference below is written inside a macro *body*, so every
// expansion of BUMP carries a macro source location -- and
// ApplyRenamesVisitor::renameAt() returns early on isMacroID(). The member is
// therefore renamed at its declaration and at ordinary use sites, while this
// one is left spelling the old name.
#define BUMP(c) ((c).itemCount += 1)

int total(const Counter& c);

}  // namespace m

#endif  // M_COUNTER_H_
