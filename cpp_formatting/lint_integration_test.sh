#!/usr/bin/env bash
# Integration tests for the --lint mode of all three binaries.
# Arguments (all Bazel $(location ...) expansions):
#   $1   normalize_variables binary
#   $2   trailing_return_types binary
#   $3   cpp_format binary
#   $4   testdata/normalize_shadow_input.cpp
#   $5   testdata/normalize_shadow_expected.cpp
#   $6   testdata/input.cpp
#   $7   testdata/expected.cpp

set -euo pipefail

normalize="$(realpath "$1")"
trailing="$(realpath "$2")"
cpp_format="$(realpath "$3")"
shadow_in="$4"
shadow_exp="$5"
trailing_in="$6"
trailing_exp="$7"

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

# Runs a lint command expecting exit code 1 (violations found).
expect_violations() {
  set +e
  "$@"
  local rc=$?
  set -e
  [[ $rc -eq 1 ]] || fail "expected exit 1 (violations), got $rc: $*"
}

# ---------------------------------------------------------------------------
# Test 1 — text lint: diagnostics on stdout, file untouched, exit 1
# ---------------------------------------------------------------------------
cp "$shadow_in" "$tmpdir/shadow.cpp"

out="$(
  expect_violations "$normalize" --lint --style=snake_case --scope=global \
    "$tmpdir/shadow.cpp" -- -std=c++17
)"

grep -q "shadow.cpp:1:5: warning: 'globalCount' should be 'global_count' \[normalize_variables/global/snake_case\]" <<<"$out" \
  || fail "text lint: missing expected diagnostic, got: $out"
[[ "$(grep -c 'warning:' <<<"$out")" -eq 3 ]] \
  || fail "text lint: expected 3 diagnostics, got: $out"
diff -u "$shadow_in" "$tmpdir/shadow.cpp" \
  || fail "text lint: input file was modified"
echo "PASS: text lint reports violations, modifies nothing, exits 1"

# ---------------------------------------------------------------------------
# Test 2 — SARIF lint
# ---------------------------------------------------------------------------
out="$(
  cd "$tmpdir"
  expect_violations "$normalize" --lint --format=sarif \
    --style=snake_case --scope=global \
    shadow.cpp -- -std=c++17
)"

grep -q '"version": "2.1.0"' <<<"$out" \
  || fail "sarif lint: missing SARIF version, got: $out"
grep -q '"ruleId": "normalize_variables/global/snake_case"' <<<"$out" \
  || fail "sarif lint: missing ruleId, got: $out"
grep -q '"uri": "shadow.cpp"' <<<"$out" \
  || fail "sarif lint: artifact URI is not repo-relative, got: $out"
diff -u "$shadow_in" "$tmpdir/shadow.cpp" \
  || fail "sarif lint: input file was modified"
echo "PASS: SARIF lint emits valid 2.1.0 log with rule id and relative URI"

# ---------------------------------------------------------------------------
# Test 3 — diff lint: the emitted patch applies cleanly with git apply
# ---------------------------------------------------------------------------
cp "$shadow_in" "$tmpdir/shadow.cpp"

(
  cd "$tmpdir"
  expect_violations "$normalize" --lint --format=diff \
    --style=snake_case --scope=global shadow.cpp -- -std=c++17 > patch.diff
  git apply patch.diff || fail "diff lint: git apply rejected the patch"
)

diff -u "$shadow_exp" "$tmpdir/shadow.cpp" \
  || fail "diff lint: patched file does not match expected"
echo "PASS: diff lint patch applies with git apply and matches --in-place output"

# ---------------------------------------------------------------------------
# Test 4 — clean file: exit 0, no diagnostics
# ---------------------------------------------------------------------------
cp "$shadow_exp" "$tmpdir/clean.cpp"

out="$("$normalize" --lint --style=snake_case --scope=global \
  "$tmpdir/clean.cpp" -- -std=c++17)"

[[ -z "$out" ]] || fail "clean lint: expected no diagnostics, got: $out"
echo "PASS: clean file lints without violations (exit 0)"

# ---------------------------------------------------------------------------
# Test 5 — trailing_return_types lint (text + diff round-trip)
# ---------------------------------------------------------------------------
cp "$trailing_in" "$tmpdir/trailing.cpp"

out="$(
  expect_violations "$trailing" --lint "$tmpdir/trailing.cpp" -- -std=c++17
)"
[[ "$(grep -c 'warning: function should use trailing return type \[trailing_return_types\]' <<<"$out")" -eq 3 ]] \
  || fail "trailing lint: expected 3 diagnostics, got: $out"

(
  cd "$tmpdir"
  expect_violations "$trailing" --lint --format=diff trailing.cpp \
    -- -std=c++17 > trailing_patch.diff
  git apply trailing_patch.diff \
    || fail "trailing lint: git apply rejected the patch"
)
diff -u "$trailing_exp" "$tmpdir/trailing.cpp" \
  || fail "trailing lint: patched file does not match expected"
echo "PASS: trailing_return_types lint reports and diffs correctly"

# ---------------------------------------------------------------------------
# Test 6 — cpp_format lint: multi-pass SARIF contains both rule ids
# ---------------------------------------------------------------------------
cat > "$tmpdir/multi.cpp" <<'EOF'
struct S { int m_value; };
int compute(S& s) { return s.m_value; }
EOF

out="$(
  expect_violations "$cpp_format" \
    --trailing-return-types \
    --normalize-variables-scope=member --normalize-variables-style=snake_case \
    --lint --format=sarif "$tmpdir/multi.cpp" -- -std=c++17
)"

grep -q '"ruleId": "normalize_variables/member/snake_case"' <<<"$out" \
  || fail "cpp_format lint: missing normalize_variables rule id, got: $out"
grep -q '"ruleId": "trailing_return_types"' <<<"$out" \
  || fail "cpp_format lint: missing trailing_return_types rule id, got: $out"
diff -u <(printf 'struct S { int m_value; };\nint compute(S& s) { return s.m_value; }\n') "$tmpdir/multi.cpp" \
  || fail "cpp_format lint: input file was modified"
echo "PASS: cpp_format lint aggregates all passes into one SARIF report"

echo "All lint integration tests passed."
