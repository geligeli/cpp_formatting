#!/usr/bin/env bash
# Integration tests for normalize_variables.
# Arguments (all Bazel $(location ...) expansions):
#   $1   normalize_variables binary
#   $2   testdata/normalize_multi_input.h
#   $3   testdata/normalize_multi_input.cpp
#   $4   testdata/normalize_multi_expected.h
#   $5   testdata/normalize_multi_expected.cpp
#   $6   testdata/normalize_shadow_input.cpp
#   $7   testdata/normalize_shadow_expected.cpp
#   $8   testdata/normalize_order_input.h
#   $9   testdata/normalize_order_impl_input.cpp
#   $10  testdata/normalize_order_test_input.cpp
#   $11  testdata/normalize_order_expected.h
#   $12  testdata/normalize_order_impl_expected.cpp
#   $13  testdata/normalize_order_test_expected.cpp
#   $14  testdata/normalize_method_input.h
#   $15  testdata/normalize_method_input.cpp
#   $16  testdata/normalize_method_expected.h
#   $17  testdata/normalize_method_expected.cpp

set -euo pipefail

binary="$1"
multi_h_in="$2"
multi_cpp_in="$3"
multi_h_exp="$4"
multi_cpp_exp="$5"
shadow_in="$6"
shadow_exp="$7"
order_h_in="$8"
order_impl_in="$9"
order_test_in="${10}"
order_h_exp="${11}"
order_impl_exp="${12}"
order_test_exp="${13}"
method_h_in="${14}"
method_cpp_in="${15}"
method_h_exp="${16}"
method_cpp_exp="${17}"

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Test 1 — multi-file member rename (m_ prefix → snake_case)
#
# The .cpp and .h are both listed as sources.  The .cpp is listed first so it
# is processed while the header is still in its original form; the header is
# processed second (as its own main file) to rename the declarations there.
# ---------------------------------------------------------------------------
cp "$multi_h_in"   "$tmpdir/normalize_multi_input.h"
cp "$multi_cpp_in" "$tmpdir/normalize_multi_input.cpp"

"$binary" \
  --style=snake_case --scope=member --in-place \
  "$tmpdir/normalize_multi_input.cpp" \
  "$tmpdir/normalize_multi_input.h" \
  -- -std=c++17 -xc++ -Wno-pragma-once-outside-header -I"$tmpdir"

diff -u "$multi_h_exp"   "$tmpdir/normalize_multi_input.h"   \
  || fail "multi_input.h does not match expected"
diff -u "$multi_cpp_exp" "$tmpdir/normalize_multi_input.cpp" \
  || fail "multi_input.cpp does not match expected"
echo "PASS: multi-file member rename (cross-file references, pointer-to-member, lambda, scope separation)"

# ---------------------------------------------------------------------------
# Test 2 — shadowed variable
#
# --scope=global renames the global 'globalCount'; the same-named function
# parameter ('reset's argument) is a local and must remain unchanged.
# ---------------------------------------------------------------------------
cp "$shadow_in" "$tmpdir/normalize_shadow_input.cpp"

"$binary" \
  --style=snake_case --scope=global --in-place \
  "$tmpdir/normalize_shadow_input.cpp" \
  -- -std=c++17

diff -u "$shadow_exp" "$tmpdir/normalize_shadow_input.cpp" \
  || fail "shadow test does not match expected"
echo "PASS: shadowed variable (global renamed, same-named local left unchanged)"

# ---------------------------------------------------------------------------
# Test 3 — source ordering: header listed between two .cpp files
#
# Without auto-reordering, processing the header second (between the two cpp
# files) renames its declarations before the third file (foo_test.cpp) is
# compiled.  foo_test.cpp is then compiled against the already-renamed header
# and its uses are silently left unrenamed.
#
# The binary must auto-promote all headers to the end of the source list so
# that every .cpp sees the original names during its pass.
# ---------------------------------------------------------------------------
cp "$order_h_in"    "$tmpdir/normalize_order_input.h"
cp "$order_impl_in" "$tmpdir/normalize_order_impl_input.cpp"
cp "$order_test_in" "$tmpdir/normalize_order_test_input.cpp"

# Deliberately pass the header in the middle — the tool must reorder.
"$binary" \
  --style=snake_case --scope=member --in-place \
  "$tmpdir/normalize_order_impl_input.cpp" \
  "$tmpdir/normalize_order_input.h" \
  "$tmpdir/normalize_order_test_input.cpp" \
  -- -std=c++17 -xc++ -Wno-pragma-once-outside-header -I"$tmpdir"

