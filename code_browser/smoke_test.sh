#!/usr/bin/env bash
# End to end over the demo index (//bazel/testdata:index.pb): import it,
# serve a copy of the demo sources, and hit every endpoint with http_get --
# then the same over the two-language index (//bazel/testdata:inventory_index.pb),
# for the link between a .proto and the C++ generated from it.
# Runs from the runfiles tree; the checkout is a *copy* of the demo files,
# since runfiles are symlinks out of the tree and the server refuses those.
set -euo pipefail

fail() { echo "FAIL: $*" >&2; exit 1; }

here="$PWD"
browser="$here/code_browser/code_browser"
import="$here/code_browser/index_import"
get="$here/code_browser/tools/http_get"
cpp_format="$here/cpp_formatting/cpp_format"
index="$here/bazel/testdata/index.pb"
[[ -x "$browser" && -x "$import" && -x "$get" && -f "$index" ]] \
  || fail "missing runfiles: $browser $import $get $index"

work="${TEST_TMPDIR:-/tmp}/smoke.$$"
mkdir -p "$work/repo/bazel/testdata"
cp -L bazel/testdata/demo.h bazel/testdata/demo.cpp bazel/testdata/demo_main.cpp \
  bazel/testdata/budget.h bazel/testdata/demo_test.cpp bazel/testdata/demo_testing.h \
  "$work/repo/bazel/testdata/"

# 1. Import.
"$import" "$index" --out="$work/index.sqlite" > "$work/import.out"
grep -q "6 files, 20 symbols" "$work/import.out" || { cat "$work/import.out"; fail "import stats"; }
[[ $("$import" "$index" --out="$work/index.sqlite" 2>&1 || true) == *"already exists"* ]] \
  || fail "a second import must refuse to overwrite"

# 1b. The browser's own import mode -- what the prebuilt Bazel kit runs, which
#     ships this one binary and no index_import.  Unlike index_import it
#     replaces an existing database (a Bazel action may be re-run over a stale
#     output), and what it writes must open to the same stats.
"$browser" --index="$index" --import-to="$work/kit.sqlite" 2> "$work/kit.err" \
  || { cat "$work/kit.err"; fail "--import-to failed"; }
"$browser" --index="$index" --import-to="$work/kit.sqlite" 2>> "$work/kit.err" \
  || { cat "$work/kit.err"; fail "--import-to must replace an existing database"; }
"$browser" --db="$work/kit.sqlite" --check 2> "$work/kit.check" || fail "--check on the kit's database"
"$browser" --db="$work/index.sqlite" --check 2> "$work/import.check" || fail "--check on index_import's"
stats() { sed -n 's/.*: \([0-9]* files, [0-9]* symbols, [0-9]* occurrences\).*/\1/p' "$1" | head -1; }
[[ -n "$(stats "$work/kit.check")" && "$(stats "$work/kit.check")" == "$(stats "$work/import.check")" ]] \
  || { cat "$work/kit.check" "$work/import.check"; fail "--import-to and index_import disagree"; }
if "$browser" --import-to="$work/none.sqlite" 2>/dev/null; then fail "--import-to without --index must fail"; fi

# 2. Serve, with coverage.  The tracefile sits under the root, as
#    `cpp_format.sh coverage` puts it next to the index: the full-text index
#    must leave it out (the 6 files below), and one of its paths is absolute,
#    as a report collected outside a sandbox spells it.
cat > "$work/repo/coverage.lcov" <<LCOV
SF:bazel/testdata/demo.cpp
DA:1,3
DA:2,0
BRDA:1,0,0,1
BRDA:1,0,1,0
end_of_record
SF:$work/repo/bazel/testdata/demo_main.cpp
DA:3,1
DA:4,0
end_of_record
LCOV
printf 'SF:a.cc\nDA:1\nend_of_record\n' > "$work/bad.lcov"
if "$browser" --db="$work/index.sqlite" --root="$work/repo" --coverage="$work/bad.lcov" \
    --check 2> "$work/bad.err"; then
  fail "a malformed tracefile was accepted"
