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

# ---------------------------------------------------------------------------
# Test 7 — the reverse direction: --reverse lint, and cpp_format's
#          return_types: leading.  The diff has to round-trip the same way.
# ---------------------------------------------------------------------------
cp "$trailing_exp" "$tmpdir/leading.cpp"

out="$(
  expect_violations "$trailing" --reverse --lint "$tmpdir/leading.cpp" \
    -- -std=c++17
)"
[[ "$(grep -c 'warning: function should use leading return type \[leading_return_types\]' <<<"$out")" -eq 4 ]] \
  || fail "reverse lint: expected 4 diagnostics, got: $out"

(
  cd "$tmpdir"
  expect_violations "$trailing" --reverse --lint --format=diff leading.cpp \
    -- -std=c++17 > leading_patch.diff
  git apply leading_patch.diff \
    || fail "reverse lint: git apply rejected the patch"
)

# Everything that can move is back in leading position; `deduced()` has no
# trailing return type to move and `reset()` never had one.
cat > "$tmpdir/leading_expected.cpp" <<'EOF'
int add(int a, int b) { return a + b; }

double scale(double x) { return x * 2.0; }

const int* sentinel() {
  static int val = -1;
  return &val;
}

void reset(int& x) { x = 0; }

bool alreadyTrailing() { return true; }

auto deduced() { return 42; }
EOF
diff -u "$tmpdir/leading_expected.cpp" "$tmpdir/leading.cpp" \
  || fail "reverse lint: patched file does not match expected"

# Applying --reverse to that result changes nothing: the direction is a
# fixpoint, which is what makes it safe to run in a loop over a repo.
"$trailing" --reverse --lint "$tmpdir/leading.cpp" -- -std=c++17 \
  || fail "reverse lint: a second pass still reports violations"
echo "PASS: --reverse lint reports, diffs and converges"

# The same direction through cpp_format's config surface.
cp "$trailing_exp" "$tmpdir/leading_cfg.cpp"
out="$(
  expect_violations "$cpp_format" --return-types=leading \
    --lint --format=sarif "$tmpdir/leading_cfg.cpp" -- -std=c++17
)"
grep -q '"ruleId": "leading_return_types"' <<<"$out" \
  || fail "cpp_format reverse lint: missing leading_return_types rule id, got: $out"
diff -u "$trailing_exp" "$tmpdir/leading_cfg.cpp" \
  || fail "cpp_format reverse lint: input file was modified"

# The two directions cannot both be asked for.
set +e
both_out="$("$cpp_format" --return-types=leading --trailing-return-types \
  --lint "$tmpdir/leading_cfg.cpp" -- -std=c++17 2>&1)"
both_rc=$?
set -e
[[ $both_rc -eq 1 ]] \
  || fail "cpp_format: expected exit 1 when both directions are requested, got $both_rc"
grep -q "not both" <<<"$both_out" \
  || fail "cpp_format: expected a 'not both' error, got: $both_out"
echo "PASS: cpp_format return_types=leading lints and rejects both directions"

# ---------------------------------------------------------------------------
# Test 8 — a rename landing *inside* a trailing return type that the reverse
#          direction then moves.  The renamed text has to travel with the type,
#          and the `-> type` removal has to be measured against the buffer as
#          the rename left it: Rewriter::RemoveText does not map its length
#          argument, so the original byte count would leave the `)` behind.
# ---------------------------------------------------------------------------
cat > "$tmpdir/subsume.cpp" <<'EOF'
struct S {
  int count_;
  auto get() -> decltype(count_) { return count_; }
};
EOF
cp "$tmpdir/subsume.cpp" "$tmpdir/subsume_orig.cpp"

(
  cd "$tmpdir"
  expect_violations "$cpp_format" --return-types=leading     --normalize-variables-scope=member --normalize-variables-style=m_prefix     --lint --format=diff subsume.cpp -- -std=c++20 > subsume.diff
  git apply subsume.diff || fail "subsume: git apply rejected the patch"
)

cat > "$tmpdir/subsume_expected.cpp" <<'EOF'
struct S {
  int m_count;
  decltype(m_count) get() { return m_count; }
};
EOF
diff -u "$tmpdir/subsume_expected.cpp" "$tmpdir/subsume.cpp"   || fail "subsume: rename inside the moved return type was not carried correctly"

# The same run through --emit-edits/--aggregate has to agree byte for byte:
# there the two rewrites are separate records, and the rename records inside
# the trailing type must be dropped rather than replayed.
cp "$tmpdir/subsume_orig.cpp" "$tmpdir/subsume_agg.cpp"
(
  cd "$tmpdir"
  "$cpp_format" --return-types=leading     --normalize-variables-scope=member --normalize-variables-style=m_prefix     --emit-edits=subsume.json subsume_agg.cpp -- -std=c++20 >/dev/null
  "$cpp_format" --aggregate --apply subsume.json >/dev/null     || fail "subsume: --aggregate --apply failed"
)
diff -u "$tmpdir/subsume_expected.cpp" "$tmpdir/subsume_agg.cpp"   || fail "subsume: aggregate path disagrees with the in-place path"
echo "PASS: a rename inside a moved trailing return type survives both paths"

echo "All lint integration tests passed."
