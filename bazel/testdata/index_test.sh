#!/usr/bin/env bash
# Reads the merged index that //bazel/testdata:index.index built and checks the
# cross-target reference: demo_main.cpp (a cc_binary) uses Widget::item_count_,
# which //bazel/testdata:demo declares in demo.h.  Both were indexed by
# separate per-file actions and merged by the index target's own action.
#
# Arguments (Bazel $(location ...) expansions):
#   $1  the merged index (index.index.pb)
#   $2  cpp_format binary
#   $3  demo.h
#   $4  demo_main.cpp

set -euo pipefail

index="$1"
cpp_format="$2"
demo_h="$3"
demo_main="$4"
out="${TEST_TMPDIR:-/tmp}"
fail() { echo "FAIL: $*" >&2; exit 1; }

# The byte offset of the first match of $2 in file $1.
offset_of() { grep -bo -m1 -- "$2" "$1" | cut -d: -f1; }

"$cpp_format" --dump-index --format=text "$index" > "$out/index.txt"

# Every file is recorded under its exec-root-relative path -- what the aspect's
# actions see -- never an absolute one, so the index is portable.
for f in demo.h demo.cpp demo_main.cpp; do
  grep -q "path: \"bazel/testdata/$f\"" "$out/index.txt" \
    || fail "$f not in the index under its exec-root-relative path"
done
grep -q 'path: "/' "$out/index.txt" && fail "absolute path in the index"
grep -q 'kind: SOURCE' "$out/index.txt" || fail "first-party files not SOURCE"
grep -q 'usr: "c:@S@Widget@FI@item_count_"' "$out/index.txt" \
  || fail "Widget::item_count_ not in the index"
echo "PASS: index lists the demo's files and symbols"

# A token in the binary's source finds the definition in the library's header.
off_use="$(offset_of "$demo_main" 'item_count_ = 4')"
off_def="$(offset_of "$demo_h" 'item_count_;')"
"$cpp_format" --dump-index --lookup="bazel/testdata/demo_main.cpp:$off_use" "$index" \
  > "$out/lookup.txt"
cat "$out/lookup.txt"
grep -q '^c:@S@Widget@FI@item_count_$' "$out/lookup.txt" \
  || fail "lookup in demo_main.cpp did not resolve to Widget::item_count_"
grep -q "^  bazel/testdata/demo.h:$off_def-$((off_def + 11)) DEFINITION$" "$out/lookup.txt" \
  || fail "definition in demo.h not listed"
grep -q "^  bazel/testdata/demo_main.cpp:$off_use-$((off_use + 11)) REFERENCE|WRITE$" "$out/lookup.txt" \
  || fail "use in demo_main.cpp not listed"
echo "PASS: cross-target lookup"

# `w.item_count_` in demo.h's template is a dependent token: demo.h's own
# action cannot resolve it, and the actions for demo.cpp and demo_main.cpp
# (which instantiate total_of) record the binding for the header's token.
# (demo.h's comment mentions the token too; take the code's spelling.)
off_dep="$(offset_of "$demo_h" 'return w.item_count_')"
off_dep=$((off_dep + 9))
grep -q '^unresolved {' "$out/index.txt" && fail "an unresolved dependent token remains"
grep -q "^  bazel/testdata/demo.h:$off_dep-$((off_dep + 11)) REFERENCE|DEPENDENT$" "$out/lookup.txt" \
  || fail "dependent use in demo.h's template not listed"
echo "PASS: dependent token resolved across targets"

echo "ALL INDEX TESTS PASSED"
