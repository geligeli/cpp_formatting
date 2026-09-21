#!/usr/bin/env bash
# Integration tests for the symbol index pipeline:
#   * `cpp_format --emit-index=<unit.pb> ...` (per-TU unit emission, in the
#     per-source-file shape the Bazel aspect runs: one invocation per file,
#     told via --owned-files which other files' occurrences to record)
#   * `cpp_format --merge-index --output=<index.pb> [--records-from=<list>]`
#   * `cpp_format --dump-index [--format=...] [--lookup=<path>:<offset>]`
#   * `cpp_format --emit-proto-index=<unit.pb> [--anchors=...] file.proto` and
#     the merge's link between generated C++ and the .proto it came from
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

# ---------------------------------------------------------------------------
# Test 7 — a second language: .proto units, anchors and the generated link
# ---------------------------------------------------------------------------
# `--emit-proto-index` describes a .proto; given what the code generator said
# about its output (protoc's GeneratedCodeInfo: byte ranges of the generated
# header, each with the path of the descriptor it came from) it also anchors
# the proto symbols on those ranges, and the merge links every C++ symbol
# declared on an anchored range to them.  The header here is written by hand
# and the metadata byte by byte, so the test needs no protoc.
mkdir -p proto/gen
printf 'syntax = "proto3";\npackage demo;\nmessage W {\n\tint32 size = 1;\n}\n' \
  > proto/w.proto
cat > proto/gen/w.pb.h <<'EOF'
struct W {
  int size() const;
  void set_size(int v);
};
EOF
cat > proto/user.cpp <<'EOF'
#include "gen/w.pb.h"
void use(W& w) { w.set_size(w.size() + 1); }
EOF
byte() { printf "\\$(printf '%03o' "$1")"; }
# One GeneratedCodeInfo.annotation: path (packed), source_file, begin, end and
# an optional semantic (1 = SET).  Every number here is below 128, so every
# varint is one byte.
annotation() {  # <begin> <end> <semantic> <path...>
  local begin="$1" end="$2" semantic="$3"; shift 3
  local source="w.proto" body
  body="$(mktemp)"
  { byte 10; byte "$#"; for p in "$@"; do byte "$p"; done
    byte 18; byte "${#source}"; printf '%s' "$source"
    byte 24; byte "$begin"; byte 32; byte "$end"
    if [[ "$semantic" != 0 ]]; then byte 40; byte "$semantic"; fi
  } > "$body"
  byte 10; byte "$(wc -c < "$body")"; cat "$body"
  rm -f "$body"
}
off_get="$(offset_of proto/gen/w.pb.h 'size() const')"
off_set="$(offset_of proto/gen/w.pb.h 'set_size')"
off_class="$(offset_of proto/gen/w.pb.h 'W {')"
{ annotation "$off_class" "$((off_class + 1))" 0 4 0
  annotation "$off_get" "$((off_get + 4))" 0 4 0 2 0
  annotation "$off_set" "$((off_set + 8))" 1 4 0 2 0
} > proto/w.pb.h.meta

( cd proto
  printf 'gen/w.pb.h\n' > owned.txt
  "$cpp_format" --emit-index=user.pb --owned-files=owned.txt user.cpp -- -std=c++17
  "$cpp_format" --emit-proto-index=w.pb --proto-path=. \
    --anchors=w.pb.h.meta=gen/w.pb.h w.proto
  "$cpp_format" --merge-index --output=index.pb user.pb w.pb
  "$cpp_format" --dump-index --format=text index.pb > index.txt )
grep -q 'usr: "proto:demo.W.size"' proto/index.txt || fail "proto: field symbol missing"
grep -q 'language: PROTO' proto/index.txt || fail "proto: language not recorded"
grep -q 'kind: MESSAGE' proto/index.txt || fail "proto: message kind missing"
grep -q 'path: "w.proto"' proto/index.txt || fail "proto: the .proto is not a file of the index"
# The field's name comes after a tab: a column the tokenizer counts as eight.
off_field="$(offset_of proto/w.proto 'size = 1')"
"$cpp_format" --dump-index --lookup="w.proto:$off_field" proto/index.pb > proto/lookup_field.txt
grep -q '^proto:demo.W.size$' proto/lookup_field.txt \
  || fail "proto: lookup at the field does not find it (tab columns?)"
grep -q "^  w.proto:$off_field-$((off_field + 4)) DEFINITION$" proto/lookup_field.txt \
  || fail "proto: field definition range wrong"
grep -q "^  gen/w.pb.h:$off_set-$((off_set + 8)) WRITE|GENERATES$" proto/lookup_field.txt \
  || fail "proto: setter anchor missing"
grep -q '^  generates c:@S@W@F@set_size#I#$' proto/lookup_field.txt \
  || fail "proto: the field does not list the setter generated from it"
grep -q '^  generates c:@S@W@F@size#1$' proto/lookup_field.txt \
  || fail "proto: the field does not list the getter generated from it"
# From the code: the call in user.cpp leads to the proto field.
off_call="$(offset_of proto/user.cpp 'set_size')"
"$cpp_format" --dump-index --lookup="user.cpp:$off_call" proto/index.pb > proto/lookup_call.txt
grep -q '^  generated from proto:demo.W.size$' proto/lookup_call.txt \
  || fail "proto: the setter call does not lead to the proto field"
off_w="$(offset_of proto/user.cpp 'W&')"
"$cpp_format" --dump-index --lookup="user.cpp:$off_w" proto/index.pb > proto/lookup_class.txt
grep -q '^  generated from proto:demo.W$' proto/lookup_class.txt \
  || fail "proto: the class does not lead to the message"
# The linked index is a merge input like any other, and a fixpoint.
"$cpp_format" --merge-index --output=proto/again.pb proto/index.pb
cmp -s proto/index.pb proto/again.pb || fail "proto: re-merging the linked index changed it"
"$cpp_format" --merge-index --output=proto/reordered.pb proto/w.pb proto/user.pb
cmp -s proto/index.pb proto/reordered.pb || fail "proto: the link depends on the input order"
# A .proto that does not compile is an error with the parser's diagnostic.
printf 'syntax = "proto3";\nmessage M { Missing m = 1; }\n' > proto/bad.proto
if ( cd proto && "$cpp_format" --emit-proto-index=bad.pb --proto-path=. bad.proto ) 2> proto/bad.err; then
  fail "proto: a broken .proto was indexed"
fi
grep -q 'bad.proto:2:13' proto/bad.err || fail "proto: no diagnostic for the broken .proto"
echo "PASS: proto units, anchors and the generated link"

echo "ALL INDEX INTEGRATION TESTS PASSED"