fi
grep -q "bad.lcov:2: DA: expected" "$work/bad.err" || { cat "$work/bad.err"; fail "the LCOV error names no line"; }
"$browser" --db="$work/index.sqlite" --root="$work/repo" --port=0 \
  --coverage="$work/repo/coverage.lcov" \
  --port-file="$work/port" --log-requests=false > "$work/server.log" 2>&1 &
server=$!
trap 'kill "$server" 2>/dev/null || true' EXIT
for _ in $(seq 1 100); do [[ -s "$work/port" ]] && break; sleep 0.1; done
[[ -s "$work/port" ]] || { cat "$work/server.log"; fail "server did not start"; }
url="http://127.0.0.1:$(cat "$work/port")"

# 3. Endpoints.
"$get" "$url/api/repo" > "$work/repo.json"
grep -q '"files": *6' "$work/repo.json" || { cat "$work/repo.json"; fail "repo stats"; }
grep -q "\"root\": *\"$work/repo\"" "$work/repo.json" || fail "repo root"

"$get" "$url/api/file?path=bazel/testdata/demo.h" > "$work/demo.h"
cmp "$work/demo.h" bazel/testdata/demo.h || fail "file bytes differ"
"$get" -I "$url/api/file?path=bazel/testdata/demo.h" > "$work/demo.head"
grep -qi "^ETag: " "$work/demo.head" || fail "no ETag on a file"
grep -qi "^X-File-Kind: SOURCE" "$work/demo.head" || fail "no X-File-Kind"
etag=$(grep -i "^ETag: " "$work/demo.head" | sed 's/^[^ ]* //' | tr -d '\r')
[[ $("$get" -I -H "If-None-Match: $etag" "$url/api/file?path=bazel/testdata/demo.h" 2>/dev/null; echo "rc=$?") == *"304"*"rc=22"* ]] \
  || fail "If-None-Match did not give 304"

"$get" "$url/api/annotations?path=bazel/testdata/demo.h" > "$work/ann.json"
grep -q 'c:@S@Widget@FI@item_count_' "$work/ann.json" || { cat "$work/ann.json"; fail "annotations lack the member"; }
grep -q '"DEPENDENT"\|"roles"' "$work/ann.json" || fail "annotations lack spans"

"$get" "$url/api/symbol?usr=c:@S@Widget@FI@item_count_" > "$work/sym.json"
id=$(grep -o '"id": *[0-9]*' "$work/sym.json" | head -1 | grep -o '[0-9]*$')
[[ -n "$id" ]] || { cat "$work/sym.json"; fail "no symbol id"; }
grep -q '"qualified_name": *"Widget::item_count_"' "$work/sym.json" || fail "symbol info"
grep -q '"line": *12' "$work/sym.json" || { cat "$work/sym.json"; fail "definition line"; }

