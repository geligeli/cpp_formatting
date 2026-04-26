# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build System

This project uses [Bazel](https://bazel.build/) with Bzlmod (MODULE.bazel).

**Build everything:**
```
bazel build //...
```

**Run all tests:**
```
bazel test //...
```

**Build a specific target:**
```
bazel build //cpp_formatting:trailing_return_types
```

**Run a specific test suite:**
```
bazel test //cpp_formatting:trailing_return_types_test
bazel test //cpp_formatting:trailing_return_types_integration_test
```

**Run the binary (dry-run on a file):**
```
bazel run //cpp_formatting:trailing_return_types -- path/to/file.cpp -- -std=c++17
```

**Run the binary in-place:**
```
bazel run //cpp_formatting:trailing_return_types -- -i path/to/file.cpp -- -std=c++17
```

## Project Structure

### Source files

All source files live under [cpp_formatting/](cpp_formatting/).

- [cpp_formatting/trailing_return_types.cpp](cpp_formatting/trailing_return_types.cpp) — `main()`: CLI option parsing and a tiny `ActionFactory` that instantiates `TrailingReturnTypesAction` with the chosen `OutputMode`.
- [cpp_formatting/trailing_return_types_lib.h](cpp_formatting/trailing_return_types_lib.h) — Public API of the shared library:
  - `TrailingReturnCallback` — AST match callback that performs the rewrite
  - `registerTrailingReturnMatchers()` — single authoritative place for the matcher predicate
  - `TrailingReturnTypesAction` — frontend action (dry-run or in-place via `OutputMode`)
  - `rewriteToTrailingReturnTypes()` — test helper that rewrites an in-memory string
- [cpp_formatting/trailing_return_types_lib.cpp](cpp_formatting/trailing_return_types_lib.cpp) — Implementation of the above, plus the private `CaptureAction` used by the test helper.

### Tests

- [cpp_formatting/trailing_return_types_test.cpp](cpp_formatting/trailing_return_types_test.cpp) — gtest unit tests that call `rewriteToTrailingReturnTypes()` directly.
- [cpp_formatting/integration_test.sh](cpp_formatting/integration_test.sh) — Shell integration tests that invoke the compiled binary on real files:
  1. Dry-run on a single file — rewritten source goes to stdout.
  2. In-place on a single file — file is modified on disk.
  3. In-place on two files in one invocation — both files are modified.
  4. In-place on a file with system `#include`s — validates that the binary auto-detects the Clang resource dir so built-in headers resolve.
- [cpp_formatting/testdata/input.cpp](cpp_formatting/testdata/input.cpp) / [cpp_formatting/testdata/expected.cpp](cpp_formatting/testdata/expected.cpp) — First integration test fixture (free functions).
- [cpp_formatting/testdata/input2.cpp](cpp_formatting/testdata/input2.cpp) / [cpp_formatting/testdata/expected2.cpp](cpp_formatting/testdata/expected2.cpp) — Second integration test fixture (class member functions).
- [cpp_formatting/testdata/input3.cpp](cpp_formatting/testdata/input3.cpp) / [cpp_formatting/testdata/expected3.cpp](cpp_formatting/testdata/expected3.cpp) — Third integration test fixture (stdlib types: `std::size_t`, `std::ostream&`; exercises Clang resource-dir auto-detection).

### Build files

- [cpp_formatting/BUILD](cpp_formatting/BUILD) — Defines `cc_library`, `cc_binary`, `cc_test`, and `sh_test` targets.
- [MODULE.bazel](MODULE.bazel) — Bzlmod dependencies: `rules_cc`, `llvm-project 17.0.3`, `googletest 1.14.0.bcr.1`, `rules_shell`.

## Key Design Decisions

### What the tool does and does not rewrite

| Input | Rewritten? | Reason |
|---|---|---|
| `int foo()` | yes | plain return type |
| `const int* foo()` | yes | leading qualifier included via backwards scan |
| `void foo()` | no | void excluded from matcher |
| `auto foo() -> int` | no | already has trailing return (`hasTrailingReturn()`) |
| `auto foo() { return 42; }` | no | deduced auto detected via `AutoTypeLoc` check |
| `decltype(auto) foo()` | no | same `AutoTypeLoc` check |
| `int foo();` (declaration only) | yes | all declarations rewritten independently |
| `int foo(); int foo() {...}` (both in same TU) | yes (both) | each declaration rewritten independently |

### Known non-obvious behaviours

- **`QualifiedTypeLoc` gap** — Clang's `QualifiedTypeLoc` does not include leading `const`/`volatile`/`restrict` in its source range. `skipQualifiersBackward()` in the library works around this by scanning the raw source buffer leftward.
- **Token merging guard** — When there is no whitespace between the return type and the function name (e.g. `Foo&operator=`), `"auto "` (with a trailing space) is emitted instead of `"auto"` to prevent tokens from merging.
- **`FunctionTypeLoc::getLocalRangeEnd()`** — For member functions with cv/ref/noexcept qualifiers, Clang sets this to the location of the last qualifier, so `InsertTextAfterToken` correctly places ` -> T` after `const`, `noexcept`, etc.

## Notes

- The `bazel-*` symlinks in the root are Bazel output/convenience symlinks — do not edit them.
- The `patches/` directory contains a patch applied to the `llvm-project` dependency via `single_version_override` in MODULE.bazel.
