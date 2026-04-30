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
bazel build //cpp_formatting:cpp_format
bazel build //cpp_formatting:trailing_return_types
bazel build //cpp_formatting:normalize_variables
```

**Run a specific test suite:**
```
bazel test //cpp_formatting:trailing_return_types_test
bazel test //cpp_formatting:trailing_return_types_integration_test
bazel test //cpp_formatting:naming_convention_test
bazel test //cpp_formatting:rename_variables_test
bazel test //cpp_formatting:normalize_variables_integration_test
```

**Run a binary (dry-run):**
```
bazel run //cpp_formatting:trailing_return_types -- path/to/file.cpp -- -std=c++17
bazel run //cpp_formatting:normalize_variables -- --style=snake_case --scope=member path/to/file.cpp -- -std=c++17
bazel run //cpp_formatting:cpp_format -- --config=cpp_format.yaml --in-place file.cpp -- -std=c++17
```

**Run a binary in-place:**
```
bazel run //cpp_formatting:trailing_return_types -- -i path/to/file.cpp -- -std=c++17
bazel run //cpp_formatting:normalize_variables -- --style=snake_case --scope=member --in-place path/to/file.cpp -- -std=c++17
```

**Debug rename detection (`normalize_variables --debug-trace`):** prints per TU the rename map and every reference site found in the AST, with `main=Y/N`, `macro=Y/N`, and a `WILL_RENAME` marker on sites that would actually be rewritten. Useful for diagnosing why a particular use was missed (e.g. it lives in a file that wasn't passed in the source list).

## Project Structure

### Source files

All source files live under [cpp_formatting/](cpp_formatting/).

#### `trailing_return_types` — rewrite functions to trailing return syntax

- [cpp_formatting/trailing_return_types.cpp](cpp_formatting/trailing_return_types.cpp) — `main()`: CLI option parsing and a tiny `ActionFactory`.
- [cpp_formatting/trailing_return_types_lib.h](cpp_formatting/trailing_return_types_lib.h) — Public API:
  - `TrailingReturnCallback` — AST match callback that performs the rewrite
  - `registerTrailingReturnMatchers()` — single authoritative place for the matcher predicate
  - `TrailingReturnTypesAction` — frontend action (dry-run or in-place via `OutputMode`)
  - `rewriteToTrailingReturnTypes()` — test helper that rewrites an in-memory string
- [cpp_formatting/trailing_return_types_lib.cpp](cpp_formatting/trailing_return_types_lib.cpp) — Implementation plus the private `CaptureAction` used by the test helper.

#### `normalize_variables` — rename variables to a consistent naming convention

- [cpp_formatting/normalize_variables.cpp](cpp_formatting/normalize_variables.cpp) — `main()`: CLI option parsing, builds a `FileSet` from source paths, drives `rename_variables_lib`.
- [cpp_formatting/rename_variables_lib.h](cpp_formatting/rename_variables_lib.h) — Public API:
  - `FileSet` — set of real absolute paths whose declarations the tool collects (enables cross-file renaming)
  - `VariableRenameCallback` — callback invoked once per canonical declaration
  - `VariableScope` — broad: `Member` | `Local` | `Global`; fine-grained: `StaticMember` | `ConstMember` | `StaticGlobal` | `ConstGlobal`
  - `PendingRewrites` — map of file path → fully rewritten content; populated during `ClangTool::run()`, drained by `flush()`
  - `RenameActionFactory` — `FrontendActionFactory` subclass that buffers every TU's edits in `PendingRewrites`; call `flush()` after `Tool.run()` returns to commit them (atomic disk writes for `InPlace`, formatted stdout for `DryRun`, no-op for `Debug`)
  - Factory functions: `RenameAllMemberVariables`, `RenameAllLocalVariables`, `RenameAllGlobalVariables`, `RenameAllStaticMemberVariables`, `RenameAllConstMemberVariables`, `RenameAllStaticGlobalVariables`, `RenameAllConstGlobalVariables`
  - `orderSourcesForRename()` — promotes header files to the end of the source list so every `.cpp` TU is parsed against the original on-disk header content
  - `rewriteVariableNames()` — test helper (single in-memory TU, returns the rewritten string)
- [cpp_formatting/rename_variables_lib.cpp](cpp_formatting/rename_variables_lib.cpp) — Implementation: two-pass `RecursiveASTVisitor` (collect declarations, then apply renames). A third visitor (`DebugTraceVisitor`) runs in `OutputMode::Debug` to print every reference site without modifying anything.
- [cpp_formatting/naming_convention.h](cpp_formatting/naming_convention.h) — `NamingStyle` enum, `splitIntoWords`, `formatName`, `renameToStyle`, `parseNamingStyle`.
- [cpp_formatting/naming_convention.cpp](cpp_formatting/naming_convention.cpp) — Splits camelCase/snake_case/prefixed names into word lists and reassembles in any target style.

#### `cpp_format` — combined tool with YAML config

- [cpp_formatting/cpp_format.cpp](cpp_formatting/cpp_format.cpp) — `main()`: parses CLI options or a YAML config file, then runs `normalize_variables` passes followed by `trailing_return_types`. Shares `rename_variables_lib` and `trailing_return_types_lib`.

#### Embedded Clang resource directory

All three binaries link the Clang built-in headers (`stddef.h`, `__stddef_max_align_t.h`, etc.) into the binary itself, so no system Clang installation is required at runtime.

- [cpp_formatting/embedded_clang_resource.h](cpp_formatting/embedded_clang_resource.h) — declares `ensureClangResourceDir()`.
- [cpp_formatting/embedded_clang_resource.cpp](cpp_formatting/embedded_clang_resource.cpp) — implementation: extracts the embedded `.tar.gz` to `$XDG_CACHE_HOME/cpp_formatting/clang_resource_<fnv1a-hash>/` on first call, returns the cached path on subsequent calls. Extraction goes via `mkdtemp` + atomic `rename` so concurrent invocations are safe.
- The `clang_include_headers` `pkg_tar` rule in [cpp_formatting/BUILD](cpp_formatting/BUILD) packages `@@llvm-project+//clang:builtin_headers_gen` into a `tar.gz`. The `clang_include_headers_embed_cc` `genrule` runs `xxd -i` on it to produce a `const unsigned char[]` translation unit linked into every binary.

