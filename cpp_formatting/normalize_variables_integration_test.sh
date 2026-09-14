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

# Absolute: the --jobs tests below run the binary from inside their fixture dirs.
binary="$(realpath "$1")"
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

# Pristine copies of the generated fixtures, for the --jobs equivalence checks
# at the end (the tests below rewrite their working copies in place).
fixtures="$tmpdir/fixtures"
mkdir -p "$fixtures"

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

cp -r "$depdir" "$fixtures/dep"

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
# Test 6 — out-of-scope instantiation vetoes the dependent token *and* the
# owned member it also binds to
#
# `set_val` is also instantiated with `Ext`, a type declared in a header that is
# NOT passed to the tool (out of the file set).  Renaming the shared token to
# `val_` would break `set_val<Ext>`, so the header token has to stay -- and
# then renaming the owned `A::val` would break `set_val<A>`, so that has to
# stay too (the rename is declined by name and reported).  The tool used to
# rename `A::val` and leave the token, an incomplete rename that surfaced as a
# compile error on the next build; protobuf's `T::_table_` is where that bit.
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
cp -r "$vetodir" "$fixtures/veto"

# Pass only dep_a.cpp and dep.h — ext.h is intentionally not owned.
cp "$vetodir/dep_a.cpp" "$vetodir/dep_a_before.cpp"
"$binary" \
  --style=trailing_ --scope=member --in-place --report-rename-conflicts \
  "$vetodir/dep_a.cpp" "$vetodir/dep.h" \
  -- -std=c++17 -xc++ -Wno-pragma-once-outside-header -I"$vetodir" \
  2> "$vetodir/stderr.txt"

diff -u "$vetodir/dep_before.h" "$vetodir/dep.h" \
  || fail "veto test: header token was rewritten despite an out-of-scope binding"
diff -u "$vetodir/dep_a_before.cpp" "$vetodir/dep_a.cpp" \
  || fail "veto test: A::val must be declined, since set_val's token cannot be rewritten"
grep -q "skipped rename 'val'.*outside the files being formatted" "$vetodir/stderr.txt" \
  || { cat "$vetodir/stderr.txt"; fail "veto test: the declined A::val was not reported with the out-of-scope binding as the reason"; }
echo "PASS: out-of-scope instantiation vetoes the shared dependent token and the owned member"

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

cp -r "$macrodir" "$fixtures/macro"

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

# ---------------------------------------------------------------------------
# Test 9 — a dependent token resolved only from another *header*
#
# `set_val`'s `x.val` in a.h is instantiated by `poke` in b.h, and no .cpp is
# involved.  Whichever header is parsed first cannot know what the other will
# record, so the tool re-runs the TU whose resolutions turned out stale.  The
# outcome must not depend on the source order or on the thread count.
# ---------------------------------------------------------------------------
chaindir="$tmpdir/chain"
mkdir -p "$chaindir"
cat > "$chaindir/a.h" <<'EOF'
#ifndef A_H
#define A_H
template <class T>
void set_val(T& x) { x.val = 1; }
#endif
EOF
cat > "$chaindir/b.h" <<'EOF'
#ifndef B_H
#define B_H
#include "a.h"
struct Bee { int val; };
inline void poke(Bee& b) { set_val(b); }
#endif
EOF
for order in "a.h b.h" "b.h a.h"; do
  for jobs in 1 4; do
    run="$chaindir/run"
    rm -rf "$run" && mkdir -p "$run" && cp "$chaindir/a.h" "$chaindir/b.h" "$run/"
    # shellcheck disable=SC2086
    (cd "$run" && "$binary" --style=trailing_ --scope=member --in-place \
      --jobs=$jobs $order -- -std=c++17 -xc++ -Wno-pragma-once-outside-header -I.) \
      >/dev/null 2>&1
    grep -q 'x.val_ = 1;' "$run/a.h" \
      || fail "header chain ($order, --jobs=$jobs): a.h token not renamed"
    grep -q 'int val_;' "$run/b.h" \
      || fail "header chain ($order, --jobs=$jobs): Bee::val not renamed"
  done
done
echo "PASS: a dependent token instantiated only from another header is renamed in every order"

# ---------------------------------------------------------------------------
# Test 10 — --debug-trace runs serially whatever --jobs says, so its per-TU
# trace stays readable, and modifies nothing.
# ---------------------------------------------------------------------------
tracedir="$tmpdir/trace"
mkdir -p "$tracedir"
cp "$order_h_in" "$tracedir/normalize_order_input.h"
cp "$order_impl_in" "$tracedir/normalize_order_impl_input.cpp"
cp "$order_test_in" "$tracedir/normalize_order_test_input.cpp"
trace="$(cd "$tracedir" && "$binary" --style=snake_case --scope=member \
  --debug-trace --jobs=4 \
  normalize_order_impl_input.cpp normalize_order_input.h normalize_order_test_input.cpp \
  -- -std=c++17 -xc++ -Wno-pragma-once-outside-header -I. 2>&1)" \
  || fail "debug trace: exit status $?"
