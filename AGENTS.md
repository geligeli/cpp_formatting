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
bazel build //cpp_formatting:const_placement
```

**Run a specific test suite:**
```
bazel test //cpp_formatting:trailing_return_types_test
bazel test //cpp_formatting:trailing_return_types_integration_test
bazel test //cpp_formatting:const_placement_test
bazel test //cpp_formatting:const_placement_integration_test
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
bazel run //cpp_formatting:const_placement -- --style=east path/to/file.cpp -- -std=c++17
bazel run //cpp_formatting:cpp_format -- --config=cpp_format.yaml --in-place file.cpp -- -std=c++17
```

**Run a binary in-place:**
```
bazel run //cpp_formatting:trailing_return_types -- -i path/to/file.cpp -- -std=c++17
bazel run //cpp_formatting:normalize_variables -- --style=snake_case --scope=member --in-place path/to/file.cpp -- -std=c++17
bazel run //cpp_formatting:const_placement -- --style=east --in-place path/to/file.cpp -- -std=c++17
```

**Lint mode (CI/CD):** all four binaries accept `--lint` (modify nothing, exit 1 when violations are found, 0 when clean) and `--format=<text|sarif|diff>` (default `text`; a non-default value implies `--lint`). `sarif` emits a SARIF 2.1.0 log; `diff` emits a git-apply-able unified patch. Output goes to stdout; `--lint`/`--format` cannot be combined with `--in-place`.

**Parallelism:** all four binaries parse translation units in parallel — one `ClangTool` per TU on a worker pool sized to the CPU count (`--jobs=N` / `-jN` caps it; `0` = every CPU; larger requests are capped). The output is byte-identical for any `-j`; see "Execution model" below. `--debug-trace` forces one TU at a time.
```
bazel run //cpp_formatting:normalize_variables -- --style=snake_case --scope=member --lint path/to/file.cpp -- -std=c++17
bazel run //cpp_formatting:cpp_format -- --config=cpp_format.yaml --format=sarif file.cpp -- -std=c++17
```

**Debug rename detection (`normalize_variables --debug-trace`):** prints per TU the rename map and every reference site found in the AST, with `main=Y/N`, `macro=Y/N`, a `WILL_RENAME` marker on sites that would actually be rewritten (a macro *argument* counts — it is rewritten at its call site, so `macro=Y WILL_RENAME` is normal), and `VETOES_RENAME` on a site with no rewritable spelling, which suppresses the rename everywhere. Useful for diagnosing why a particular use was missed (e.g. it lives in a file that wasn't passed in the source list).

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
  - `TrailingReturnTypesAction` — frontend action; buffers the main file into its `TUSlot` in every mode (the whole file for a dry run, only a changed file otherwise) and never prints or writes from inside a TU
  - `TrailingReturnActionFactory` — `TUSlotClient` for the driver (no cross-TU state); `finish()` merges the slots, `flush()` prints (DryRun: one banner, `=== path ===` headers when multi-file) or writes (InPlace) the buffered content
  - `rewriteToTrailingReturnTypes()` / `rewriteToLeadingReturnTypes()` — test helpers that rewrite an in-memory string
- [cpp_formatting/trailing_return_types_lib.cpp](cpp_formatting/trailing_return_types_lib.cpp) — Implementation: `runToTrailing()` and `runToLeading()` behind one `run()`, the guard helpers the Leading direction needs (`isMovableTypeShape`, `rangeMentionsName`, `LookupEscapeChecker`), plus the private `CaptureAction` used by the test helpers.

#### `tu_driver` — parallel translation-unit driver (shared by all four binaries)

- [cpp_formatting/tu_driver.h](cpp_formatting/tu_driver.h) — Public API:
  - `TUSlot` — everything one TU produces (`Pending`, `Edits`, `Conflicts`, `Report`, its seeded-plus-own `Vetoes` and per-rule `DepRes` maps, buffered Clang diagnostics, `Rc`). Written only by that TU's action, on its worker thread.
  - `CrossTUState` — the shared `Vetoes` + `DepResPerRule`, touched only by the driver thread at barriers.
  - `TUSlotClient` — what a tool implements: `ruleCount()`, `createAction(TUSlot&)` (called on the worker from inside `ClangTool::run`, may touch only immutable config and the slot), `shared()`, `rerunNeededOnVeto()` / `crossTUSeeding()` (both false in Emit mode), `finish(slots)` (merge in slot order, once).
  - `runTranslationUnits()` — the driver: phases, barriers, re-runs (see "Execution model" below); returns the combined `ClangTool::run` code (any 1 → 1, else any 2 → 2, else 0).
  - `isHeaderSource()` (h/hh/hpp/hxx/h++, the aspect's `_HDR_EXTS`), `resolveJobs()` (0 → every CPU, affinity-aware; larger requests capped), `makeStandardArgumentsAdjuster()` (drop `-fno-canonical-system-headers`, supply the embedded `-resource-dir` unless one is given — the adjusters every main used to build by hand).
- [cpp_formatting/tu_driver.cpp](cpp_formatting/tu_driver.cpp) — Implementation. One `ClangTool` per TU with `llvm::vfs::createPhysicalFileSystem()` (a per-instance cwd, so `ClangTool::run`'s per-compile-command chdir never touches the process — the `AllTUsToolExecutor` pattern). Workers are `llvm::thread`s with explicit 8 MiB stacks (a pool's default is 1 MiB on Windows; Clang's parser recurses deeply). A per-slot `SlotActionFactory` overrides `runInvocation()` to point `CompilerInstance::setVerboseOutputStream` at the slot's buffer (Clang's `N warnings generated.` bypasses the `DiagnosticConsumer`) and to build the buffered `TextDiagnosticPrinter` from the invocation's own `DiagnosticOptions`; buffered diagnostics are replayed to stderr in slot order under one mutex. The serial path (`-j1`, `--debug-trace`) leaves Clang's stderr printer in place, so its output is exactly the old one.
- [cpp_formatting/rename_state.h](cpp_formatting/rename_state.h) / [rename_state.cpp](cpp_formatting/rename_state.cpp) — the cross-TU rename types (`DependentResolution(s)`, `RenameVetoes`, `RenameConflict(s)`) split out of `rename_variables_lib` so the driver can carry them without depending on the visitors, plus `recordResolution` / `vetoResolution`, `mergeDependentResolutions()` (a lattice join on `(HasName, NewName, Vetoed)`: veto absorbing, disagreement vetoes, same name idempotent — so replaying a map seeded from the target is safe) and `dependentResolutionsDifferFor()` (the staleness test, scoped to one file).

#### `const_placement` — move cv-qualifiers to the east or west of the type they qualify

- [cpp_formatting/const_placement.cpp](cpp_formatting/const_placement.cpp) — `main()`: CLI option parsing (`--style=east|west`, `--lint`/`--format`, `--jobs`), drives `ConstPlacementActionFactory`.
- [cpp_formatting/const_placement_lib.h](cpp_formatting/const_placement_lib.h) — Public API:
  - `ConstStyle` — `East` (`const int x` → `int const x`) | `West` (the reverse). One value rather than two booleans, so "both directions at once" is unrepresentable, as for `ReturnTypeStyle`.
  - `parseConstStyle()` / `constStyleRuleId()` — the `east`/`west` spelling and the `east_const`/`west_const` lint rule ids.
  - `runConstPlacementOnAST()` — the pass itself, exposed so `cpp_format` can run it on an AST it has already parsed, sharing one `Rewriter` with the other passes.
  - `ConstPlacementAction` / `ConstPlacementActionFactory` — frontend action and `TUSlotClient`, mirroring the return-type pair: the rewrite is local to one TU, so there is no cross-TU state, nothing to seed and nothing that can veto.
  - `rewriteConstPlacement()` — test helper that rewrites an in-memory string.
- [cpp_formatting/const_placement_lib.cpp](cpp_formatting/const_placement_lib.cpp) — Implementation: a `RecursiveASTVisitor` over `QualifiedTypeLoc`, plus the shape guard (`isPlainTypeSpecifier`) that decides which qualifiers may move and the raw-buffer scans that find the written qualifier run. See "`const_placement`: which qualifier moves, and why" below.

#### `normalize_variables` — rename variables and member functions to a consistent naming convention

- [cpp_formatting/normalize_variables.cpp](cpp_formatting/normalize_variables.cpp) — `main()`: CLI option parsing, builds a `FileSet` from source paths, drives `rename_variables_lib` through `runTranslationUnits()`.
- [cpp_formatting/rename_variables_lib.h](cpp_formatting/rename_variables_lib.h) — Public API:
  - `FileSet` — set of real absolute paths whose declarations the tool collects (enables cross-file renaming)
  - `VariableRenameCallback` — callback invoked once per canonical declaration
  - `VariableScope` — broad: `Member` | `Local` | `Global`; fine-grained: `StaticMember` | `ConstMember` | `StaticGlobal` | `ConstGlobal`; functions: `Method`
  - `RenameActionFactory` — `TUSlotClient` for one rule: `createAction()` hands the per-TU action pointers into its `TUSlot` (the visitors' pointer-taking signatures are unchanged); `finish()` merges the slots in source order; call `flush()` afterwards to commit (atomic disk writes for `InPlace`, formatted stdout for `DryRun`, no-op for `Debug`/`Lint`). `setLintReport()` attaches a `LintReport` for lint diagnostics; `rewrites()` exposes the buffered content (used by lint `diff` output); `vetoes()` exposes the shared veto set.
  - Factory functions: `RenameAllMemberVariables`, `RenameAllLocalVariables`, `RenameAllGlobalVariables`, `RenameAllStaticMemberVariables`, `RenameAllConstMemberVariables`, `RenameAllStaticGlobalVariables`, `RenameAllConstGlobalVariables`, `RenameAllMemberFunctions`
  - `orderSourcesForRename()` — the driver's source order (non-headers, then headers, each group in the order given); exposed for callers that need the same order
  - `rewriteVariableNames()` — test helper (single in-memory TU, returns the rewritten string)
- [cpp_formatting/rename_variables_lib.cpp](cpp_formatting/rename_variables_lib.cpp) — Implementation: `RecursiveASTVisitor` passes — collect declarations, scan for references that cannot be rewritten (`ApplyMode::Scan`), then apply the renames that survived — plus `DependentTokenCollector` + `RecordDependentResolutionsVisitor` (the latter walks template instantiations) that together resolve template-dependent member tokens across TUs. A separate `DebugTraceVisitor` runs in `OutputMode::Debug` to print every reference site without modifying anything.
- [cpp_formatting/naming_convention.h](cpp_formatting/naming_convention.h) — `NamingStyle` enum, `splitIntoWords`, `formatName`, `renameToStyle`, `parseNamingStyle`.
- [cpp_formatting/naming_convention.cpp](cpp_formatting/naming_convention.cpp) — Splits camelCase/snake_case/prefixed names into word lists and reassembles in any target style.

#### `cpp_format` — combined tool with YAML config

- [cpp_formatting/cpp_format.cpp](cpp_formatting/cpp_format.cpp) — `main()`: parses CLI options or a YAML config file, then runs every `normalize_variables` rule plus `const_placement` and `trailing_return_types` in a **single** `ClangTool` pass via `CppFormatActionFactory` (each TU is parsed exactly once, regardless of how many rules are configured). Also hosts the `--emit-edits=<file>` mode (writes per-TU edit records for Bazel aggregation), the `--owned-files=<list>` mode (newline-separated paths added to the `FileSet` but *not* to the source list, so a dependency's declarations are renameable at their use sites here without being re-parsed — see the Bazel integration below), and the `--aggregate` mode (merges those records; dispatched before `CommonOptionsParser`, delegates to `lint_lib`'s `runEditAggregation`).
- [cpp_formatting/cpp_format_lib.h](cpp_formatting/cpp_format_lib.h) — `NormalizeRule` (scope + rename callback + lint rule id) and `CppFormatActionFactory`: the `TUSlotClient` for the combined tool (`ruleCount()` = number of rules, one `DepRes` map per rule in every slot); `finish()` merges the slots and `flush()` commits like `RenameActionFactory`. The qualifier move and the return-type pass are carried as a `std::optional<ConstStyle>` and a `std::optional<ReturnTypeStyle>` — `nullopt` skips that pass.
- [cpp_formatting/cpp_format_lib.cpp](cpp_formatting/cpp_format_lib.cpp) — Implementation: per TU, `runRenameRuleOnAST()` (from `rename_variables_lib`) runs each rule's collect+apply visitors, then `runConstPlacementOnAST()`, then the trailing-return `MatchFinder` via `matchAST()` on the same AST, all sharing one `Rewriter`. **That order is load-bearing.** Each later pass reads the text an earlier one produced with `Rewriter::getRewrittenText()`, so a rename that landed inside a return type (a member in `decltype(count_)`) is carried into the moved `-> decltype(m_count)` text instead of being clobbered by the wholesale `auto` replacement — and a qualifier moved east is carried into `-> int const` instead of being stranded on the `auto` placeholder. In Emit mode each pass drops the records of the earlier ones that fall inside the span it re-emits, for the same reason.

#### Lint support (CI/CD)

- [cpp_formatting/lint_lib.h](cpp_formatting/lint_lib.h) — `PendingRewrites` (path → rewritten content, shared by both rewrite libs), `EditRecord`/`ResolutionRecord`/`RenameVeto`/`RenameSkip`/`EditReport` (the Bazel aggregation model; `RenameSkip` is `rename_state.h`'s `RenameConflict`, and `reportRenameSkips()` is the one printer both a direct run and `--aggregate` use), `LintDiagnostic`, `LintReport` (`emitText`/`emitSARIF`), `emitUnifiedDiff()` (Myers line diff, git-apply-able), `relativizeToCwd()` (the cwd is resolved once and cached — it is called per edit record from several threads, and the tool never changes its own cwd), `emitLintResults()` (shared by the four mains: emits the chosen format and returns the exit code).
- [cpp_formatting/lint_lib.cpp](cpp_formatting/lint_lib.cpp) — Implementation. JSON via `llvm/Support/JSON.h` (no new dependency). Lint diagnostics are recorded at the same choke points that perform rewrites (`ApplyRenamesVisitor::renameAt`, `TrailingReturnCallback::run`), so lint results exactly match what in-place mode would change. Also implements the edit-record model (`EditReport`/`parseEditReport`/`mergeEditReports`/`aggregateEdits`) and `runEditAggregation()` — the shared CLI entry point behind both `aggregate_edits` and `cpp_format --aggregate`.

#### Embedded Clang resource directory

All four binaries link the Clang built-in headers (`stddef.h`, `__stddef_max_align_t.h`, etc.) into the binary itself, so no system Clang installation is required at runtime.

- [cpp_formatting/embedded_clang_resource.h](cpp_formatting/embedded_clang_resource.h) — declares `ensureClangResourceDir()`.
- [cpp_formatting/embedded_clang_resource.cpp](cpp_formatting/embedded_clang_resource.cpp) — implementation: extracts the embedded `.tar.gz` to `$XDG_CACHE_HOME/cpp_formatting/clang_resource_<fnv1a-hash>/` on first call, returns the cached path on subsequent calls. Decompression (zlib, `@llvm_zlib//:zlib`) and tar parsing happen in-process — no system `tar`/`rm` or POSIX calls — and extraction goes via a uniquely-named temp directory + atomic `std::filesystem::rename` so concurrent invocations are safe.
- The `clang_include_headers` `pkg_tar` rule in [cpp_formatting/BUILD](cpp_formatting/BUILD) packages `@llvm-project//clang:builtin_headers_gen` into a `tar.gz`. The `clang_include_headers_embed_cc` `genrule` runs the `embed_file` host tool ([cpp_formatting/embed_file.cpp](cpp_formatting/embed_file.cpp)) on it to produce a `const unsigned char[]` translation unit linked into every binary. The `clang_include_headers` `strip_prefix` hardcodes the extension-generated canonical repo name `+llvm+llvm-project`; if the module extension name changes, update it too.

