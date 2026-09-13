#!/usr/bin/env bash
# Integration tests for the symbol index pipeline:
#   * `cpp_format --emit-index=<unit.pb> ...` (per-TU unit emission, in the
#     per-source-file shape the Bazel aspect runs: one invocation per file,
#     told via --owned-files which other files' occurrences to record)
#   * `cpp_format --merge-index --output=<index.pb> [--records-from=<list>]`
#   * `cpp_format --dump-index [--format=...] [--lookup=<path>:<offset>]`
#
# Arguments (Bazel $(location ...) expansions):
#   $1  cpp_format binary

set -euo pipefail

cpp_format="$(realpath "$1")"

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

cat > "$tmpdir/widget.h" <<'EOF'
#ifndef WIDGET_H_
#define WIDGET_H_
#define BUMP(w) ((w).itemCount += 1)
struct Widget {
  int itemCount;
  int total() const;
};
// `w.itemCount` is a dependent token: which member it names is known only
// where the template is instantiated (widget.cpp), and that TU is the one
// that records the binding for this header's token.
template <class T>
int total_of(T& w) {
  return w.itemCount;
}
#endif
EOF
cat > "$tmpdir/widget.cpp" <<'EOF'
#include "widget.h"
int Widget::total() const { return itemCount; }
int use() {
  Widget w;
  w.itemCount = 3;
  BUMP(w);
  return w.total() + total_of(w);
}
EOF

cd "$tmpdir"
printf 'widget.h\n' > owned.txt

# The byte offset of the first match of $2 in file $1.
offset_of() { grep -bo -m1 -- "$2" "$1" | cut -d: -f1; }

# ---------------------------------------------------------------------------
# Test 1 — per-file emission, as the aspect runs it
# ---------------------------------------------------------------------------
"$cpp_format" --emit-index=widget_cpp.pb --owned-files=owned.txt \
  widget.cpp -- -x c++ -std=c++17 -I.
"$cpp_format" --emit-index=widget_h.pb widget.h -- -x c++ -std=c++17 -I.
[[ -s widget_cpp.pb && -s widget_h.pb ]] || fail "emit: no unit written"

# The .cpp's unit records the header's occurrences too (it is owned), so the
# header's definition of itemCount appears in it with the header's path.
"$cpp_format" --dump-index --format=text widget_cpp.pb > widget_cpp.txt
grep -q 'path: "widget.h"' widget_cpp.txt \
  || fail "emit: owned header not in the .cpp's unit"
grep -q 'usr: "c:@S@Widget@FI@itemCount"' widget_cpp.txt \
  || fail "emit: itemCount not indexed"
grep -q 'translation_units: "widget.cpp"' widget_cpp.txt \
  || fail "emit: translation unit not recorded relative to the cwd"
echo "PASS: --emit-index writes a unit with owned-file occurrences"

# ---------------------------------------------------------------------------
# Test 2 — the unit is byte-identical with --jobs=1 and --jobs=4
# ---------------------------------------------------------------------------
"$cpp_format" --emit-index=both_j1.pb --jobs=1 widget.cpp widget.h -- \
  -x c++ -std=c++17 -I.
"$cpp_format" --emit-index=both_j4.pb --jobs=4 widget.cpp widget.h -- \
  -x c++ -std=c++17 -I.
cmp both_j1.pb both_j4.pb || fail "emit: units differ between --jobs=1 and --jobs=4"
echo "PASS: --emit-index is identical with --jobs=1 and --jobs=4"

# ---------------------------------------------------------------------------
# Test 3 — merge through --records-from, dump as text and JSON
# ---------------------------------------------------------------------------
# Blank and padded lines are tolerated, and a unit listed twice is one unit.
printf '\n  widget_cpp.pb  \nwidget_h.pb\nwidget_cpp.pb\n\n' > units.txt
"$cpp_format" --merge-index --output=index.pb --records-from=units.txt
[[ -s index.pb ]] || fail "merge: no index written"
"$cpp_format" --dump-index --format=text index.pb > index.txt
grep -q 'usr: "c:@S@Widget@FI@itemCount"' index.txt || fail "merge: symbol lost"
grep -q '^per_file {' index.txt || fail "merge: not grouped per file"
"$cpp_format" --dump-index --format=json index.pb > index.json
grep -q '"path": "widget.cpp"' index.json || fail "merge: JSON dump lacks the .cpp"
grep -q '"path": "widget.h"' index.json || fail "merge: JSON dump lacks the header"
# Paths are relative to the working directory, never absolute.
grep -q "\"path\": \"$tmpdir" index.json && fail "merge: absolute path in index"
echo "PASS: --merge-index + --dump-index (text, json)"

# Merging the per-file units gives the same index as one invocation over both
# sources: a file's occurrences are the same whichever TU records them.
"$cpp_format" --merge-index --output=index_both.pb both_j1.pb
cmp index.pb index_both.pb \
  || fail "merge: per-file units and a two-source unit merge differently"
# An index is itself a valid merge input.
"$cpp_format" --merge-index --output=index_again.pb index.pb widget_h.pb
cmp index.pb index_again.pb || fail "merge: re-merging an index changed it"
echo "PASS: merge is independent of how the files were split into TUs"

