# cpp_formatting

A collection of Clang-based source-to-source rewrite tools for C++ codebases.

## Add cpp_format to your Bazel codebase

Get a parallel, cached **lint / diff / fix** pass over your C++ using the
**prebuilt release binary** — no need to build Clang/LLVM from source, and **no
per-target wiring**: one command formats the whole repo (or any target
pattern). The Bazel glue is imported straight from this repo **by URL** — you
don't vendor it. An [aspect](bazel/integration/cpp_format.bzl) derives each
target's compile flags and header set from `CcInfo` + the toolchain, so every
include is reachable under sandboxing with **no `compile_commands.json`**. The
binary embeds and self-extracts the Clang builtin headers, so nothing else is
fetched.

**1. Import the kit in `MODULE.bazel`.** Take the **latest** release tag from
the [Releases](https://github.com/geligeli/cpp_formatting/releases) page (tags
are `<YYYYMMDD>-<shortsha>`) and put it in `CPP_FORMAT_VERSION`.
`archive_override` pulls the Bazel glue by URL; the `cpp_format` extension
downloads the prebuilt binary for your host platform. **Both must come from the
same tag** — the aspect passes flags that only a matching binary understands —
so the tag is written *once* and every use derives from it. **Mark it a dev
dependency** (`dev_dependency = True` on both the `bazel_dep` and the
`use_extension`): the formatter is a tool of *your* repository, and modules
that depend on yours must not have to fetch the glue or the release binary:

```starlark
# dev_dependency: a tool of this repo, invisible to modules that depend on it.
bazel_dep(name = "cpp_formatting", version = "0.1.0", dev_dependency = True)

# The latest tag on https://github.com/geligeli/cpp_formatting/releases,
# e.g. "20260913-5ea87d3".  Always the latest: older tags predate flags the
# current Bazel glue relies on.
CPP_FORMAT_VERSION = "<latest release tag>"

archive_override(
    module_name = "cpp_formatting",
    urls = ["https://github.com/geligeli/cpp_formatting/archive/refs/tags/" +
            CPP_FORMAT_VERSION + ".tar.gz"],
    strip_prefix = "cpp_formatting-" + CPP_FORMAT_VERSION,
)

cpp_format = use_extension(
    "@cpp_formatting//bazel/integration:extensions.bzl",
    "cpp_format",
    dev_dependency = True,
)
cpp_format.release(
    version = CPP_FORMAT_VERSION,
    # Optional but recommended for reproducible CI — pin per-asset hashes:
    # sha256 = {"cpp_format-linux-x86_64": "…"},
)
use_repo(cpp_format, "cpp_format_bin")
```

Upgrading is then a one-line change, and the glue and the binary can never
drift apart. The host asset is selected automatically (Linux x86_64/aarch64,
macOS arm64, Windows x64).

