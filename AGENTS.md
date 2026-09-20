# AGENTS.md

Guidance for AI coding agents (Claude Code, Kimi Code, Codex, etc.) working in this repository. `CLAUDE.md` is a symlink to this file.

**Keep this file short** — it is loaded into every session. The detail (why things are the way they are, every sharp edge found on a real corpus) lives in [docs/](docs/). Read the doc for an area *before* changing it, and record new findings there, not here.

| Working on | Read first |
|---|---|
| `.bazelrc` configs, sanitizers, the hermetic toolchain / static glibc, an LLVM or IWYU bump, pre-commit hooks, IWYU + Gazelle, `patches/` | [docs/build.md](docs/build.md) |
| Finding a file, a public API or a test; what each test pins | [docs/source-map.md](docs/source-map.md) |
| Why a pass rewrites or declines something; `tu_driver` phases and re-runs; cross-file renames; template-dependent tokens; macros | [docs/design.md](docs/design.md) |
| A reference kind, veto, collision, capture/hiding check, spelling audit or index behaviour that looks odd | [docs/non-obvious-behaviours.md](docs/non-obvious-behaviours.md) |
| The aspects, edit records and aggregation, `--owned-files`, `compile_commands`, the index aspect, the prebuilt kit | [docs/integration.md](docs/integration.md) |
| e2e corpus scenarios and what each one found | [docs/e2e.md](docs/e2e.md), [e2e/README.md](e2e/README.md) |
| `code_browser` routes | [code_browser/README.md](code_browser/README.md) |
| End-user documentation (keep in step with behaviour changes) | [README.md](README.md) |

## Build and test

Bazel with Bzlmod ([MODULE.bazel](MODULE.bazel)).

```
bazel build //...
bazel test //...
bazel test //cpp_formatting:rename_variables_test        # one suite
```

Test targets in `//cpp_formatting`: `trailing_return_types_test`, `const_placement_test`, `naming_convention_test`, `rename_variables_test`, `rename_state_test`, `tu_driver_test`, `lint_lib_test`, `cpp_index_merge_test`, `cpp_index_test` (gtest) and `trailing_return_types_integration_test`, `const_placement_integration_test`, `normalize_variables_integration_test`, `lint_integration_test`, `aggregate_integration_test`, `index_integration_test` (shell). More under `//bazel/testdata`, `//code_browser`, `//tools/platforms/smoke`.

Running a binary (dry run prints to stdout; `-i`/`--in-place` writes; compile flags follow the second `--`):

```
bazel run //cpp_formatting:trailing_return_types -- [-i] [--reverse] file.cpp -- -std=c++17
bazel run //cpp_formatting:const_placement -- --style=east [--in-place] file.cpp -- -std=c++17
bazel run //cpp_formatting:normalize_variables -- --style=snake_case --scope=member [--in-place] file.cpp -- -std=c++17
bazel run //cpp_formatting:cpp_format -- --config=cpp_format.yaml --in-place file.cpp -- -std=c++17
```

- **Lint (CI):** all four binaries take `--lint` (change nothing, exit 1 on violations) and `--format=<text|sarif|diff>` (non-default implies `--lint`; `diff` is git-apply-able). Not combinable with `--in-place`.
- **Parallelism:** `--jobs=N` / `-jN` (`0` = every CPU). Output is byte-identical for any `-j`. `--debug-trace` forces serial.
- **`normalize_variables --debug-trace`** prints per TU the rename map and every reference site (`main=`, `macro=`, `WILL_RENAME`, `VETOES_RENAME`) — the first thing to reach for when a use was missed or a rename declined. `--report-rename-conflicts` lists every declined rename with its reason.
- **Configs:** `--config=minify-x86_64` / `minify-aarch64` (release binary: LTO, stripped, static glibc), `--config=asan|tsan|ubsan` (instruments LLVM too — first build is long), `--config=lint` / `--config=index` (run the aspects). All builds are `-fno-rtti` except the sanitizer configs.
- **Remote-execution check:** `bazel test //... --spawn_strategy=remote --test_tag_filters=-local`.
- **e2e** is not a Bazel target: `e2e/run_e2e.sh --list`, `e2e/run_e2e.sh mini_repo-all` (~10 s), `e2e/run_e2e.sh re2-…` is the cheap corpus gate (minutes); abseil/googletest/protobuf are slow. A scenario that passes unexpectedly fails as XPASS.

### Style and BUILD hygiene

Pre-commit ([.pre-commit-config.yaml](.pre-commit-config.yaml)) runs, in order: `clang-format` (Google style), `iwyu` (check only; `SKIP=iwyu git commit` to bypass), `gazelle`, `buildifier`.

