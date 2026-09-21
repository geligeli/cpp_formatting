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

```sh
# One command: indexes the whole repository (or a target pattern), prints the
# browser's binary and command line, and serves the workspace on the index.
# Run it again after editing: only the changed translation units are re-parsed.
tools/cpp_format.sh browse                         # [pattern] [--port=N] [--check]

# Or by hand:
# 1. An index of the repository:
tools/cpp_format.sh index                          # writes index.pb
# 2. Serve it.  --index imports into index.pb.sqlite when that is missing or
#    older than the .pb; --db skips the import.
bazel run //code_browser -- --index=$PWD/index.pb --root=$PWD
# 3. Open the URL it prints: a free port by default, --port=N to choose.
```

`index_import index.pb [--out=x.sqlite] [--force]` does the import on its
own. `code_browser --check` opens the database, prints its stats and exits.

## Layers

| layer | files | knows about |
|---|---|---|
| storage | `index_schema.*` (writer), `index_db.*` (reader), `sqlite_util.*` | SQLite, the proto |
| checkout | `repo.*`, `file_cache.*` | the filesystem, `.git`, the exec root |
| API | `api.proto`, `api.*` | requests and JSON; nothing about sockets |
| HTTP | `http_server.*`, `static_assets.*` | Boost.Beast/Asio; nothing about the index |
| page | `web/index.html`, `web/app.js`, `web/app.css` | `/api/*` only |

Each layer has its own test (`index_db_test`, `repo_test`, `api_test`,
`http_server_test`); `cross_language_test` is the API over an index of two
languages, merged and linked as `--merge-index` does it; `smoke_test` runs the
binaries end to end over the demo index in `//bazel/testdata`, and then over
the two-language one (`inventory_index.pb`: real protoc annotations, real Clang
ranges) for the link between a `.proto` and the C++ generated from it.

## API

`GET`/`HEAD`; JSON bodies are the messages of `api.proto` (proto3 JSON,
fields as declared, defaults omitted).

| route | returns |
|---|---|
| `/api/repo` | `RepoInfo`: root, exec root, HEAD, database, stats |
| `/api/files?prefix=<dir>` | `FileList`: one level of the tree the index spans (`""` is the root; absolute SYSTEM paths sit under `/`) |
| `/api/file?path=<p>` | the bytes (`text/plain`), with `ETag`, `Last-Modified`, `X-File-Id`, `X-File-Kind`, and `X-Newer-Than-Index: 1` when the file changed after the index was built |
| `/api/annotations?path=<p>` | `Annotations`: every occurrence as a byte-range `Span`, plus a `SymbolSummary` for every symbol they name. A summary has the symbol's `language` and, for generated code, its `origin` -- the summary of what it was generated from (`set_size` -> the proto field `size`), with `modifies_origin` when the generator marked it a setter -- so the page knows where a click should lead without asking again |
| `/api/includes?path=<p>` | `Includes`: every `#include` line's spelling as a byte range, with the indexed files it can name, best first. The index records no include edges and no include paths, so this is a resolution by path: the includer's sibling (quoted form), the spelling from the root, then the indexed paths ending in it — first-party first, then the fewest directories in front of the spelling. One candidate is a link; several open the panel to choose from. In a `.proto` the directives are its `import`s, whose spelling is a path from an import root and never relative to the file |
| `/api/symbol/<id>`, `/api/symbol?usr=<u>` | `SymbolInfo`: definitions, declarations, relations both ways (`GENERATED_FROM` among them: forward on generated code, reverse on what it came from), counts -- `generated` being the occurrences of the symbols generated from this one |
| `/api/refs/<id>?role=&exclude=&file=&offset=&limit=&expand=generated` | `References`, grouped per file with line text; `role`/`exclude` take a bitmask or `DEFINITION\|CALL`; `limit` ≤ 5000. `expand=generated` lists the occurrences of everything generated from the symbol along with its own -- one listing, filtered and paged as one -- with `symbols` summarising those symbols and each `Reference.symbol` saying which it is. `GENERATES` anchors (generated text, not uses) are left out unless `role` asks for them |
| `/api/search?q=&limit=&kind=&locals=1` | `SearchResults`: name prefix, substring (3+ chars, via trigrams) or a qualified name, `ns::Name` or `pkg.Message` |
| `/api/at?path=<p>&offset=<n>` | `OccurrencesAt`: what `cpp_format --dump-index --lookup` says |

Errors are `Error{status, message}` with 400/404/405/414. Responses that
depend only on the index carry its ETag and answer `If-None-Match` with 304
(`/api/includes` reads the file too, so its ETag is the index's and the
file's).

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

`index_import` writes one transaction: `files`/`dirs` (the tree),
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

## Building

C++20 (Asio coroutines): every target carries `-std=c++20`, and `.bazelrc`
compiles the BCR `boost.asio` source the same way. `--config=minify` sets
`-fno-exceptions`, which Asio needs, so that config excludes this package.
