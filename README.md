# cpp_formatting

A collection of Clang-based source-to-source rewrite tools for C++ codebases.

## Add cpp_format to your Bazel codebase

Get a parallel, cached **lint / diff / fix** gate over your C++ sources using
the **prebuilt release binary** — no need to build Clang/LLVM from source. A
per-target [aspect](bazel/integration/cpp_format.bzl) derives each target's
compile flags and header set from `CcInfo` + the toolchain, so every include is
reachable under sandboxing with **no `compile_commands.json`**. The binary
embeds and self-extracts the Clang builtin headers, so nothing else is fetched.

**1. Vendor the integration kit.** Copy [bazel/integration/](bazel/integration/)
from this repo into your own (e.g. as `third_party/cpp_format/`):

```
third_party/cpp_format/
  extensions.bzl     # fetches the release binary for the host platform
  cpp_format.bzl     # the aspect + check/diff/fix rules
  BUILD.bazel
```

**2. Fetch the binary in `MODULE.bazel`.** Pick a release tag from the
[Releases](https://github.com/geligeli/cpp_formatting/releases) page (tags are
`<YYYYMMDD>-<shortsha>`):

```starlark
cpp_format = use_extension("//third_party/cpp_format:extensions.bzl", "cpp_format")
cpp_format.release(
    version = "20260720-39c5de9",
    # Optional but recommended for reproducible CI — pin per-asset hashes:
    # sha256 = {"cpp_format-linux-x86_64": "…"},
)
use_repo(cpp_format, "cpp_format_bin")
```

The host platform's asset is selected automatically (Linux x86_64/aarch64,
macOS arm64, Windows x64).

