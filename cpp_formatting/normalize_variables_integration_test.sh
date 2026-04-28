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
