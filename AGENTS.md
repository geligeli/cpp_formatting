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

#### `trailing_return_types` — move return types between leading and trailing syntax

- [cpp_formatting/trailing_return_types.cpp](cpp_formatting/trailing_return_types.cpp) — `main()`: CLI option parsing (including `--reverse` and `--lint`/`--format`), drives `TrailingReturnActionFactory`.
- [cpp_formatting/trailing_return_types_lib.h](cpp_formatting/trailing_return_types_lib.h) — Public API:
  - `ReturnTypeStyle` — `Trailing` (`int f()` → `auto f() -> int`) | `Leading` (the reverse). One value rather than two booleans, so "both directions at once" is unrepresentable.
  - `TrailingReturnCallback` — AST match callback that performs the rewrite in either direction (records lint diagnostics via `setLintReport()`, edit records via `setEmitReport()`)
  - `registerTrailingReturnMatchers()` — single authoritative place for both matcher predicates
  - `TrailingReturnTypesAction` — frontend action (dry-run, in-place, or lint via `OutputMode`)
  - `TrailingReturnActionFactory` — factory for `ClangTool::run()`; buffers per-file rewrites and lint diagnostics
  - `rewriteToTrailingReturnTypes()` / `rewriteToLeadingReturnTypes()` — test helpers that rewrite an in-memory string
- [cpp_formatting/trailing_return_types_lib.cpp](cpp_formatting/trailing_return_types_lib.cpp) — Implementation: `runToTrailing()` and `runToLeading()` behind one `run()`, the guard helpers the Leading direction needs (`isMovableTypeShape`, `rangeMentionsName`, `LookupEscapeChecker`), plus the private `CaptureAction` used by the test helpers.

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

- [cpp_formatting/cpp_format.cpp](cpp_formatting/cpp_format.cpp) — `main()`: parses CLI options or a YAML config file, then runs every `normalize_variables` rule plus `trailing_return_types` in a **single** `ClangTool` pass via `CppFormatActionFactory` (each TU is parsed exactly once, regardless of how many rules are configured). Also hosts the `--emit-edits=<file>` mode (writes per-TU edit records for Bazel aggregation), the `--owned-files=<list>` mode (newline-separated paths added to the `FileSet` but *not* to the source list, so a dependency's declarations are renameable at their use sites here without being re-parsed — see the Bazel integration below), and the `--aggregate` mode (merges those records; dispatched before `CommonOptionsParser`, delegates to `lint_lib`'s `runEditAggregation`).
- [cpp_formatting/cpp_format_lib.h](cpp_formatting/cpp_format_lib.h) — `NormalizeRule` (scope + rename callback + lint rule id) and `CppFormatActionFactory`: buffers rewritten content in `PendingRewrites` like `RenameActionFactory` and commits it via `flush()` after `ClangTool::run()`. The return-type pass is carried as a `std::optional<ReturnTypeStyle>` — `nullopt` skips it.
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

- [cpp_formatting/trailing_return_types_test.cpp](cpp_formatting/trailing_return_types_test.cpp) — gtest unit tests for `trailing_return_types_lib`, including a `TrailingReturnTypesDeducingThis` suite covering C++23 explicit object parameters (P0847, run with `-std=c++23`) and a `LeadingReturnTypes` suite for the reverse direction (every guard gets a "left unchanged" case, plus a leading→trailing→leading round trip and a fixpoint check).
- [cpp_formatting/integration_test.sh](cpp_formatting/integration_test.sh) — Shell integration tests for `trailing_return_types`:
  1. Dry-run on a single file — rewritten source goes to stdout.
  2. In-place on a single file — file is modified on disk.
  3. In-place on two files in one invocation — both files are modified.
  4. In-place on a file with system `#include`s — validates the embedded Clang resource directory works (no system Clang required).
  5. In-place `--reverse` — the Leading direction reaches disk through the same `overwriteChangedFiles()` path, and a second pass over its own output changes nothing.
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
  7. `--reverse` lint — `leading_return_types` diagnostics, a diff that `git apply`s to the expected leading form, a second pass that reports nothing (fixpoint), the same direction through `cpp_format --return-types=leading`, and the error when both directions are requested at once.
