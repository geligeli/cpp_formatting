# code_browser

An HTTP server that serves a git checkout with every indexed token
annotated: click an identifier and a panel below the code says what it is,
where it is defined and declared, what it is related to, and lists its
references (one tab per symbol when a token names several); the panel stays
open while you follow them into other files. An `#include` is a link to the
file it names, and so is a `.proto`'s `import`. The index spans languages:
a field in a `.proto` lists the uses of the C++ accessors generated from it,
and a generated symbol leads with what it was generated from (a second tab on
the token, and where ctrl-click goes). The index is the one `cpp_format
--merge-index` produces (see
[cpp_formatting/index.proto](../cpp_formatting/index.proto)), imported once
into SQLite.

The search box finds symbols as you type; Enter searches the *text* of the
checkout instead -- every text file under the root, indexed or not (BUILD
files, docs, scripts) -- and shows the matching lines, grouped by file, in the
middle of the page (`#?text=<q>`, so Back returns to them). The file tree, the
panel and the panel's two columns (occurrences on the left, everything else
about the symbol on the right, uses in test code last -- the files of
testonly Bazel targets, as the index aspect recorded them) are resizable; the
sizes are kept per browser.

With the tests' coverage (`--coverage=<lcov>`, or `cpp_format.sh coverage`,
which runs the tests and starts the server with it), every instrumented line
is tinted -- green hit, red missed, orange ran but left a branch untaken --
with its hit count in a column after the line number; the file tree shows
each file's and directory's line coverage, the status bar the open file's,
and `u` / `U` step through the runs of missed lines. The **coverage** button
in the header (it shows the report's overall percentage) turns all of it off
and on, remembered per browser.

```sh
# One command: indexes the whole repository (or a target pattern), prints the
# browser's binary and command line, and serves the workspace on the index.
# Run it again after editing: only the changed translation units are re-parsed.
tools/cpp_format.sh browse                         # [pattern] [--port=N] [--check]
# The same, after running the tests under the pattern with `bazel coverage`,
# with their line coverage overlaid (the report is copied to index.pb.lcov):
tools/cpp_format.sh coverage                       # [pattern] [--port=N] [--check]

# Or by hand:
# 1. An index of the repository:
tools/cpp_format.sh index                          # writes index.pb
# 2. Serve it.  --index imports into index.pb.sqlite when that is missing or
#    older than the .pb; --db skips the import.
bazel run //code_browser -- --index=$PWD/index.pb --root=$PWD
# 3. Open the URL it prints: a free port by default, --port=N to choose.
```

`index_import index.pb [--out=x.sqlite] [--force]` does the import on its
own. `code_browser --check` opens the database (and the full-text index,
building it if it is stale, and the coverage), prints their stats and exits.
`--text-search=false` turns full-text search off; `--text-index=<path>`
moves its file (default `<index>.fts`, or `<db>.fts`); `--text-max-file-kb`
(4096) leaves larger files out. `--coverage=<lcov>` overlays an LCOV
tracefile; see "Coverage".

## Layers

| layer | files | knows about |
|---|---|---|
| storage | `index_schema.*` (writer), `index_db.*` (reader), `sqlite_util.*` | SQLite, the proto |
| checkout | `repo.*`, `file_cache.*` | the filesystem, `.git`, the exec root |
| full text | `text_index.*`, `suffix_array.*` | the files under the root, libsais; nothing about the symbol index |
| coverage | `coverage.*` | an LCOV tracefile; nothing about the index but its path spelling |
| API | `api.proto`, `api.*` | requests and JSON; nothing about sockets |
| HTTP | `http_server.*`, `static_assets.*` | Boost.Beast/Asio; nothing about the index |
| page | `web/index.html`, `web/app.js`, `web/app.css` | `/api/*` only |

Each layer has its own test (`index_db_test`, `repo_test`, `text_index_test`
and `suffix_array_test`, `coverage_test`, `api_test`, `http_server_test`);
`cross_language_test` is the API over an index of two languages, merged and
linked as `--merge-index` does it; `smoke_test` runs the
binaries end to end over the demo index in `//bazel/testdata`, and then over
the two-language one (`inventory_index.pb`: real protoc annotations, real Clang
ranges) for the link between a `.proto` and the C++ generated from it.

## API

`GET`/`HEAD`; JSON bodies are the messages of `api.proto` (proto3 JSON,
fields as declared, defaults omitted).

| route | returns |
|---|---|
| `/api/repo` | `RepoInfo`: root, exec root, HEAD, database, stats; with `--coverage`, `coverage`: the tracefile, when it was written, its ETag and its totals |
| `/api/files?prefix=<dir>` | `FileList`: one level of the tree the index spans (`""` is the root; absolute SYSTEM paths sit under `/`). With `--coverage`, an entry has `coverage`: a file's totals, or a directory's everything under it (covered files the index does not list included) |
| `/api/file?path=<p>` | the bytes (`text/plain`), with `ETag`, `Last-Modified`, `X-File-Id`, `X-File-Kind`, and `X-Newer-Than-Index: 1` when the file changed after the index was built |
| `/api/annotations?path=<p>` | `Annotations`: every occurrence as a byte-range `Span`, plus a `SymbolSummary` for every symbol they name. A summary has the symbol's `language` and, for generated code, its `origin` -- the summary of what it was generated from (`set_size` -> the proto field `size`), with `modifies_origin` when the generator marked it a setter -- so the page knows where a click should lead without asking again |
| `/api/includes?path=<p>` | `Includes`: every `#include` line's spelling as a byte range, with the indexed files it can name, best first. The index records no include edges and no include paths, so this is a resolution by path: the includer's sibling (quoted form), the spelling from the root, then the indexed paths ending in it — first-party first, then the fewest directories in front of the spelling. One candidate is a link; several open the panel to choose from. In a `.proto` the directives are its `import`s, whose spelling is a path from an import root and never relative to the file |
| `/api/symbol/<id>`, `/api/symbol?usr=<u>` | `SymbolInfo`: definitions, declarations, relations both ways (`GENERATED_FROM` among them: forward on generated code, reverse on what it came from), counts -- `generated` being the occurrences of the symbols generated from this one |
| `/api/refs/<id>?role=&exclude=&file=&offset=&limit=&expand=generated` | `References`, grouped per file with line text, the files of tests (`test`: a testonly Bazel target's, from the index's `testonly` attribute) after all the others; `role`/`exclude` take a bitmask or `DEFINITION\|CALL`; `limit` ≤ 5000. `expand=generated` lists the occurrences of everything generated from the symbol along with its own -- one listing, filtered and paged as one -- with `symbols` summarising those symbols and each `Reference.symbol` saying which it is. `GENERATES` anchors (generated text, not uses) are left out unless `role` asks for them |
| `/api/search?q=&limit=&kind=&locals=1` | `SearchResults`: name prefix, substring (3+ chars, via trigrams) or a qualified name, `ns::Name` or `pkg.Message` |
| `/api/at?path=<p>&offset=<n>` | `OccurrencesAt`: what `cpp_format --dump-index --lookup` says |
| `/api/text?q=&case=sensitive\|insensitive&offset=&limit=` | `TextSearchResults`: the lines that contain `q` exactly (the default) or ignoring ASCII case, by path then line, `limit` (≤ 2000, default 200) lines from `offset`; each line with its 1-based number, its text (clipped around the first match when long, from byte column `text_offset`) and the matches as byte spans into it. A file the symbol index knows carries its `file_id` and `kind`, others `file_id: -1`. More than 100 000 matches set `truncated`: the counts and lines then cover a subset. 503 when the server runs with `--text-search=false`; the ETag is the text index's and the symbol index's |
| `/api/coverage?path=<p>` | `FileCoverage`: the file's instrumented lines and their hit counts, as parallel arrays (`lines[i]` ran `hits[i]` times, capped at 2³²−1), the lines with branches likewise (`branch_lines`, `branches`, `branches_taken`), the file's totals, and `stale` when the file changed after the tracefile was written. 404 for a file the tracefile does not name, 503 without `--coverage`; the ETag is the tracefile's and the file's |

Errors are `Error{status, message}` with 400/404/405/414, and 503 for a
feature the server was started without. Responses that depend only on the
index carry its ETag and answer `If-None-Match` with 304 (`/api/includes`
reads the file too, so its ETag is the index's and the file's).

## Files the index names

Paths are the index's (relative to the checkout, exec-root-relative under
Bazel). `SOURCE` files are read from `--root`, and only from there: a path
whose canonical form leaves the root (`..`, a symlink out) is refused.
`GENERATED` (`bazel-out/...`) and `EXTERNAL` (`external/...`) files come from
the exec root — `--exec-root`, or the root's `bazel-out` symlink — and are
"unavailable" when that build is not present; their annotations and
references still work. `SYSTEM` files (absolute paths) are served as written
unless `--serve-system-files=false`.

