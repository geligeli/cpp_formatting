# cpp_formatting

A collection of Clang-based source-to-source rewrite tools for C++ codebases.

| Tool | Description |
|---|---|
| `trailing_return_types` | Converts functions from leading to trailing return type syntax |
| `normalize_variables` | Renames variables to a consistent naming convention |
| `cpp_format` | Combined tool — runs any combination of the above, driven by a YAML config |

## Requirements

- [Bazel](https://bazel.build/) 7+ with Bzlmod support
- A C++17-capable host compiler (for building the tools)
- Internet access on the first build (Bazel downloads LLVM 17, GoogleTest, etc.)

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

Renames variables (member, local, or global) to a chosen naming convention using Clang's AST. Handles cross-file renaming: list the header alongside its `.cpp` and both files are updated consistently.

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
#                   static_global, const_global
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
| `--normalize-variables-scope=<scope>` | One of `member`, `local`, `global`, `static_member`, `const_member`, `static_global`, `const_global` |
| `--normalize-variables-style=<style>` | Target naming style |
| `--in-place` / `-i` | Overwrite files on disk (default: dry-run) |

**Pass ordering:** `normalize_variables` rules are applied first (in the order listed in the config), then `trailing_return_types`. For in-place mode each pass reads the output of the previous one from disk.

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

  # Shared
  output_mode.h                           # OutputMode enum (DryRun / InPlace / Debug)
  BUILD                                   # all Bazel targets, plus the clang_include_headers
                                          #   pkg_tar and the genrule that embeds it via xxd -i

  testdata/                               # input/expected pairs for integration tests

MODULE.bazel                              # Bzlmod dependencies
patches/                                  # patch applied to llvm-project
```

## Dependencies

Managed via Bzlmod ([MODULE.bazel](MODULE.bazel)):

| Dependency | Version |
|---|---|
| `llvm-project` (Clang libraries + LLVM YAML + builtin headers) | 17.0.3 |
| `googletest` | 1.14.0.bcr.1 |
| `rules_cc` | 0.2.17 |
| `rules_shell` | 0.4.1 |
| `rules_pkg` | (bundles the clang builtin headers into the binary) |

## License

See [LICENSE](LICENSE).