# ---------------------------------------------------------------------------
# Test 4 — lookup: a token in the .cpp finds every use across both files
# ---------------------------------------------------------------------------
off_write="$(offset_of widget.cpp 'itemCount = 3')"
off_def="$(offset_of widget.h 'itemCount;')"
off_read="$(offset_of widget.cpp 'itemCount; }')"
off_bump="$(offset_of widget.cpp 'BUMP(w)')"
"$cpp_format" --dump-index --lookup="widget.cpp:$off_write" index.pb > lookup.txt
cat lookup.txt
grep -q '^c:@S@Widget@FI@itemCount$' lookup.txt || fail "lookup: wrong symbol"
grep -q '^  FIELD Widget::itemCount : int$' lookup.txt || fail "lookup: no kind line"
grep -q "^  canonical widget.h:$off_def-$((off_def + 9))$" lookup.txt \
  || fail "lookup: canonical location wrong"
grep -q "^  widget.h:$off_def-$((off_def + 9)) DEFINITION$" lookup.txt \
  || fail "lookup: definition in the header not listed"
grep -q "^  widget.cpp:$off_write-$((off_write + 9)) REFERENCE|WRITE$" lookup.txt \
  || fail "lookup: write in the .cpp not listed"
grep -q "^  widget.cpp:$off_read-$((off_read + 9)) REFERENCE|READ$" lookup.txt \
  || fail "lookup: read in the member function not listed"
# The reference spelled inside BUMP's body lands on the invocation token.
grep -q "^  widget.cpp:$off_bump-$((off_bump + 4)) REFERENCE|READ|WRITE macro-body$" lookup.txt \
  || fail "lookup: macro-body reference not on the invocation token"
# The same token from the other end: a lookup in the header.
"$cpp_format" --dump-index --lookup="widget.h:$off_def" index.pb > lookup2.txt
grep -q "^  widget.cpp:$off_write-$((off_write + 9)) REFERENCE|WRITE$" lookup2.txt \
  || fail "lookup: header token does not find the .cpp use"
"$cpp_format" --dump-index --lookup="widget.cpp:0" index.pb > lookup3.txt
grep -q '^no symbol at widget.cpp:0$' lookup3.txt || fail "lookup: miss not reported"
echo "PASS: --lookup finds cross-file occurrences"

# ---------------------------------------------------------------------------
# Test 5 — errors
# ---------------------------------------------------------------------------
"$cpp_format" --merge-index widget_h.pb 2>/dev/null && fail "merge without --output accepted"
[[ $("$cpp_format" --merge-index --output=x.pb --records-from=missing.txt 2>/dev/null; echo $?) == 2 ]] \
  || fail "merge: missing list did not exit 2"
[[ $("$cpp_format" --merge-index --output=x.pb --bogus 2>/dev/null; echo $?) == 2 ]] \
  || fail "merge: unknown flag did not exit 2"
[[ $("$cpp_format" --dump-index --lookup=nocolon index.pb 2>/dev/null; echo $?) == 2 ]] \
  || fail "dump: malformed --lookup did not exit 2"
[[ $("$cpp_format" --dump-index nonexistent.pb 2>/dev/null; echo $?) == 2 ]] \
  || fail "dump: missing input did not exit 2"
cat > cpp_format.yaml <<'EOF'
normalize_variables:
  - scope: member
    style: snake_case
EOF
[[ $("$cpp_format" --config=cpp_format.yaml --emit-index=y.pb widget.h -- \
      -x c++ -std=c++17 2>/dev/null; echo $?) == 1 ]] \
  || fail "emit: --emit-index with --config did not exit 1"
[[ ! -e y.pb ]] || fail "emit: unit written despite the refused combination"
echo "PASS: error handling"

# ---------------------------------------------------------------------------
# Test 6 — a dependent token is resolved from the instantiating TU
# ---------------------------------------------------------------------------
# (The comment in widget.h mentions the token too; take the code's spelling.)
off_dep="$(offset_of widget.h 'return w.itemCount')"
off_dep=$((off_dep + 9))
# The header's own unit spells the token but instantiates nothing: pending
# (`--dump-index` shows a unit in index form, where the field is `unresolved`).
"$cpp_format" --dump-index --format=text widget_h.pb > widget_h.txt
grep -q '^unresolved {' widget_h.txt || fail "dependent: header unit has no pending token"
grep -q 'name: "itemCount"' widget_h.txt || fail "dependent: pending token not named"
# The .cpp's unit binds it -- for a token in a file it does not own.
"$cpp_format" --dump-index --format=text widget_cpp.pb > widget_cpp2.txt
grep -q '^unresolved {' widget_cpp2.txt && fail "dependent: the .cpp unit left the token pending"
# Merged: no unresolved token, and the header token is a DEPENDENT use of the
# member from either end.
grep -q '^unresolved {' index.txt && fail "dependent: merged index still has an unresolved token"
grep -q "^  widget.h:$off_dep-$((off_dep + 9)) REFERENCE|DEPENDENT$" lookup.txt \
  || fail "dependent: use in the template not listed for the member"
"$cpp_format" --dump-index --lookup="widget.h:$off_dep" index.pb > lookup4.txt
grep -q '^c:@S@Widget@FI@itemCount$' lookup4.txt \
  || fail "dependent: lookup at the template token does not find the member"
grep -q "^  widget.cpp:$off_write-$((off_write + 9)) REFERENCE|WRITE$" lookup4.txt \
  || fail "dependent: lookup at the template token misses the .cpp use"
# The header alone: the token stays unresolved and the lookup says so.
"$cpp_format" --merge-index --output=header_only.pb widget_h.pb
"$cpp_format" --dump-index --lookup="widget.h:$off_dep" header_only.pb > lookup5.txt
grep -q "^dependent token 'itemCount' at widget.h:$off_dep-$((off_dep + 9)) is unresolved" lookup5.txt \
  || fail "dependent: unresolved token not reported"
echo "PASS: dependent tokens resolved across TUs"

echo "ALL INDEX INTEGRATION TESTS PASSED"