diff -u "$order_h_exp"    "$tmpdir/normalize_order_input.h" \
  || fail "order test: header does not match expected"
diff -u "$order_impl_exp" "$tmpdir/normalize_order_impl_input.cpp" \
  || fail "order test: impl file does not match expected"
diff -u "$order_test_exp" "$tmpdir/normalize_order_test_input.cpp" \
  || fail "order test: test file does not match expected"
echo "PASS: source ordering (header mid-list, two cpp files both renamed correctly)"

# ---------------------------------------------------------------------------
# Test 4 — member function rename (camelCase → snake_case)
#
# --scope=method renames member functions across files: the pure-virtual base
# declaration, the override, the out-of-line static definition, and all call
# sites.  The destructor, data members, and free functions must be untouched.
# ---------------------------------------------------------------------------
cp "$method_h_in"   "$tmpdir/normalize_method_input.h"
cp "$method_cpp_in" "$tmpdir/normalize_method_input.cpp"

"$binary" \
  --style=snake_case --scope=method --in-place \
  "$tmpdir/normalize_method_input.cpp" \
  "$tmpdir/normalize_method_input.h" \
  -- -std=c++17 -xc++ -Wno-pragma-once-outside-header -I"$tmpdir"

diff -u "$method_h_exp"   "$tmpdir/normalize_method_input.h"   \
  || fail "method test: header does not match expected"
diff -u "$method_cpp_exp" "$tmpdir/normalize_method_input.cpp" \
  || fail "method test: impl file does not match expected"
echo "PASS: member function rename (virtual hierarchy, static method, cross-file call sites)"

# ---------------------------------------------------------------------------
# Test 5 — template-dependent member token resolved across files
#
# `set_val`'s `x.val` is a dependent member access: which member it names is
# only known once the template is instantiated, and the instantiations live in
# the .cpp files, not in the header.  Renaming `val` -> `val_` must rewrite the
# concrete members in the .cpp files AND the dependent token in the header,
# using the resolution recorded while the .cpp TUs were processed.  These files
# are self-contained (no committed testdata needed).
# ---------------------------------------------------------------------------
depdir="$tmpdir/dep"
mkdir -p "$depdir"
cat > "$depdir/dep.h" <<'EOF'
#ifndef DEP_H
#define DEP_H
template <class T>
void set_val(T& x) {
  x.val = 12;
}
#endif
EOF
cat > "$depdir/dep_a.cpp" <<'EOF'
#include "dep.h"
struct A { int val; };
int use_a() { A a; set_val(a); return a.val; }
EOF
cat > "$depdir/dep_b.cpp" <<'EOF'
#include "dep.h"
struct B { int val; };
int use_b() { B b; set_val(b); return b.val; }
EOF

"$binary" \
  --style=trailing_ --scope=member --in-place \
  "$depdir/dep_a.cpp" "$depdir/dep_b.cpp" "$depdir/dep.h" \
  -- -std=c++17 -xc++ -Wno-pragma-once-outside-header -I"$depdir"

cat > "$depdir/dep_expected.h" <<'EOF'
#ifndef DEP_H
#define DEP_H
template <class T>
void set_val(T& x) {
  x.val_ = 12;
}
#endif
EOF
grep -q 'x.val_ = 12;' "$depdir/dep.h" \
  || fail "dependent test: header dependent token x.val was not renamed to x.val_"
diff -u "$depdir/dep_expected.h" "$depdir/dep.h" \
  || fail "dependent test: header does not match expected"
grep -q 'int val_;' "$depdir/dep_a.cpp" \
  || fail "dependent test: A::val was not renamed"
grep -q 'return a.val_;' "$depdir/dep_a.cpp" \
  || fail "dependent test: use a.val was not renamed"
echo "PASS: template-dependent member token resolved across files (x.val -> x.val_ in header)"

# ---------------------------------------------------------------------------
# Test 6 — out-of-scope instantiation vetoes the dependent token
#
# `set_val` is also instantiated with `Ext`, a type declared in a header that is
# NOT passed to the tool (out of the file set).  Renaming the shared token to
# `val_` would break `set_val<Ext>`, so the tool must leave the header token
# alone (veto) even though it renames the owned type `A` in the .cpp.
# ---------------------------------------------------------------------------
vetodir="$tmpdir/veto"
mkdir -p "$vetodir"
cat > "$vetodir/ext.h" <<'EOF'
#ifndef EXT_H
#define EXT_H
struct Ext { int val; };
#endif
EOF
cat > "$vetodir/dep.h" <<'EOF'
#ifndef DEP_H
#define DEP_H
template <class T>
void set_val(T& x) { x.val = 12; }
#endif
EOF
cat > "$vetodir/dep_a.cpp" <<'EOF'
#include "ext.h"
#include "dep.h"
struct A { int val; };
int use() { A a; set_val(a); Ext e; set_val(e); return a.val + e.val; }
EOF
cp "$vetodir/dep.h" "$vetodir/dep_before.h"