### Tests

- [cpp_formatting/trailing_return_types_test.cpp](cpp_formatting/trailing_return_types_test.cpp) — gtest unit tests for `trailing_return_types_lib`.
- [cpp_formatting/integration_test.sh](cpp_formatting/integration_test.sh) — Shell integration tests for `trailing_return_types`:
  1. Dry-run on a single file — rewritten source goes to stdout.
  2. In-place on a single file — file is modified on disk.
  3. In-place on two files in one invocation — both files are modified.
  4. In-place on a file with system `#include`s — validates the embedded Clang resource directory works (no system Clang required).
- [cpp_formatting/naming_convention_test.cpp](cpp_formatting/naming_convention_test.cpp) — gtest unit tests for `naming_convention`: `splitIntoWords`, `formatName`, `renameToStyle`.
- [cpp_formatting/rename_variables_test.cpp](cpp_formatting/rename_variables_test.cpp) — gtest unit tests for `rename_variables_lib` (member, local, global, static data members, const members, static globals, const globals, templates, cross-file, constructor initializers).
- [cpp_formatting/normalize_variables_integration_test.sh](cpp_formatting/normalize_variables_integration_test.sh) — Shell integration tests for `normalize_variables`:
  1. Multi-file member rename — cross-file references, pointer-to-member, lambda, scope separation.
  2. Shadowed variable — global renamed, same-named local parameter unchanged.
  3. Source ordering — header passed in the middle of the source list; tool auto-promotes it to the end so every `.cpp` is parsed against the original header.

### Build files