- [cpp_formatting/aggregate_integration_test.sh](cpp_formatting/aggregate_integration_test.sh) — Shell integration tests for the per-TU emit + aggregate pipeline (`cpp_format --emit-edits` then `cpp_format --aggregate`), on a two-file fixture whose header holds a template-dependent member token resolved from the instantiating `.cpp`:
  1. `--aggregate` diff is byte-identical to the standalone `aggregate_edits` binary and rewrites both the member decl and the cross-TU dependent token.
  2. `--aggregate --check` exits 1 with per-file edit counts.
  3. `--aggregate --apply` rewrites the files on disk; re-emitting against the fixed sources and re-checking exits 0.
  4. `--owned-files` (the cross-target rename path): a second fixture emitted the way the aspect does it — one invocation per "target" — asserts that a dependent parsing only its own source emits nothing for a dep's member, emits the use-site edit once the dep's header is passed as owned, and still emits no edits for the dep's own files.

### End-to-end corpus tests ([e2e/](e2e/))

Not part of `bazel test //...`, and **not a Bazel target** — `e2e/run_e2e.sh`
drives `bazel` inside a *second* workspace, which a Bazel action cannot do
(nesting a server inside a running one deadlocks on the workspace lock, the same
constraint that keeps `bazel/integration/cpp_format.sh` a plain script). CI runs
it nightly / on dispatch via [.github/workflows/e2e.yml](.github/workflows/e2e.yml).

Each scenario pairs a pinned repo ([e2e/repos/](e2e/repos/)) with a ruleset
([e2e/rulesets/](e2e/rulesets/)) and runs twelve phases: build the unmodified
repo, run the transform over all of it, build it again. The load-bearing
assertions are `rebuild` (still compiles — a missed reference surfaces here),
`check` (edits > 0, so a vacuous ruleset cannot pass), `applied` (the files that
changed on disk are exactly those reported — `flush()` writes with
`std::ofstream` and never checks the failbit, so an unwritable file is otherwise
skipped with exit 0), and `converge` (the transform is a fixpoint).

**The target repo must be the root module of its own Bazel invocation** — the
aspect emits nothing for `workspace_name != ""`, so a repo pulled in as a
`bazel_dep` can never be formatted. The harness therefore materializes the repo
into a work dir, vendors `bazel/integration/` into it as
`third_party/cpp_format/`, and injects the locally built binary through the
kit's own `release()` tag class with a `file://` base URL. The staged `version`
string encodes the binary's sha256 — repo attrs are the refetch key, so a stable
version would silently test a stale binary.

Scenarios declare their expected outcome; an unexpected **pass** fails as XPASS,
so a fixed limitation gets reported rather than absorbed.
`macro_repo-member_snake_case` is a deterministic, seconds-long reproduction of
the macro limitation (a member referenced only from inside a macro body).

**Two-pass scenarios (`RULESET_THEN`).** A scenario may name a second ruleset,
which runs over the *output* of the first: `swap` commits pass 1 (so `applied`
keeps measuring only what the current pass wrote) and drops the second ruleset
in, then `check2 … converge2` reuse the same phase bodies. This is how the two
return-type directions are tested against each other —
`mini_repo-return_types_roundtrip` and `googletest-return_types_roundtrip` run
trailing, rebuild, then leading, and rebuild again. The optional
`EXPECT_ROUNDTRIP_IDENTICAL=1` adds a final `roundtrip` phase asserting the
sources came back byte-identical to the pre-transform tree (diffed against the
`e2e-wired` tag, excluding the ruleset itself). Only declare it where the
second ruleset really does undo the first: the Leading direction's guards are
strictly tighter than the Trailing ones, so on a large corpus some
declarations are expected to stay in trailing form, and there `rebuild2` and
`converge2` are the assertions that matter.

Each scenario gets its **own** Bazel disk cache: edit records hold absolute
paths, and the devcontainer sets a machine-wide `--disk_cache`, so two
workspaces sharing one can restore each other's records. [.bazelignore](.bazelignore)
keeps `e2e/testdata` (self-contained workspaces) and `.e2e` out of `//...`.