## Storage

`index_import` writes one transaction: `files`/`dirs` (the tree; `files.test`
is the index's `testonly` attribute),
`symbols` (with each symbol's definition occurrence precomputed and a lower-
cased name), `occurrences` indexed by `(file, begin, end)` and
`(symbol, file, begin)`, the relations both ways, `unresolved`, and
`symbol_trigrams` for substring search; `meta` remembers the source index and
an ETag. Ids equal the proto's indexes. A `GENERATED` file that only
`GENERATES` anchors name -- the `.pb.h` of a `proto_library` whose C++ nothing
in the index includes -- is left out: no symbol is declared there and nothing
in it is a use, so it would be a dead entry in the tree. The server opens it read-only with one
connection per in-flight query (a small pool), `mmap`ed; every query is an
index lookup, and nothing is loaded up front.

## Full-text search

`text_index.*` walks `--root` at startup: every regular file, except symlinks
(so `bazel-out`, `bazel-bin`, `bazel-<workspace>` and `external/` through
them are never followed), `bazel-*` and `external/` at the root even when
real directories, dot-files and dot-directories (`.git`), files over
`--text-max-file-kb`, the server's own files (`index.pb*`, the database, the
text index), and -- once read -- binary files (a NUL byte). The walk only
stats; when every path, size and mtime matches what the text index was built
from, the server maps it and starts (0.13 s over Clang's 30 000 files).
Otherwise it rebuilds first: 14 s and 1.7 GB peak for those 340 MB of text,
0.2 s for this repository.

The file (`<index>.fts`) is one mapping: a header, the manifest (path, size,
mtime of every walked file, binary ones flagged), the corpus (each indexed
file followed by a NUL, so no match crosses into the next file), and a suffix
array over it -- libsais over the ASCII-lowercased corpus, the separators
left out. A query is one binary search for its range; a case-insensitive
search is the range, a case-sensitive one the range filtered against the
original bytes (folding ASCII only keeps every offset). The matches are then
sorted by offset, which is by path and line, and grouped into lines. A query
takes milliseconds (a single letter over 340 MB, 90 ms). The suffix array is
bounds-checked as it is read rather than when the file is opened, which would
read all of it. With 32-bit offsets the text is at most 2 GiB (the build says
so and suggests `--text-max-file-kb`).

## Coverage

`coverage.*` reads an LCOV tracefile once, at startup -- what `bazel coverage
--combined_report=lcov` writes to `bazel-out/_coverage/_coverage_report.dat`,
and what `cpp_format.sh coverage` copies next to the index as
`index.pb.lcov`. The parser is strict: a record it does not know, a count
that is not an unsigned number (a negative one is a coverage bug, not data),
or a record outside `SF`…`end_of_record` stops the server with the file and
line (`index.pb.lcov:17: DA: expected <line>,<count>, got 'DA:x,1'`) rather
than drawing a wrong overlay. Function records are read past, and
`LF`/`LH`/`BRF`/`BRH` recomputed from the `DA`/`BRDA` data, so that two
records of one file -- one per test binary, in a report nobody merged -- add
up line by line and branch by branch (counts saturate). A `BRDA` count of `-`
is a branch that was never evaluated: found, not taken.

`SF` paths are spelled as the index spells them: `/proc/self/cwd/` and `./`
dropped, the root or the exec root (each canonical and as given) cut off,
and, failing that, whatever follows a sandbox's `/execroot/<workspace>/`.
The startup line says how many of the covered files the index knows
(`coverage index.pb.lcov: 48 files (48 in the index), lines ...`), and warns
when it knows none -- a report from another checkout. The full-text index
leaves the tracefile out.

The page marks lines by class alone (`cov-hit`, `cov-miss`, `cov-partial`,
and the count in `data-hits`), so the header's toggle is a CSS switch and
re-renders nothing. A file edited after the tracefile was written gets a
banner, since its counts may sit on the wrong lines.

## Keys

`/` search · `Esc` close the panel · `n` / `p` the next / previous occurrence
of the selected symbol (outlined) · `u` / `U` the next / previous run of
lines no test ran.

## Building

C++20 (Asio coroutines): every target carries `-std=c++20`, and `.bazelrc`
compiles the BCR `boost.asio` source the same way. `--config=minify` sets
`-fno-exceptions`, which Asio needs, so that config excludes this package.