[[ "$(grep -c '^TU: ' <<<"$trace")" -eq 3 ]] \
  || fail "debug trace: expected one trace per TU, got: $trace"
diff -u "$order_h_in" "$tracedir/normalize_order_input.h" \
  || fail "debug trace: modified a file"
echo "PASS: --debug-trace prints every TU's trace and modifies nothing"

# ---------------------------------------------------------------------------
# Test 11 — --jobs equivalence: every scenario above gives byte-identical files
# and the same conflict report with one thread and with four.
# ---------------------------------------------------------------------------
mkdir -p "$fixtures/multi" "$fixtures/order"
cp "$multi_h_in"   "$fixtures/multi/normalize_multi_input.h"
cp "$multi_cpp_in" "$fixtures/multi/normalize_multi_input.cpp"
cp "$order_h_in"    "$fixtures/order/normalize_order_input.h"
cp "$order_impl_in" "$fixtures/order/normalize_order_impl_input.cpp"
cp "$order_test_in" "$fixtures/order/normalize_order_test_input.cpp"

# jobs_equivalent <label> <style> <scope> <sources...>
jobs_equivalent() {
  local label="$1" style="$2" scope="$3"
  shift 3
  local d1="$tmpdir/jobs1_$label" d4="$tmpdir/jobs4_$label" out1 out4
  rm -rf "$d1" "$d4"
  cp -r "$fixtures/$label" "$d1"
  cp -r "$fixtures/$label" "$d4"
  out1="$(cd "$d1" && "$binary" --style="$style" --scope="$scope" --in-place \
    --report-rename-conflicts --jobs=1 "$@" \
    -- -std=c++17 -xc++ -Wno-pragma-once-outside-header -I. 2>&1 \
    | { grep -v 'Processing file' || true; } | sort)"
  out4="$(cd "$d4" && "$binary" --style="$style" --scope="$scope" --in-place \
    --report-rename-conflicts --jobs=4 "$@" \
    -- -std=c++17 -xc++ -Wno-pragma-once-outside-header -I. 2>&1 \
    | { grep -v 'Processing file' || true; } | sort)"
  diff -r "$d1" "$d4" \
    || fail "jobs equivalence ($label): --jobs=1 and --jobs=4 rewrote differently"
  [[ "$out1" == "$out4" ]] \
    || fail "jobs equivalence ($label): reports differ:\n$out1\n---\n$out4"
}
jobs_equivalent multi snake_case member normalize_multi_input.cpp normalize_multi_input.h
jobs_equivalent order snake_case member normalize_order_impl_input.cpp normalize_order_input.h normalize_order_test_input.cpp
jobs_equivalent dep trailing_ member dep_a.cpp dep_b.cpp dep.h
jobs_equivalent veto trailing_ member dep_a.cpp dep.h
jobs_equivalent macro snake_case member counter.cpp main.cpp counter.h
echo "PASS: --jobs=1 and --jobs=4 produce identical files and reports in every scenario"

# ---------------------------------------------------------------------------
# Test 12 — the project's own warning flags cannot fail the run.
#
# The compile command belongs to the project being formatted, and a project
# that builds with -Werror hands us one.  Clang then counts its own warning as
# an error, ClangTool::run reports a translation unit that parsed perfectly
# well as failed, and the tool exits 1 -- failing the build (under the Bazel
# aspect, the emit action) over a diagnostic it never reads.  The warning set
# is not even the one the project compiles with: a different Clang version, and
# per-target copts the aspect cannot recover.  A real error still fails, since
# it does mean the AST cannot be trusted.
# ---------------------------------------------------------------------------
werrordir="$tmpdir/werror"
mkdir -p "$werrordir"
cat > "$werrordir/engine.cpp" <<'EOF'
struct Engine {
  int itemCount = 0;
  void run() {
    int index = 0;
    for (int i = 0; i < 3; ++i) index = i;  // -Wunused-but-set-variable
    itemCount += 1;
  }
};
EOF

werror_out="$("$binary" \
  --style=snake_case --scope=member --in-place "$werrordir/engine.cpp" \
  -- -std=c++17 -xc++ -Wall -Wextra -Werror 2>&1)" \
  || fail "-Werror test: exited nonzero on a file that only warns: $werror_out"
[ -z "$werror_out" ] \
  || fail "-Werror test: the project's warnings were not silenced: $werror_out"
grep -q 'int item_count = 0;' "$werrordir/engine.cpp" \
  || fail "-Werror test: member not renamed"
grep -q 'int index = 0;' "$werrordir/engine.cpp" \
  || fail "-Werror test: the warned-about local should be left alone"