"$get" "$url/api/refs/$id" > "$work/refs.json"
grep -q 'bazel/testdata/demo_main.cpp' "$work/refs.json" || { cat "$work/refs.json"; fail "refs lack demo_main.cpp"; }
grep -q '"line_text": *"  w.item_count_ = 4;"' "$work/refs.json" || { cat "$work/refs.json"; fail "line text"; }
grep -q 'REFERENCE|DEPENDENT' "$work/refs.json" || fail "the dependent use in the template"
# Test code (the files of //bazel/testdata:demo_test and :demo_testing, which
# the aspect marked testonly) comes after every other use, and says so.
[[ "$(grep -o '"path": *"[^"]*"' "$work/refs.json" | tail -1)" == *demo_test* ]] \
  || { cat "$work/refs.json"; fail "the uses in tests are not last"; }
grep -q '"test": *true' "$work/refs.json" || { cat "$work/refs.json"; fail "no file of the refs is marked test"; }

"$get" "$url/api/search?q=item_count" > "$work/search.json"
grep -q '"name": *"item_count_"' "$work/search.json" || { cat "$work/search.json"; fail "search"; }

# Full text: every file under the root, whether indexed or not.
grep -q '"text_index": *{[^}]*"files": *"6"' "$work/repo.json" || { cat "$work/repo.json"; fail "repo text_index stats"; }
grep -q "text index $work/index.sqlite.fts: 6 files .*built in" "$work/server.log" \
  || { cat "$work/server.log"; fail "the text index was not built"; }
"$get" "$url/api/text?q=item_count_%20%3D%204" > "$work/text.json"
grep -q '"path": *"bazel/testdata/demo_main.cpp"' "$work/text.json" || { cat "$work/text.json"; fail "text search path"; }
grep -q '"text": *"  w.item_count_ = 4;"' "$work/text.json" || { cat "$work/text.json"; fail "text search line"; }
[[ $("$get" "$url/api/text?q=ITEM_COUNT_%20%3D%204") != *'"files"'* ]] || fail "text search ignored case"
"$get" "$url/api/text?q=ITEM_COUNT_%20%3D%204&case=insensitive" | grep -q 'demo_main.cpp' \
  || fail "case-insensitive text search"
"$get" "$url/api/text?q=" > /dev/null 2>&1 && fail "an empty text query was answered"

"$get" "$url/api/files?prefix=bazel/testdata" > "$work/files.json"
grep -q '"name": *"demo_main.cpp"' "$work/files.json" || fail "file list"
grep -q '"available": *true' "$work/files.json" || fail "files should be available"

# Coverage: the stats line, the report's totals, a file's lines, the tree.
grep -q "coverage $work/repo/coverage.lcov: 2 files (2 in the index), lines 2/4 (50.0%), branches 1/2 (50.0%)" \
  "$work/server.log" || { cat "$work/server.log"; fail "coverage stats line"; }
grep -q '"coverage": *{[^}]*"path": *"'"$work"'/repo/coverage.lcov"' "$work/repo.json" \
  || { cat "$work/repo.json"; fail "repo coverage info"; }
"$get" "$url/api/coverage?path=bazel/testdata/demo.cpp" > "$work/cov.json"
grep -q '"lines": *\[1, *2\]' "$work/cov.json" || { cat "$work/cov.json"; fail "coverage lines"; }
grep -q '"hits": *\[3, *0\]' "$work/cov.json" || { cat "$work/cov.json"; fail "coverage hits"; }
"$get" "$url/api/coverage?path=bazel/testdata/demo_main.cpp" | grep -q '"lines_hit": *1' \
  || fail "the absolute SF path did not reach demo_main.cpp"
"$get" "$url/api/coverage?path=bazel/testdata/demo.h" > /dev/null 2>&1 \
  && fail "coverage for a file the tracefile does not name"
grep -q '"coverage": *{' "$work/files.json" || { cat "$work/files.json"; fail "no coverage in the tree"; }

# 4. /api/at agrees with cpp_format --dump-index --lookup.
off=$(grep -bo -m1 'item_count_ = 4' bazel/testdata/demo_main.cpp | cut -d: -f1)
"$get" "$url/api/at?path=bazel/testdata/demo_main.cpp&offset=$off" > "$work/at.json"
grep -q 'c:@S@Widget@FI@item_count_' "$work/at.json" || { cat "$work/at.json"; fail "/api/at"; }
if [[ -x "$cpp_format" ]]; then
  "$cpp_format" --dump-index --lookup="bazel/testdata/demo_main.cpp:$off" "$index" > "$work/lookup.txt"
  grep -q '^c:@S@Widget@FI@item_count_$' "$work/lookup.txt" || fail "dump-index lookup differs"
fi

# 5. Errors and safety.
"$get" "$url/api/file?path=../../etc/passwd" > /dev/null 2>&1 && fail "path traversal served"
"$get" "$url/api/file?path=/etc/passwd" > /dev/null 2>&1 && fail "absolute non-index path served"
"$get" "$url/api/nope" > /dev/null 2>&1 && fail "unknown endpoint served"
"$get" "$url/" | grep -q "<html" || fail "index page"
"$get" "$url/app.js" > /dev/null || fail "app.js"

# 6. Shutdown on SIGINT.
kill -INT "$server"
wait "$server" || fail "server exited non-zero on SIGINT"
trap - EXIT

# 7. Two languages.  //bazel/testdata:inventory_index.pb is the C++ aspect's
#    units for inventory_user.cpp merged with the proto aspect's for the two
#    .proto files -- real protoc annotations, real Clang ranges, linked by the
#    merge.  The server answers the link from both ends.
inventory="$here/bazel/testdata/inventory_index.pb"
[[ -f "$inventory" ]] || fail "missing runfile: $inventory"
mkdir -p "$work/inventory/bazel/testdata/proto"
cp -L bazel/testdata/inventory_user.cpp "$work/inventory/bazel/testdata/"
cp -L bazel/testdata/proto/inventory.proto bazel/testdata/proto/unit.proto \
  "$work/inventory/bazel/testdata/proto/"
"$browser" --index="$inventory" --import-to="$work/inventory.sqlite" 2> "$work/inventory.err" \
  || { cat "$work/inventory.err"; fail "importing the two-language index"; }
rm -f "$work/port"
"$browser" --db="$work/inventory.sqlite" --root="$work/inventory" --port=0 \
  --port-file="$work/port" --log-requests=false > "$work/inventory.log" 2>&1 &
server=$!
trap 'kill "$server" 2>/dev/null || true' EXIT
for _ in $(seq 1 100); do [[ -s "$work/port" ]] && break; sleep 0.1; done
[[ -s "$work/port" ]] || { cat "$work/inventory.log"; fail "second server did not start"; }
url="http://127.0.0.1:$(cat "$work/port")"

# The .proto files are in the tree under their real paths (unit.proto is
# imported as "proto/unit.proto" and reaches protoc as a symlink).
# No --coverage here: the route says so.
[[ $("$get" "$url/api/coverage?path=bazel/testdata/inventory_user.cpp" 2>&1; echo "rc=$?") == *"503"*"rc=22"* ]] \
  || fail "/api/coverage without coverage is not a 503"

"$get" "$url/api/files?prefix=bazel/testdata/proto" > "$work/protos.json"
grep -q '"path": *"bazel/testdata/proto/unit.proto"' "$work/protos.json" \
  || { cat "$work/protos.json"; fail "unit.proto is not in the tree"; }
# ... and the import is a link to it.
"$get" "$url/api/includes?path=bazel/testdata/proto/inventory.proto" > "$work/imports.json"
grep -q '"path": *"bazel/testdata/proto/unit.proto"' "$work/imports.json" \
  || { cat "$work/imports.json"; fail "the import of proto/unit.proto does not resolve"; }

# proto -> C++: the field lists its accessors, and its references are theirs.
"$get" "$url/api/symbol?usr=proto:demo.inventory.Inventory.Slot.count" > "$work/count.json"
grep -q '"language": *"PROTO"' "$work/count.json" || { cat "$work/count.json"; fail "the field's language"; }
grep -q '"qualified_name": *"demo::inventory::Inventory_Slot::set_count"' "$work/count.json" \
  || { cat "$work/count.json"; fail "set_count is not generated from the field"; }
count_id="$(grep -o '"id": *[0-9]*' "$work/count.json" | head -1 | grep -o '[0-9]*$')"
"$get" "$url/api/refs/$count_id?expand=generated" > "$work/count_refs.json"
# (`>` is \u003e in the JSON, so the line is matched from `set_count` on.)
grep -q 'set_count(3);"' "$work/count_refs.json" \
  || { cat "$work/count_refs.json"; fail "the call of set_count is not a reference of the field"; }
grep -q '"modifies_origin": *true' "$work/count_refs.json" \
  || { cat "$work/count_refs.json"; fail "the setter is not marked as writing the field"; }

# C++ -> proto: what the user's tokens are annotated with names the field, in
# the .proto.
"$get" "$url/api/annotations?path=bazel/testdata/inventory_user.cpp" > "$work/user_ann.json"
grep -q '"usr": *"proto:demo.inventory.Inventory.Slot.count"' "$work/user_ann.json" \
  || fail "the accessors in inventory_user.cpp carry no origin"
grep -q '"path": *"bazel/testdata/proto/inventory.proto"' "$work/user_ann.json" \
  || fail "the origin is not located in the .proto"
# The enumerator of the file with the stripped import prefix.
grep -q '"usr": *"proto:demo.inventory.PIECE"' "$work/user_ann.json" \
  || fail "PIECE does not lead to its enumerator"

kill -INT "$server"
wait "$server" || fail "second server exited non-zero on SIGINT"
trap - EXIT
echo "smoke test passed"
