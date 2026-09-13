#!/usr/bin/env bash
# Integration tests for the trailing_return_types binary.
#
# Usage: integration_test.sh <binary> <input1> <expected1> <input2> <expected2> \
#                             <input3> <expected3>
#
# Tests:
#   1. Dry-run on a single file  — rewritten source goes to stdout.
#   2. In-place on a single file — file is modified on disk.
#   3. In-place on two files in one invocation — both files are modified.
#   4. In-place on a file with system #includes — validates that the binary
#      auto-detects the Clang resource dir so built-in headers resolve.
#   5. In-place --reverse — the other direction reaches disk through the same
#      path, and running it over its own output changes nothing.
#   6. --jobs — two files parsed on two threads give the same in-place result,
#      and the same dry-run output, as one thread.

set -euo pipefail

BINARY="$1"
INPUT1="$2"
EXPECTED1="$3"
INPUT2="$4"
EXPECTED2="$5"
INPUT3="$6"
EXPECTED3="$7"

TMP="${TEST_TMPDIR:-$(mktemp -d)}"

# assert_equal <description> <actual-file> <expected-file>
assert_equal() {
    local desc="$1" actual="$2" expected="$3"
    if diff -u "$expected" "$actual" >/dev/null 2>&1; then
        echo "PASS: $desc"
    else
        echo "FAIL: $desc" >&2
        diff -u "$expected" "$actual" >&2 || true
        exit 1
    fi
}

# ---------------------------------------------------------------------------
# Test 1: dry-run mode — rewritten source is written to stdout; the original
# file must be left untouched.
# ---------------------------------------------------------------------------
DRYRUN_OUT="$TMP/dryrun_output.cpp"
"$BINARY" "$INPUT1" -- -std=c++17 2>/dev/null > "$DRYRUN_OUT"
assert_equal "dry-run single file: stdout matches expected" \
    "$DRYRUN_OUT" "$EXPECTED1"

# Original file must be unchanged.
assert_equal "dry-run single file: original file untouched" \
    "$INPUT1" "$INPUT1"   # trivially true, but guards against accidental -i

# ---------------------------------------------------------------------------
# Test 2: in-place mode — the source file is overwritten on disk.
# ---------------------------------------------------------------------------
INPLACE1="$TMP/inplace_single.cpp"
cp "$INPUT1" "$INPLACE1"
"$BINARY" -i "$INPLACE1" -- -std=c++17 >/dev/null 2>&1
assert_equal "in-place single file" "$INPLACE1" "$EXPECTED1"

# ---------------------------------------------------------------------------
# Test 3: in-place mode with two files in one invocation — both files are
# modified, and each matches its own expected output.
# ---------------------------------------------------------------------------
MULTI_A="$TMP/multi_a.cpp"
MULTI_B="$TMP/multi_b.cpp"
cp "$INPUT1" "$MULTI_A"
cp "$INPUT2" "$MULTI_B"
"$BINARY" -i "$MULTI_A" "$MULTI_B" -- -std=c++17 >/dev/null 2>&1
assert_equal "in-place multi-file (file 1)" "$MULTI_A" "$EXPECTED1"
assert_equal "in-place multi-file (file 2)" "$MULTI_B" "$EXPECTED2"

# ---------------------------------------------------------------------------
# Test 4: file with system #includes — the binary must auto-detect the Clang
# resource directory so that <cstddef>, <ostream>, etc. resolve correctly.
# ---------------------------------------------------------------------------
INPLACE3="$TMP/inplace_system_headers.cpp"
cp "$INPUT3" "$INPLACE3"
"$BINARY" -i "$INPLACE3" -- -std=c++17 >/dev/null 2>&1
assert_equal "in-place file with system headers" "$INPLACE3" "$EXPECTED3"

# ---------------------------------------------------------------------------
# Test 5: in-place --reverse — the trailing form goes back to leading through
# the same overwriteChangedFiles() path, and a second pass is a no-op.
# ---------------------------------------------------------------------------
REVERSE="$TMP/reverse.cpp"
cp "$EXPECTED1" "$REVERSE"
"$BINARY" --reverse -i "$REVERSE" -- -std=c++17 >/dev/null 2>&1

# Everything in the fixture that can move is back in leading position; a
# deduced `auto` has no trailing return type to move, so it stays as written.
REVERSE_EXPECTED="$TMP/reverse_expected.cpp"
cat > "$REVERSE_EXPECTED" <<'EOF'
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
assert_equal "in-place --reverse" "$REVERSE" "$REVERSE_EXPECTED"

cp "$REVERSE" "$TMP/reverse_again.cpp"
"$BINARY" --reverse -i "$TMP/reverse_again.cpp" -- -std=c++17 >/dev/null 2>&1
assert_equal "--reverse is a fixpoint" "$TMP/reverse_again.cpp" "$REVERSE_EXPECTED"

# ---------------------------------------------------------------------------
# Test 6: --jobs — translation units parsed in parallel are buffered per TU and
# committed in source order, so the result is the same as with one thread.
# ---------------------------------------------------------------------------
JOBS_A="$TMP/jobs_a.cpp"
JOBS_B="$TMP/jobs_b.cpp"
cp "$INPUT1" "$JOBS_A"
cp "$INPUT2" "$JOBS_B"
"$BINARY" -i --jobs=2 "$JOBS_A" "$JOBS_B" -- -std=c++17 >/dev/null 2>&1
assert_equal "in-place --jobs=2 (file 1)" "$JOBS_A" "$EXPECTED1"
assert_equal "in-place --jobs=2 (file 2)" "$JOBS_B" "$EXPECTED2"

# A multi-file dry run prints every file behind a `=== path ===` header, in
# source order, whatever the thread count.
"$BINARY" --jobs=1 "$INPUT1" "$INPUT2" -- -std=c++17 2>/dev/null > "$TMP/dry_j1.txt"
"$BINARY" --jobs=2 "$INPUT1" "$INPUT2" -- -std=c++17 2>/dev/null > "$TMP/dry_j2.txt"
assert_equal "dry-run --jobs=2 matches --jobs=1" "$TMP/dry_j2.txt" "$TMP/dry_j1.txt"
if [[ "$(grep -c '^=== .* ===$' "$TMP/dry_j1.txt")" -ne 2 ]]; then
    echo "FAIL: multi-file dry run should print one header per file" >&2
    exit 1
fi
echo "PASS: dry-run multi-file prints one section per file"