### Tests

- [cpp_formatting/trailing_return_types_test.cpp](cpp_formatting/trailing_return_types_test.cpp) — gtest unit tests for `trailing_return_types_lib`, including a `TrailingReturnTypesDeducingThis` suite covering C++23 explicit object parameters (P0847, run with `-std=c++23`) and a `LeadingReturnTypes` suite for the reverse direction (every guard gets a "left unchanged" case, plus a leading→trailing→leading round trip and a fixpoint check). An east-const group pins both directions on a return type whose cv-qualifier is written *after* the type specifier — the case the forward qualifier scan exists for.
- [cpp_formatting/integration_test.sh](cpp_formatting/integration_test.sh) — Shell integration tests for `trailing_return_types`:
  1. Dry-run on a single file — rewritten source goes to stdout.
  2. In-place on a single file — file is modified on disk.
  3. In-place on two files in one invocation — both files are modified.
  4. In-place on a file with system `#include`s — validates the embedded Clang resource directory works (no system Clang required).
  5. In-place `--reverse` — the Leading direction reaches disk through the same buffered `flush()` path, and a second pass over its own output changes nothing.
  6. `--jobs=2` — two files on two threads give the expected in-place result and the same multi-file dry-run output (one `=== path ===` section per file) as one thread.
- [cpp_formatting/tu_driver_test.cpp](cpp_formatting/tu_driver_test.cpp) — gtest unit tests for `tu_driver`: `isHeaderSource` (incl. `h++`, and a dotted directory name), `resolveJobs` (0 = every CPU, requests capped), `makeStandardArgumentsAdjuster`.
- [cpp_formatting/rename_state_test.cpp](cpp_formatting/rename_state_test.cpp) — gtest unit tests for `rename_state`: merge is idempotent and commutative on the resolved state, disagreement vetoes, veto is absorbing, `dependentResolutionsDifferFor` is scoped to one file and ignores the informational fields.
- [cpp_formatting/const_placement_test.cpp](cpp_formatting/const_placement_test.cpp) — gtest unit tests for `const_placement_lib`: the move in both directions over every spelling that carries a qualifier (variable, parameter, return type, elaborated and qualified names, multi-token builtins, template arguments, alias and typedef, `auto`, `decltype`, casts, array elements); the declarator-component cases that must *not* move (`int* const p` either way, `const int* const p` moving only the pointee's qualifier, a member function's own `const`); nested qualified types including an argument list that closes with `>>`; the macro declines; one decl-specifier-seq shared by several declarators; `volatile` and runs of qualifiers; and that each direction is a fixpoint and undoes the other.
- [cpp_formatting/const_placement_integration_test.sh](cpp_formatting/const_placement_integration_test.sh) — Shell integration tests for `const_placement`: in-place in both directions over a fixture and back to a byte-identical round trip; both directions as fixpoints; a pointer's own qualifier left alone; `--lint` text/SARIF/diff agreeing with the in-place result and a clean file exiting 0; `--jobs=1` vs `--jobs=4`; `cpp_format` running the same pass from its config **composed with `return_types: trailing`**; `--emit-edits` + `--aggregate --apply` reproducing the in-place result exactly; an unknown style refused by both entry points; and nested qualified types carrying a rename made *inside* one of the specifiers the move re-emits (`decltype(count_) const` becoming `decltype(m_count) const`), in-place and through aggregation.
- [cpp_formatting/naming_convention_test.cpp](cpp_formatting/naming_convention_test.cpp) — gtest unit tests for `naming_convention`: `splitIntoWords`, `formatName`, `renameToStyle`.
- [cpp_formatting/rename_variables_test.cpp](cpp_formatting/rename_variables_test.cpp) — gtest unit tests for `rename_variables_lib` (member, local, global, static data members, const members, static globals, const globals, member functions, templates, template-dependent member tokens resolved through instantiations, cross-file, constructor initializers, C++23 explicit object parameters / "deducing this"). A `RenameNestedTemplateClass` suite pins members of a class nested in a class template (plain use, dependent use, the method/static-member cases that already worked, and the partial/explicit specialization cases where index matching must not cross member lists). A `RenameMacros` suite covers both halves of the macro rule: arguments (direct, forwarded through macros, expanded twice, a declaration written as an argument, and a template-dependent `this->member` in an argument) are renamed; a macro-body reference, a `##`-pasted reference, a macro-body reference to one member of an override hierarchy, and a macro-body *dependent* token each veto the rename. `RenameOffsetOf`, `RenameAnonymousUnion`, and the `ProtocolNamesAreDeclined` / `MembersOfForeignSpecializationAreDeclined` / `TraitValueIsDeclined` cases pin the reference kinds and name rules described under "Known non-obvious behaviours" (offsetof renames, offsetof on a dependent type declines; an anonymous-union member of a dependent base renames; `begin`/`end`, `value` and `std::numeric_limits<Fix>::is_specialized` decline while siblings rename). `RenameHiding` pins the per-use hiding check (a derived class declaring the new name declines; qualified and base-typed accesses do not; the dependent-base case is caught on the instantiation) and `RenameSpellingAudit` the raw-lexer backstop (a using-declaration, an unresolved call inside a template and a `##`-consumed macro argument decline; comments, strings and unrelated declarations of the same name do not).
- [cpp_formatting/normalize_variables_integration_test.sh](cpp_formatting/normalize_variables_integration_test.sh) — Shell integration tests for `normalize_variables`:
  1. Multi-file member rename — cross-file references, pointer-to-member, lambda, scope separation.
  2. Shadowed variable — global renamed, same-named local parameter unchanged.
  3. Source ordering — header passed in the middle of the source list; tool auto-promotes it to the end so every `.cpp` is parsed against the original header.
  4. Member function rename — virtual override hierarchy, out-of-line static definition, cross-file call sites; destructor, data members, and free functions unchanged.
  5. Template-dependent member token — `set_val`'s `x.val` in a header is rewritten from the resolution recorded while the instantiating `.cpp` files are processed (self-contained fixtures generated inline).
  6. Out-of-scope veto — the same template instantiated with a type outside the passed source set leaves the shared header token untouched while still renaming the owned type.
  7. Macro-body veto — a member referenced from a macro body keeps its name in *every* file, including one the tool rewrote before reaching the TU that expands the macro (the re-run), while a sibling member renames; the skip is reported.
  8. Macro argument — a declaration and its uses written as macro arguments are renamed at their call sites.
  9. Header-only instantiation chain — `a.h`'s dependent token is instantiated only from `b.h`; both source orders and both `--jobs=1`/`--jobs=4` rename it (the stale-TU re-run; this case was order-dependent before the driver).
  10. `--debug-trace --jobs=4` — runs serially, prints one trace per TU, modifies nothing.
  11. `--jobs` equivalence — the multi-file, ordering, dependent-token, out-of-scope-veto and macro-veto scenarios each give byte-identical files and the same (sorted) conflict report with `--jobs=1` and `--jobs=4`.
  12. `-Werror` — the project's own warning flags cannot fail the run; a real parse error still does.
  13. Reference from a file that is never formatted — a member referenced from an included `.inc` that is not in the source list keeps its name (the skip is reported with the `.inc` location), a sibling member renames, and the `.inc` is untouched.
  14. A dependent use no instantiation resolves — `Impl<T>::kIsNothrow` inside an alias template's argument is folded away at instantiation; the name is declined after the first pass (the re-run), the two declarations and the use are untouched, a sibling constant renames.
