# trailing_return_types

A Clang-based source-to-source rewrite tool that converts C++ functions from
leading return type syntax to [trailing return type](https://en.cppreference.com/w/cpp/language/function)
syntax.

```cpp
// Before
int add(int a, int b) { return a + b; }
const int* sentinel() { static int v = -1; return &v; }

// After
auto add(int a, int b) -> int { return a + b; }
auto sentinel() -> const int* { static int v = -1; return &v; }
```

## Requirements

- [Bazel](https://bazel.build/) 7+ with Bzlmod support
- A C++17-capable host compiler (for building the tool itself)
- Internet access on the first build (Bazel downloads LLVM 17, GoogleTest, etc.)

## Building

```sh
bazel build //cpp_formatting:trailing_return_types
```

Build everything including tests:

```sh
bazel build //...
```

## Usage

**Dry-run** — print the rewritten source to stdout without modifying the file:

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

The `--` separates the tool's own flags from the Clang compilation flags passed
to the parser. At minimum `-std=c++17` (or your project's standard) is required.
For files that use standard-library headers, the tool automatically detects the
Clang resource directory so that built-ins like `<cstddef>` resolve correctly.

## What gets rewritten

| Input | Rewritten? | Notes |
|---|---|---|
| `int foo()` | yes | plain return type |
| `const int* foo()` | yes | leading cv-qualifiers captured via backward scan |
| `int foo();` (declaration only) | yes | forward declarations are rewritten too |
| `void foo()` | no | `void` excluded by matcher |
| `auto foo() -> int` | no | already has a trailing return |
| `auto foo() { return 42; }` | no | deduced `auto` — rewriting would be redundant |
| `decltype(auto) foo()` | no | same deduced-auto check |
| `operator bool()` (conversion) | no | `cxxConversionDecl()` excluded by matcher |

## Running tests

Unit tests (gtest):

```sh
bazel test //cpp_formatting:trailing_return_types_test
```

Integration tests (shell script, runs the compiled binary on real files):

```sh
bazel test //cpp_formatting:trailing_return_types_integration_test
```

All tests:

```sh
bazel test //...
```

## Project structure

```
cpp_formatting/
  trailing_return_types.cpp       # main(): CLI parsing, ActionFactory
  trailing_return_types_lib.h     # public API: callback, action, test helper
  trailing_return_types_lib.cpp   # implementation
  trailing_return_types_test.cpp  # gtest unit tests
  integration_test.sh             # shell integration tests
  testdata/                       # input/expected pairs for integration tests
  BUILD                           # Bazel targets
MODULE.bazel                      # Bzlmod dependencies
patches/                          # patch applied to llvm-project
```

## Dependencies

Managed entirely via Bzlmod ([MODULE.bazel](MODULE.bazel)):

| Dependency | Version |
|---|---|
| `llvm-project` (Clang libraries) | 17.0.3 |
| `googletest` | 1.14.0.bcr.1 |
| `rules_cc` | 0.2.17 |
| `rules_shell` | 0.4.1 |

## License

See [LICENSE](LICENSE).