```
tools/iwyu/iwyu.sh check | fix            # includes = exactly what is used; review the diff of `fix`
bazel run //tools/gazelle                 # deps / implementation_deps derived from #includes
bazel run //tools/gazelle -- -mode=diff   # the CI gate
tools/gazelle/llvm_index.sh               # after an LLVM bump
```

Gazelle owns `cc_*` deps — do not hand-edit them. It needs: a `package(default_visibility = …)`, `# keep` on generated sources listed by label, and a `cc_binary` named after its source file. `fix_includes.py` is line-based: check the diff of any test that embeds C++ source in a raw string. IWYU ↔ Clang versions are pinned together in [third_party/llvm/extensions.bzl](third_party/llvm/extensions.bzl); after a toolchain bump regenerate `tools/iwyu/libcxx.imp`.

## Layout

Two unrelated things are called LLVM: `@llvm` is the **toolchain** (hermetic-llvm, prebuilt Clang 23 + libc++, Linux only, dev dependency, taken from the `geligeli/hermetic-llvm` fork for static glibc) and `@llvm-project` is the Clang **21.1.8 library** the tools link, built from source through [third_party/llvm/](third_party/llvm/) (the BCR stops at 17).

[cpp_formatting/](cpp_formatting/) — everything the four binaries are made of:

- `trailing_return_types{,_lib}` — `int f()` ↔ `auto f() -> int` (`ReturnTypeStyle::Trailing|Leading`; Leading is mostly guards).
- `const_placement{,_lib}` — `const int` ↔ `int const` (`ConstStyle::East|West`); only a qualifier of the type *specifier* moves.
- `normalize_variables` + `rename_variables_lib`, `naming_convention`, `rename_state` — renames by scope (`member`, `local`, `global`, `method`, `type`, `namespace`, and the fine-grained `static_`/`const_`/`public_`/`protected_`/`private_member`, `static_`/`const_local`, `static_`/`const_global`): collect → scan (vetoes) → rewrite, plus cross-TU resolution of template-dependent tokens.
- `cpp_format` + `cpp_format_lib` — all passes from a YAML config in **one** parse per TU; also hosts `--emit-edits`, `--owned-files`, `--aggregate`, `--emit-index`, `--merge-index`, `--dump-index`.
- `tu_driver` — the parallel TU driver all four binaries share (`TUSlot`, `TUSlotClient`, `runTranslationUnits()`).
- `lint_lib` — `LintReport` (text/SARIF), unified diff, the edit-record model and `runEditAggregation()` (also behind the standalone `aggregate_edits`).
- `cpp_index_lib` (per TU, on `clang::index`), `cpp_index_merge` (Clang-free), `index.proto` — the symbol index.
- `embedded_clang_resource`, `embed_file` — Clang's builtin headers are embedded in every binary and self-extracted to the cache dir.

Elsewhere: [bazel/cpp_format.bzl](bazel/cpp_format.bzl) (from-source aspects and rules, demo in `bazel/testdata/`), [bazel/integration/](bazel/integration/) (the prebuilt-binary kit consumers import by URL), [code_browser/](code_browser/) (HTTP server over the index in SQLite; C++20, Boost.Beast), [e2e/](e2e/), [tools/](tools/) (`gazelle`, `iwyu`, `hermetic_std`, `platforms`), [patches/](patches/) (applied to LLVM at fetch time).

## YAML config (`cpp_format --config=<file>`)

```yaml
# All fields are optional; omit any section to skip that pass.
return_types: trailing      # or `leading`; `trailing_return_types: true` is the old spelling (not both)
const_placement: east       # or `west`; `int* const p` qualifies the pointer and stays put
normalize_variables:        # applied in order
  - scope: member
    style: snake_case
  - scope: type             # classes, enums, typedefs, aliases (STL protocol names exempt)
    style: UpperCamelCase
```

Scopes: see Layout. Styles: `snake_case`, `_leading`, `trailing_`, `m_prefix`, `camelCase`, `UpperCamelCase`, `UPPER_SNAKE_CASE`, `kConstant`. Rules may overlap (`local` + `const_local`, `member` + `public_member`): the most specific one renames the declaration -- constness, then storage, then access, then the broad scope; only the same scope twice is refused.

## Invariants — do not break these

Each is explained, with the case that found it, in the docs above.