- [cpp_formatting/lint_lib_test.cpp](cpp_formatting/lint_lib_test.cpp) — gtest unit tests for `lint_lib`: unified diff (hunks, context merging, missing trailing newline, empty inputs), SARIF emission (parsed back with `llvm::json`), and the edit-record model (JSON round trip including owners, vetoes, pending resolutions and name-keyed vetoes; aggregation dropping the edits and dependent-token resolutions of a vetoed declaration, of a name declined outright, and of a dependent token that no report resolved).
- [cpp_formatting/lint_integration_test.sh](cpp_formatting/lint_integration_test.sh) — Shell integration tests for `--lint`/`--format` across `normalize_variables`, `trailing_return_types` and `cpp_format` (`const_placement`'s lint modes are covered by its own integration test):
  1. Text lint — diagnostics on stdout, exit 1, file byte-identical.
  2. SARIF lint — valid 2.1.0 log with rule id and cwd-relative URI.
  3. Diff lint — emitted patch applies with `git apply` and matches the `--in-place` result.
  4. Clean file — exit 0, no diagnostics.
  5. `trailing_return_types` lint — text and diff round-trip.
  6. `cpp_format` lint — multi-pass run aggregates both rule ids into one SARIF report.
  7. Macro veto — a rename vetoed by a macro-body reference produces no diagnostic, and the re-run that makes the veto order-independent leaves no duplicates from the discarded first pass; text, SARIF and diff output are byte-identical with `--jobs=1` and `--jobs=4` (and `cpp_format`'s SARIF likewise, in test 6).
  7. `--reverse` lint — `leading_return_types` diagnostics, a diff that `git apply`s to the expected leading form, a second pass that reports nothing (fixpoint), the same direction through `cpp_format --return-types=leading`, and the error when both directions are requested at once.
- [cpp_formatting/aggregate_integration_test.sh](cpp_formatting/aggregate_integration_test.sh) — Shell integration tests for the per-TU emit + aggregate pipeline (`cpp_format --emit-edits` then `cpp_format --aggregate`), on a two-file fixture whose header holds a template-dependent member token resolved from the instantiating `.cpp`:
  0. `--emit-edits` records are byte-identical with `--jobs=1` and `--jobs=4` (Emit mode seeds no cross-TU state into a TU, so every TU's records are independent of what ran before it).
  1. `--aggregate` diff is byte-identical to the standalone `aggregate_edits` binary and rewrites both the member decl and the cross-TU dependent token.
  1b. One emit invocation per source file, as the aspect runs them (each told via `--owned-files` that the target's other file is renameable): the `.cpp`'s action emits no edit for the header but records the dependent-token resolution, and the records merge — through `--records-from` (blank and padded lines tolerated, a record listed twice dedups, a missing list exits 2), for both CLIs — to the same diff as one invocation over the target.
  2. `--aggregate --check` exits 1 with per-file edit counts.
  3. `--aggregate --apply` rewrites the files on disk; re-emitting against the fixed sources and re-checking exits 0.
  4. `--owned-files` (the cross-target rename path): a second fixture emitted the way the aspect does it — one invocation per "target" — asserts that a dependent parsing only its own source emits nothing for a dep's member, emits the use-site edit once the dep's header is passed as owned, and still emits no edits for the dep's own files (it does name that header as the edits' `owner_file`).
  5. Cross-target veto: the library's action emits a rename for a member it cannot know is unsafe, the dependent's action expands the macro and emits a veto instead, and aggregation drops the library's edits while keeping an unaffected sibling member — via both `--aggregate` and the standalone `aggregate_edits`, and again with the library split into one action per file. The macro is defined in the *dependent's* file on purpose: a macro body in the library's own header that names the member is caught by the spelling audit of that header's action (asserted last), which would leave nothing for the cross-target propagation to prove. That last case also pins the skip report travelling through the records: aggregation counts the declined rename and, with `--report-rename-conflicts`, names its site, identically from both CLIs.

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
`macro_repo-member_snake_case` is a deterministic, seconds-long check of the
macro rule: a member referenced only from inside a macro body keeps its name
(the rename is vetoed), while a sibling member with no macro reference renames
normally, so the `check` phase stays non-vacuous.

Every rename scenario passes, and the corpus ones each pin something the
fixtures cannot: `googletest-member_trailing_method_snake` is the only scenario
that renames member functions (3024 edits; its trail from 14868 errors is in the
scenario file), and `abseil_cpp-member_snake_case` runs the member rename over
the independent corpus (5281 edits across 275 files; its trail from 753 errors
likewise). Between them they found the reference kinds and name rules listed
under "Known non-obvious behaviours" — every one on a real corpus rather than by
inspection. What neither renames is declined and reported (roughly 190
declarations on abseil, most by the spelling audit and the unmapped-instantiation
backstop); that is where recall work would start, and the scenarios' `rebuild`
phase is what keeps it from ever becoming correctness work again.

**re2** widens the shapes rather than the size, and is the cheap gate --
minutes, not abseil's half hour, so it is the one to run while iterating. It
found the anonymous-union lookup-home bug above on its first run (1069 errors
from 7 files); most of what it declines is capture, since re2 names a parameter
after the member it initialises throughout.

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

The two `*-const_placement_roundtrip` scenarios are the same shape for the
east/west qualifier move (`east_const`, then `west_const`). Unlike the
return-type pair these two directions are exactly as selective as each other —
both move the same qualifier runs — so `mini_repo` does declare
`EXPECT_ROUNDTRIP_IDENTICAL=1`; googletest does not, because the moved run is
re-emitted with single spaces and a corpus that size contains qualifiers
written `const    int`.

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

# Move cv-qualifiers to one side of the type they qualify: `east` rewrites
# `const int x` to `int const x`; `west` rewrites it back.  Only a qualifier of
# the type *specifier* moves -- the `const` in `int* const p` qualifies the
# pointer and stays put.
const_placement: east

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

Supported const placements: `east`, `west`.

Supported styles: `snake_case`, `_leading`, `trailing_`, `m_prefix`, `camelCase`, `UpperCamelCase`, `UPPER_SNAKE_CASE`, `kConstant`.

## Bazel integration ([bazel/cpp_format.bzl](bazel/cpp_format.bzl))

Runs `cpp_format` from the build graph so compile info and the full header set come from Bazel, not a committed `compile_commands.json`. This solves the sandboxing problem where one target's TU can't reach headers owned by other targets.

**How it works** — a per-target **aspect** (`cpp_format_aspect`) derives each `cc_*` target's compile flags from `CcInfo.compilation_context` + the toolchain (`cc_common.get_memory_inefficient_command_line(CPP_COMPILE_ACTION_NAME)`), and runs **one action per translation unit** (each `.cc`/`.cpp`/`.cxx`/`.c++` the target lists, deduplicated, first-party files only — **not** its headers, see "A header is not a translation unit" below): `cpp_format --config=cpp_format.yaml --emit-edits=<name>.cpp_format/<short_path>.json --owned-files=<name>.cpp_format/owned-files.txt <file> -- -x c++ <flags> -resource-dir=<staged builtin headers>`. Each action declares that one file **plus `compilation_context.headers` (transitive)** and `cc_toolchain.all_files` as inputs, exactly like `CppCompile` — that declaration is what makes headers reachable under sandboxing, and a `#include` of a sibling `.cc` fails the same way it fails the compile. First-party targets only (guarded by `ctx.label.workspace_name == ""`).

**A header is not a translation unit** — C++ has no way to compile a header, only to include it, and parsing one standalone assumes it is self-contained *in its own target's compilation context*. A build does not guarantee that and Bazel does not check it without `layering_check`. protobuf is where the assumption breaks: `arena_cleanup.h` ends with `#include "google/protobuf/port_def.inc"` while its target depends only on abseil (every TU that includes it has already pulled `:port` in), and `protobuf_headers` globs every header in the tree with no `deps` at all. Both parse fine as part of a real TU and fail as a TU of their own, which is how the aspect used to parse them.

So headers get no action. They are reached through `--owned-files` and rewritten by the TUs that include them, which is also where they are type-checked. Three consequences:
- **Emit mode widens what an action may edit** from "its own main file" to "any owned file" (`isRewritableFile()` in [tu_driver.h](cpp_formatting/tu_driver.h), used by all three passes). A direct run does **not** widen, and that asymmetry is load-bearing: a direct run merges whole rewritten *file contents* (`PendingRewrites` is path → content, last writer wins), so two TUs rewriting one header would silently keep one result; Emit mode merges *records*, which aggregation unions per file and dedups byte-identically. Measured on googletest, the duplication is 1.63 raw records per surviving edit.
- **A header-only target emits nothing**, and propagates its headers as owned exactly as before. What is lost is a header that *no* TU in the formatted set includes: nothing parses it, so nothing renames it — and nothing renames its members anywhere either, so the result is less reach, never a half-applied rename.
- **The spelling audit follows**, covering every owned file the TU read rather than the main file alone (Emit mode only, for the same reason). That makes it strictly more conservative: googletest's audit declines went 51 → 81 and its edits 2593 → 2576, because a header is now audited from the point of view of every TU that includes it.

Live record files dropped 141 → 91 on googletest and 87 → 51 on re2, and re2's `check` phase went 1m49s → 0m46s.

**Why one action per file** — Bazel parallelizes across actions, so with one action per *target* a big target (googletest's `gtest`, ~60 TUs) was a serial tail; a first attempt gave that action threads (`--jobs` + `resource_set`), but per-file actions are the model Bazel already has for compilation: they parallelize on every executor including remote ones, and they cache per file, so editing one `.cpp` re-parses one file instead of its whole target. Records were always merged across processes by aggregation (that is how cross-target renames, dependent tokens and vetoes already worked), so splitting a target's TUs across processes needs no new merge logic. The owned list — the target's own sources plus the dep closure's headers — is written once per target with `ctx.actions.write` (multiline `Args`, the closure can be large) and shared by all of that target's actions. The binary's own `--jobs` pool is for direct runs; the aspect never passes it (a one-file action runs inline).

**The target's own `copts` are passed** — `_compile_flags()` appends `ctx.rule.attr.copts` (minus any flag still carrying a make variable or `$(location)`) to the fragment's `copts`/`cxxopts`, so the tool parses each file under the preprocessor conditions the compiler uses. abseil's `randen_hwaes.cc` sits behind `#if ABSL_HAVE_ACCELERATED_AES`, which only the target's `-maes -msse4.1` turns on; without them the tool looked at an empty file, renamed `RandenTraits::kFeistelBlocks` from the header, and the real compile broke where the tool had seen nothing. `defines` and `local_defines` already arrive through the compilation context.

**`textual_hdrs` are declined, not renamed** — the aspect passes each target's `srcs` and `hdrs`; a file listed in `textual_hdrs` is in neither, so it is never parsed as its own TU and never named as an owned file. It *is* parsed as part of every TU that includes it, so the tool sees its references — and since nothing would ever rewrite them, the scan pass vetoes the declaration they name (shape (c) under "Known non-obvious behaviours"): the member keeps its name everywhere and the skip is reported. abseil's `absl/log/log_basic_test_impl.inc` naming `LogEntry::kNoVerbosityLevel` is the measured case; it used to be a build break. Actually renaming such a member would mean rewriting the `.inc` from one of the TUs that include it, which the per-file ownership model does not do; a textual header is not self-contained (that is why it is not a `hdr`), so it cannot be given an action of its own either.

**Cross-target renames (`--owned-files`)** — the tool's `FileSet` decides which *declarations* may be renamed, while the source list decides which files are *parsed as TUs*. If a target's action only knew its own sources, a `cc_binary`'s use of a `cc_library` member would be invisible to the rename (the member's declaration lives in the dep's header, outside the FileSet) — the member would be renamed by the library's own action and every use site in dependents left behind, i.e. a build break. So `CppFormatEditsInfo` carries a second field, `headers` (the dep closure's first-party header sources, propagated over `deps` alongside `records`), and the aspect passes it as `--owned-files`. That flag widens the FileSet **without** widening the source list, which is what keeps the model sound: `ApplyRenamesVisitor` only rewrites its own main file, so each file is still rewritten by exactly one action (its own) — no duplicate or conflicting records — while *uses* are rewritten wherever they appear. With per-file actions the same flag also carries the target's *own* other sources, since they are no longer in the action's source list. It also lets a dependent resolve a template-dependent token in a dep's header from its own instantiations. Only headers are propagated across targets (a dep's `.cpp` is unreachable from this TU), and a `no-cpp-format` target contributes none — its declarations are never renamed, so a dependent must not rename their uses. The list is a file the target writes once (a dep closure can be large enough to blow the command-line limit), and the flag must precede the `--` or it would be parsed as a compile flag. **The aspect and the binary are versioned together**: an older published binary rejects `--owned-files` and `--aggregate --records-from`, so bumping `cpp_format.release(version=…)` in [MODULE.bazel](MODULE.bazel) is part of shipping an aspect change like this.

**Records, not diffs** — the emit action writes offset-level edit records (`{file, offset, length, old, new, owner_file, owner_offset}`) plus a template-dependent-token resolution sidecar (see "template-dependent member tokens"; each of its records also carries the index of the **rule** that produced it, because every rule looks at every dependent token and the rules disagree by construction — the one that owns the member the token resolves to records a name and the others record a veto, and merging is veto-absorbing, so pooling them under one `(file, offset)` key lets one rule cancel another's rename and leave the dependent use spelling the old name) a `vetoes` list (see "names spelled through macros": a rename's `owner_*` names the declaration it belongs to, and aggregation drops every edit whose owner any target vetoed) and a `skips` list (reporting only — see "Reporting what was skipped" below). Textual diffs can't be merged (hunk offsets don't compose); records can. Aggregation drops everything belonging to a vetoed declaration, unions all targets' records, resolves dependent tokens across TUs (agree → edit, veto/disagree → dropped), merges per file (dedup identical, flag overlapping-distinct conflicts), and renders/applies via `emitUnifiedDiff`. This is the clang-tidy `--export-fixes` + `clang-apply-replacements` model. The rename↔trailing-return overlap (a member renamed inside a `decltype(...)` return type that trailing-return also rewrites) is handled in [trailing_return_types_lib.cpp](cpp_formatting/trailing_return_types_lib.cpp): the trailing-return edit subsumes (drops) rename records inside its range, since its `-> type` text already carries the rename. The aggregation logic lives in one place — `runEditAggregation()` in [lint_lib.cpp](cpp_formatting/lint_lib.cpp) — exposed by two thin CLIs: the standalone [aggregate_edits](cpp_formatting/aggregate_edits.cpp) binary (used by the from-source aspect) and `cpp_format --aggregate` (so the single published binary is self-sufficient — see the prebuilt kit below). `cpp_format --aggregate` is dispatched before `CommonOptionsParser` in [cpp_format.cpp](cpp_formatting/cpp_format.cpp) and hand-parses `--apply`/`--check`/`--report-rename-conflicts`/`--root`/`--records-from` (it has no source paths or `--` compile args). Both CLIs take record files positionally or through `--records-from=<file>` (newline-separated, `appendRecordListFrom()` in `lint_lib`): a repository's worth of per-file records does not fit on a command line (macOS caps `ARG_MAX` at 1 MB), so the generated `.check`/`.diff`/`.fix` scripts and `cpp_format.sh` always pass a list file.

**Reporting what was skipped** — a rename the tool declines is the one outcome the diff cannot show: it is precisely a change that is *not* there. A direct run prints it to stderr (`reportRenameConflicts()`), but under the aspect the run that found the skip is a different process from the one that renders the change, and Bazel hides a successful action's stderr. So every emit action serializes its `RenameConflicts` into the record's `skips` array, and `runEditAggregation()` reports the repository's whole set in all three modes — a one-line count always, every site with `--report-rename-conflicts` (which `cpp_format.sh` and the generated `.diff`/`.fix` scripts pass through). `skips` is **reporting only**: a skip means no edit was emitted in the first place, and a veto that has to drop *another* action's edit travels in `vetoes`, as before. Aggregation adds one skip of its own — a dependent token no report resolved, whose spelling is then declined everywhere (`mergeEditReports`'s `Declined` out-param), because no single action can see that nobody resolved it. One declaration is reported once: the same decl is seen by every action that includes it, and can be declined twice over (by a check that knows the new name and by a backstop that only knows the spelling), so `reportRenameSkips()` collapses on `(file, line, column, old name)` and keeps the entry that names the new name — that one carries the more specific reason.

**Rules** — `cpp_format_targets(name, deps)` generates four targets (see [bazel/testdata/BUILD.bazel](bazel/testdata/BUILD.bazel) for the demo):
- `<name>.check` — a **test** (hermetic lint gate): `mergeEditReports` counts edits without reading sources; exit 1 if any. `bazel test //…:<name>.check`.
- `<name>.diff` — `bazel run` prints the merged git-apply-able unified diff (review artifact).
- `<name>.fix` — `bazel run` applies edits in `$BUILD_WORKSPACE_DIRECTORY` (outside the action graph, since Bazel actions can't mutate sources).
- `<name>.compile_commands` — `bazel run` writes a `compile_commands.json` for the deps (transitively) in `$BUILD_WORKSPACE_DIRECTORY` (a positional arg overrides the path). Also exposed standalone as the `cpp_format_compile_commands(name, deps)` rule.

**`compile_commands.json` (no second Bazel dependency)** — the aspect already derives each target's compile command, so it also writes it out as a `<name>.compile_commands.jsonl` fragment: one JSON object per source file (`{file, directory, arguments}`, `arguments` = the toolchain's compiler from `cc_common.get_tool_for_action`, `-x c++`, the derived flags minus the gcc-only `-fno-canonical-system-headers`, `-c <file>`), written by `ctx.actions.write` — no tool runs, nothing is compiled. It is written for every first-party target with sources, `no-cpp-format` or not (an IDE wants the whole repo), and carried in the provider's `compile_commands` field and the `cpp_format_compile_commands` output group; that group also holds the target's transitive headers, so building it materializes the generated headers the entries mention. Two values are only known at run time and are left as placeholders the merger fills in: `directory` = `__EXEC_ROOT__` (the execution root, where `-iquote .`, `bazel-out/...` and `external/...` all resolve — no `external`/`bazel-out` symlinks are planted in the workspace, and includes realpath back into it through the exec root's source symlinks) and `file` = `__WORKSPACE__/<short_path>` (the absolute workspace path an editor opens; clangd matches entries by that path). The merge (`merge_compile_commands`, a bash function kept verbatim in both `_MERGE_COMPILE_COMMANDS_SNIPPET` in the `.bzl` files and `cpp_format.sh`) dedups by `file` (first entry wins) and writes the array atomically. The `.compile_commands` script derives the exec root from `RUNFILES_DIR` (the runfiles tree sits inside bazel-bin, inside the exec root) and puts the transitive headers in its `DefaultInfo.files` rather than runfiles (which would symlink every one); `cpp_format.sh compile_commands` builds with `--output_groups=cpp_format_compile_commands` (no `+`, so the default outputs and the emit actions are *not* built), reads each target's fragment at `<bazel-bin>/pkg/name.compile_commands.jsonl`, and writes `compile_commands.json` in the workspace root (`COMPILE_COMMANDS_OUT` overrides). The command is the target's, so per-target `copts` are missing from it exactly as they are from the emit action. [bazel/testdata/compile_commands_test.sh](bazel/testdata/compile_commands_test.sh) runs the demo's generated script the way `bazel run` would and has `cpp_format -p` consume the result: a copy of `demo_main.cpp` alone in a scratch workspace can find `demo.h` only through the entry's exec-root-relative `-isystem`, so the test proves both placeholders and the flags.

`--config=lint` in [.bazelrc](.bazelrc) runs the aspect and materializes each file's records and each target's manifest (`bazel build --config=lint //…`) for inspection/CI; the failing gate is the `.check` test.

**Non-obvious behaviours** — each target also writes `<name>.cpp_format.manifest` (exec-root-relative paths of its current record files, in the `cpp_format_edits` output group) and `cpp_format.sh` reads that rather than globbing `<name>.cpp_format/`: Bazel never deletes the record of a source that was removed from a target, and a stale record would apply stale edits. Record file keys are the source's **real path**, which under Bazel resolves to the absolute workspace path (source symlinks) — stable across actions/sandboxes; `aggregate_edits` uses absolute keys directly and joins relative keys with `--root`. `-x c++` is forced so headers parse as C++ (not C). `-resource-dir` is derived from the staged `@llvm-project//clang:builtin_headers_gen` paths (robust to a module-extension rename). The aspect needs `cpp_format` in the **exec** configuration (a one-time from-source Clang/LLVM build). Portability caveat: absolute paths in records make remote-cache reuse across machines suboptimal.

### Prebuilt-binary integration kit ([bazel/integration/](bazel/integration/))

A variant of the integration for external repos that want the lint/fix gate **without** building Clang/LLVM from source. It is documented for end users in the README quick start. Consumed two ways: **imported by URL** — `bazel_dep(name="cpp_formatting") + archive_override(urls=[…github…/archive/refs/tags/<tag>.tar.gz])`, then `load("@cpp_formatting//bazel/integration:cpp_format.bzl", …)` and `use_extension("@cpp_formatting//bazel/integration:extensions.bzl", "cpp_format")` — or by copying the directory in. Importing by URL does **not** build LLVM: the `//third_party/llvm` extension in this module is lazy and the kit never references `@llvm-project` (verified — `bazel build @cpp_format_bin//:cpp_format` from a consumer touches no LLVM). Files:

- [bazel/integration/cpp_format.sh](bazel/integration/cpp_format.sh) — the **ergonomic entry point**: `cpp_format.sh <check|diff|fix|compile_commands> [pattern] [flags]` (pattern defaults to `//...`; any argument starting with `-` is passed through to `cpp_format --aggregate`, `--report-rename-conflicts` being the one worth knowing; `compile_commands` takes no flags and writes the compilation database before the binary is even resolved — see above). It `bazel query`s the first-party `cc_*` targets under the pattern (`except attr(tags, 'no-cpp-format', …)`), `bazel build`s them with `--aspects=…%cpp_format_aspect --output_groups=+cpp_format_edits` to emit each file's records, reads each target's manifest at its deterministic path (`//pkg:name` → `<bazel-bin>/pkg/name.cpp_format.manifest`, skipped if absent — source-less targets write none) to collect the record paths (exec-root relative, joined with `bazel info execution_root`), and runs `cpp_format --aggregate --records-from=<list>` over them. This is what lets users format the **whole repo with no per-target wiring**. It is a plain script, **not** a `bazel run` target, so it can invoke `bazel build` without nesting a Bazel server in a running one (which would deadlock on the workspace lock). `ASPECT`/`BIN_LABEL` are overridable via env (`CPP_FORMAT_ASPECT`/`CPP_FORMAT_BIN_LABEL`) so the same script drives the in-repo from-source labels for dogfooding. The query excludes `cpp_format_targets` rule targets, so the aspect is applied to each source target exactly once (two applications collide on the shared record files). Consumers get it via `bazel run @cpp_formatting//bazel/integration:install` (see the `cpp_format_install` rule below), which places it in `$BUILD_WORKSPACE_DIRECTORY` (default `tools/cpp_format.sh`) with the **canonical** aspect label baked into its `ASPECT` default — `expand_template` substitutes the third-party placeholder with `str(Label(":cpp_format.bzl")) + "%cpp_format_aspect"`, which resolves to `@@<cpp_formatting-canonical>//…` in the consumer's graph and is valid from their command line — so the placed script runs with no env var. The `install` launcher itself only copies a file (no nested `bazel`), so it *is* a safe `bazel run` target.
- [bazel/integration/extensions.bzl](bazel/integration/extensions.bzl) — a Bzlmod `module_extension` (`cpp_format`) with a `release(version, base_url, sha256)` tag class. Its repo rule detects the host OS/arch (`rctx.os`), downloads the matching release asset (`cpp_format-{linux-x86_64,linux-aarch64,darwin-aarch64,windows-x86_64.exe}`) into a `bin/` subdir, and exposes it as `@cpp_format_bin//:cpp_format`. **The download target file must not share the `cpp_format` filegroup's name** — a same-name `src` is a self-edge cycle (hence `bin/`). `@cpp_format_bin` is **one** repo shared by every module that uses the extension, so exactly one `release()` tag can win and `_ext_impl` picks the **root module's** (`mod.is_root`); under URL import cpp_formatting is itself a dependency module whose own `release()` would otherwise replace the consumer's pin, handing them a binary older than the aspect driving it — the same root-vs-dependency trap as `_config`'s `@@//:cpp_format.yaml` default. This repo's own `release()` tag in [MODULE.bazel](MODULE.bazel) is therefore issued through a second, `dev_dependency = True` proxy of the extension (Bazel merges the proxies into one usage and drops the dev one when the module is not the root; a tag cannot carry `dev_dependency` itself), so as a dependency it contributes no tag at all (the root-wins rule stays as a guard for vendored copies). Only the *pin* is dev: the `use_extension`/`use_repo` pair must stay a normal dependency, because the kit's `.bzl` names `@cpp_format_bin//:cpp_format` and that label resolves through cpp_formatting's own repo mapping even under URL import — marking that use dev would drop the mapping and break every consumer. The README tells consumers to take the latest release tag and to mark *their* `bazel_dep` and `use_extension` dev (they are the root, so their dev tags apply); this repo's pin only matters when it is the root, and is bumped after each aspect change ships (releases are cut on every push to `main`, tagged `<date>-<shortsha>`).
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

### `const_placement`: which qualifier moves, and why

`const int x` and `int const x` declare the same thing, so moving the qualifier
across the type specifier is a pure formatting change. `int* const p` is a
*different type* from `const int* p`, so moving that one is not. The whole pass
turns on telling those apart, and Clang already does it: a `QualifiedTypeLoc`'s
qualifiers belong to whatever its **unqualified loc** is, and those locs nest
the way the declarator reads.

| Written | TypeLoc nesting | What the `const` qualifies |
|---|---|---|
| `const int* p` | `Pointer( Qualified{const}( Builtin int ) )` | the `int` — movable |
| `int const* p` | the same | the `int` — movable |
| `int* const p` | `Qualified{const}( Pointer( Builtin int ) )` | the *pointer* — declined |

So `isPlainTypeSpecifier()` walks the unqualified loc's `getNextTypeLoc()` chain
and declines the whole node if any component is a pointer, reference, array,
function type or paren — the same shape test `isMovableTypeShape()` makes for
the return-type pass. Declining those also keeps the rewrite's source range
honest: an array or function declarator's TypeLoc range runs *past* the type
specifier and over the declarator-id (`const int a[3]`'s `ConstantArrayTypeLoc`
spells `int a[3]`), so appending ` const` to its end would produce
`int a[3] const`. Every loc that survives the filter spells the type specifier
and nothing else.

A member function's own `const` (`int f() const`) is not a `QualifiedTypeLoc` at
all — it lives on the `FunctionProtoType`'s method qualifiers — so the pass
never sees it, and `const int f() const` moves exactly one of its two `const`s.

**Finding the written qualifier.** `QualifiedTypeLoc::getSourceRange()` covers
only the type specifier; the qualifier keywords are outside it on one side or
the other, exactly as they are for `skipQualifiersBackward()` in the return-type
pass. `scanCvRunBackward`/`scanCvRunForward` scan the raw buffer for the run of
`const`/`volatile` keywords adjacent to the specifier. Consequences of scanning
the buffer rather than the AST, all deliberate:

- A qualifier the preprocessor produced is never found, because the buffer
  spells the macro name (`#define CONST const` leaves `CONST int x` alone), and
  a specifier whose own location is a macro expansion is declined outright.
- A run is only recognised when it is *adjacent* to the specifier. `const static
  int x` is left alone (`static` is not a cv keyword), as is
  `const /*why*/ int x` — whitespace is skipped, comments are not.
- The run is re-emitted with single spaces, so `const    int` comes back as
  `int const`. That is the one thing an east→west round trip cannot undo.

**Whole runs move, in source order.** `const volatile int x` becomes
`int const volatile x`; a declaration with qualifiers on *both* sides
(`const int volatile x`) gathers them on the requested side. `volatile` moves
with `const` because the style is about where cv-qualifiers go, and leaving
`volatile int` next to `int const` would be neither style.

**One contiguous replacement per move, innermost first.** The move is a single
`ReplaceText` spanning the qualifier run *and* the specifier, not an insert on
one side plus a delete on the other. Two things force this:

- Where the pass meets the return-type pass in `cpp_format`, an insertion
  sitting exactly on the boundary of the range `runToTrailing()` lifts is
  neither carried into the moved text nor removed by the replacement, and
  `const int Get()` came out as `auto const Get() -> int`. A replacement of the
  same byte range composes instead — the two passes agree on that range because
  `skipQualifiersBackward`/`Forward` find the same run this scan did.
- Qualified types nest (`const std::map<std::string const, T const*>` is three
  of them, and the outer one's specifier spans both inner ones), so the visitor
  handles children **before** the node (`TraverseQualifiedTypeLoc` recurses
  first). An outer move applied first would write a specifier text that the
  inner moves then edit in the middle of, giving `std::stconst ring`. Handling
  the innermost first means each outer move reads its specifier back with the
  inner results already in it, via `getRewrittenText()`.

Two smaller sharp edges, both about the closing `>` of a nested template
argument list: the lexer reads `>>` as one token that the parser splits, so the
inner list's end location is an *expansion* location (mapped back with
`getFileLoc()`), and measuring a token there runs one character long and
swallows the enclosing list's bracket. Hence every range in the pass is a
**character** range over offsets computed by hand, and the end offset is
`+1` whenever the last token spells `>`. Requiring that character to be `>` is
also what keeps the `getFileLoc()` path from being a macro hole: a type whose
end really comes from a macro body maps to the macro's own name token.

### Execution model: parallel TUs (`tu_driver`)

Every translation unit is parsed by its own `ClangTool` on a worker thread (`--jobs`, default every CPU) and writes only into its own `TUSlot`. Nothing is shared while a TU runs; what other TUs need is exchanged at barriers by the driver thread, always in **source order** (non-headers in the order given, then headers in the order given), which is what makes the output byte-identical for any `-j`:

- **Two phases.** Non-headers run first, then headers. Between them, the dependent-token resolutions every `.cpp` recorded are merged into `CrossTUState` and each header TU is **seeded** with a copy (its slot's `Vetoes`/`DepRes` start as copies of the shared maps). This is the ordering `orderSourcesForRename()` always imposed — a header is where a dependent token is spelled, the `.cpp` files are where it is resolved — so the common case needs one parse per TU.
- **Rule (a): re-run on a veto.** A veto found in one TU invalidates what another TU already buffered. When a pass discovers any new veto (checked at each phase barrier, so the header phase is skipped on a restart), every slot is discarded, the shared `DepRes` maps are **cleared**, `Vetoes` are kept, and everything runs again (`MaxFullPasses` = 4, as `runWithVetoRerun` did; the cap now warns). `DepRes` must be cleared because the recorder is gated on `!Renames.empty()`: a member vetoed in pass 2 can leave a TU with no renames, whose stale `HasName` entry no TU would overwrite with a veto — the header token would be rewritten while the member keeps its name.
- **Rule (b): re-run a stale TU.** After a pass with no new vetoes, a TU is stale if any shared entry whose key file is its own main file differs (in `HasName`/`NewName`/`Vetoed`) from its slot's post-run map (= seed + own records, exactly what its applier consulted; `dependentResolutionsDifferFor`). Only those slots are re-run, with the shared maps **kept** (complete once no new vetoes appear) — one round converges. This is what makes resolution independent of the source order: a header instantiated only from another header, or a token spelled in `a.cpp` and instantiated only from a `b.cpp` that includes it, were silently order-dependent before.
- **Emit mode seeds nothing** (`crossTUSeeding()` false) and never re-runs (`rerunNeededOnVeto()` false): every TU is independent, aggregation drops the edits and resolutions of vetoed owners, and the JSON is identical for any `-j` and any source order. The shared maps are still merged for the sidecar.
- **Debug mode** (`--debug-trace`) forces serial, unbuffered output. The serial path (`-j1` too) leaves Clang's own stderr printer in place, so its diagnostics are exactly the old ones; parallel batches buffer them per TU (`SlotActionFactory::runInvocation` redirects the verbose stream and builds the printer from the invocation's `DiagnosticOptions`) and replay them in slot order.
- Per-TU outputs (`Pending`, `Edits`, `Conflicts`, `Report`) are merged only once, at the end, by the client's `finish()`, so a re-run TU simply replaces its slot. `LintReport` sorts on output; `reportRenameConflicts` dedups; every other collection is a `std::map` or appended in slot order.
- **Key spaces:** `Vetoes` keys are cwd-relative (`scan()`, `vetoed()`), `DependentResolutions` keys are absolute real paths (`locKey`). The staleness check compares against `sys::fs::real_path(Source)`, never a relativized path.
- Workers are `llvm::thread`s with explicit 8 MiB stacks — a pool's default stack is 1 MiB on Windows and Clang's parser/Sema/`RecursiveASTVisitor` recurse deeply. Each `ClangTool` gets `llvm::vfs::createPhysicalFileSystem()`, so the per-compile-command chdir inside `ClangTool::run` is per instance and the process cwd never changes (`relativizeToCwd()` caches it). Compile commands containing `-mllvm` would parse `llvm::cl` options concurrently — unsupported with `-j > 1`.

### `normalize_variables`: cross-file renaming

The tool processes one translation unit at a time (many at once, but each in isolation). To rename declarations in a header alongside their uses in a `.cpp`, the source list must include both files — but the order does not matter: the driver runs every non-header before any header, so every `.cpp` TU is parsed against the original on-disk header content.

Edits are buffered per TU (`TUSlot::Pending`), merged into `RenameActionFactory::Pending` (a path → content map) by `finish()`, and only committed by `flush()` after `runTranslationUnits()` returns. This is what makes multi-file in-place renaming correct: every TU compiles against the original on-disk source regardless of how many headers are in the list.

`FileSet` (in `rename_variables_lib.h`) holds the real absolute paths of all source files. `CollectRenamesVisitor` collects declarations from any file in the set (not just the main file), enabling the header's declarations to be found when compiling the `.cpp`.

### `normalize_variables`: template-dependent tokens

Two spellings need this, and they share one mechanism:

- **A member access** — `x.val` where `x` is dependent (`CXXDependentScopeMemberExpr`).
- **A qualified name** — `Helper<T>::val` (`DependentScopeDeclRefExpr`), which is a *name* whose lookup is deferred, not an access on an object. Its resolved form in an instantiation is an ordinary `DeclRefExpr`, so the recorder matches on that. googletest's `&(TypeIdHelper<T>::dummy_)` is the motivating case.

A member accessed through a template parameter — e.g. `x.val` in `auto set_val(auto& x) { x.val = 12; }` (or the explicit `template <class T> void set_val(T& x)`) — is a **dependent** expression (`CXXDependentScopeMemberExpr`): which member `val` names is unknown until the template is instantiated, and the instantiations usually live in the `.cpp` files, not in the header that spells the token. The per-main-file `Rewriter` model can't rename it on its own — the header's own TU never instantiates the template, and an instantiating `.cpp`'s TU doesn't rewrite the header.

The tool bridges this with a **cross-TU resolution map** (`DependentResolutions` in `rename_state.h`), keyed by the token's `(real path, byte offset)`. Each TU works on its own copy (seeded from the shared `CrossTUState::DepResPerRule` — one map **per rule**, so a token resolved by one rule is never re-applied by another) and the driver merges every TU's copy back at the barriers with `mergeDependentResolutions()`. Per TU, `runRenameRuleOnAST` does three things:

1. `DependentTokenCollector` (cheap, no instantiations) finds dependent token locations in owned files — both spellings above. If there are none, the rest is skipped — the feature is zero-cost for non-template code. A token written as a macro *argument* counts: it is spelled at the call site, and `ownedKey()` keys on the spelling, so the collector, the recorder and the applier all agree on that location. This is what makes googletest's `EXPECT_FALSE(this->table_)` inside a `TYPED_TEST` renameable — the single largest class of missed references that corpus had.
2. `RecordDependentResolutionsVisitor` (`shouldVisitTemplateInstantiations() == true`) walks this TU's instantiations and, for each resolved member access (`MemberExpr`) or qualified reference (`DeclRefExpr`) landing on a known dependent-token location, records the new name it resolves to. A binding to a member that is **not** being renamed (e.g. a type outside the `FileSet`) or a disagreement between instantiations **vetoes** the location.
3. `ApplyRenamesVisitor::VisitCXXDependentScopeMemberExpr` / `VisitDependentScopeDeclRefExpr` rewrite the token in its **own** main-file TU using the agreed name (skipping vetoed/unresolved entries).

Because the driver runs headers after every other source, every instantiating `.cpp` TU runs before the header TU that consumes its resolutions; a TU whose tokens were resolved by a TU of the *same* phase is re-run (rule (b) above), so the order within a phase does not matter either. `ApplyRenamesVisitor` itself keeps `shouldVisitTemplateInstantiations() == false`, so all non-dependent paths are unchanged — the feature is purely additive. **Soundness is bounded by the passed source set:** an instantiation in a TU that is *not* passed to the tool is invisible, so its member may be renamed while its dependent use is missed; conversely a visible out-of-scope binding is vetoed conservatively (leaving the token), which can produce an incomplete rename. Both cases surface as a compile error on the next build — the intended review gate — rather than a silent miscompile. Full all-or-nothing propagation (skipping a member's declaration rename when a dependent use can't be safely rewritten) is a possible future hardening for *those* two cases; the third — a dependent token with no rewritable spelling at all — is already handled that way by the macro veto above.

### `normalize_variables`: names spelled through macros

Whether a reference can be renamed depends on what the preprocessor did to the token, which `ApplyRenamesVisitor::rewriteLoc()` decides with one test: `SM.getFileLoc(Loc) == SM.getSpellingLoc(Loc)`. `getFileLoc()` walks macro-*argument* levels down to the spelling and macro-*body* levels up to the expansion, so the two agree exactly when the token was typed by the user.

| Reference | `rewriteLoc` | Outcome |
|---|---|---|
| `c.itemCount` (ordinary) | the token | renamed |
| `FWD(c.itemCount)` (macro **argument**) | the call site | renamed — the token is written once, in ordinary source |
| `OUTER(c.itemCount)` (argument forwarded through macros) | the call site | renamed — the walk is recursive |
| `TWICE(c.itemCount)` (argument expanded twice) | one call site, two AST nodes | renamed **once** (`renameAt` dedups by `(FileID, offset)`) |
| `struct S { FIELD(itemCount) };` (declaration as an argument) | the call site | renamed (`shouldCollect` resolves through the expansion) |
| `EXPECT_FALSE(this->itemCount)` in a class template (**dependent**, in an argument) | the call site | renamed via the dependent-token path, which keys on the spelling |
| `#define BUMP(c) ((c).itemCount += 1)` (macro **body**) | invalid | **vetoes the rename** |
| `c.PASTE(item)` where `#define PASTE(p) p##Count` | invalid (`<scratch space>`) | **vetoes the rename** |

**Renaming is all-or-nothing.** A macro body spells its token once, at a location every expansion shares — expansions need not agree on what it names, and rewriting it would change all of them. A `##`-pasted token exists only in Clang's scratch buffer, so no byte range of any file holds the name. Neither can be rewritten, and renaming the declaration without them does not compile. So one unrewritable reference vetoes the declaration, which keeps its old name everywhere and is reported through the existing `RenameConflict` channel (`--report-rename-conflicts` for the sites). Turning a build break into a reported skip is the point: the tool declines the rename instead of half-applying it. The scan deliberately ignores file ownership — a macro in a third-party header that names one of our members is just as fatal and just as unrewritable.

The scan pass is the one place `ApplyRenamesVisitor` sets `shouldVisitTemplateInstantiations() == true`. It has to: a **dependent** token spelled in a macro body has no `MemberExpr` in the pattern at all — which member it names is known only in an instantiation — so nothing else would ever discover that the member has an unrewritable reference, and it would be renamed at its declaration with the macro left behind. The rewrite pass keeps `shouldVisitTemplateInstantiations() == false` as before, since instantiation locations point back into the pattern, which is rewritten once through the pattern's own nodes.

`RenameVetoes` is keyed by the declaration's `(real path, byte offset)`, stable across TUs *and* processes. For a virtual function the key is the **base-most** declaration of the override hierarchy (`renameOwnerKey()`), so a veto found on any override suppresses the whole family — `collectOverrideFamily()` walks upwards only, so every override agrees on that key without needing to see its siblings.

Two mechanisms make the veto order-independent, one per driver path:

- **Direct runs** (`normalize_variables`, `cpp_format` without `--emit-edits`): a veto discovered while processing one TU can invalidate a rename another TU already buffered, and a `Rewriter`'s edits cannot be taken back. Nothing reaches disk before `flush()`, so the driver discards the whole pass and runs again with the vetoes known (rule (a) in "Execution model"). It costs nothing when no macro names anything being renamed, which is the common case, and nothing in Emit mode either (`rerunNeededOnVeto()` is false there — records carry their own vetoes, so aggregation does the dropping and the Bazel path never pays for a second parse). Per TU, `runRenameRuleOnAST` runs `ApplyRenamesVisitor` twice — `ApplyMode::Scan` collects vetoes and rewrites nothing, then `ApplyMode::Rewrite` applies what survived — so within one TU no edit is ever buffered before every reference in it has been seen.
- **The Bazel path**: each target's action is its own process, and the library's action never sees the macro its dependent expands. So every rename `EditRecord` (and every `ResolutionRecord`) names its owning declaration in `owner_file`/`owner_offset`, each report carries a `vetoes` list, and `mergeEditReports` drops every edit whose owner *any* report vetoed. That is what stops the library's action renaming a member while the binary's action is left spelling the old name.

**The bound is the same one the dependent-token feature has:** a veto can only come from a TU that is actually parsed. A macro expanded only in a repo you do not format, in a `no-cpp-format` target, or under an `#if` branch this build does not take is invisible, and its reference is missed. As before, that surfaces as a compile error on the next build rather than a silent miscompile.

### Known non-obvious behaviours

- **`QualifiedTypeLoc` gap** — Clang's `QualifiedTypeLoc` does not include leading `const`/`volatile`/`restrict` in its source range. `skipQualifiersBackward()` scans the raw source buffer leftward.
- **Token merging guard** — When there is no whitespace between the return type and the function name (e.g. `Foo&operator=`), `"auto "` (with a trailing space) is emitted to prevent token merging.
- **`FunctionTypeLoc::getLocalRangeEnd()`** — For member functions with cv/ref/noexcept qualifiers, Clang sets this to the location of the last qualifier.
- **Pointer-to-member** — `&S::field` produces a `DeclRefExpr` with `FieldDecl` (not `VarDecl`). `VisitDeclRefExpr` handles both cases.
- **Constructor mem-initializers** — `S() : val_(0) {}` is a `CXXCtorInitializer`, not a `Stmt` or `Decl`, so it is not visited by the standard `Visit*` callbacks. `ApplyRenamesVisitor` overrides `TraverseConstructorInitializer` and rewrites at `getMemberLocation()`.
- **Designated initializers** — `S s{.val_ = 0}` stores the field name in the `Designator` of a `DesignatedInitExpr`, not a `MemberExpr`. `VisitDesignatedInitExpr` rewrites field designators at `getFieldLoc()`.
- **Template instantiation** — the rename map is keyed by the *pattern's* declaration, so every instantiated declaration has to be mapped back to it. A `FieldDecl` has no back-pointer to what it was instantiated from, so `primaryTemplateMember()` matches by field index against the pattern record returned by `instantiationPattern()`, recursing to walk a chain of nested instantiations. That pattern comes from three different places: `Outer<int>` is a `ClassTemplateSpecializationDecl` (→ the primary template's `CXXRecordDecl`); `Action<T*>` instantiated is *also* one, but its pattern is the **partial specialization** (`getSpecializedTemplateOrPartial()`), a different class with a different member list; and `Outer<int>::Inner` — a class *nested* in a class template — is an ordinary `CXXRecordDecl` (→ `getInstantiatedFromMemberClass()`). An explicit specialization is not an instantiation at all (`TSK_ExplicitSpecialization`) and is its own pattern. Because fields are matched **by index**, getting the pattern wrong is worse than missing a rename: it rebinds a use to an unrelated member (`a.otherValue` → `a.primary_field`). Handling only the first silently renamed such a field's declaration while skipping every use, and vetoed every dependent use for binding to a member "not being renamed". Member functions and static data members need none of this: `getInstantiatedFromMemberFunction()` and `getInstantiatedFromStaticDataMember()` are declaration-level back-pointers that already cross nesting.
- **Macro-spelled names** — a reference written as a macro *argument* renames normally (its spelling is the call site); one spelled in a macro *body*, or formed by `##`, vetoes the rename entirely. See "names spelled through macros" above.
- **Template-dependent tokens** — `x.val` where `x` is a template parameter is a `CXXDependentScopeMemberExpr` with no resolved member, and `Helper<T>::val` is a `DependentScopeDeclRefExpr` with no resolved declaration; it is renamed via the cross-TU `DependentResolutions` map populated from instantiations (see "template-dependent member tokens" above), not by the ordinary `Visit*` paths. Only the `member`/`method` scopes are affected. `ApplyRenamesVisitor` deliberately does **not** visit template instantiations, so resolved member accesses inside instantiations are never double-rewritten (their source locations point back into the pattern, which is rewritten once via the dependent-token path).
- **Member-function scope (`Method`)** — constructors, destructors, conversion functions, and overloaded operators are never renamed (`isRenamableMethod()`); their names are not plain identifiers. A virtual function is renamed together with its entire override hierarchy (`collectOverrideFamily()`); if any function in the hierarchy is declared outside the `FileSet`, the rename is skipped entirely so `override` checking can never be broken.
- **Name collisions are skipped, not applied** — if a rename's new name is already taken in the *same* scope, `CollectRenamesVisitor::collides()` drops the rename entirely (declaration and every use) and records a `RenameConflict`. Renaming into an occupied name is not a formatting change: at best it fails to compile, at worst it silently rebinds uses to the other entity. The name is looked up in the declaration's **lookup home** — the nearest enclosing record that is not an anonymous struct or union (`lookupHomeOf()`) — because that is the scope C++ resolves it in: re2's `Regexp` keeps its variant fields in anonymous structs inside an anonymous union and has an accessor of the same name on the class for each (`runes_` next to `Rune* runes()`), and consulting the anonymous struct finds nothing, so each of those renames produced a class declaring a field and a method of one name. A sibling anonymous struct's field is visible in that class only as an **implicit `IndirectFieldDecl`**, so that one kind of implicit declaration must not be skipped — it is the only way the sibling's name appears at all. Nothing beyond the lookup home is consulted: shadowing an inherited or outer-scope name is legal C++ and still allowed (what makes it *unsafe* at a particular use is a separate check, below). Two functions may share a name (that is an overload set) unless the signatures match, which would be a redeclaration; two data members, or a member and a method, may not. A second declaration renaming to a name a first already claimed is skipped too. `reportRenameConflicts()` prints a one-line count; `--report-rename-conflicts` (on `cpp_format` and `normalize_variables`, and on both aggregation CLIs — see "Reporting what was skipped") lists every site — the same channel also reports renames vetoed by an unrewritable macro reference. googletest's `RE::pattern_`/`RE::pattern()` is the motivating case.
- **The collision check does not span rules, so unsound combinations are refused up front** — `collides()` asks whether the new name is taken in the `DeclContext` *as the AST spells it*, and `Claimed` lives on one `CollectRenamesVisitor`. Two rules of one `cpp_format` run therefore decide in ignorance of each other, and the result is not a reported conflict but a silent miscompile: with `member: snake_case` + `method: snake_case`, `class Widget { int Value() const { return value_; } int value_; };` becomes `int value() const { return value; }` next to `int value;`. That is the getter/field pair, so on a real corpus it is not an edge case. Since the configuration is the only cheap place to catch it, `cpp_format` validates the rule list before parsing anything and exits 1 with an explanation:
  - Two rules whose scopes can match the **same declaration** (`scopesCanMatchSameDecl()` — the fine-grained scopes are subsets of the broad ones, so `member` also matches every `static_member`) are refused whatever their styles: both would rewrite the same bytes, and the second rewrite lands on text the first already replaced. `member: snake_case` + `static_member: kConstant` turned `MaxCount` into `kMaxCountt`.
  - Two rules whose scopes **share a DeclContext** (`scopesShareADeclContext()` — data members and member functions share a class, the three global scopes share a namespace, locals share neither) are refused unless their styles provably cannot produce the same name (`namingStylesCanCollide()`). Disjointness is proved from a property that holds for every name one style produces and no name of the other's: only `trailing_` ever ends in `_`, only `_leading` starts with one, `snake_case` never has a capital while `UpperCamelCase` and `kConstant` always do. `m_prefix` + `snake_case` is *not* disjoint — a method named `MType` snake_cases to `m_type`.
  - `e2e/rulesets/member_trailing_method_snake.yaml` is the worked example of a combination that passes the check.
- **Every reference the tool can see but cannot rewrite declines the rename — including in template instantiations.** The rule that makes a rename safe is that a reference with no rewritable spelling vetoes the *declaration* (`RenameVeto`, keyed on the owner), so `mergeEditReports` and the direct-run re-run drop every edit that belongs to it. The scan pass applies that rule to three shapes: (a) a token spelled in a macro body or formed by `##`; (b) a use that a local of the new name would capture; (c) **a token spelled in a file no invocation will rewrite** — not this TU's main file and not an owned file. Shape (c) is what a Bazel `textual_hdrs` `.inc` lands in (parsed as part of the including TU, never listed as a source), and also what a standard-library template instantiated with one of our types lands in when it calls one of our members by name — `std::back_inserter` on our container spells `push_back` inside `<iterator>`, and the scan pass walks instantiations. The dependent-token recorder participates too: instantiations that *disagree* on what a token names veto both owners, not just the token. And two **by-name backstops** cover the case where a use in an instantiation resolves to a member that `primaryTemplateMember()` failed to map back to its pattern: if a declaration of that name is being renamed, every rename of that name is declined (`vetoByName`), on the reading that an unmapped instantiated member is a mapping failure rather than a different entity. That backstop is deliberately blunt — over-declining is the safe side — and it is what turned abseil's `StatusOr` from a build break into a reported skip before the mapping itself was fixed:

  ```cpp
  template <class T> struct Data { union { Status status_; }; };
  template <class T> struct StatusOr : Data<T> {
    bool ok() const { return this->status_.ok(); }
  };
  ```

  A member of an **anonymous union** is reached in the instantiation through an implicit `MemberExpr` on the union's *unnamed* field, at the same location as the token. The recorder took that node for "a member not being renamed" and vetoed the token; it now skips unnamed members. Separately, the scan pass keys an instantiated `FieldDecl` by `primaryTemplateMember()` like every other node — keying it by itself is what made every instantiated field look unmapped once the scan pass started walking instantiations. `instantiationPattern()` also follows a **member class template** (`Outer<int>::Impl<R>`, gmock's `ReturnRefOfCopyAction<T>::Impl`) back through `ClassTemplateDecl::getInstantiatedFromMemberTemplate()` — the pattern link lives on the template, not on its record — and `primaryTemplateMethod()` follows a member function template specialization (`Init<int*>`) through its `FunctionTemplateDecl` the same way.
- **The new name must still bind from every use site** — `collides()` asks whether the new name is taken in the *declaring* class, but an unqualified access (`obj.m`, `this->m`, plain `m`) looks the name up starting at the **object's** class and stops at the first class on the derivation path that has it. abseil's `StatusOr<T>` declares a method `status()` while its base `StatusOrData<T>` has the field `status_`; renamed to `status`, every `this->status_` in the derived class binds the method. `renameWouldBeHiddenAt()` runs per use in the scan pass (instantiations included, so a dependent access is checked on its resolved form): starting from the class *under* Sema's implicit derived-to-base cast — the cast is why the base expression's type is never the one to check — it walks the derivation path down to the declaring class and vetoes the rename if any class on the way declares the new name. A qualified access (`Base::m`) starts lookup at the qualifier and is exempt.
- **Every spelling of an old name in the main file must be accounted for** — after the scan pass, `auditMainFileSpellings()` runs Clang's raw lexer over the main file and declines (by name, see below) every identifier that spells a name being renamed and that **no reference the rewrite pass would reach** marked as seen. This is the generic backstop for reference kinds nobody has thought of: it would have caught `OffsetOfExpr` without anyone knowing it existed. "Seen" deliberately excludes nodes inside template instantiations (`isInstantiationRoot()` / `InstantiationDepth`), because the rewrite pass never walks them — the call `Init(p)` in a constructor template is an `UnresolvedMemberExpr` in the pattern and a resolved `MemberExpr` only in the instantiation, and counting the latter would hide exactly the token that gets left behind. Also unaccounted, and therefore declined: `using Base::x_;` (a `UsingDecl` names a member the tool does not rewrite, so it is left out of the seen set on purpose), a macro argument only ever consumed by `##` (gmock's `MOCK_METHOD(…, DoThis, …)` forms `gmock_DoThis` from it, and `ON_CALL(m, DoThis())` spells the argument nowhere else), a name in a `#if` branch this build does not take, and an unexpanded macro body. Comments and string literals are not identifiers to the raw lexer and cost nothing. Only the main file is audited — every owned file is the main file of some TU, and the AST rule above already covers references in files that are not.
- **A dependent token nobody resolves declines its name** — `Impl<T>::kIsNothrow` spelled inside a template argument of an alias template is folded to the value `false` when `IsNothrow<int>` is used; no instantiation ever holds a `DeclRefExpr` for it, so cross-TU resolution has nothing to observe, and the two declarations were renamed while the use stayed (abseil's `any_invocable_test.h`). The collector therefore enters every dependent token it spells into the resolution map as **pending** (spelling only: `OldName` set, neither `HasName` nor `Vetoed`), the merge carries pending entries, and both drivers act on what is still pending once every TU has run: `runTranslationUnits()` intersects the pending spellings with the union of every TU's `RenamedNames` and, for each hit, adds a **name-keyed veto** and re-runs (rule (a)); `mergeEditReports` treats a pending record with no resolution and no veto as unresolved and drops every rename edit of that old spelling. A name-keyed veto (`nameVetoKey()` in `rename_state.h`: the `RenameVetoes` key is `'\x01' + name`, offset 0 — a key no file can have) travels, seeds, merges, serializes and re-runs exactly like a declaration veto; every collector checks each declaration's *name* against it (`vetoed()`), and `vetoByName()` records one alongside the per-declaration vetoes so a TU that declares the name without seeing the reference declines it too. Over-declining — every declaration of that spelling — is the safe side.
- **Protocol names are never renamed.** Some member functions are called by the core language or the standard library *without being spelled anywhere in the user's code*: a range-for calls `begin()`/`end()`, a structured binding calls `get<I>()`, `std::lock_guard` calls `lock()`/`unlock()`, `std::back_inserter` calls `push_back()`, the coroutine machinery calls `await_ready()` and friends, `allocator_traits` calls `allocate()`. Renaming one is a *complete* rename — every spelled reference rewritten — that still does not compile, and no reference audit can see the call because there is no reference. `isProtocolMethodName()` declines them by fiat and reports the skip. Two more rules of the same kind: a static data member named `value` is the type-trait convention `std::conjunction` reads, so it is declined; and a member of an explicit or partial specialization of a template declared **outside** the files being formatted (`std::hash<T>`, `std::formatter<T>`, `std::tuple_size<T>`, `std::numeric_limits<T>`) is declined, because the primary template names it (`isMemberOfForeignSpecialization()`). A specialization of one of our own templates is unaffected.
- **`offsetof(T, member)`** — the member designator is a component of an `OffsetOfExpr`, not a `MemberExpr`; `VisitOffsetOfExpr` rewrites it (through the macro the name is an argument spelled at the call site). On a *dependent* type the component is an identifier with no declaration, resolved only per instantiation, which the rewrite pass never walks — so the name is declined. Clang models all of this (`OffsetOfNode::getField()` and a range over the identifier); the gap was ours, and it needed no templates to bite: abseil's `str_format` had a `static_assert(offsetof(FormatConversionSpecImpl, conv_) == 0)`.
- **A declaration spelled as an argument of a macro that pastes (`##`) is never renamed.** gmock's `ACTION_P3(Plus, foo, bar, baz)` makes a member `foo` *and* a typedef `foo##_type`; renaming the member to `foo_` turns the typedef into `foo__type` while the action body, which gmock documents as being allowed to spell `foo_type`, still does. The argument's spelling is part of other identifiers the macro forms, and those are never references the tool can see. The scan pass therefore asks the `Preprocessor` for the definition of every macro on the declaration's expansion chain (`Lexer::getImmediateMacroName`, `MacroInfo::tokens()`) and declines the declaration if any of them contains `##`. Only *declarations* are checked this way — a *use* spelled as a pasted-only argument (`ON_CALL(m, DoThis())`) has no AST node of its own and is caught by the spelling audit; a use spelled as an ordinary argument of a pasting macro is a real reference that rewrites fine.
- **A return type's cv-qualifier can be written on either side of it, and both sides have to move.** Clang's `QualifiedTypeLoc` range covers only `int` in `int const f()`, exactly as it covers only `int` in `const int f()`, so the return-type pass scans outward in *both* directions (`skipQualifiersBackward`, `skipQualifiersForward`) to find the whole written type. Leaving a trailing qualifier behind is not cosmetic in the Leading direction: `auto f() -> int const` became `int f() const`, which for a member function is a silently *different* declaration — a const member function returning `int`, not a function returning `const int`. The declarator-id always separates a return type from a function's own cv-qualifiers, so the forward scan can never reach them.
- **Shadowed variables** — `matchesScope()` filters by scope: a parameter with the same name as a global is not collected when renaming globals.
- **The project's warning flags are silenced** — every binary appends `-w` to the compile command it is handed (`makeStandardArgumentsAdjuster`). That command belongs to the project being formatted, so under `-Werror` Clang counts its own warning as an error, `CompilerInstance::ExecuteAction` returns false for a TU that parsed perfectly well, `ClangTool::run` reports the run as failed, and the tool exits 1 — failing the build (under the Bazel aspect, the emit action) over a diagnostic it never reads. The warning set is not even the one the project compiles with: a different Clang version, and per-target `copts`, which the aspect cannot recover (it passes only `ctx.fragments.cpp.copts`, the command-line `--copt`s). `-w` is tested *before* the `-Werror` promotion in `DiagnosticIDs::getDiagnosticSeverity`, so it outranks it, and it ignores only diagnostics that are warnings by default — a real error, which does mean the AST cannot be trusted, still fails the run. Test 12 of [cpp_formatting/normalize_variables_integration_test.sh](cpp_formatting/normalize_variables_integration_test.sh) is the end-to-end case.
- **Per-file-content cache key** — the embedded Clang resource directory is extracted under a directory whose name includes the FNV-1a hash of the embedded `.tar.gz`. If the embedded headers change (e.g. after an LLVM upgrade) a fresh cache directory is created automatically.
- **`--jobs` never changes the result** — slots are merged in source order and the re-run rules make the cross-TU state order-independent, so `-j1` and `-jN` produce the same bytes in every mode; the integration tests assert it. Only stderr interleaving of the `[i/N] Processing file` progress lines varies.
- **cwd relativization** — with the per-instance file systems the process is no longer `chdir`'d per compile command. For a JSON compilation database whose `directory` differs from the launch cwd, veto keys, edit records and SARIF URIs are now consistently relative to the launch cwd (they used to follow each command's directory, inconsistently across TUs). `--`-style fixed databases have `.` as their directory and are unaffected.
- **Trailing dry run over several files** — prints one `** Rewritten Output (Dry Run): **` banner and then each file behind a `=== path ===` header (the format the other two tools use), instead of one banner per file; single-file output is unchanged.

## Notes

- The `bazel-*` symlinks in the root are Bazel output/convenience symlinks — do not edit them.
- The `patches/` directory holds patches applied to the LLVM source at fetch time, listed in the `llvm-raw` `http_archive` in [third_party/llvm/extensions.bzl](third_party/llvm/extensions.bzl). The three applied ones keep zlib/zstd/blake3 building under MSVC; `llvm_smallvector_cstdint.patch` is unused, having gone obsolete with the LLVM 19 upgrade (upstream `SmallVector.h` now includes `<cstdint>` itself). Other MSVC breakage is handled with conformance flags in [.bazelrc](.bazelrc) rather than by patching LLVM — see `/Zc:preprocessor` and `/permissive-` there. Clang/LLVM is built from source via the module extension in [third_party/llvm/](third_party/llvm/); see the "Clang/LLVM from source" section above.