**3. Add a ruleset** — `cpp_format.yaml` at your repo root describes the passes
to run (see [YAML config](#config-file---config)):

```yaml
normalize_variables:
  - scope: member
    style: snake_case
trailing_return_types: true
```

The aspect reads it as `//:cpp_format.yaml`, so export it from your root
`BUILD.bazel` (a cross-package file reference needs this):

```starlark
exports_files(["cpp_format.yaml"])
```

**4. Declare the targets** to format in any `BUILD.bazel`:

```starlark
load("//third_party/cpp_format:cpp_format.bzl", "cpp_format_targets")

cpp_format_targets(
    name = "format",
    deps = [
        "//src:mylib",   # any first-party cc_library / cc_binary targets;
        "//src:myapp",   # their transitive first-party sources are covered
    ],
)
```

**5. Run it:**

```sh
bazel run  //:format.diff    # preview the merged, git-apply-able repo patch
bazel test //:format.check   # CI lint gate — fails if any edit would be made
bazel run  //:format.fix     # apply the edits to your sources in place
```

`.check` is a hermetic test (parallel & cached per target); `.fix` and `.diff`
merge every target's edit records — deduping, resolving template-dependent
member tokens across translation units, and flagging genuine conflicts — into
one repository-wide change. See [Bazel integration](#bazel-integration) below
for how it works.

> **Notes.** Only first-party `cc_*` targets are formatted (external deps are
> skipped). Each header is owned by the single target that lists it in `hdrs`;
> a header in no target's `srcs`/`hdrs` is never visited. The binary extracts
> its embedded headers to a temp cache per action, so a strict sandbox needs a
> writable `TMPDIR` (Bazel provides one by default). To format the whole repo,
> point `deps` at your top-level targets — the aspect follows `deps`
> transitively. If you'd rather build the tool from source instead of using a
> release, depend on `//cpp_formatting:cpp_format` and use
> [bazel/cpp_format.bzl](bazel/cpp_format.bzl) instead of the vendored kit.

---

| Tool | Description |
|---|---|
| `trailing_return_types` | Converts functions from leading to trailing return type syntax |
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

The `--` separates the tool's own flags from the Clang compilation flags. At minimum `-std=c++17` is required. The tool ships its own Clang built-in headers, so files using standard-library headers (`<cstddef>`, etc.) work out of the box without a system Clang.

### What gets rewritten

| Input | Rewritten? | Notes |
|---|---|---|
| `int foo()` | yes | plain return type |
| `const int* foo()` | yes | leading cv-qualifiers captured via backward scan |
| `int foo();` (declaration only) | yes | forward declarations rewritten too |
| `void foo()` | no | `void` excluded by matcher |
| `auto foo() -> int` | no | already has a trailing return |
| `auto foo() { return 42; }` | no | deduced `auto` — rewriting would be redundant |
| `decltype(auto) foo()` | no | same deduced-auto check |
| `operator bool()` (conversion) | no | `cxxConversionDecl()` excluded by matcher |

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
| `--debug-trace` | Print, per TU, every rename target and reference site found in the AST. Makes no modifications. |

**Supported scopes:**

| Scope | Targets |
|---|---|
| `member` | non-static member variables (`FieldDecl`) and static data members |
| `local` | local variables and function parameters |
| `global` | file- and namespace-scope variables (non-member, non-local) |
| `static_member` | static data members only |
| `const_member` | static data members that are `const` or `constexpr` |
| `static_global` | file- and namespace-scope variables declared `static` |
| `const_global` | file- and namespace-scope variables that are `const` or `constexpr` |
| `method` | member functions, static and non-static (never constructors, destructors, conversion functions, or overloaded operators; a virtual function is renamed together with its whole override hierarchy — if any override is declared outside the listed files, the rename is skipped) |

**Cross-file renaming:** list all files that share declarations — order does not matter. The tool auto-promotes header files to the end of the source list so each `.cpp` is parsed against the original on-disk header content; edits are buffered and committed atomically once every TU has been processed.

**Debugging missed renames:** if you suspect the tool isn't renaming everything you expected, run with `--debug-trace`. It prints the full rename map and every reference site found in each TU, with `main=Y/N`, `macro=Y/N`, and a `WILL_RENAME` marker on sites that would actually be rewritten. A site that never appears with `WILL_RENAME` in any TU is missing from the source list passed to the tool.

### Tests

```sh
bazel test //cpp_formatting:naming_convention_test
bazel test //cpp_formatting:rename_variables_test
bazel test //cpp_formatting:normalize_variables_integration_test
```

---

## `cpp_format` — combined tool

Runs any combination of the above passes in a single invocation, driven by a YAML configuration file or individual CLI flags.

### Config file (`--config`)

Create a YAML file describing which passes to run:

```yaml
# cpp_format.yaml

# Rewrite functions to trailing return type syntax.
trailing_return_types: true

# Rename variables — multiple rules are applied in order.
# Supported scopes: member, local, global,
#                   static_member, const_member,
#                   static_global, const_global, method
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
```

### Options

| Flag | Description |
|---|---|
| `--config=<file>` | YAML configuration file (takes precedence over per-pass flags) |
| `--trailing-return-types` | Enable the trailing-return-type pass |
| `--normalize-variables-scope=<scope>` | One of `member`, `local`, `global`, `static_member`, `const_member`, `static_global`, `const_global`, `method` |
| `--normalize-variables-style=<style>` | Target naming style |
| `--in-place` / `-i` | Overwrite files on disk (default: dry-run) |
| `--lint` | Analyze only — report violations, modify nothing, exit 1 if any are found |
| `--format=<fmt>` | Output format for `--lint`: `text` (default), `sarif`, or `diff` |

**Pass ordering:** `normalize_variables` rules are applied first (in the order listed in the config), then `trailing_return_types`. For in-place mode each pass reads the output of the previous one from disk.

---

## Lint mode (CI/CD)

All three binaries support a lint mode that reports what *would* change without modifying any files:

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
(`cc_common.get_memory_inefficient_command_line`) and runs `cpp_format
--emit-edits`, declaring the target's sources **plus its transitive headers** as
action inputs — that declaration is what makes headers reachable under
sandboxing. Each action is parallel and cached; first-party targets only.

**Records, not diffs.** Each emit action writes offset-level **edit records**
(`{file, offset, length, old, new}`) plus a template-dependent-token resolution
sidecar — never a rendered diff, which cannot be merged because hunk offsets
don't compose. `cpp_format --aggregate` unions all targets' records, resolves
dependent tokens across TUs (agreeing instantiations → an edit; a veto or
disagreement → dropped), merges per file (dedup identical, flag
overlapping-distinct conflicts), and renders or applies the result. This is the
clang-tidy `--export-fixes` + `clang-apply-replacements` model.

**Targets.** `cpp_format_targets(name, deps)` generates three:

| Target | Kind | Purpose |
|---|---|---|
| `<name>.check` | `bazel test` | Hermetic lint gate — counts edits without reading sources; exits 1 if any. |
| `<name>.diff` | `bazel run` | Prints the merged, git-apply-able unified diff (review artifact). |
| `<name>.fix` | `bazel run` | Applies the edits in `$BUILD_WORKSPACE_DIRECTORY` (outside the action graph, since Bazel actions cannot mutate sources). |

**Two delivery paths.** The vendored kit in [bazel/integration/](bazel/integration/)
uses the **prebuilt release binary** (no LLVM build). Alternatively, if
`cpp_formatting` is a source dependency of your module, build the tool from
source and use [bazel/cpp_format.bzl](bazel/cpp_format.bzl) directly — it is the
same aspect, but it drives `//cpp_formatting:cpp_format` and stages the Clang
builtin headers from `@llvm-project` (see [bazel/testdata/BUILD.bazel](bazel/testdata/BUILD.bazel)
for a worked example). The single published binary both emits records and
aggregates them (`--aggregate`), so the release-based kit needs only that one
download.

---

## Running all tests

```sh
bazel test //...
```

---

## Project structure

```
cpp_formatting/
  # Combined tool
  cpp_format.cpp                          # main(): YAML config + multi-pass driver

  # trailing_return_types
  trailing_return_types.cpp               # main(): CLI parsing, ActionFactory
  trailing_return_types_lib.h             # public API: callback, action, test helper
  trailing_return_types_lib.cpp           # implementation
  trailing_return_types_test.cpp          # gtest unit tests
  integration_test.sh                     # shell integration tests

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
| `llvm-project` (Clang libraries + LLVM YAML + builtin headers) | 19.1.7 (built from source, see below) |
| `googletest` | 1.14.0.bcr.1 |
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