- [cpp_formatting/BUILD](cpp_formatting/BUILD) — Defines all `cc_library`, `cc_binary`, `cc_test`, and `sh_test` targets, plus the `clang_include_headers` `pkg_tar` and the `clang_include_headers_embed_cc` `genrule` that embeds the headers into every binary.
- [MODULE.bazel](MODULE.bazel) — Bzlmod dependencies: `rules_cc`, `llvm-project 17.0.3`, `googletest 1.14.0.bcr.1`, `rules_shell`, `rules_pkg`.

## YAML Config Format (`cpp_format`)

`cpp_format` reads a YAML file (via `--config=<file>`) that specifies which passes to run:

```yaml
# All fields are optional; omit any section to skip that pass.

# Rewrite functions to use trailing return type syntax.
trailing_return_types: true

# Rename variables in one or more scopes (applied in order).
normalize_variables:
  - scope: member   # non-static and static data members
    style: snake_case
  - scope: global   # file- and namespace-scope variables
    style: snake_case
  # - scope: local  # local variables and parameters
  #   style: camelCase
```

Supported scopes: `member`, `local`, `global`, `static_member`, `const_member`, `static_global`, `const_global`.

Supported styles: `snake_case`, `_leading`, `trailing_`, `m_prefix`, `camelCase`, `UpperCamelCase`, `UPPER_SNAKE_CASE`, `kConstant`.

## Key Design Decisions

### `trailing_return_types`: what gets rewritten

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

### `normalize_variables`: cross-file renaming

The tool processes one translation unit at a time. To rename declarations in a header alongside their uses in a `.cpp`, the source list must include both files — but the order does not matter: `orderSourcesForRename()` always promotes header files to the end of the list so every `.cpp` TU is parsed against the original on-disk header content.

Edits are buffered in `RenameActionFactory::Pending` (a path → content map) and only committed by `flush()` after `ClangTool::run()` returns. This is what makes multi-file in-place renaming correct: every TU compiles against the original on-disk source regardless of how many headers are in the list.

`FileSet` (in `rename_variables_lib.h`) holds the real absolute paths of all source files. `CollectRenamesVisitor` collects declarations from any file in the set (not just the main file), enabling the header's declarations to be found when compiling the `.cpp`.

### Known non-obvious behaviours

- **`QualifiedTypeLoc` gap** — Clang's `QualifiedTypeLoc` does not include leading `const`/`volatile`/`restrict` in its source range. `skipQualifiersBackward()` scans the raw source buffer leftward.
- **Token merging guard** — When there is no whitespace between the return type and the function name (e.g. `Foo&operator=`), `"auto "` (with a trailing space) is emitted to prevent token merging.
- **`FunctionTypeLoc::getLocalRangeEnd()`** — For member functions with cv/ref/noexcept qualifiers, Clang sets this to the location of the last qualifier.
- **Pointer-to-member** — `&S::field` produces a `DeclRefExpr` with `FieldDecl` (not `VarDecl`). `VisitDeclRefExpr` handles both cases.
- **Constructor mem-initializers** — `S() : val_(0) {}` is a `CXXCtorInitializer`, not a `Stmt` or `Decl`, so it is not visited by the standard `Visit*` callbacks. `ApplyRenamesVisitor` overrides `TraverseConstructorInitializer` and rewrites at `getMemberLocation()`.
- **Template instantiation** — `FieldDecl` instances in template specializations are mapped back to the primary-template field by index walk through `ClassTemplateSpecializationDecl`.
- **Shadowed variables** — `matchesScope()` filters by scope: a parameter with the same name as a global is not collected when renaming globals.
- **Per-file-content cache key** — the embedded Clang resource directory is extracted under a directory whose name includes the FNV-1a hash of the embedded `.tar.gz`. If the embedded headers change (e.g. after an LLVM upgrade) a fresh cache directory is created automatically.

## Notes

- The `bazel-*` symlinks in the root are Bazel output/convenience symlinks — do not edit them.
- The `patches/` directory contains a patch applied to the `llvm-project` dependency via `single_version_override` in MODULE.bazel.
