#!/usr/bin/env bash
# End to end over the demo index (//bazel/testdata:index.index): import it,
# serve a copy of the demo sources, and hit every endpoint with http_get.
# Runs from the runfiles tree; the checkout is a *copy* of the demo files,
# since runfiles are symlinks out of the tree and the server refuses those.
set -euo pipefail

fail() { echo "FAIL: $*" >&2; exit 1; }

here="$PWD"
browser="$here/code_browser/code_browser"
import="$here/code_browser/index_import"
get="$here/code_browser/tools/http_get"
cpp_format="$here/cpp_formatting/cpp_format"
index="$here/bazel/testdata/index.index.pb"
[[ -x "$browser" && -x "$import" && -x "$get" && -f "$index" ]] \
  || fail "missing runfiles: $browser $import $get $index"

work="${TEST_TMPDIR:-/tmp}/smoke.$$"
mkdir -p "$work/repo/bazel/testdata"
cp -L bazel/testdata/demo.h bazel/testdata/demo.cpp bazel/testdata/demo_main.cpp \
  bazel/testdata/budget.h "$work/repo/bazel/testdata/"

# 1. Import.
"$import" "$index" --out="$work/index.sqlite" > "$work/import.out"
grep -q "4 files, 15 symbols" "$work/import.out" || { cat "$work/import.out"; fail "import stats"; }
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

# 2. Serve.
"$browser" --db="$work/index.sqlite" --root="$work/repo" --port=0 \
  --port-file="$work/port" --log-requests=false > "$work/server.log" 2>&1 &
server=$!
trap 'kill "$server" 2>/dev/null || true' EXIT
for _ in $(seq 1 100); do [[ -s "$work/port" ]] && break; sleep 0.1; done
[[ -s "$work/port" ]] || { cat "$work/server.log"; fail "server did not start"; }
url="http://127.0.0.1:$(cat "$work/port")"

# 3. Endpoints.
"$get" "$url/api/repo" > "$work/repo.json"
grep -q '"files": *4' "$work/repo.json" || { cat "$work/repo.json"; fail "repo stats"; }
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

"$get" "$url/api/search?q=item_count" > "$work/search.json"
grep -q '"name": *"item_count_"' "$work/search.json" || { cat "$work/search.json"; fail "search"; }

"$get" "$url/api/files?prefix=bazel/testdata" > "$work/files.json"
grep -q '"name": *"demo_main.cpp"' "$work/files.json" || fail "file list"
grep -q '"available": *true' "$work/files.json" || fail "files should be available"

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
echo "smoke test passed"
