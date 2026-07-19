# AGENTS.md

This file provides guidance to AI coding agents (Claude Code, Kimi Code, Codex, etc.) when working with code in this repository.

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
bazel test //cpp_formatting:lint_lib_test
bazel test //cpp_formatting:lint_integration_test
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

**Lint mode (CI/CD):** all three binaries accept `--lint` (modify nothing, exit 1 when violations are found, 0 when clean) and `--format=<text|sarif|diff>` (default `text`; a non-default value implies `--lint`). `sarif` emits a SARIF 2.1.0 log; `diff` emits a git-apply-able unified patch. Output goes to stdout; `--lint`/`--format` cannot be combined with `--in-place`.
```
bazel run //cpp_formatting:normalize_variables -- --style=snake_case --scope=member --lint path/to/file.cpp -- -std=c++17
bazel run //cpp_formatting:cpp_format -- --config=cpp_format.yaml --format=sarif file.cpp -- -std=c++17
```

**Debug rename detection (`normalize_variables --debug-trace`):** prints per TU the rename map and every reference site found in the AST, with `main=Y/N`, `macro=Y/N`, and a `WILL_RENAME` marker on sites that would actually be rewritten. Useful for diagnosing why a particular use was missed (e.g. it lives in a file that wasn't passed in the source list).

### Bazel configuration (.bazelrc)

- All builds pass `--cxxopt=-fno-rtti` (required when linking against LLVM/Clang libraries built without RTTI).
- `bazel build --config=minify //...` produces size-optimized, fully statically linked binaries (LTO, section GC, stripped; requires `clang`/`clang++` on PATH).

### Code style

- C++ formatting is enforced by `clang-format` (Google style, see [.clang-format](.clang-format)) via the pre-commit hook in [.pre-commit-config.yaml](.pre-commit-config.yaml): `clang-format -i -style=file` on all `*.h`/`*.cpp` files.

## Project Structure

### Source files

All source files live under [cpp_formatting/](cpp_formatting/).

#### `trailing_return_types` — rewrite functions to trailing return syntax

- [cpp_formatting/trailing_return_types.cpp](cpp_formatting/trailing_return_types.cpp) — `main()`: CLI option parsing (including `--lint`/`--format`), drives `TrailingReturnActionFactory`.
- [cpp_formatting/trailing_return_types_lib.h](cpp_formatting/trailing_return_types_lib.h) — Public API:
  - `TrailingReturnCallback` — AST match callback that performs the rewrite (records lint diagnostics via `setLintReport()`)
  - `registerTrailingReturnMatchers()` — single authoritative place for the matcher predicate
  - `TrailingReturnTypesAction` — frontend action (dry-run, in-place, or lint via `OutputMode`)
  - `TrailingReturnActionFactory` — factory for `ClangTool::run()`; buffers per-file rewrites and lint diagnostics
  - `rewriteToTrailingReturnTypes()` — test helper that rewrites an in-memory string
- [cpp_formatting/trailing_return_types_lib.cpp](cpp_formatting/trailing_return_types_lib.cpp) — Implementation plus the private `CaptureAction` used by the test helper.

#### `normalize_variables` — rename variables and member functions to a consistent naming convention

- [cpp_formatting/normalize_variables.cpp](cpp_formatting/normalize_variables.cpp) — `main()`: CLI option parsing, builds a `FileSet` from source paths, drives `rename_variables_lib`.
- [cpp_formatting/rename_variables_lib.h](cpp_formatting/rename_variables_lib.h) — Public API:
  - `FileSet` — set of real absolute paths whose declarations the tool collects (enables cross-file renaming)
  - `VariableRenameCallback` — callback invoked once per canonical declaration
  - `VariableScope` — broad: `Member` | `Local` | `Global`; fine-grained: `StaticMember` | `ConstMember` | `StaticGlobal` | `ConstGlobal`; functions: `Method`
  - `RenameActionFactory` — `FrontendActionFactory` subclass that buffers every TU's edits in `PendingRewrites`; call `flush()` after `Tool.run()` returns to commit them (atomic disk writes for `InPlace`, formatted stdout for `DryRun`, no-op for `Debug`/`Lint`). `setLintReport()` attaches a `LintReport` for lint diagnostics; `rewrites()` exposes the buffered content (used by lint `diff` output).
  - Factory functions: `RenameAllMemberVariables`, `RenameAllLocalVariables`, `RenameAllGlobalVariables`, `RenameAllStaticMemberVariables`, `RenameAllConstMemberVariables`, `RenameAllStaticGlobalVariables`, `RenameAllConstGlobalVariables`, `RenameAllMemberFunctions`
  - `orderSourcesForRename()` — promotes header files to the end of the source list so every `.cpp` TU is parsed against the original on-disk header content
  - `rewriteVariableNames()` — test helper (single in-memory TU, returns the rewritten string)
- [cpp_formatting/rename_variables_lib.cpp](cpp_formatting/rename_variables_lib.cpp) — Implementation: two-pass `RecursiveASTVisitor` (collect declarations, then apply renames). A third visitor (`DebugTraceVisitor`) runs in `OutputMode::Debug` to print every reference site without modifying anything.
- [cpp_formatting/naming_convention.h](cpp_formatting/naming_convention.h) — `NamingStyle` enum, `splitIntoWords`, `formatName`, `renameToStyle`, `parseNamingStyle`.
- [cpp_formatting/naming_convention.cpp](cpp_formatting/naming_convention.cpp) — Splits camelCase/snake_case/prefixed names into word lists and reassembles in any target style.

#### `cpp_format` — combined tool with YAML config

- [cpp_formatting/cpp_format.cpp](cpp_formatting/cpp_format.cpp) — `main()`: parses CLI options or a YAML config file, then runs `normalize_variables` passes followed by `trailing_return_types`. Shares `rename_variables_lib` and `trailing_return_types_lib`. In lint mode all passes report into one shared `LintReport`, and each pass's buffered rewrites are overlaid onto the next pass's `ClangTool` via `mapVirtualFile()` so lint results match sequential in-place runs.

#### Lint support (CI/CD)

- [cpp_formatting/lint_lib.h](cpp_formatting/lint_lib.h) — `PendingRewrites` (path → rewritten content, shared by both rewrite libs), `LintDiagnostic`, `LintReport` (`emitText`/`emitSARIF`), `emitUnifiedDiff()` (Myers line diff, git-apply-able), `relativizeToCwd()`, `emitLintResults()` (shared by the three mains: emits the chosen format and returns the exit code).
- [cpp_formatting/lint_lib.cpp](cpp_formatting/lint_lib.cpp) — Implementation. JSON via `llvm/Support/JSON.h` (no new dependency). Lint diagnostics are recorded at the same choke points that perform rewrites (`ApplyRenamesVisitor::renameAt`, `TrailingReturnCallback::run`), so lint results exactly match what in-place mode would change.

#### Embedded Clang resource directory

All three binaries link the Clang built-in headers (`stddef.h`, `__stddef_max_align_t.h`, etc.) into the binary itself, so no system Clang installation is required at runtime.

- [cpp_formatting/embedded_clang_resource.h](cpp_formatting/embedded_clang_resource.h) — declares `ensureClangResourceDir()`.
- [cpp_formatting/embedded_clang_resource.cpp](cpp_formatting/embedded_clang_resource.cpp) — implementation: extracts the embedded `.tar.gz` to `$XDG_CACHE_HOME/cpp_formatting/clang_resource_<fnv1a-hash>/` on first call, returns the cached path on subsequent calls. Decompression (zlib, `@llvm_zlib//:zlib`) and tar parsing happen in-process — no system `tar`/`rm` or POSIX calls — and extraction goes via a uniquely-named temp directory + atomic `std::filesystem::rename` so concurrent invocations are safe.
- The `clang_include_headers` `pkg_tar` rule in [cpp_formatting/BUILD](cpp_formatting/BUILD) packages `@llvm-project//clang:builtin_headers_gen` into a `tar.gz`. The `clang_include_headers_embed_cc` `genrule` runs the `embed_file` host tool ([cpp_formatting/embed_file.cpp](cpp_formatting/embed_file.cpp)) on it to produce a `const unsigned char[]` translation unit linked into every binary. The `clang_include_headers` `strip_prefix` hardcodes the extension-generated canonical repo name `+llvm+llvm-project`; if the module extension name changes, update it too.

### Tests

- [cpp_formatting/trailing_return_types_test.cpp](cpp_formatting/trailing_return_types_test.cpp) — gtest unit tests for `trailing_return_types_lib`, including a `TrailingReturnTypesDeducingThis` suite covering C++23 explicit object parameters (P0847, run with `-std=c++23`).
- [cpp_formatting/integration_test.sh](cpp_formatting/integration_test.sh) — Shell integration tests for `trailing_return_types`:
  1. Dry-run on a single file — rewritten source goes to stdout.
  2. In-place on a single file — file is modified on disk.
  3. In-place on two files in one invocation — both files are modified.
  4. In-place on a file with system `#include`s — validates the embedded Clang resource directory works (no system Clang required).
- [cpp_formatting/naming_convention_test.cpp](cpp_formatting/naming_convention_test.cpp) — gtest unit tests for `naming_convention`: `splitIntoWords`, `formatName`, `renameToStyle`.
- [cpp_formatting/rename_variables_test.cpp](cpp_formatting/rename_variables_test.cpp) — gtest unit tests for `rename_variables_lib` (member, local, global, static data members, const members, static globals, const globals, member functions, templates, cross-file, constructor initializers, C++23 explicit object parameters / "deducing this").
- [cpp_formatting/normalize_variables_integration_test.sh](cpp_formatting/normalize_variables_integration_test.sh) — Shell integration tests for `normalize_variables`:
  1. Multi-file member rename — cross-file references, pointer-to-member, lambda, scope separation.
  2. Shadowed variable — global renamed, same-named local parameter unchanged.
  3. Source ordering — header passed in the middle of the source list; tool auto-promotes it to the end so every `.cpp` is parsed against the original header.
  4. Member function rename — virtual override hierarchy, out-of-line static definition, cross-file call sites; destructor, data members, and free functions unchanged.
- [cpp_formatting/lint_lib_test.cpp](cpp_formatting/lint_lib_test.cpp) — gtest unit tests for `lint_lib`: unified diff (hunks, context merging, missing trailing newline, empty inputs) and SARIF emission (parsed back with `llvm::json`).
- [cpp_formatting/lint_integration_test.sh](cpp_formatting/lint_integration_test.sh) — Shell integration tests for `--lint`/`--format` across all three binaries:
  1. Text lint — diagnostics on stdout, exit 1, file byte-identical.
  2. SARIF lint — valid 2.1.0 log with rule id and cwd-relative URI.
  3. Diff lint — emitted patch applies with `git apply` and matches the `--in-place` result.
  4. Clean file — exit 0, no diagnostics.
  5. `trailing_return_types` lint — text and diff round-trip.
  6. `cpp_format` lint — multi-pass run aggregates both rule ids into one SARIF report.

### Build files

- [cpp_formatting/BUILD](cpp_formatting/BUILD) — Defines all `cc_library`, `cc_binary`, `cc_test`, and `sh_test` targets, plus the `clang_include_headers` `pkg_tar` and the `clang_include_headers_embed_cc` `genrule` that embeds the headers into every binary.
- [MODULE.bazel](MODULE.bazel) — Bzlmod dependencies: `rules_cc`, `googletest 1.14.0.bcr.1`, `rules_shell`, `rules_pkg`, plus `bazel_skylib`, `platforms`, `rules_python`, `apple_support` (needed by the LLVM overlay's BUILD files). Clang/LLVM (19.1.7) is **not** a BCR dependency — the Bazel Central Registry only publishes llvm-project up to 17.0.4 — so it is built from source via a local module extension (see below).

#### Clang/LLVM from source ([third_party/llvm/](third_party/llvm/))

The BCR tops out at llvm-project 17.0.4, so to track a newer Clang (19.1.7, needed for C++23 features like deducing `this`) the project pulls LLVM's own Bazel overlay (`utils/bazel`) from the monorepo source instead of a BCR module.

- [third_party/llvm/extensions.bzl](third_party/llvm/extensions.bzl) — a Bzlmod `module_extension` (`llvm`) that `http_archive`s the pinned `llvm-project` source (`llvm-raw`), `llvm_zlib`, and `llvm_zstd`, then runs `llvm_configure` to generate the `@llvm-project` repo. Only the `X86` and `AArch64` targets are registered (the tools just need the host target for a `TargetInfo`, and the release workflow builds on both x86_64 and aarch64 runners), which keeps the from-source build small. Bump `LLVM_VERSION`/`LLVM_SHA256` here to change the Clang version.
- [third_party/llvm/configure.bzl](third_party/llvm/configure.bzl) — vendored copy of the upstream overlay rule. One local change vs. upstream: `_extract_cmake_settings` resolves the CMake files by a plain repo-root-relative path instead of `Label("//:...")`, which is a self-reference that fails to package-load under Bazel 8 + Bzlmod while the repo is still being fetched.
- [MODULE.bazel](MODULE.bazel) — wires it up via `use_extension` + `use_repo(llvm, "llvm-project", ...)`.

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

Supported scopes: `member`, `local`, `global`, `static_member`, `const_member`, `static_global`, `const_global`, `method`.

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
- **Designated initializers** — `S s{.val_ = 0}` stores the field name in the `Designator` of a `DesignatedInitExpr`, not a `MemberExpr`. `VisitDesignatedInitExpr` rewrites field designators at `getFieldLoc()`.
- **Template instantiation** — `FieldDecl` instances in template specializations are mapped back to the primary-template field by index walk through `ClassTemplateSpecializationDecl`. For member functions, `primaryTemplateMethod()` walks `getInstantiatedFromMemberFunction()` instead.
- **Member-function scope (`Method`)** — constructors, destructors, conversion functions, and overloaded operators are never renamed (`isRenamableMethod()`); their names are not plain identifiers. A virtual function is renamed together with its entire override hierarchy (`collectOverrideFamily()`); if any function in the hierarchy is declared outside the `FileSet`, the rename is skipped entirely so `override` checking can never be broken.
- **Shadowed variables** — `matchesScope()` filters by scope: a parameter with the same name as a global is not collected when renaming globals.
- **Per-file-content cache key** — the embedded Clang resource directory is extracted under a directory whose name includes the FNV-1a hash of the embedded `.tar.gz`. If the embedded headers change (e.g. after an LLVM upgrade) a fresh cache directory is created automatically.

## Notes

- The `bazel-*` symlinks in the root are Bazel output/convenience symlinks — do not edit them.
- The `patches/` directory is unused since the LLVM 19 upgrade (the old `<cstdint>` patch is obsolete — upstream `SmallVector.h` now includes `<cstdint>`). Clang/LLVM is built from source via the module extension in [third_party/llvm/](third_party/llvm/); see the "Clang/LLVM from source" section above.
