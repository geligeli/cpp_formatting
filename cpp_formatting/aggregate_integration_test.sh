#!/usr/bin/env bash
# Integration tests for the per-TU emit + aggregate pipeline:
#   * `cpp_format --emit-edits=<records.json> ...` (per-TU record emission)
#   * `cpp_format --aggregate [--check|--apply] <records.json>...`
#   * equivalence with the standalone `aggregate_edits` binary.
#
# Exercises the cross-TU template-dependent-token path: a header-only template
# whose member token is resolved from the instantiating .cpp's records.
#
# Arguments (Bazel $(location ...) expansions):
#   $1  cpp_format binary
#   $2  aggregate_edits binary

set -euo pipefail

cpp_format="$(realpath "$1")"
aggregate_edits="$(realpath "$2")"

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

cat > "$tmpdir/cpp_format.yaml" <<'EOF'
normalize_variables:
  - scope: member
    style: snake_case
EOF

# Header-only template with a dependent member token, instantiated in the .cpp.
cat > "$tmpdir/widget.h" <<'EOF'
#ifndef WIDGET_H_
#define WIDGET_H_
struct Widget {
  int itemCount;
};
template <class T>
int total_of(T& w) {
  return w.itemCount;
}
#endif
EOF
cat > "$tmpdir/widget.cpp" <<'EOF'
#include "widget.h"
int use() {
  Widget w;
  w.itemCount = 3;
  return total_of(w);
}
EOF

cd "$tmpdir"

# Phase 1 — emit per-TU records (the parallelizable step under Bazel).
"$cpp_format" --config=cpp_format.yaml --emit-edits=widget_cpp.json \
  widget.cpp widget.h -- -x c++ -std=c++17 -I.
"$cpp_format" --config=cpp_format.yaml --emit-edits=widget_h.json \
  widget.h -- -x c++ -std=c++17 -I.

# The header TU alone cannot resolve the dependent token; the .cpp's records
# must carry the resolution.
grep -q '"resolutions"' widget_cpp.json \
  || fail "emit: widget.cpp records missing resolutions sidecar"

# ---------------------------------------------------------------------------
# Test 1 — folded `cpp_format --aggregate` diff == standalone aggregate_edits
# ---------------------------------------------------------------------------
"$aggregate_edits" --root="$tmpdir" widget_cpp.json widget_h.json > diff_standalone.patch
"$cpp_format" --aggregate --root="$tmpdir" widget_cpp.json widget_h.json > diff_folded.patch
diff -u diff_standalone.patch diff_folded.patch \
  || fail "aggregate: folded diff differs from standalone aggregate_edits"
grep -q '+  int item_count;' diff_folded.patch \
  || fail "aggregate: member decl not renamed in diff"
grep -q '+  return w.item_count;' diff_folded.patch \
  || fail "aggregate: cross-TU dependent token not rewritten in diff"
echo "PASS: cpp_format --aggregate diff matches aggregate_edits (incl. dependent token)"

# ---------------------------------------------------------------------------
# Test 2 — `--aggregate --check` reports violations and exits 1
# ---------------------------------------------------------------------------
set +e
check_out="$("$cpp_format" --aggregate --check --root="$tmpdir" \
  widget_cpp.json widget_h.json 2>/dev/null)"
rc=$?
set -e
[[ $rc -eq 1 ]] || fail "check: expected exit 1 on violations, got $rc"
grep -q 'widget.h' <<<"$check_out" || fail "check: missing per-file count"
echo "PASS: cpp_format --aggregate --check exits 1 with per-file counts"

# ---------------------------------------------------------------------------
# Test 3 — `--aggregate --apply` rewrites files; result is consistent + clean
# ---------------------------------------------------------------------------
"$cpp_format" --aggregate --apply --root="$tmpdir" widget_cpp.json widget_h.json
grep -q 'int item_count;' widget.h || fail "apply: member decl not renamed on disk"
grep -q 'return w.item_count;' widget.h \
  || fail "apply: dependent token not renamed on disk"
grep -q 'w.item_count = 3;' widget.cpp || fail "apply: use not renamed on disk"

# Re-emitting against the fixed sources yields no edits -> check exits 0.
"$cpp_format" --config=cpp_format.yaml --emit-edits=fixed.json \
  widget.cpp widget.h -- -x c++ -std=c++17 -I.
"$cpp_format" --aggregate --check --root="$tmpdir" fixed.json \
  || fail "check: clean tree should exit 0"
echo "PASS: cpp_format --aggregate --apply rewrites files; re-check is clean"

echo "All aggregate integration tests passed."
