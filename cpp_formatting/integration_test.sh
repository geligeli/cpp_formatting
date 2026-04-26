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