**2. Add a ruleset** — `cpp_format.yaml` at your repo root describes the passes
to run (see [YAML config](#config-file---config)):

```yaml
normalize_variables:
  - scope: member
    style: snake_case
trailing_return_types: true
```

The aspect reads it as your root module's `//:cpp_format.yaml`, so export it
from your root `BUILD.bazel`:

```starlark
exports_files(["cpp_format.yaml"])
```

**3. Lint / fix / browse the whole repo** — or any target pattern — with the
`cpp_format.sh` wrapper. It runs *outside* Bazel (so it can drive `bazel build`
for the aspect without nesting), so it's the one script you keep locally. Drop
it in with one command — the labels of the aspect and of the release's binaries
are baked in, so it needs no configuration and nothing in your BUILD files:

```sh
bazel run @cpp_formatting//bazel/integration:install   # -> tools/cpp_format.sh
#   ... or `bazel run …:install -- scripts/fmt.sh` to choose the path.

tools/cpp_format.sh check          # CI gate: exit 1 if anything would change
tools/cpp_format.sh diff           # print the merged, git-apply-able patch
tools/cpp_format.sh fix            # apply the fixes in place
tools/cpp_format.sh fix //app/...  # scope to a package tree
tools/cpp_format.sh compile_commands   # write compile_commands.json for clangd
tools/cpp_format.sh index          # write index.pb, the repo's symbol index
tools/cpp_format.sh browse         # ... and serve the repo in the code browser
```

Commit the placed script (it's a normal, editable file). It queries the
matching first-party `cc_*` targets, runs the aspect over them (one action per
source file emits that file's edit records — parallel, cached, and incremental
per file, like compilation), and merges every record into one repository-wide
change — deduping, resolving template-dependent member tokens across
translation units, and flagging genuine conflicts. Tag a target
`no-cpp-format` to exclude it.

There is deliberately no BUILD-file macro for any of this. What to format or
index is a *pattern* — `//...` — and a pattern exists only on the command line:
a rule's `deps` can name labels, never "everything". The script queries the
targets under the pattern instead, so the whole repository is the default and
nothing has to be listed or kept up to date. `tools/cpp_format.sh check` is the
CI gate.

**Browse the repository.** One command indexes every `cc_*` target and starts
the code browser on the result:

```sh
tools/cpp_format.sh browse                  # http://127.0.0.1:8080/
tools/cpp_format.sh browse --port=9000      # flags go to the server
tools/cpp_format.sh browse //app/...        # index one package tree only
tools/cpp_format.sh browse --check          # index, print the stats, exit
```

It builds the symbol index (see [Symbol index](#symbol-index)) into `index.pb`
in the workspace root — add `index.pb*` to your `.gitignore` — and serves *your
checkout* with every indexed token annotated: click an identifier for its
definition and references. Before the server starts it prints the browser
binary it resolved and the exact command line it runs, so you can restart the
server by hand. **The index is a snapshot**, read once at start: after editing
sources, stop the server and run `browse` again. Only the translation units
that changed are re-parsed, and if the merged index comes out byte-identical it
is left alone, so the browser does not re-import it either.

The browser is a prebuilt release asset like `cpp_format` itself (Linux
x86_64/aarch64 and macOS arm64; there is no Windows build yet), fetched the
first time you browse — a repository that only formats never downloads it. The
installed script names it by its canonical label, so your `use_repo(...)` does
not have to list it. If you **vendored** the kit, the script's default labels
resolve in your own module: add `"code_browser_bin"` to your `use_repo(...)`.

**A `compile_commands.json` for free.** The aspect already derives every
target's compile command, so the script can also write it out as a
compilation database for clangd and other editors — no second Bazel
dependency (Hedron & co.) needed. `cpp_format.sh compile_commands [pattern]`
writes `compile_commands.json` in the workspace root (set
`COMPILE_COMMANDS_OUT` to put it elsewhere). It neither compiles
anything nor runs `cpp_format`: the entries are written at analysis time, and
only the generated headers they mention get built. Entries use the execution
root as `directory`, so relative flags (`-iquote .`, `bazel-out/...`,
`external/...`) resolve without planting any symlinks in your tree, and the
absolute workspace path as `file`, which is what an editor opens.
`no-cpp-format` targets are included here (they are only excluded from
formatting). The command is the target's, not the file's: per-target `copts`
are not part of it.

> **Notes.** Only first-party `cc_*` targets are formatted (external deps are
> skipped). Each header is owned by the single target that lists it in `hdrs`;
> a header in no target's `srcs`/`hdrs` is never visited. The binary extracts
> its embedded headers to a temp cache per action, so a strict sandbox needs a
> writable `TMPDIR` (Bazel provides one by default). Importing by URL does not
> build LLVM — the LLVM-from-source parts of this module are lazy and never
> referenced by the kit. Prefer to keep everything local? Copy
> [bazel/integration/](bazel/integration/) into your repo instead of the
> `archive_override`, load it as `//third_party/cpp_format:…`, and drop the
> `CPP_FORMAT_ASPECT` export (the vendored default matches). Vendoring also
> needs `bazel_dep(name = "rules_cc", …)` in your `MODULE.bazel` **even if you
> already get it transitively**: the copied `.bzl` now lives in your root
> module, so its `load("@rules_cc//…")` resolves through *your* repo mapping.
> (Importing by URL does not need this — there it resolves through
> cpp_formatting's own deps.) To build the tool
> from source rather than download it, set `CPP_FORMAT_BIN_LABEL` /
> `CPP_FORMAT_ASPECT` (see the top of `cpp_format.sh`) to the in-repo
> [bazel/cpp_format.bzl](bazel/cpp_format.bzl) aspect and `//cpp_formatting:cpp_format`.

---

| Tool | Description |
|---|---|
| `trailing_return_types` | Converts functions from leading to trailing return type syntax (or back, with `return_types: leading`) |
| `normalize_variables` | Renames variables to a consistent naming convention |
| `cpp_format` | Combined tool — runs any combination of the above, driven by a YAML config |

## Requirements

- [Bazel](https://bazel.build/) 7+ with Bzlmod support
- A C++17-capable host compiler (for building the tools)
- Internet access on the first build (Bazel downloads the LLVM 19 source, GoogleTest, etc.)

The Clang built-in headers (`stddef.h`, `__stddef_max_align_t.h`, etc.) are embedded into every binary and extracted on-demand to `$XDG_CACHE_HOME/cpp_formatting/`, so no system Clang installation is required at runtime.

## Building

```sh
bazel build //...
```

Build a specific tool:

```sh
bazel build //cpp_formatting:trailing_return_types
bazel build //cpp_formatting:normalize_variables
bazel build //cpp_formatting:cpp_format
```

### Keeping includes and BUILD deps in sync

The `deps` of every `cc_*` target are generated from the `#include`s of its
sources by [Gazelle](https://github.com/bazel-contrib/bazel-gazelle) with the
[gazelle_cc](https://github.com/EngFlow/gazelle_cc) extension, and
[include-what-you-use](https://include-what-you-use.org/) keeps the `#include`s
themselves honest. After adding or removing an include:

```sh
tools/iwyu/iwyu.sh check       # optional: what IWYU would change (exit 1 if anything)
tools/iwyu/iwyu.sh fix         # ... apply it; review the diff
bazel run //tools/gazelle      # regenerate deps / implementation_deps
```

`pre-commit install` wires all of it into `git commit`: clang-format, an IWYU
check, Gazelle and buildifier, in that order (see `.pre-commit-config.yaml`).
CI fails when `bazel run //tools/gazelle -- -mode=diff` has anything to say.
IWYU is built from source against the same Clang as the tools (the first run
takes a while); its mappings live in `tools/iwyu/mappings.imp`. Both are
dev-only and invisible to a repository that imports this one.

---

## `trailing_return_types`

Rewrites C++ functions from leading return type syntax to [trailing return type](https://en.cppreference.com/w/cpp/language/function) syntax.

```cpp
// Before
int add(int a, int b) { return a + b; }
const int* sentinel() { static int v = -1; return &v; }

// After
auto add(int a, int b) -> int { return a + b; }
auto sentinel() -> const int* { static int v = -1; return &v; }
```

### Usage

**Dry-run** — print rewritten source to stdout:

```sh
bazel run //cpp_formatting:trailing_return_types -- path/to/file.cpp -- -std=c++17
```

**In-place** — overwrite the file on disk:

```sh
bazel run //cpp_formatting:trailing_return_types -- -i path/to/file.cpp -- -std=c++17
```

**Multiple files at once:**

```sh
bazel run //cpp_formatting:trailing_return_types -- -i file1.cpp file2.cpp -- -std=c++17
```

Several files are parsed in parallel, one translation unit per thread on every CPU by default; `--jobs=N` / `-jN` caps the thread count (see [Parallel parsing](#parallel-parsing)). A multi-file dry run prints each file behind a `=== path ===` header, in the order given.

The `--` separates the tool's own flags from the Clang compilation flags. At minimum `-std=c++17` is required. The tool ships its own Clang built-in headers, so files using standard-library headers (`<cstddef>`, etc.) work out of the box without a system Clang.

### What gets rewritten

| Input | Rewritten? | Notes |
|---|---|---|
| `int foo()` | yes | plain return type |
| `const int* foo()` | yes | leading cv-qualifiers captured via backward scan |
| `int foo();` (declaration only) | yes | forward declarations rewritten too |
| a template instantiated in the same TU | pattern only | an implicit instantiation carries the pattern's source locations, so matching it too would rewrite the same place twice (`unless(isTemplateInstantiation())`) |
| `void foo()` | no | `void` excluded by matcher |
| `auto foo() -> int` | no | already has a trailing return |
| `auto foo() { return 42; }` | no | deduced `auto` — rewriting would be redundant |
| `decltype(auto) foo()` | no | same deduced-auto check |
| `operator bool()` (conversion) | no | `cxxConversionDecl()` excluded by matcher |

### The reverse direction (`--reverse`)

`--reverse` moves trailing return types back to leading position:

```sh
bazel run //cpp_formatting:trailing_return_types -- --reverse -i path/to/file.cpp -- -std=c++17
```

```cpp
// Before
auto add(int a, int b) -> int { return a + b; }

// After
int add(int a, int b) { return a + b; }
```

It is deliberately much more selective than the forward direction: a trailing
return type can say things that leading position cannot express, so anything
that would not mean the same thing after the move is left alone rather than
rewritten. A declaration the tool skips keeps compiling exactly as it did.

| Input | Rewritten? | Notes |
|---|---|---|
| `auto foo() -> int` | yes | |
| `auto foo() const noexcept -> int` | yes | qualifiers stay where they are |
| `static constexpr auto foo() -> int` | yes | the placeholder is replaced in place, so specifiers keep their order |
| `auto Foo::bar() -> std::string` | yes | a name written qualified resolves the same from either position |
| `auto f(T a) -> decltype(a.size())` | no | parameters are not in scope before the declarator-id |
| `auto make() -> int (*)(bool)` | no | leading position needs a restructured declarator (`int (*make())(bool)`), not the same text moved |
| `auto get() -> int (&)[4]` | no | same |
| `auto Foo::bar() -> Inner` | no | `Inner` is found in class scope only *after* the declarator-id |
| `auto foo() -> auto` / `-> decltype(auto)` | no | a placeholder has no type to move |
| `[]() -> int {}` | no | a lambda has no declarator-id to put a return type on |
| `auto foo() -> INT_T` (macro) | no | the rewrite never edits through a macro expansion |

Both directions are fixpoints, so either is safe to run repeatedly, and both
are available through `cpp_format` as `return_types: trailing` / `leading`.

### Tests

```sh
bazel test //cpp_formatting:trailing_return_types_test
bazel test //cpp_formatting:trailing_return_types_integration_test
```

---

## `normalize_variables`

Renames variables (member, local, or global) and member functions to a chosen naming convention using Clang's AST. Handles cross-file renaming: list the header alongside its `.cpp` and both files are updated consistently.

```cpp
// Before (member variables with m_ prefix)
struct Rect {
  int m_width;
  int m_height;
};

// After (--style=snake_case --scope=member)
struct Rect {
  int width;
  int height;
};
```

### Supported naming styles

| Keyword | Example output |
|---|---|
| `snake_case` | `my_variable` |
| `_leading` | `_myVariable` |
| `trailing_` | `myVariable_` |
| `m_prefix` | `m_myVariable` |
| `camelCase` | `myVariable` |
| `UpperCamelCase` | `MyVariable` |
| `UPPER_SNAKE_CASE` | `MY_VARIABLE` |
| `kConstant` | `kMyVariable` |

All of the above are also recognized as *input* patterns — the tool splits names into words regardless of which convention the source currently uses.

### Usage

**Rename member variables in-place:**

```sh
bazel run //cpp_formatting:normalize_variables -- \
  --style=snake_case --scope=member --in-place \
  src/rect.cpp src/rect.h \
  -- -std=c++17 -I src
```

**Rename global variables (dry-run):**

```sh
bazel run //cpp_formatting:normalize_variables -- \
  --style=snake_case --scope=global \
  src/globals.cpp \
  -- -std=c++17
```

**Options:**

| Flag | Description |
|---|---|
| `--style=<style>` | Target naming style (required) |
| `--scope=<scope>` | Scope to rename (default: `member`). See "Supported scopes" below. |
| `--in-place` / `-i` | Overwrite files on disk (default: dry-run to stdout) |
| `--lint` | Analyze only — report violations, modify nothing, exit 1 if any are found (see [Lint mode (CI/CD)](#lint-mode-cicd)) |
| `--format=<fmt>` | Output format for `--lint`: `text` (default), `sarif`, or `diff` |
| `--jobs=<N>` / `-j<N>` | Translation units to parse in parallel. `0` (default) uses every CPU; larger values are capped at the CPU count. The result does not depend on it (see [Parallel parsing](#parallel-parsing)). |
| `--debug-trace` | Print, per TU, every rename target and reference site found in the AST, and whether each would be rewritten. Makes no modifications; always runs one TU at a time. |

**Supported scopes:**

| Scope | Targets |
|---|---|
| `member` | non-static member variables (`FieldDecl`) and static data members |
| `local` | local variables and function parameters |
| `global` | file- and namespace-scope variables (non-member, non-local) |
| `static_member` | static data members only |
| `const_member` | static data members that are `const` or `constexpr` |
| `public_member`, `protected_member`, `private_member` | data members (static or not) by access. A field of an anonymous struct or union takes the access of that anonymous member. This is how "class members get a trailing underscore, struct members do not" is written: `member: trailing_` plus `public_member: snake_case` |
| `static_local` | locals with static storage duration (`static`, block-scope `thread_local`); never parameters |
| `const_local` | locals whose value is fixed for the whole program: `constexpr` ones, and `static` ones of const type. A plain `const auto x = f(y);` is a new value on every call and stays an ordinary `local` |
| `static_global` | file- and namespace-scope variables declared `static` |
| `const_global` | file- and namespace-scope variables that are `const` or `constexpr` |
| `method` | member functions, static and non-static (never constructors, destructors, conversion functions, or overloaded operators; a virtual function is renamed together with its whole override hierarchy — if any override is declared outside the listed files, the rename is skipped) |
| `type` | classes, structs, unions, enums (nested ones too), class templates, typedefs and aliases; STL protocol names such as `value_type` and `iterator` are never renamed |
| `namespace` | named namespaces and namespace aliases; the closing `}  // namespace x` comment is rewritten with the declaration |

**Overlapping scopes — the most specific rule wins.** The fine-grained scopes
are subsets of the broad ones, so a ruleset can state a convention and its
exceptions, in any order:

```yaml
normalize_variables:
  - scope: local          # snake_case ...
    style: snake_case
  - scope: const_local    # ... except `static const` / `constexpr`: kArenaOverride stays
    style: kConstant
```

A declaration several rules match is renamed by exactly one of them: within a
family the order is **constness, then storage, then access, then the broad
scope** (`const_member` > `static_member` > `public_`/`protected_`/`private_member`
> `member`; `const_local` > `static_local` > `local`; `const_global` >
`static_global` > `global`). Constness first because that is the distinction
naming conventions make -- `kMaxSize` is `kMaxSize` whether it is private or
not. Naming the *same* scope twice is refused.

**Cross-file renaming:** list all files that share declarations — order does not matter. The tool parses every non-header first and every header after them (each group in parallel), so each `.cpp` is parsed against the original on-disk header content and the header sees what the `.cpp` files instantiated; edits are buffered per TU and committed atomically once every TU has been processed.

**Debugging missed renames:** if you suspect the tool isn't renaming everything you expected, run with `--debug-trace`. It prints the full rename map and every reference site found in each TU, with `main=Y/N`, `macro=Y/N`, and a marker per site: `WILL_RENAME` if it would be rewritten, `VETOES_RENAME` if it cannot be (see "Names spelled through macros" below), nothing if the site belongs to another TU. A site that never appears with `WILL_RENAME` in any TU is either vetoed or missing from the source list passed to the tool.

### Tests

```sh
bazel test //cpp_formatting:naming_convention_test
bazel test //cpp_formatting:rename_variables_test
bazel test //cpp_formatting:normalize_variables_integration_test
```

---

## `const_placement`

Moves cv-qualifiers to one side of the type they qualify — "east const" or
"west const", whichever your codebase has settled on.

```cpp
// Before                              // After (--style=east)
const int kCount = 1;                  int const kCount = 1;
const char* Name();                    char const* Name();
void Take(const Widget& w);            void Take(Widget const& w);
std::vector<const Widget*> items;      std::vector<Widget const*> items;

int* const slot = nullptr;             int* const slot = nullptr;   // unchanged
int Area() const;                      int Area() const;            // unchanged
```

`--style=west` is the exact reverse, and running one direction over the other's
output gives the original back.

### Usage

```sh
# Dry-run — print the rewritten source to stdout
bazel run //cpp_formatting:const_placement -- --style=east file.cpp -- -std=c++17

# In-place
bazel run //cpp_formatting:const_placement -- --style=east -i file.cpp -- -std=c++17

# Lint (CI): report, change nothing, exit 1 if anything would move
bazel run //cpp_formatting:const_placement -- --style=east --lint file.cpp -- -std=c++17
```

| Flag | Description |
|---|---|
| `--style=<east\|west>` | Which side the qualifiers go on (default `east`) |
| `--in-place` / `-i` | Overwrite files on disk (default: dry-run) |
| `--lint`, `--format=<fmt>` | As for the other binaries — see [Lint mode](#lint-mode-cicd) |
| `--jobs=<N>` / `-j<N>` | Translation units to parse in parallel |

### What moves, and what does not

Only a qualifier of the **type specifier** moves, because only that one can be
written on either side without changing the type:

| Input | `--style=east` | Why |
|---|---|---|
| `const int x` | `int const x` | the qualifier is on the `int` |
| `const int* p` | `int const* p` | pointer *to const* — the qualifier is still on the `int` |
| `int* const p` | unchanged | **const pointer** — `const int* p` is a different type |
| `const int* const p` | `int const* const p` | only the pointee's qualifier moves |
| `int f() const` | unchanged | that `const` qualifies the member function, not a type |
| `const int a[3]` | `int const a[3]` | the qualifier is on the element type |
| `V<const T>` | `V<T const>` | template arguments move too, innermost first |
| `const volatile int x` | `int const volatile x` | whole runs move, in source order |
| `CONST int x` (macro) | unchanged | the qualifier has no byte range of its own to move |
| `const /*why*/ int x` | unchanged | the run has to be adjacent to the type |
| `const static int x` | unchanged | `static` is not a cv-qualifier, so the run is not adjacent |

The qualifier is re-emitted with single spaces, so `const    int x` becomes
`int const x` — the one thing an east→west round trip does not restore exactly.

### Tests

```sh
bazel test //cpp_formatting:const_placement_test
bazel test //cpp_formatting:const_placement_integration_test
```

---

## `cpp_format` — combined tool

Runs any combination of the above passes in a single invocation, driven by a YAML configuration file or individual CLI flags.

### Config file (`--config`)

Create a YAML file describing which passes to run:

```yaml
# cpp_format.yaml

# Move cv-qualifiers to one side of the type they qualify:
# `east` rewrites `const int x` to `int const x`; `west` rewrites it back.
const_placement: east

# Return type style: `trailing` rewrites `int f()` to `auto f() -> int`,
# `leading` rewrites it back.  The two cannot be combined.
# `trailing_return_types: true` is the older spelling of `return_types: trailing`
# and still works.
return_types: trailing

# Rename variables — multiple rules are applied in order.
# Supported scopes: member, local, global, method, type, namespace, and the
#   fine-grained {static,const,public,protected,private}_member,
#   {static,const}_local, {static,const}_global.  When several rules match a
#   declaration the most specific one renames it (see "Overlapping scopes").
normalize_variables:
  - scope: member   # non-static and static data members
    style: snake_case
  - scope: const_global  # only const/constexpr namespace-scope vars
    style: kConstant
  - scope: global   # remaining file- and namespace-scope variables
    style: snake_case
  # - scope: local  # local variables and parameters (disabled)
  #   style: camelCase
```

Then run:

```sh
bazel run //cpp_formatting:cpp_format -- \
  --config=cpp_format.yaml --in-place \
  src/rect.cpp src/rect.h \
  -- -std=c++17 -I src
```

### One-shot CLI (no config file)

Run a single pass directly without a config file:

```sh
# Normalize member variables
bazel run //cpp_formatting:cpp_format -- \
  --normalize-variables-scope=member --normalize-variables-style=snake_case \
  --in-place src/rect.cpp -- -std=c++17

# Trailing return types only
bazel run //cpp_formatting:cpp_format -- \
  --trailing-return-types --in-place src/rect.cpp -- -std=c++17

# East const only
bazel run //cpp_formatting:cpp_format -- \
  --const-placement=east --in-place src/rect.cpp -- -std=c++17
```

### Options

| Flag | Description |
|---|---|
| `--config=<file>` | YAML configuration file (takes precedence over per-pass flags) |
| `--trailing-return-types` | Enable the trailing-return-type pass (same as `--return-types=trailing`) |
| `--return-types=<style>` | Return type style: `trailing` or `leading`. Cannot be combined with `--trailing-return-types`. |
| `--const-placement=<style>` | Where cv-qualifiers go: `east` (`int const x`) or `west` (`const int x`) |
| `--normalize-variables-scope=<scope>` | Any scope of the table under `normalize_variables` above: `member`, `local`, `global`, `method`, `type`, `namespace`, or a fine-grained one (`const_local`, `private_member`, …) |
| `--normalize-variables-style=<style>` | Target naming style |
| `--in-place` / `-i` | Overwrite files on disk (default: dry-run) |
| `--lint` | Analyze only — report violations, modify nothing, exit 1 if any are found |
| `--format=<fmt>` | Output format for `--lint`: `text` (default), `sarif`, or `diff` |
| `--jobs=<N>` / `-j<N>` | Translation units to parse in parallel. `0` (default) uses every CPU; larger values are capped at the CPU count. The result does not depend on it (see [Parallel parsing](#parallel-parsing)). |
| `--emit-index=<file>` | Index mode: parse the sources and write one `cpp_index.IndexUnit` (binary protobuf). Runs no formatting pass and takes no config; see [Symbol index](#symbol-index). |
| `--merge-index --output=<file> [--records-from=<list>] <unit.pb>...` | Merge index units (or earlier indexes) into one `cpp_index.Index`. `--format=binary\|text\|json` picks the encoding (default binary). |
| `--dump-index [--format=text\|json\|binary] [--lookup=<path>:<offset>] <file>` | Print a unit or index, or list the symbol at a byte offset and every occurrence of it. |

**Pass ordering:** `normalize_variables` rules are applied first (in the order
listed in the config), then `const_placement`, then `return_types`. The order
matters where the passes overlap: a qualifier moved east is carried into the
return type that `return_types: trailing` then lifts, so
`const int Get() const` becomes `auto Get() const -> int const` rather than
stranding the `const` on the `auto` placeholder. Every pass runs on the same
parsed AST, sharing one `Rewriter`.

---

### Name collisions

A rename whose new name is already taken in the same scope is **skipped** —
the declaration and all of its uses are left alone — rather than applied.
Renaming into an occupied name does not compile, or silently rebinds uses to
the other entity. Shadowing an inherited or outer-scope name is legal C++ and
is still allowed, and two functions may share a name (an overload set) unless
their signatures match.

A one-line count of skipped renames is printed to stderr; pass
`--report-rename-conflicts` to list every site:

```
src/re.h:3:15: skipped rename 'pattern_' -> 'pattern': existing CXXMethod 'pattern'
1 rename(s) skipped (name collision, or a reference that cannot be rewritten)
```

The same report comes out of the Bazel integration, where the run that found
the skip is a different process from the one that renders the change: each
emit action records what it declined alongside its edits, and the aggregation
step reports the repository's whole set. So `cpp_format.sh fix
--report-rename-conflicts` (or `diff`, or `check`) says what it left alone —
which no diff can show, a skipped rename being precisely a change that is not
there.

---

### Names spelled through macros

Whether a reference can be renamed depends on what the preprocessor did to it.

A name written as a macro **argument** is spelled at the call site, in ordinary
source, so it renames like any other reference — including when the argument is
forwarded through several macros, and including a declaration written that way:

```c
#define FIELD(n) int n;
#define FWD(x)   ((x) + 0)

struct Counter { FIELD(itemCount) };              // -> FIELD(item_count)
int use(Counter& c) { return FWD(c.itemCount); }  // -> FWD(c.item_count)
```

This covers the common test-framework shape, where a member of a class template
is reached through `this->` inside a macro argument:

```c
TYPED_TEST(MyFixture, Works) { EXPECT_FALSE(this->table_->IsPrime(0)); }
```

A name spelled inside a macro **body**, or built by token pasting (`##`), has no
byte range of its own that the tool could rewrite: the body token is a single
location shared by every expansion, and a pasted token exists only inside the
compiler. Renaming the declaration without them would leave those references
spelling the old name — a broken build. So a single such reference **vetoes the
whole rename**: the declaration keeps its name everywhere, and the skip is
reported like a name collision.

```c
struct Counter { int itemCount; };
#define BUMP(c) ((c).itemCount += 1)   // vetoes renaming Counter::itemCount
```

```
src/counter.h:4:7: skipped rename 'itemCount' -> 'item_count': referenced from a macro body at src/counter.h:7
```

The veto only accounts for expansions the tool actually parses. A macro expanded
solely in code outside the files you passed (or, under Bazel, in a target tagged
`no-cpp-format`, or under an `#if` branch this build does not take) is invisible,
and that reference is missed — which shows up as a compile error on the next
build rather than as silently wrong code.

---

## Lint mode (CI/CD)

All four binaries support a lint mode that reports what *would* change without modifying any files:

| Flag | Description |
|---|---|
| `--lint` | Analyze only — modify nothing, exit 1 when violations are found (exit 0 when clean) |
| `--format=<fmt>` | Output format: `text` (default), `sarif`, or `diff`. A non-default value implies `--lint` |

`--lint`/`--format` cannot be combined with `--in-place`. Diagnostics are collected at the exact sites the rewriter would change, so lint results always match what `--in-place` would do.

**Text (default):** one `file:line:col: warning: message [rule-id]` line per violation, e.g.

```sh
$ normalize_variables --lint --style=snake_case --scope=member src/rect.cpp -- -std=c++17
src/rect.cpp:2:7: warning: 'm_width' should be 'width' [normalize_variables/member/snake_case]
```

**SARIF** — [SARIF 2.1.0](https://sarifweb.azurewebsites.net/) JSON for code scanning integration. Paths are relativized to the working directory so uploaders can match them to repository files:

```sh
cpp_format --config=cpp_format.yaml --lint --format=sarif \
  $(git ls-files '*.cpp' '*.h') > results.sarif || true

# GitHub Actions: upload with github/codeql-action/upload-sarif
- uses: github/codeql-action/upload-sarif@v3
  with:
    sarif_file: results.sarif
```

**Diff** — a git-apply-able unified patch proposing the fixes (for `cpp_format` with multiple passes, the patch is the cumulative result of all passes):

```sh
# Print the proposed patch (CI artifact, PR comment, ...):
cpp_format --config=cpp_format.yaml --lint --format=diff src/rect.cpp -- -std=c++17

# Or apply it locally:
cpp_format --config=cpp_format.yaml --lint --format=diff \
  $(git ls-files '*.cpp' '*.h') | git apply || true
```

---

## Parallel parsing

All four binaries parse their translation units in parallel — one Clang
instance per TU on a pool of worker threads sized to the machine's CPUs, the
way `bazel build` sizes its own pool. `--jobs=N` (`-jN`) caps the pool; `0`,
the default, means every CPU the process may use, and a larger request is
capped at that. Memory is the trade-off: every thread holds one Clang AST, so
on a small machine with heavy headers pass a smaller `-j`.

**The result never depends on `-j`.** Each TU writes into its own buffer and
the buffers are merged in source order, so `--in-place`, `--lint` (text,
SARIF and diff), `--emit-edits` and the dry run are byte for byte what a
single thread produces. Rename state that later TUs need — a veto found in one
TU, a template-dependent token resolved by another TU's instantiations — is
exchanged at barriers: every non-header runs before any header, a pass that
discovers a new veto is discarded and run again with the veto known (as before),
and a TU whose dependent tokens the rest of the run resolved differently is
re-parsed on its own. That last rule also makes the cross-file rename
independent of the source order in cases it previously was not, such as a
template instantiated only from another header. Clang's diagnostics are
buffered per TU and printed in source order.

`normalize_variables --debug-trace` always runs one TU at a time, so its
per-TU trace stays readable.

---

## Using `cpp_format` as a pre-commit hook

### 1. Install the binary

Download the latest `cpp_format-linux-x86_64` from the [Releases](../../releases) page and place it somewhere on your `PATH`:

```sh
curl -L -o ~/.local/bin/cpp_format \
  https://github.com/<owner>/<repo>/releases/latest/download/cpp_format-linux-x86_64
chmod +x ~/.local/bin/cpp_format
```

### 2. Generate a compilation database

`cpp_format` uses Clang to parse your source files and therefore needs to know the compiler flags for each file. The easiest way to provide them is via a `compile_commands.json` in your project root, which `cpp_format` auto-detects.

- **CMake:** `cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ...`
- **Bazel:** if you use the [Bazel integration](#add-cpp_format-to-your-bazel-codebase)
  you don't need one at all — and it writes one for your editor anyway:
  `tools/cpp_format.sh compile_commands`
- **Bazel (Hedron plugin):** `bazel run @hedron_compile_commands//:refresh_all`
- **Bear:** `bear -- make` (or your build command)

If you do not have a `compile_commands.json` you can pass compiler flags directly — see the wrapper script approach below.

### 3. Add a config file

Commit a `cpp_format.yaml` at the root of your repository:

```yaml
# cpp_format.yaml
trailing_return_types: true

normalize_variables:
  - scope: member
    style: snake_case
  - scope: global
    style: snake_case
```

### 4. Add the hook to `.pre-commit-config.yaml`

**With `compile_commands.json` (recommended):**

```yaml
repos:
  - repo: local
    hooks:
      - id: cpp-format
        name: cpp_format
        entry: cpp_format --config=cpp_format.yaml --in-place
        language: system
        files: \.(h|cpp)$
```

pre-commit appends the list of staged files as positional arguments; `cpp_format` picks up `compile_commands.json` automatically to resolve include paths and compiler flags.

**Without `compile_commands.json` — wrapper script:**

If you cannot generate a `compile_commands.json`, create a small wrapper script (e.g. `tools/cpp_format_hook.sh`) that appends the required compiler flags:

```sh
#!/usr/bin/env bash
# tools/cpp_format_hook.sh
exec cpp_format --config=cpp_format.yaml --in-place "$@" -- -std=c++17 -Iinclude
```

Then reference the script in your hook:

```yaml
repos:
  - repo: local
    hooks:
      - id: cpp-format
        name: cpp_format
        entry: tools/cpp_format_hook.sh
        language: script
        files: \.(h|cpp)$
```

### Running pre-commit manually

```sh
pre-commit run cpp-format --all-files
```

---

## Bazel integration

The [quick start](#add-cpp_format-to-your-bazel-codebase) above wires
`cpp_format` into the build graph so compile info and the full header set come
from Bazel — not a committed `compile_commands.json`. This solves the
sandboxing problem where one target's translation unit cannot reach headers
owned by other targets.

**How it works.** A per-target **aspect** derives each `cc_*` target's compile
flags from `CcInfo.compilation_context` + the toolchain
(`cc_common.get_memory_inefficient_command_line`) and runs one `cpp_format
--emit-edits` action **per source file** — like `CppCompile` — declaring that
file **plus the target's transitive headers** as action inputs; that
declaration is what makes headers reachable under sandboxing. Because the unit
is a file, Bazel parallelizes, caches and remotely executes at file
granularity: editing one `.cpp` re-parses one file, not its whole target.
First-party targets only.

**Renaming across targets.** An action parses one file, but a rename must reach
*every* use of a declaration — the target's other files, and targets that
merely depend on the header declaring it. So every action is told, via
`--owned-files`, that the target's own sources and the dep closure's
first-party headers are renameable: those files are **not** parsed as extra
translation units, so each file is still rewritten by exactly one action (its
own) while its uses are rewritten wherever they appear. Without this a
`cc_binary`'s use of a `cc_library` member would be left behind when the member
is renamed, breaking the build. Headers of a `no-cpp-format` target are
excluded, so an unformatted target's declarations are never renamed at their
use sites either.

**Records, not diffs.** Each emit action writes offset-level **edit records**
(`{file, offset, length, old, new}`) plus a template-dependent-token resolution
sidecar — never a rendered diff, which cannot be merged because hunk offsets
don't compose. `cpp_format --aggregate` unions every file's records, resolves
dependent tokens across TUs (agreeing instantiations → an edit; a veto or
disagreement → dropped), merges per file (dedup identical, flag
overlapping-distinct conflicts), and renders or applies the result. This is the
clang-tidy `--export-fixes` + `clang-apply-replacements` model. Splitting a
target's translation units across actions changes nothing in it: the records
were always merged across processes.

**The entry point.** `cpp_format.sh <check|diff|fix> [pattern] [flags]` is the
front door: it `bazel query`s the first-party `cc_*` targets under `pattern`
(default `//...`), builds them with `--aspects=…%cpp_format_aspect
--output_groups=+cpp_format_edits` to materialize each file's record, reads
each target's manifest (`//pkg:name` → `<bazel-bin>/pkg/name.cpp_format.manifest`,
which lists that target's current records — never a glob, which would pick up
the stale record of a removed source), and runs `cpp_format --aggregate
--records-from=<list>` over them. It is a plain script, not a `bazel run`
target, precisely so it can invoke `bazel build` without nesting a Bazel server
inside a running one. Any argument starting with `-` is passed through to the
aggregation step — `--report-rename-conflicts` lists every rename the run
declined. Because the aspect and the wrapper rely on flags of the
binary (`--owned-files`, `--aggregate --records-from`), the kit and the
published binary are versioned together (see the release pin in
`MODULE.bazel`). `compile_commands`, `index` and `browse` are the same
query-and-build with a different output group and a different merge. There is
no rule that does this inside the build graph: a rule's `deps` cannot be a
pattern, and a fix cannot be an action (Bazel actions cannot mutate sources).

**Compilation database.** The aspect writes each target's compile command out
as a `<name>.compile_commands.jsonl` fragment (one JSON object per source
file, a plain `ctx.actions.write`; output group `cpp_format_compile_commands`,
which also carries the target's transitive headers so the generated ones get
built). `cpp_format.sh compile_commands`
merges the fragments into one `compile_commands.json`, filling in the two
things only known at run time: `directory` (the execution root, where every
relative flag resolves) and `file` (the source's absolute workspace path). A
file listed by several targets keeps the first entry. Nothing is compiled and
`cpp_format` never runs for this.

**Delivery paths.** The kit in [bazel/integration/](bazel/integration/) uses the
**prebuilt release binary** (no LLVM build) and is consumed either by URL
(`archive_override` — nothing copied but the one `cpp_format.sh` script) or by
copying the directory in. Its aspect reads the config as `@@//:cpp_format.yaml`
— the **root module's** file — so the same `.bzl` picks up *your* ruleset
whether it was imported or vendored. The single published binary both emits
records and aggregates them (`--aggregate`), so the kit needs only that one
download. Alternatively, if `cpp_formatting` is already a source dependency you
build, use [bazel/cpp_format.bzl](bazel/cpp_format.bzl) directly — the same
aspect, but it drives `//cpp_formatting:cpp_format` from source and stages the
Clang builtin headers from `@llvm-project`; point the script at it through its
environment, as this repository's own [tools/cpp_format.sh](tools/cpp_format.sh)
does.

---

## Symbol index

The same per-file pipeline that emits edit records can emit a **symbol index**:
for every symbol -- class, struct, enum, enumerator, field, function, method,
variable, parameter, alias, namespace, template parameter, macro -- every
declaration, definition and reference across the repository, as byte ranges.
Take any token in any file and find every use of it anywhere.

```sh
tools/cpp_format.sh index                # -> index.pb in the workspace root
tools/cpp_format.sh index //app/...      # one package tree; INDEX_OUT= to place it

# Look at it:
cpp_format --dump-index --format=text index.pb          # or json
cpp_format --dump-index --lookup=lib/foo.cpp:1234 index.pb
```

`--lookup` prints the symbol under the byte offset and every occurrence of it:

```
c:@S@Widget@FI@itemCount
  FIELD Widget::itemCount : int
  canonical widget.h:95-104
  widget.cpp:55-64 REFERENCE|READ
  widget.cpp:96-105 REFERENCE|WRITE
  widget.cpp:113-117 REFERENCE|READ|WRITE macro-body
  widget.h:95-104 DEFINITION
```

**The format** is a protocol buffer, [cpp_formatting/index.proto](cpp_formatting/index.proto).
An `IndexUnit` is what one translation unit produces: a `files` table
(paths relative to the working directory -- under Bazel, exec-root-relative
like `lib/foo.cpp`, `bazel-out/.../foo.pb.h` or `external/...`, so a unit is
portable), a `symbols` table keyed by Clang's USR (the symbol's identity across
TUs and machines: `c:@N@demo@S@Widget`), and `occurrences` that refer to both
by index and carry a `[begin, end)` byte range, role bits (`DECLARATION`,
`DEFINITION`, `REFERENCE`, `READ`, `WRITE`, `CALL`, `DEPENDENT`, `PASTED`, ...), whether
the token was spelled through a macro, and relations (`CALLED_BY`,
`CONTAINED_BY`); there is one occurrence per token and symbol, its roles the
union of every report of it. A unit also lists its `pending` dependent tokens
(below). Symbols
carry their kind, qualified name, printed type, canonical declaration and
symbol-level relations (`CHILD_OF`, `BASE_OF`, `OVERRIDE_OF`,
`SPECIALIZATION_OF`). The merged `Index` has the same tables with the
occurrences grouped per file, sorted by offset. Every list has a defined order
and there are no maps, so the bytes are a pure function of the content: units
cache under Bazel, and a merge does not depend on its inputs' order.

**How it is built.** `cpp_format --emit-index` parses a TU with Clang's own
indexing library (the one behind clangd) and records the occurrences in the
main file and in every *owned* file (`--owned-files`, or every source given);
symbols referenced from those files but declared elsewhere -- `std::`, other
repositories, system headers -- get a symbol entry with their canonical
location but no occurrences of their own. Under Bazel the `cpp_index_aspect`
runs one such action per translation unit, never for a header on its own (a
header is only ever compiled as part of a TU that includes it); the owned set
is the target's `srcs`, `hdrs` and `textual_hdrs` plus the dep closure's
first-party headers, so a header is indexed by every TU that includes it, and
a header-only dependency by its dependents' TUs. A `no-cpp-index` tag opts a
target out. Then
`cpp_format --merge-index` unions the units: files by path, symbols by USR,
identical occurrences deduplicated. `cpp_format.sh index [pattern]` does both
for the targets under a pattern -- the whole repository by default. An index is
itself a valid merge input, so it can be extended with further units.

**Browsing it.** [code_browser/](code_browser/) is an HTTP server that serves
a checkout with every indexed token annotated: click an identifier to see
what it is, jump to its definition, list its references. It reads the index
from SQLite (`code_browser --index=index.pb` imports it into `index.pb.sqlite`
when that is missing or older), so nothing is loaded up front.
`tools/cpp_format.sh browse` is the one command: it indexes the repository,
prints the browser's binary and command line, and serves the workspace at
`http://127.0.0.1:8080/` (`--port=N` picks a port); run it again to update the
index. See [code_browser/README.md](code_browser/README.md) for the routes. In
this repository `tools/cpp_format.sh` builds everything from source; a consumer
of the prebuilt kit gets the same command from a release asset, with nothing to
build.

**Extending it.** Any producer may emit `IndexUnit`s -- a proto-aware indexer
would describe `.proto` files with `Language.PROTO` symbols and let the C++
symbols of a generated `.pb.h` point back at them through a `GENERATED_FROM`
relation -- and `--merge-index` unions them all under one symbol table. `File`
and `Symbol` carry free-form `attributes` for whatever has no field yet.

**Dependent tokens.** `t.m` where `t` is a template parameter, `Helper<T>::k`,
or a call `f(t)` with dependent arguments names no declaration until the
template is instantiated, and Clang's indexer reports nothing for the opaque
cases. The indexer reuses the rename tool's cross-TU machinery for them: the
unit of the TU that *instantiates* the template records what the token
resolved to, as a `REFERENCE|DEPENDENT` occurrence of that symbol on the token
as written in the pattern (one per distinct symbol when the template is
instantiated with several types), while the unit of the file that *spells* the
token lists it as `pending`. The merge drops a pending entry once any unit
resolved it, and what is left over is reported as `unresolved` in the index;
`--lookup` on such a token says so. The two directions work across targets:
`demo_main.cpp`'s action resolves the token in `demo.h`'s template.

**What is not indexed.** A dependent token whose template no indexed
translation unit instantiates. Occurrences in files that are not owned
(system headers, other repositories) are dropped; their symbols stay. A token
spelled inside a macro *body* is recorded on the invocation's name at the
call site, which every symbol that expansion names shares; a macro *argument*
is recorded on its own spelling. `~Foo` and `operator+` occurrences cover
their first token only.

## Running all tests

```sh
bazel test //...
```

### End-to-end corpus tests

`bazel test //...` covers the tool against unit tests and small fixtures. The
corpus tests cover the question those cannot answer: **does a whole-repo
transform leave a real codebase compiling?** Each scenario builds a pinned
open-source Bazel repo, runs a ruleset over all of it, and builds it again — a
missed reference shows up as a compile error, exactly as it would for a user.

```sh
e2e/run_e2e.sh --list                     # what is in the corpus
e2e/run_e2e.sh mini_repo-all              # the fast local fixture (~10s)
e2e/run_e2e.sh                            # everything (googletest, abseil, ...)
```

This is **not** a Bazel target and cannot be one: it drives `bazel` inside a
second workspace, and nesting a Bazel server inside a running one deadlocks —
the same reason [bazel/integration/cpp_format.sh](bazel/integration/cpp_format.sh)
is a plain script. CI runs it nightly and on demand from
[.github/workflows/e2e.yml](.github/workflows/e2e.yml), never on the per-PR path.

Scenarios that are *expected* to fail declare so (`EXPECT=known_fail` plus the
phase and the reason), and a scenario that unexpectedly passes fails as an
XPASS — so a limitation that gets fixed is reported rather than silently
absorbed. See [e2e/README.md](e2e/README.md) for the phase list, how to add a
repo or ruleset, and how to triage a failure.

---

## Project structure

```
cpp_formatting/
  # Combined tool
  cpp_format.cpp                          # main(): YAML config + multi-pass driver

  # Parallel translation-unit driver (shared by all four binaries)
  tu_driver.h                             # TUSlot, TUSlotClient, runTranslationUnits(), --jobs sizing
  tu_driver.cpp                           # one ClangTool per TU on worker threads; barriers, re-runs
  tu_driver_test.cpp                      # gtest unit tests
  rename_state.h                          # cross-TU rename state: DependentResolutions, RenameVetoes, merge
  rename_state.cpp                        # record/veto/merge semantics
  rename_state_test.cpp                   # gtest unit tests

  # trailing_return_types
  trailing_return_types.cpp               # main(): CLI parsing, ActionFactory
  trailing_return_types_lib.h             # public API: callback, action, test helper
  trailing_return_types_lib.cpp           # implementation
  trailing_return_types_test.cpp          # gtest unit tests
  trailing_return_types_integration_test.sh  # shell integration tests

  # const_placement
  const_placement.cpp                     # main(): CLI parsing, ActionFactory
  const_placement_lib.h                   # public API: ConstStyle, action, factory, test helper
  const_placement_lib.cpp                 # QualifiedTypeLoc visitor + the shape guard
  const_placement_test.cpp                # gtest unit tests
  const_placement_integration_test.sh     # shell integration tests

  # normalize_variables
  normalize_variables.cpp                 # main(): CLI parsing, FileSet builder
  rename_variables_lib.h                  # public API: FileSet, callback, RenameActionFactory, factories
  rename_variables_lib.cpp                # two-pass AST visitor implementation + DebugTraceVisitor
  naming_convention.h                     # NamingStyle enum + word-split/format API
  naming_convention.cpp                   # split + format implementation
  naming_convention_test.cpp              # gtest unit tests
  rename_variables_test.cpp               # gtest unit tests
  normalize_variables_integration_test.sh # shell integration tests

  # Embedded clang resource directory
  embedded_clang_resource.h               # ensureClangResourceDir()
  embedded_clang_resource.cpp             # extract embedded headers tar.gz to a per-content-hash cache dir

  # Lint mode (CI/CD)
  lint_lib.h                              # LintDiagnostic, LintReport (text/SARIF), emitUnifiedDiff
  lint_lib.cpp                            # implementation (JSON via llvm/Support/JSON.h)
  lint_lib_test.cpp                       # gtest unit tests
  lint_integration_test.sh                # shell integration tests for --lint/--format

  # Symbol index (--emit-index / --merge-index / --dump-index)
  index.proto                             # the schema: IndexUnit (per TU) and Index (merged)
  cpp_index_lib.h                         # IndexDataConsumer -> IndexUnit, IndexActionFactory, test helper
  cpp_index_lib.cpp                       # location mapping, owned-file filter, symbol interning
  cpp_index_test.cpp                      # gtest unit tests (in-memory TUs)
  cpp_index_merge.h                       # Clang-free: normalize, merge, group per file, lookup, I/O
  cpp_index_merge.cpp                     # implementation
  cpp_index_merge_test.cpp                # gtest unit tests
  index_integration_test.sh               # shell integration tests for the three modes

  # Shared
  output_mode.h                           # OutputMode enum (DryRun / InPlace / Debug / Lint)
  BUILD                                   # all Bazel targets, plus the clang_include_headers
                                          #   pkg_tar and the genrule that embeds it via xxd -i

  testdata/                               # input/expected pairs for integration tests

MODULE.bazel                              # Bzlmod dependencies
third_party/llvm/                         # module extension that builds Clang/LLVM from source
patches/                                  # (unused since the LLVM 19 upgrade)
```

## Dependencies

Managed via Bzlmod ([MODULE.bazel](MODULE.bazel)):

| Dependency | Version |
|---|---|
| `llvm-project` (Clang libraries + LLVM YAML + builtin headers) | 21.1.8 (built from source, see below) |
| `protobuf` (the symbol index format; its own abseil pin comes with it) | 31.1 |
| `googletest` | 1.15.2 |
| `rules_cc` | 0.2.17 |
| `rules_shell` | 0.4.1 |
| `rules_pkg` | (bundles the clang builtin headers into the binary) |
| `bazel_skylib`, `platforms`, `rules_python`, `apple_support` | (required by the LLVM overlay) |

Clang/LLVM is **not** pulled from the Bazel Central Registry — the BCR only
publishes `llvm-project` up to 17.0.4. To use Clang 19 (needed for C++23
features such as deducing `this`), it is built from source via a local module
extension in [third_party/llvm/](third_party/llvm/), which fetches the pinned
`llvm-project` monorepo and runs LLVM's own Bazel overlay. The first build
therefore compiles Clang/LLVM and takes a while; change the pinned version by
editing `LLVM_VERSION`/`LLVM_SHA256` in
[third_party/llvm/extensions.bzl](third_party/llvm/extensions.bzl).

## License

See [LICENSE](LICENSE).