cat > "$werrordir/broken.cpp" <<'EOF'
struct Broken { int itemCount = ; };
EOF
if "$binary" \
    --style=snake_case --scope=member --in-place "$werrordir/broken.cpp" \
    -- -std=c++17 -xc++ >/dev/null 2>&1; then
  fail "-Werror test: a real parse error must still fail the run"
fi
echo "PASS: the project's -Werror cannot fail the run, a real error still does"

# ---------------------------------------------------------------------------
# Test 13 — a reference in a file this run will never rewrite declines the
# rename instead of leaving it behind.
#
# A textual header (a Bazel textual_hdrs .inc) is parsed as part of the TU that
# includes it, so the tool *sees* the reference -- but the file is not in the
# source list, so nothing would ever rewrite it.  Renaming the declaration
# anyway is exactly how abseil's LogEntry::kNoVerbosityLevel broke.  The member
# keeps its name everywhere and the skip is reported; a sibling member with no
# such reference renames normally; the .inc is not touched.
# ---------------------------------------------------------------------------
incdir="$tmpdir/inc"
mkdir -p "$incdir"
cat > "$incdir/log.cpp" <<'EOF'
struct Log { int itemCount = 0; int otherCount = 0; };
#include "log_impl.inc"
int use(Log& l) { return l.itemCount + l.otherCount + peek(l); }
EOF
cat > "$incdir/log_impl.inc" <<'EOF'
inline int peek(Log& l) { return l.itemCount; }
EOF
inc_out="$("$binary" --style=snake_case --scope=member --in-place \
  --report-rename-conflicts "$incdir/log.cpp" -- -std=c++17 -xc++ 2>&1)" \
  || fail "inc test: exited nonzero: $inc_out"
grep -q "skipped rename 'itemCount' -> 'item_count': referenced from a file that is not being formatted: .*log_impl.inc:1" <<<"$inc_out" \
  || fail "inc test: the decline was not reported: $inc_out"
grep -q 'int itemCount = 0;' "$incdir/log.cpp" \
  || fail "inc test: a member referenced from the .inc was renamed"
grep -q 'l.itemCount + l.other_count' "$incdir/log.cpp" \
  || fail "inc test: the sibling member should have renamed"
grep -q 'return l.itemCount;' "$incdir/log_impl.inc" \
  || fail "inc test: the .inc must not be modified"
echo "PASS: a reference from a file that is not being formatted declines the rename"

# ---------------------------------------------------------------------------
# Test 14 — a template-dependent use that no instantiation ever resolves
# declines the name.
#
# Impl<T>::kIsNothrow is spelled inside a template argument of an alias
# template.  When IsNothrow<int> is used, Clang folds the argument to the value
# `false`; no DeclRefExpr for kIsNothrow ever appears in any instantiation, so
# the cross-TU resolution has nothing to observe.  Before, the two declarations
# were renamed and the use was left behind (abseil's any_invocable_test.h).
# Now the token stays pending after the pass, the driver declines the *name*
# and re-runs; a sibling constant still renames.
# ---------------------------------------------------------------------------
aliasdir="$tmpdir/alias"
mkdir -p "$aliasdir"
cat > "$aliasdir/alias.cpp" <<'EOF'
template <class T, class = void>
struct Impl { static constexpr bool kIsNothrow = false; };
template <bool B> struct BoolC { static constexpr bool value = B; };
template <class T> using IsNothrow = BoolC<Impl<T>::kIsNothrow>;
struct S { static constexpr int kOther = 1; };
bool f() { return IsNothrow<int>::value && S::kOther == 1; }
EOF
alias_out="$("$binary" --style=snake_case --scope=member --in-place \
  --report-rename-conflicts "$aliasdir/alias.cpp" -- -std=c++17 -xc++ 2>&1)" \
  || fail "alias test: exited nonzero: $alias_out"
grep -q "skipped rename 'kIsNothrow' -> 'is_nothrow': a template-dependent use of the name at .*alias.cpp .* is never resolved by any instantiation" <<<"$alias_out" \
  || fail "alias test: the decline was not reported: $alias_out"
grep -q 'static constexpr bool kIsNothrow = false;' "$aliasdir/alias.cpp" \
  || fail "alias test: the declaration was renamed although its use cannot be"
grep -q 'BoolC<Impl<T>::kIsNothrow>' "$aliasdir/alias.cpp" \
  || fail "alias test: the dependent use changed"
grep -q 'static constexpr int other = 1;' "$aliasdir/alias.cpp" \
  || fail "alias test: the sibling constant should have renamed"
grep -q 'S::other == 1' "$aliasdir/alias.cpp" \
  || fail "alias test: the sibling's use should have renamed"
echo "PASS: a dependent use no instantiation resolves declines the name"