# Pass only dep_a.cpp and dep.h — ext.h is intentionally not owned.
"$binary" \
  --style=trailing_ --scope=member --in-place \
  "$vetodir/dep_a.cpp" "$vetodir/dep.h" \
  -- -std=c++17 -xc++ -Wno-pragma-once-outside-header -I"$vetodir"

diff -u "$vetodir/dep_before.h" "$vetodir/dep.h" \
  || fail "veto test: header token was rewritten despite an out-of-scope binding"
grep -q 'struct A { int val_; };' "$vetodir/dep_a.cpp" \
  || fail "veto test: owned type A::val should still be renamed"
if grep -q 'e.val_' "$vetodir/dep_a.cpp"; then
  fail "veto test: out-of-scope Ext::val must be left unchanged"
fi
echo "PASS: out-of-scope instantiation vetoes the shared dependent token"

# ---------------------------------------------------------------------------
# Test 7 — a reference inside a macro body vetoes the rename, in every TU
#
# `BUMP`'s body spells `itemCount` once, at a location shared by every
# expansion, so it cannot be rewritten -- and renaming the declaration without
# it does not compile.  The catch is ordering: counter.cpp is processed before
# the TU that expands BUMP, so the veto arrives after that file was already
# rewritten.  The tool discards the first pass and runs again, which is what
# makes the result independent of the source order.  `otherCount`, which no
# macro names, is renamed throughout.
# ---------------------------------------------------------------------------
macrodir="$tmpdir/macro"
mkdir -p "$macrodir"
cat > "$macrodir/counter.h" <<'EOF'
#ifndef COUNTER_H
#define COUNTER_H
struct Counter {
  int itemCount;
  int otherCount;
};
#define BUMP(c) ((c).itemCount += 1)
int total(const Counter& c);
#endif
EOF
cat > "$macrodir/counter.cpp" <<'EOF'
#include "counter.h"
int total(const Counter& c) { return c.itemCount + c.otherCount; }
EOF
cat > "$macrodir/main.cpp" <<'EOF'
#include "counter.h"
int main() {
  Counter c{0, 0};
  BUMP(c);
  return total(c);
}
EOF

report="$("$binary" \
  --style=snake_case --scope=member --in-place --report-rename-conflicts \
  "$macrodir/counter.cpp" "$macrodir/main.cpp" "$macrodir/counter.h" \
  -- -std=c++17 -xc++ -Wno-pragma-once-outside-header -I"$macrodir" 2>&1)"

grep -q 'referenced from a macro body' <<<"$report" \
  || fail "macro test: the skipped rename was not reported"
if grep -q 'item_count' "$macrodir/counter.h" "$macrodir/counter.cpp"; then
  fail "macro test: itemCount was renamed despite a reference in a macro body"
fi
grep -q 'int other_count;' "$macrodir/counter.h" \
  || fail "macro test: otherCount should still be renamed in the header"
grep -q 'c.other_count' "$macrodir/counter.cpp" \
  || fail "macro test: otherCount use should still be renamed in the .cpp"
echo "PASS: a macro-body reference vetoes the rename regardless of source order"

# ---------------------------------------------------------------------------
# Test 8 — a name written as a macro *argument* is spelled at the call site,
# so it is renamed like any other reference.
# ---------------------------------------------------------------------------
argdir="$tmpdir/macroarg"
mkdir -p "$argdir"
cat > "$argdir/arg.cpp" <<'EOF'
#define FWD(x) ((x) + 0)
#define OUTER(y) FWD(y)
#define FIELD(n) int n;
struct Boxed { FIELD(innerCount) };
int use(Boxed& b) { return FWD(b.innerCount) + OUTER(b.innerCount); }
EOF

"$binary" \
  --style=snake_case --scope=member --in-place "$argdir/arg.cpp" \
  -- -std=c++17 -xc++

grep -q 'FIELD(inner_count)' "$argdir/arg.cpp" \
  || fail "macro arg test: declaration written as a macro argument not renamed"
grep -q 'FWD(b.inner_count) + OUTER(b.inner_count)' "$argdir/arg.cpp" \
  || fail "macro arg test: macro-argument uses not renamed"
echo "PASS: names written as macro arguments are renamed at the call site"
