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
bazel test //cpp_formatting:aggregate_integration_test
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
- [cpp_formatting/rename_variables_lib.cpp](cpp_formatting/rename_variables_lib.cpp) — Implementation: two-pass `RecursiveASTVisitor` (collect declarations, then apply renames), plus `DependentTokenCollector` + `RecordDependentResolutionsVisitor` (the latter walks template instantiations) that together resolve template-dependent member tokens across TUs. A separate `DebugTraceVisitor` runs in `OutputMode::Debug` to print every reference site without modifying anything.
- [cpp_formatting/naming_convention.h](cpp_formatting/naming_convention.h) — `NamingStyle` enum, `splitIntoWords`, `formatName`, `renameToStyle`, `parseNamingStyle`.
- [cpp_formatting/naming_convention.cpp](cpp_formatting/naming_convention.cpp) — Splits camelCase/snake_case/prefixed names into word lists and reassembles in any target style.

#### `cpp_format` — combined tool with YAML config

- [cpp_formatting/cpp_format.cpp](cpp_formatting/cpp_format.cpp) — `main()`: parses CLI options or a YAML config file, then runs every `normalize_variables` rule plus `trailing_return_types` in a **single** `ClangTool` pass via `CppFormatActionFactory` (each TU is parsed exactly once, regardless of how many rules are configured). Also hosts the `--emit-edits=<file>` mode (writes per-TU edit records for Bazel aggregation) and the `--aggregate` mode (merges those records; dispatched before `CommonOptionsParser`, delegates to `lint_lib`'s `runEditAggregation`).
- [cpp_formatting/cpp_format_lib.h](cpp_formatting/cpp_format_lib.h) — `NormalizeRule` (scope + rename callback + lint rule id) and `CppFormatActionFactory`: buffers rewritten content in `PendingRewrites` like `RenameActionFactory` and commits it via `flush()` after `ClangTool::run()`.
- [cpp_formatting/cpp_format_lib.cpp](cpp_formatting/cpp_format_lib.cpp) — Implementation: per TU, `runRenameRuleOnAST()` (from `rename_variables_lib`) runs each rule's collect+apply visitors, then the trailing-return `MatchFinder` runs via `matchAST()` on the same AST, all sharing one `Rewriter`. Return-type text is extracted via `Rewriter::getRewrittenText()`, so a rename that landed inside a return type (e.g. a member in `decltype(count_)`) is carried into the moved `-> decltype(m_count)` text instead of being clobbered by the wholesale `auto` replacement.

#### Lint support (CI/CD)

- [cpp_formatting/lint_lib.h](cpp_formatting/lint_lib.h) — `PendingRewrites` (path → rewritten content, shared by both rewrite libs), `LintDiagnostic`, `LintReport` (`emitText`/`emitSARIF`), `emitUnifiedDiff()` (Myers line diff, git-apply-able), `relativizeToCwd()`, `emitLintResults()` (shared by the three mains: emits the chosen format and returns the exit code).
- [cpp_formatting/lint_lib.cpp](cpp_formatting/lint_lib.cpp) — Implementation. JSON via `llvm/Support/JSON.h` (no new dependency). Lint diagnostics are recorded at the same choke points that perform rewrites (`ApplyRenamesVisitor::renameAt`, `TrailingReturnCallback::run`), so lint results exactly match what in-place mode would change. Also implements the edit-record model (`EditReport`/`parseEditReport`/`mergeEditReports`/`aggregateEdits`) and `runEditAggregation()` — the shared CLI entry point behind both `aggregate_edits` and `cpp_format --aggregate`.

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
- [cpp_formatting/rename_variables_test.cpp](cpp_formatting/rename_variables_test.cpp) — gtest unit tests for `rename_variables_lib` (member, local, global, static data members, const members, static globals, const globals, member functions, templates, template-dependent member tokens resolved through instantiations, cross-file, constructor initializers, C++23 explicit object parameters / "deducing this").
- [cpp_formatting/normalize_variables_integration_test.sh](cpp_formatting/normalize_variables_integration_test.sh) — Shell integration tests for `normalize_variables`:
  1. Multi-file member rename — cross-file references, pointer-to-member, lambda, scope separation.
  2. Shadowed variable — global renamed, same-named local parameter unchanged.
  3. Source ordering — header passed in the middle of the source list; tool auto-promotes it to the end so every `.cpp` is parsed against the original header.
  4. Member function rename — virtual override hierarchy, out-of-line static definition, cross-file call sites; destructor, data members, and free functions unchanged.
  5. Template-dependent member token — `set_val`'s `x.val` in a header is rewritten from the resolution recorded while the instantiating `.cpp` files are processed (self-contained fixtures generated inline).
  6. Out-of-scope veto — the same template instantiated with a type outside the passed source set leaves the shared header token untouched while still renaming the owned type.
- [cpp_formatting/lint_lib_test.cpp](cpp_formatting/lint_lib_test.cpp) — gtest unit tests for `lint_lib`: unified diff (hunks, context merging, missing trailing newline, empty inputs) and SARIF emission (parsed back with `llvm::json`).
- [cpp_formatting/lint_integration_test.sh](cpp_formatting/lint_integration_test.sh) — Shell integration tests for `--lint`/`--format` across all three binaries:
  1. Text lint — diagnostics on stdout, exit 1, file byte-identical.
  2. SARIF lint — valid 2.1.0 log with rule id and cwd-relative URI.
  3. Diff lint — emitted patch applies with `git apply` and matches the `--in-place` result.
  4. Clean file — exit 0, no diagnostics.
  5. `trailing_return_types` lint — text and diff round-trip.
  6. `cpp_format` lint — multi-pass run aggregates both rule ids into one SARIF report.
- [cpp_formatting/aggregate_integration_test.sh](cpp_formatting/aggregate_integration_test.sh) — Shell integration tests for the per-TU emit + aggregate pipeline (`cpp_format --emit-edits` then `cpp_format --aggregate`), on a two-file fixture whose header holds a template-dependent member token resolved from the instantiating `.cpp`:
  1. `--aggregate` diff is byte-identical to the standalone `aggregate_edits` binary and rewrites both the member decl and the cross-TU dependent token.
  2. `--aggregate --check` exits 1 with per-file edit counts.
  3. `--aggregate --apply` rewrites the files on disk; re-emitting against the fixed sources and re-checking exits 0.

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

## Bazel integration ([bazel/cpp_format.bzl](bazel/cpp_format.bzl))

Runs `cpp_format` from the build graph so compile info and the full header set come from Bazel, not a committed `compile_commands.json`. This solves the sandboxing problem where one target's TU can't reach headers owned by other targets.

**How it works** — a per-target **aspect** (`cpp_format_aspect`) derives each `cc_*` target's compile flags from `CcInfo.compilation_context` + the toolchain (`cc_common.get_memory_inefficient_command_line(CPP_COMPILE_ACTION_NAME)`), and runs `cpp_format --config=cpp_format.yaml --emit-edits=<target>.cpp_format.json <srcs+hdrs> -- -x c++ <flags> -resource-dir=<staged builtin headers>`. It declares the target's sources **plus `compilation_context.headers` (transitive)** and `cc_toolchain.all_files` as action inputs — that declaration is what makes headers reachable under sandboxing. Each action is parallel and cached; first-party targets only (guarded by `ctx.label.workspace_name == ""`).

**Records, not diffs** — the emit action writes offset-level edit records (`{file, offset, length, old, new}`) plus a template-dependent-token resolution sidecar (see "template-dependent member tokens"). Textual diffs can't be merged (hunk offsets don't compose); records can. Aggregation unions all targets' records, resolves dependent tokens across TUs (agree → edit, veto/disagree → dropped), merges per file (dedup identical, flag overlapping-distinct conflicts), and renders/applies via `emitUnifiedDiff`. This is the clang-tidy `--export-fixes` + `clang-apply-replacements` model. The rename↔trailing-return overlap (a member renamed inside a `decltype(...)` return type that trailing-return also rewrites) is handled in [trailing_return_types_lib.cpp](cpp_formatting/trailing_return_types_lib.cpp): the trailing-return edit subsumes (drops) rename records inside its range, since its `-> type` text already carries the rename. The aggregation logic lives in one place — `runEditAggregation()` in [lint_lib.cpp](cpp_formatting/lint_lib.cpp) — exposed by two thin CLIs: the standalone [aggregate_edits](cpp_formatting/aggregate_edits.cpp) binary (used by the from-source aspect) and `cpp_format --aggregate` (so the single published binary is self-sufficient — see the prebuilt kit below). `cpp_format --aggregate` is dispatched before `CommonOptionsParser` in [cpp_format.cpp](cpp_formatting/cpp_format.cpp) and hand-parses `--apply`/`--check`/`--root` (it has no source paths or `--` compile args).