### Build files

- [cpp_formatting/BUILD](cpp_formatting/BUILD) — Defines all `cc_library`, `cc_binary`, `cc_test`, and `sh_test` targets, plus the `clang_include_headers` `pkg_tar` and the `clang_include_headers_embed_cc` `genrule` that embeds the headers into every binary.
- [MODULE.bazel](MODULE.bazel) — Bzlmod dependencies: `rules_cc`, `googletest 1.14.0.bcr.1`, `rules_shell`, `rules_pkg`, plus `bazel_skylib`, `platforms`, `rules_python`, `apple_support` (needed by the LLVM overlay's BUILD files). Clang/LLVM (21.1.8) is **not** a BCR dependency — the Bazel Central Registry only publishes llvm-project up to 17.0.4 — so it is built from source via a local module extension (see below).

#### Clang/LLVM from source ([third_party/llvm/](third_party/llvm/))

The BCR tops out at llvm-project 17.0.4, so to track a newer Clang (21.1.8, needed for C++23 features like deducing `this`) the project pulls LLVM's own Bazel overlay (`utils/bazel`) from the monorepo source instead of a BCR module.

**Why 21.1.8 and not something older.** 19.1.7 aborts on abseil's `absl/log/log_format_test.cc` with an assertion inside Clang's own Sema (`ExprClassification.cpp`, `` `isLValue()` ``), reached while instantiating a local variable whose initializer needs a user-defined conversion. It is Clang's bug, not ours — it reproduces with an empty ruleset, so with none of this project's visitors or matchers running — and it took abseil out of the e2e corpus entirely. Only assertion-enabled builds hit it (`-c opt` compiles the assertion out), which is why released binaries were never affected but the fastbuild dev loop was.

- [third_party/llvm/extensions.bzl](third_party/llvm/extensions.bzl) — a Bzlmod `module_extension` (`llvm`) that `http_archive`s the pinned `llvm-project` source (`llvm-raw`), `llvm_zlib`, and `llvm_zstd`, then runs `llvm_configure` to generate the `@llvm-project` repo. Only the `X86` and `AArch64` targets are registered (the tools just need the host target for a `TargetInfo`, and the release workflow builds on both x86_64 and aarch64 runners), which keeps the from-source build small. Bump `LLVM_VERSION`/`LLVM_SHA256` here to change the Clang version.
- [third_party/llvm/configure.bzl](third_party/llvm/configure.bzl) — vendored copy of the upstream overlay rule. One local change vs. upstream: `_extract_cmake_settings` resolves the CMake files by a plain repo-root-relative path instead of `Label("//:...")`, which is a self-reference that fails to package-load under Bazel 8 + Bzlmod while the repo is still being fetched.
- [MODULE.bazel](MODULE.bazel) — wires it up via `use_extension` + `use_repo(llvm, "llvm-project", ...)`.

## YAML Config Format (`cpp_format`)

`cpp_format` reads a YAML file (via `--config=<file>`) that specifies which passes to run:

```yaml
# All fields are optional; omit any section to skip that pass.

# Return type style: `trailing` rewrites `int f()` to `auto f() -> int`;
# `leading` rewrites it back.  `trailing_return_types: true` is the original
# spelling of `return_types: trailing` and still works, but the two keys cannot
# both be given.
return_types: trailing

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

Supported return-type styles: `trailing`, `leading`.

Supported styles: `snake_case`, `_leading`, `trailing_`, `m_prefix`, `camelCase`, `UpperCamelCase`, `UPPER_SNAKE_CASE`, `kConstant`.

## Bazel integration ([bazel/cpp_format.bzl](bazel/cpp_format.bzl))

Runs `cpp_format` from the build graph so compile info and the full header set come from Bazel, not a committed `compile_commands.json`. This solves the sandboxing problem where one target's TU can't reach headers owned by other targets.

**How it works** — a per-target **aspect** (`cpp_format_aspect`) derives each `cc_*` target's compile flags from `CcInfo.compilation_context` + the toolchain (`cc_common.get_memory_inefficient_command_line(CPP_COMPILE_ACTION_NAME)`), and runs `cpp_format --config=cpp_format.yaml --emit-edits=<target>.cpp_format.json <srcs+hdrs> --owned-files=<param file> -- -x c++ <flags> -resource-dir=<staged builtin headers>`. It declares the target's sources **plus `compilation_context.headers` (transitive)** and `cc_toolchain.all_files` as action inputs — that declaration is what makes headers reachable under sandboxing. Each action is parallel and cached; first-party targets only (guarded by `ctx.label.workspace_name == ""`).

**Cross-target renames (`--owned-files`)** — the tool's `FileSet` decides which *declarations* may be renamed, while the source list decides which files are *parsed as TUs*. If a target's action only knew its own sources, a `cc_binary`'s use of a `cc_library` member would be invisible to the rename (the member's declaration lives in the dep's header, outside the FileSet) — the member would be renamed by the library's own action and every use site in dependents left behind, i.e. a build break. So `CppFormatEditsInfo` carries a second field, `headers` (the dep closure's first-party header sources, propagated over `deps` alongside `records`), and the aspect passes it as `--owned-files`. That flag widens the FileSet **without** widening the source list, which is what keeps the model sound: `ApplyRenamesVisitor` only rewrites its own main file, so each file is still rewritten by exactly one action (the target that lists it in `srcs`/`hdrs`) — no duplicate or conflicting records — while *uses* are rewritten wherever they appear. It also lets a dependent resolve a template-dependent token in a dep's header from its own instantiations. Only headers are propagated (a dep's `.cpp` is unreachable from this TU), and a `no-cpp-format` target contributes none — its declarations are never renamed, so a dependent must not rename their uses. The flag goes through a param file (`use_param_file`), since a dep closure can be large enough to blow the command-line limit, and it must precede the `--` or it would be parsed as a compile flag. **The aspect and the binary are versioned together**: an older published binary rejects `--owned-files`, so bumping `cpp_format.release(version=…)` in [MODULE.bazel](MODULE.bazel) is part of shipping an aspect change like this.

**Records, not diffs** — the emit action writes offset-level edit records (`{file, offset, length, old, new}`) plus a template-dependent-token resolution sidecar (see "template-dependent member tokens"). Textual diffs can't be merged (hunk offsets don't compose); records can. Aggregation unions all targets' records, resolves dependent tokens across TUs (agree → edit, veto/disagree → dropped), merges per file (dedup identical, flag overlapping-distinct conflicts), and renders/applies via `emitUnifiedDiff`. This is the clang-tidy `--export-fixes` + `clang-apply-replacements` model. The rename↔trailing-return overlap (a member renamed inside a `decltype(...)` return type that trailing-return also rewrites) is handled in [trailing_return_types_lib.cpp](cpp_formatting/trailing_return_types_lib.cpp): the trailing-return edit subsumes (drops) rename records inside its range, since its `-> type` text already carries the rename. The aggregation logic lives in one place — `runEditAggregation()` in [lint_lib.cpp](cpp_formatting/lint_lib.cpp) — exposed by two thin CLIs: the standalone [aggregate_edits](cpp_formatting/aggregate_edits.cpp) binary (used by the from-source aspect) and `cpp_format --aggregate` (so the single published binary is self-sufficient — see the prebuilt kit below). `cpp_format --aggregate` is dispatched before `CommonOptionsParser` in [cpp_format.cpp](cpp_formatting/cpp_format.cpp) and hand-parses `--apply`/`--check`/`--root` (it has no source paths or `--` compile args).

**Rules** — `cpp_format_targets(name, deps)` generates three targets (see [bazel/testdata/BUILD.bazel](bazel/testdata/BUILD.bazel) for the demo):
- `<name>.check` — a **test** (hermetic lint gate): `mergeEditReports` counts edits without reading sources; exit 1 if any. `bazel test //…:<name>.check`.
- `<name>.diff` — `bazel run` prints the merged git-apply-able unified diff (review artifact).
- `<name>.fix` — `bazel run` applies edits in `$BUILD_WORKSPACE_DIRECTORY` (outside the action graph, since Bazel actions can't mutate sources).

`--config=lint` in [.bazelrc](.bazelrc) runs the aspect and materializes each target's records (`bazel build --config=lint //…`) for inspection/CI; the failing gate is the `.check` test.

**Non-obvious behaviours** — record file keys are the source's **real path**, which under Bazel resolves to the absolute workspace path (source symlinks) — stable across actions/sandboxes; `aggregate_edits` uses absolute keys directly and joins relative keys with `--root`. `-x c++` is forced so headers parse as C++ (not C). `-resource-dir` is derived from the staged `@llvm-project//clang:builtin_headers_gen` paths (robust to a module-extension rename). The aspect needs `cpp_format` in the **exec** configuration (a one-time from-source Clang/LLVM build). Portability caveat: absolute paths in records make remote-cache reuse across machines suboptimal.

### Prebuilt-binary integration kit ([bazel/integration/](bazel/integration/))

A variant of the integration for external repos that want the lint/fix gate **without** building Clang/LLVM from source. It is documented for end users in the README quick start. Consumed two ways: **imported by URL** — `bazel_dep(name="cpp_formatting") + archive_override(urls=[…github…/archive/refs/tags/<tag>.tar.gz])`, then `load("@cpp_formatting//bazel/integration:cpp_format.bzl", …)` and `use_extension("@cpp_formatting//bazel/integration:extensions.bzl", "cpp_format")` — or by copying the directory in. Importing by URL does **not** build LLVM: the `//third_party/llvm` extension in this module is lazy and the kit never references `@llvm-project` (verified — `bazel build @cpp_format_bin//:cpp_format` from a consumer touches no LLVM). Files:

- [bazel/integration/cpp_format.sh](bazel/integration/cpp_format.sh) — the **ergonomic entry point**: `cpp_format.sh <check|diff|fix> [pattern]` (pattern defaults to `//...`). It `bazel query`s the first-party `cc_*` targets under the pattern (`except attr(tags, 'no-cpp-format', …)`), `bazel build`s them with `--aspects=…%cpp_format_aspect --output_groups=+cpp_format_edits` to emit each target's record file, derives the record paths deterministically (`//pkg:name` → `<bazel-bin>/pkg/name.cpp_format.json`, kept only if it exists — source-less targets emit nothing), and runs `cpp_format --aggregate` over them. This is what lets users format the **whole repo with no per-target wiring**. It is a plain script, **not** a `bazel run` target, so it can invoke `bazel build` without nesting a Bazel server in a running one (which would deadlock on the workspace lock). `ASPECT`/`BIN_LABEL` are overridable via env (`CPP_FORMAT_ASPECT`/`CPP_FORMAT_BIN_LABEL`) so the same script drives the in-repo from-source labels for dogfooding. The query excludes `cpp_format_targets` rule targets, so the aspect is applied to each source target exactly once (two applications collide on the shared record file). Consumers get it via `bazel run @cpp_formatting//bazel/integration:install` (see the `cpp_format_install` rule below), which places it in `$BUILD_WORKSPACE_DIRECTORY` (default `tools/cpp_format.sh`) with the **canonical** aspect label baked into its `ASPECT` default — `expand_template` substitutes the third-party placeholder with `str(Label(":cpp_format.bzl")) + "%cpp_format_aspect"`, which resolves to `@@<cpp_formatting-canonical>//…` in the consumer's graph and is valid from their command line — so the placed script runs with no env var. The `install` launcher itself only copies a file (no nested `bazel`), so it *is* a safe `bazel run` target.
- [bazel/integration/extensions.bzl](bazel/integration/extensions.bzl) — a Bzlmod `module_extension` (`cpp_format`) with a `release(version, base_url, sha256)` tag class. Its repo rule detects the host OS/arch (`rctx.os`), downloads the matching release asset (`cpp_format-{linux-x86_64,linux-aarch64,darwin-aarch64,windows-x86_64.exe}`) into a `bin/` subdir, and exposes it as `@cpp_format_bin//:cpp_format`. **The download target file must not share the `cpp_format` filegroup's name** — a same-name `src` is a self-edge cycle (hence `bin/`).
- [bazel/integration/cpp_format.bzl](bazel/integration/cpp_format.bzl) — the same aspect + `cpp_format_targets` as [bazel/cpp_format.bzl](bazel/cpp_format.bzl), with four differences: it runs the prebuilt binary as a plain `File` (`ctx.file._cpp_format`, `executable=`/`tools=`, no `cfg="exec"` build); it stages **no** `@llvm-project` builtin headers and passes **no** `-resource-dir` (the published binary self-extracts its embedded Clang headers via `ensureClangResourceDir()` — verified to work even with a cleared env, falling back to `TMPDIR`); the aggregator rules exec `cpp_format --aggregate` instead of the separate `aggregate_edits` binary; and `_config` defaults to **`Label("@@//:cpp_format.yaml")`** — the *root module's* config, **not** `//:cpp_format.yaml`. This is load-bearing for URL import: when the `.bzl` is loaded from `@cpp_formatting`, a bare `//:cpp_format.yaml` resolves to *cpp_formatting's* config (silently the wrong ruleset — reproduced and fixed), whereas `@@//` resolves to the consumer's root config whether the kit is imported or vendored. `_rlocation_path()` derives each runfiles key from `File.short_path` (`../<canonical>/…` for the external binary → strip `../`; main-repo records → `_main/…`), so it works for both the external binary and generated records.

`cpp_format.sh` (whole-repo/pattern, query-driven) and `cpp_format_targets` (a pinned `bazel test` gate over an explicit dep set) are the two entry points — use one or the other, never the wrapper over a pattern that also has `cpp_format_targets` defined on it. For URL import, `cpp_format_targets` needs nothing local, while the wrapper is the **one** file a consumer keeps locally — obtained with `bazel run @cpp_formatting//bazel/integration:install` (the `cpp_format_install` rule; [bazel/integration/BUILD.bazel](bazel/integration/BUILD.bazel) instantiates `:install`), which bakes the aspect label in so it runs with no env. The in-repo [bazel/cpp_format.bzl](bazel/cpp_format.bzl) (from-source, staged headers) remains the path used by this repo's own `//bazel/testdata` demo and tests; the kit is what external consumers use. The whole URL-import flow is verified end-to-end against a `local_path_override` consumer module — `install` places a self-contained `tools/cpp_format.sh`, and both `cpp_format_targets` and the wrapper apply the consumer's ruleset (not cpp_formatting's).

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
| a template instantiated in the same TU | pattern only | an implicit instantiation carries the pattern's source locations, so matching it too would rewrite the same place twice (`unless(isTemplateInstantiation())`) |

### `leading_return_types`: the reverse direction, and why it rejects more

`ReturnTypeStyle::Leading` (`--reverse`, or `return_types: leading`) is the
mirror rewrite, and the core of it is genuinely symmetric: for a
trailing-return declarator the parser leaves both endpoints on the
`FunctionTypeLoc` that `runToTrailing()` already uses — `getLocalRangeBegin()`
is the `auto` placeholder and `getLocalRangeEnd()` is the `->`
(`Parser::ParseFunctionDeclarator` sets `StartLoc` to the `TST_auto` decl-spec
and `LocalEndLoc` to the arrow). Both are re-checked by spelling rather than
trusted. Replacing the placeholder *in place* is what keeps `static` /
`constexpr` / attributes in their original order for free.

What is not symmetric is what may move. A trailing return type can say things
leading position cannot express, so `runToLeading()` is mostly guards:

| Input | Rewritten? | Reason |
|---|---|---|
| `auto foo() -> int` | yes | |
| `auto foo() const noexcept -> int` | yes | the qualifiers sit before the arrow and never move |
| `static constexpr auto foo() -> int` | yes | the placeholder is replaced in place |
| `auto Foo::bar() -> std::string` | yes | a name written qualified resolves the same from either position |
| `auto f(T a) -> decltype(a.size())` | no | parameters are not in scope before the declarator-id (`rangeMentionsName` on the parameter names — the dependent spellings carry no resolved `ParmVarDecl` to test against) |
| `auto make() -> int (*)(bool)` | no | leading position needs a *restructured* declarator (`int (*make())(bool)`); `isMovableTypeShape` walks `getNextTypeLoc()` for a function/array component |
| `auto get() -> int (&)[4]` | no | same |
| `auto Foo::bar() -> Inner` | no | `Inner` is found in class scope only after the declarator-id (`LookupEscapeChecker`) |
| `auto Foo::bar() -> ns::Vec<Inner>` | no | the *template argument* is unqualified even though the template name is not — `LookupEscapeChecker` traverses argument locs for exactly this |
| `auto foo() -> auto` / `-> decltype(auto)` | no | a placeholder has no type to move (`isMovableTypeShape` again) |
| `[]() -> int {}` | no | a lambda has no declarator-id to put a return type on. The Trailing direction never needed this guard: a lambda either has a trailing return (excluded by `unless(hasTrailingReturn())`) or a deduced one (excluded by its `AutoTypeLoc` check) |
| `auto foo() -> INT_T` (macro) | no | the rewrite never edits through a macro expansion |

`LookupEscapeChecker` only runs when `getLexicalDeclContext() !=
getDeclContext()`. An in-class or in-namespace declaration is looked up in the
same scope from either side of the declarator-id, so there is nothing to check
— and running the check anyway would reject ordinary `using namespace` code.

In `Emit` mode the direction mirrors the subsumption in `runToTrailing()`: a
rename edit recorded *inside the trailing type* rode along in the text that
moves to the placeholder (via `Rewriter::getRewrittenText()`), so those records
are dropped before the two new ones are pushed. Forgetting that half is how
the same offset ends up with conflicting edits after aggregation.

Both directions are fixpoints, and each is a fixpoint on the other's output
for everything it moved — see `mini_repo-return_types_roundtrip` below.

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
- **Name collisions are skipped, not applied** — if a rename's new name is already taken in the *same* scope, `CollectRenamesVisitor::collides()` drops the rename entirely (declaration and every use) and records a `RenameConflict`. Renaming into an occupied name is not a formatting change: at best it fails to compile, at worst it silently rebinds uses to the other entity. Only the immediate `DeclContext` is consulted — shadowing an inherited or outer-scope name is legal C++ and still allowed. Two functions may share a name (that is an overload set) unless the signatures match, which would be a redeclaration; two data members, or a member and a method, may not. A second declaration renaming to a name a first already claimed is skipped too. `reportRenameConflicts()` prints a one-line count; `--report-rename-conflicts` (on `cpp_format` and `normalize_variables`) lists every site. googletest's `RE::pattern_`/`RE::pattern()` is the motivating case.
- **Shadowed variables** — `matchesScope()` filters by scope: a parameter with the same name as a global is not collected when renaming globals.
- **Per-file-content cache key** — the embedded Clang resource directory is extracted under a directory whose name includes the FNV-1a hash of the embedded `.tar.gz`. If the embedded headers change (e.g. after an LLVM upgrade) a fresh cache directory is created automatically.

## Notes

- The `bazel-*` symlinks in the root are Bazel output/convenience symlinks — do not edit them.
- The `patches/` directory holds patches applied to the LLVM source at fetch time, listed in the `llvm-raw` `http_archive` in [third_party/llvm/extensions.bzl](third_party/llvm/extensions.bzl). The three applied ones keep zlib/zstd/blake3 building under MSVC; `llvm_smallvector_cstdint.patch` is unused, having gone obsolete with the LLVM 19 upgrade (upstream `SmallVector.h` now includes `<cstdint>` itself). Other MSVC breakage is handled with conformance flags in [.bazelrc](.bazelrc) rather than by patching LLVM — see `/Zc:preprocessor` and `/permissive-` there. Clang/LLVM is built from source via the module extension in [third_party/llvm/](third_party/llvm/); see the "Clang/LLVM from source" section above.