**Rewriting**
- **A rename is all-or-nothing.** Any reference the tool can see but cannot rewrite — macro body, `##` paste, a file no invocation rewrites (`textual_hdrs`, system templates instantiated with our types), a use the new name would capture or hide, a dependent token nobody resolves, a spelling the audit cannot account for, a protocol name (`begin`, `value_type`, …) — **vetoes the declaration (or the name) everywhere** and is reported as a skip. Over-declining is the safe side; never half-apply. A missed reference must surface as a compile error, never a silent rebind.
- **Never edit through a macro expansion.** Rewritable ⇔ `getFileLoc(Loc) == getSpellingLoc(Loc)`; a macro *argument* is rewritten once at its call site.
- **Nothing reaches disk before `flush()`**, so every TU parses the original sources. Lint diagnostics are recorded at the same choke points that rewrite, so lint ≡ in-place.
- **`cpp_format` pass order is load-bearing:** renames → const placement → return types, sharing one `Rewriter`; each later pass reads `getRewrittenText()`, and in Emit mode drops earlier records inside the span it re-emits.
- **`const_placement`:** one contiguous `ReplaceText` per move (never insert + delete), innermost `QualifiedTypeLoc` first, hand-computed character ranges (`>>`).
- The rewrite pass does **not** visit template instantiations (locations point into the pattern); the scan pass and the dependent-token recorder do. Instantiated members map back through `primaryTemplateMember()` / `instantiationPattern()` — fields match *by index*, so a wrong pattern rebinds to an unrelated member.
- Every pass is a fixpoint; the two directions of each pass undo each other where both apply.

**Execution model**
- A TU writes only into its own `TUSlot`; cross-TU state changes hands at barriers on the driver thread, in **source order** (non-headers, then headers). That is what makes `-j` invisible — keep every merged collection ordered.
- Direct runs: a new veto discards the pass and re-runs everything (shared `DepRes` cleared, `Vetoes` kept); a TU whose seeded resolutions went stale is re-run alone. **Emit mode seeds nothing and never re-runs** — records carry owners, vetoes, dependencies and skips, and `mergeEditReports` does the dropping.
- Key spaces differ: `Vetoes` are cwd-relative, `DependentResolutions` and edit records use absolute real paths, index paths are the cwd-relative names Clang opened (an index must be machine-independent).
- `tu_driver` and the three single-pass binaries stay protobuf-free (`TUSlot::IndexBytes`). Index serialization is a pure function of content: no `map<>`, every repeated field ordered, enums open.

**Bazel integration**
- One action per **translation unit**; a header is never a TU — it is reached through `--owned-files` and rewritten by the TUs that include it. Emit mode widens "rewritable" to any owned file; a direct run must not (`PendingRewrites` is last-writer-wins on whole files).
- Records, not diffs. A target always writes its manifest, even empty (Bazel never deletes stale outputs). Record lists go through `--records-from` (command-line limits).
- **The aspect exists twice** — [bazel/cpp_format.bzl](bazel/cpp_format.bzl) and [bazel/integration/cpp_format.bzl](bazel/integration/cpp_format.bzl) — and `merge_compile_commands` is kept verbatim in both plus `cpp_format.sh`. Change them together. Both build the compile context with `_compilation_context()` (merges `implementation_deps`) and pass the target's `copts`.
- **Aspect and binary are versioned together:** an aspect change that needs a new flag ships with a bump of `cpp_format.release(version=…)` in [MODULE.bazel](MODULE.bazel).
- This module is **imported by URL** by kit consumers: `llvm` (toolchain), `gazelle`, `gazelle_cc` and the `release()` pin are `dev_dependency`; the kit never names `@llvm-project`; the `gazelle` targets live in `//tools/gazelle`, not the root package; the kit's `_config` defaults to `@@//:cpp_format.yaml` (the *root* module's); the `use_extension`/`use_repo` for `@cpp_format_bin` must stay non-dev.
- Scripts that call `bazel` (`cpp_format.sh`, `e2e/run_e2e.sh`) are plain scripts, never `bazel run` targets — nesting deadlocks on the workspace lock.

**Build**
- Fixture-parsing tests get their standard library through `HERMETIC_STD_DATA` / `HERMETIC_STD_ENV` ([tools/hermetic_std/](tools/hermetic_std/)), or they fail on a remote worker with no C++ headers.
- **Exactly one rule renames a declaration** (`scopeClaims()`): two rules rewriting the same bytes corrupt them (`MaxCount` → `kMaxCountt`). A new scope needs a `scopeSpecificity()` that orders it against every scope it can overlap; `ScopeRelations.SpecificityOrdersEveryOverlap` checks that.
- **Nothing that loads for a consumer may name a dev dependency at load time** (`@llvm`, `@gazelle`): a `load()`, or a transition's `inputs`/`outputs`. A label in a BUILD file is resolved lazily and is fine. `bazel query @cpp_formatting//...` from a dependent repository is the check.
- Sanitizers use the toolchain's switches (`--@llvm//config:asan`), never `--copt=-fsanitize=…`. `@llvm_zlib` is an `alias` to `@zlib` (one zlib; an alias because of `layering_check`). After touching zlib or the toolchain, `bazel build --config=minify-x86_64 //cpp_formatting:cpp_format` is the local stand-in for the release jobs.
- `clang_include_headers`'s `strip_prefix` hardcodes the canonical repo name `+llvm+llvm-project`.
- The `bazel-*` symlinks in the root are Bazel's — do not edit them.