**Rules** — `cpp_format_targets(name, deps)` generates three targets (see [bazel/testdata/BUILD.bazel](bazel/testdata/BUILD.bazel) for the demo):
- `<name>.check` — a **test** (hermetic lint gate): `mergeEditReports` counts edits without reading sources; exit 1 if any. `bazel test //…:<name>.check`.
- `<name>.diff` — `bazel run` prints the merged git-apply-able unified diff (review artifact).
- `<name>.fix` — `bazel run` applies edits in `$BUILD_WORKSPACE_DIRECTORY` (outside the action graph, since Bazel actions can't mutate sources).

`--config=lint` in [.bazelrc](.bazelrc) runs the aspect and materializes each target's records (`bazel build --config=lint //…`) for inspection/CI; the failing gate is the `.check` test.

**Non-obvious behaviours** — record file keys are the source's **real path**, which under Bazel resolves to the absolute workspace path (source symlinks) — stable across actions/sandboxes; `aggregate_edits` uses absolute keys directly and joins relative keys with `--root`. `-x c++` is forced so headers parse as C++ (not C). `-resource-dir` is derived from the staged `@llvm-project//clang:builtin_headers_gen` paths (robust to a module-extension rename). The aspect needs `cpp_format` in the **exec** configuration (a one-time from-source Clang/LLVM build). Portability caveat: absolute paths in records make remote-cache reuse across machines suboptimal.

### Prebuilt-binary integration kit ([bazel/integration/](bazel/integration/))

A self-contained, **vendorable** variant of the integration for external repos that want the lint/fix gate **without** building Clang/LLVM from source. It is documented for end users in the README quick start. Files, copied wholesale into a consumer repo (e.g. `third_party/cpp_format/`):

- [bazel/integration/cpp_format.sh](bazel/integration/cpp_format.sh) — the **ergonomic entry point**: `cpp_format.sh <check|diff|fix> [pattern]` (pattern defaults to `//...`). It `bazel query`s the first-party `cc_*` targets under the pattern (`except attr(tags, 'no-cpp-format', …)`), `bazel build`s them with `--aspects=…%cpp_format_aspect --output_groups=+cpp_format_edits` to emit each target's record file, derives the record paths deterministically (`//pkg:name` → `<bazel-bin>/pkg/name.cpp_format.json`, kept only if it exists — source-less targets emit nothing), and runs `cpp_format --aggregate` over them. This is what lets users format the **whole repo with no per-target wiring**. It is a plain script, **not** a `bazel run` target, so it can invoke `bazel build` without nesting a Bazel server in a running one (which would deadlock on the workspace lock). `ASPECT`/`BIN_LABEL` are overridable via env (`CPP_FORMAT_ASPECT`/`CPP_FORMAT_BIN_LABEL`) so the same script drives the in-repo from-source labels for dogfooding. The query excludes `cpp_format_targets` rule targets, so the aspect is applied to each source target exactly once (two applications collide on the shared record file).
- [bazel/integration/extensions.bzl](bazel/integration/extensions.bzl) — a Bzlmod `module_extension` (`cpp_format`) with a `release(version, base_url, sha256)` tag class. Its repo rule detects the host OS/arch (`rctx.os`), downloads the matching release asset (`cpp_format-{linux-x86_64,linux-aarch64,darwin-aarch64,windows-x86_64.exe}`) into a `bin/` subdir, and exposes it as `@cpp_format_bin//:cpp_format`. **The download target file must not share the `cpp_format` filegroup's name** — a same-name `src` is a self-edge cycle (hence `bin/`).
- [bazel/integration/cpp_format.bzl](bazel/integration/cpp_format.bzl) — the same aspect + `cpp_format_targets` as [bazel/cpp_format.bzl](bazel/cpp_format.bzl), with three differences: it runs the prebuilt binary as a plain `File` (`ctx.file._cpp_format`, `executable=`/`tools=`, no `cfg="exec"` build); it stages **no** `@llvm-project` builtin headers and passes **no** `-resource-dir` (the published binary self-extracts its embedded Clang headers via `ensureClangResourceDir()` — verified to work even with a cleared env, falling back to `TMPDIR`); and the aggregator rules exec `cpp_format --aggregate` instead of the separate `aggregate_edits` binary. `_rlocation_path()` derives each runfiles key from `File.short_path` (`../<canonical>/…` for the external binary → strip `../`; main-repo records → `_main/…`), so it works for both the external binary and generated records.

`cpp_format.sh` (whole-repo/pattern, query-driven) and `cpp_format_targets` (a pinned `bazel test` gate over an explicit dep set) are the two entry points — use one or the other, never the wrapper over a pattern that also has `cpp_format_targets` defined on it. The in-repo [bazel/cpp_format.bzl](bazel/cpp_format.bzl) (from-source, staged headers) remains the path used by this repo's own `//bazel/testdata` demo and tests; the kit is the copy external consumers use.

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

### `normalize_variables`: template-dependent member tokens

A member accessed through a template parameter — e.g. `x.val` in `auto set_val(auto& x) { x.val = 12; }` (or the explicit `template <class T> void set_val(T& x)`) — is a **dependent** expression (`CXXDependentScopeMemberExpr`): which member `val` names is unknown until the template is instantiated, and the instantiations usually live in the `.cpp` files, not in the header that spells the token. The per-main-file `Rewriter` model can't rename it on its own — the header's own TU never instantiates the template, and an instantiating `.cpp`'s TU doesn't rewrite the header.

The tool bridges this with a **cross-TU resolution map** (`DependentResolutions` in `rename_variables_lib.h`), keyed by the token's `(real path, byte offset)` and threaded through every TU of one `ClangTool::run()` (the factories own it — `RenameActionFactory::DepRes`, and one **per rule** in `CppFormatActionFactory::DepResPerRule` so a token resolved by one rule is never re-applied by another). Per TU, `runRenameRuleOnAST` does three things:

1. `DependentTokenCollector` (cheap, no instantiations) finds dependent-member token locations in owned files. If there are none, the rest is skipped — the feature is zero-cost for non-template code.
2. `RecordDependentResolutionsVisitor` (`shouldVisitTemplateInstantiations() == true`) walks this TU's instantiations and, for each resolved member access landing on a known dependent-token location, records the new name it resolves to. A binding to a member that is **not** being renamed (e.g. a type outside the `FileSet`) or a disagreement between instantiations **vetoes** the location.
3. `ApplyRenamesVisitor::VisitCXXDependentScopeMemberExpr` rewrites the token in its **own** main-file TU using the agreed name (skipping vetoed/unresolved entries).

Because `orderSourcesForRename()` puts headers last, every instantiating `.cpp` TU runs before the header TU that consumes its resolutions. `ApplyRenamesVisitor` itself keeps `shouldVisitTemplateInstantiations() == false`, so all non-dependent paths are unchanged — the feature is purely additive. **Soundness is bounded by the passed source set:** an instantiation in a TU that is *not* passed to the tool is invisible, so its member may be renamed while its dependent use is missed; conversely a visible out-of-scope binding is vetoed conservatively (leaving the token), which can produce an incomplete rename. Both cases surface as a compile error on the next build — the intended review gate — rather than a silent miscompile. Full all-or-nothing propagation (skipping a member's declaration rename when a dependent use can't be safely rewritten) is a possible future hardening.

### Known non-obvious behaviours

- **`QualifiedTypeLoc` gap** — Clang's `QualifiedTypeLoc` does not include leading `const`/`volatile`/`restrict` in its source range. `skipQualifiersBackward()` scans the raw source buffer leftward.
- **Token merging guard** — When there is no whitespace between the return type and the function name (e.g. `Foo&operator=`), `"auto "` (with a trailing space) is emitted to prevent token merging.
- **`FunctionTypeLoc::getLocalRangeEnd()`** — For member functions with cv/ref/noexcept qualifiers, Clang sets this to the location of the last qualifier.
- **Pointer-to-member** — `&S::field` produces a `DeclRefExpr` with `FieldDecl` (not `VarDecl`). `VisitDeclRefExpr` handles both cases.
- **Constructor mem-initializers** — `S() : val_(0) {}` is a `CXXCtorInitializer`, not a `Stmt` or `Decl`, so it is not visited by the standard `Visit*` callbacks. `ApplyRenamesVisitor` overrides `TraverseConstructorInitializer` and rewrites at `getMemberLocation()`.
- **Designated initializers** — `S s{.val_ = 0}` stores the field name in the `Designator` of a `DesignatedInitExpr`, not a `MemberExpr`. `VisitDesignatedInitExpr` rewrites field designators at `getFieldLoc()`.
- **Template instantiation** — `FieldDecl` instances in template specializations are mapped back to the primary-template field by index walk through `ClassTemplateSpecializationDecl`. For member functions, `primaryTemplateMethod()` walks `getInstantiatedFromMemberFunction()` instead.
- **Template-dependent member tokens** — `x.val` where `x` is a template parameter is a `CXXDependentScopeMemberExpr` with no resolved member; it is renamed via the cross-TU `DependentResolutions` map populated from instantiations (see "template-dependent member tokens" above), not by the ordinary `Visit*` paths. Only the `member`/`method` scopes are affected. `ApplyRenamesVisitor` deliberately does **not** visit template instantiations, so resolved member accesses inside instantiations are never double-rewritten (their source locations point back into the pattern, which is rewritten once via the dependent-token path).
- **Member-function scope (`Method`)** — constructors, destructors, conversion functions, and overloaded operators are never renamed (`isRenamableMethod()`); their names are not plain identifiers. A virtual function is renamed together with its entire override hierarchy (`collectOverrideFamily()`); if any function in the hierarchy is declared outside the `FileSet`, the rename is skipped entirely so `override` checking can never be broken.
- **Shadowed variables** — `matchesScope()` filters by scope: a parameter with the same name as a global is not collected when renaming globals.
- **Per-file-content cache key** — the embedded Clang resource directory is extracted under a directory whose name includes the FNV-1a hash of the embedded `.tar.gz`. If the embedded headers change (e.g. after an LLVM upgrade) a fresh cache directory is created automatically.

## Notes

- The `bazel-*` symlinks in the root are Bazel output/convenience symlinks — do not edit them.
- The `patches/` directory is unused since the LLVM 19 upgrade (the old `<cstdint>` patch is obsolete — upstream `SmallVector.h` now includes `<cstdint>`). Clang/LLVM is built from source via the module extension in [third_party/llvm/](third_party/llvm/); see the "Clang/LLVM from source" section above.
