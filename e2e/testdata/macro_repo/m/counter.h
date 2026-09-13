#ifndef M_COUNTER_H_
#define M_COUNTER_H_

namespace m {

struct Counter {
  // Referenced from BUMP's macro body below.  That body token is a single
  // location shared by every expansion of BUMP, so no rewrite can express it,
  // and renaming this declaration alone would leave the macro spelling the old
  // name.  The rename is therefore vetoed and the member keeps its name --
  // reported as a skipped rename, not silently dropped.
  int itemCount;

  // No macro names this one, so it renames normally.  Its presence is what
  // makes this scenario's `check` phase non-vacuous.
  int otherCount;
};

#define BUMP(c) ((c).itemCount += 1)

int total(const Counter& c);

}  // namespace m

#endif  // M_COUNTER_H_
