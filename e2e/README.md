# End-to-end corpus tests

Build a real Bazel C++ repo, run a `cpp_format` transformation over all of it,
and build it again. A rebuild that fails is the signal: a rename that missed a
reference, or a rewrite that produced invalid code, shows up as a compile error
exactly as it would for a user.

```sh
e2e/run_e2e.sh --list                          # what is in the corpus
e2e/run_e2e.sh mini_repo-all                   # the fast one (~10s)
e2e/run_e2e.sh googletest-trailing_return_types
e2e/run_e2e.sh                                 # everything
```

Exit codes: `0` every scenario met its expectation · `1` an expectation was
violated · `2` setup/environment failure, so nothing was learned about the tool.

**This is not a Bazel target, and cannot be one.** It drives `bazel` inside a
second workspace, and nesting a Bazel server inside a running one deadlocks on
the workspace lock — the same reason
[bazel/integration/cpp_format.sh](../bazel/integration/cpp_format.sh) is a plain
script. Run it from a shell. CI runs it from
[.github/workflows/e2e.yml](../.github/workflows/e2e.yml) nightly and on demand,
never on the per-PR path: a from-source LLVM build plus two builds of abseil is
far outside that budget.

## How a scenario runs

The target repo has to be the **root module** of its own Bazel invocation — the
aspect emits nothing for targets with `workspace_name != ""`, so a repo pulled
in as a `bazel_dep` can never be formatted. So the harness materializes the repo
into a work dir and vendors the integration kit into it as
`third_party/cpp_format/`, which is the layout
[cpp_format.sh](../bazel/integration/cpp_format.sh) already defaults to. The
locally built binary is injected through the kit's own `release()` tag class
with a `file://` base URL.

Twelve phases, each timed and logged:

| phase | what it asserts |
|---|---|
| `tool` | the binary under test builds (or is downloaded / taken from a path) |
| `materialize` | the pinned checkout is present |
| `wire` | the kit, ruleset and rc land, and `@cpp_format_bin` resolves |
| `enumerate` | **more than zero** `cc_*` targets — the wrapper hides query errors, which would otherwise masquerade as "no edits" |
| `baseline` | the *unmodified* repo builds. A failure here is SETUP-FAIL |
| `check` | edits are reported, **and there is at least one** — a ruleset that changes nothing proves nothing |
| `diff` | the patch is captured as a review artifact |
| `fix` | the edits apply |
| `applied` | the files that changed on disk are **exactly** those `check` reported. `flush()` writes with `std::ofstream` and never checks the failbit, so an unwritable file is skipped with exit code 0 — this is what catches that |
| `rebuild` | **the repo still compiles.** The core assertion |
| `converge` | a second pass has nothing left to do, i.e. the transform is a fixpoint |
| `report` | summary + artifacts |

### Two-pass scenarios

A scenario may name a **second ruleset** to run over the first one's output:

```bash
RULESET=trailing_return_types
RULESET_THEN=leading_return_types
```

`swap` commits pass 1 — so `applied` keeps measuring only what the *current*
pass wrote — and drops the second ruleset in; then `check2 diff2 fix2 applied2
rebuild2 converge2` repeat the same assertions for it. This is how the two
return-type directions are tested against each other: rewrite every function to
a trailing return type, rebuild, move them all back, rebuild again.

Optionally:

```bash
EXPECT_ROUNDTRIP_IDENTICAL=1
```

adds a final `roundtrip` phase asserting the sources came back **byte-identical**
to the pre-transform tree. Only declare it where the second ruleset really does
undo the first — the `leading` direction rejects strictly more than `trailing`
does, so on a large corpus some declarations are expected to stay where the
first pass put them, and there `rebuild2` and `converge2` are the assertions
that carry the weight.

## Expected outcomes

Some transformations are *expected* to break some repos, and the corpus says so
rather than pretending otherwise. A scenario declares:

```bash
EXPECT=known_fail
EXPECT_FAIL_PHASE=rebuild
EXPECT_FAIL_REASON="why, with a file:line pointing at the mechanism"
```

- Failing at the declared phase → **XFAIL**, exit 0.
- Failing at a *different* phase → FAIL. A `known_fail` must never mask an
  unrelated break.
- **Passing** when `known_fail` is declared → **XPASS, which fails.** If someone
  fixes the limitation, the corpus says so instead of quietly staying green;
  promote the scenario to `EXPECT=pass`.

## Adding to the corpus

Three independent files; usually you only add one.

**A repo** — `e2e/repos/<name>.repo`:
```bash
REPO_KIND=git                 # git | local
REPO_URL=https://github.com/org/project.git
REPO_REV=<full SHA>           # immutable; a tag is not
TARGET_PATTERN='//...'        # narrow it for a faster signal
RUN_TESTS=0                   # 1 runs the repo's own tests instead of building
BUILD_FLAGS=()                # extra flags for its builds
```

**A ruleset** — `e2e/rulesets/<name>.yaml`, a literal `cpp_format.yaml`.

**A scenario** — `e2e/scenarios/<repo>-<ruleset>.scenario`, pairing the two plus
the expectation (and optionally a second ruleset, see above). Run it once locally before committing `EXPECT`: a wrong value
fails either way (normally, or via XPASS), so it cannot rot silently, but the
first commit should be green.

## Triaging a failure

Artifacts land in `$E2E_WORK_DIR/artifacts/<scenario>/` — `summary.txt`,
`<ruleset>.patch` (one per pass), and a log per phase. The target workspace is left in place at
`$E2E_WORK_DIR/<scenario>/src`, so you can `cd` there and re-run `bazel build`
or `./tools/cpp_format.sh diff` by hand.

For a `rebuild` failure, `rebuild.log` holds the full error set (`--keep_going`
is on). The usual causes, in rough order of likelihood:

1. **A macro-expanded reference the veto did not see.** A member referenced from
   a macro body is not renamed at all — the reference has no rewritable spelling,
   so the whole rename is vetoed (AGENTS.md, "names spelled through macros").
   That only holds for expansions in a TU the tool actually parses, so a macro
   expanded solely in a `no-cpp-format` target, under an `#if` branch this build
   does not take, or outside the formatted pattern is still invisible.
   `macro_repo-member_snake_case` pins the case that *is* covered.
2. **`textual_hdrs`.** The aspect's `_own_files()` walks only `srcs` and `hdrs`,
   so declarations in `textual_hdrs` are neither parsed nor propagated as owned
   headers.
3. **A dependency edge the aspect does not follow.** It propagates over `deps`
   only; a target reachable through another attribute contributes no owned
   headers, so uses of its declarations are not rewritten.
4. **Template-dependent tokens outside the passed source set** — documented in
   AGENTS.md under "template-dependent member tokens".

`normalize_variables --debug-trace` prints every candidate site with `main=`,
`macro=` and `WILL_RENAME` flags, which is usually the fastest way to confirm
which of these you are looking at; `--report-rename-conflicts` lists every
rename that was skipped and why.

## Notes

- The work dir defaults to `$XDG_CACHE_HOME/cpp_format_e2e`, deliberately
  **outside** the repo. CI overrides it to `/workspace/.e2e`, which
  [.bazelignore](../.bazelignore) keeps out of `//...`.
- Each scenario gets **its own Bazel disk cache**. Edit records store absolute
  paths, so two workspaces sharing a disk cache can restore each other's
  records; the `applied` phase also asserts no record names a path outside the
  target tree, as a tripwire.
- `--tool=release:<tag>` tests a published binary instead of building from
  source, which skips the LLVM build entirely. `--tool=path:<abs>` reuses a
  binary you already have.
- A **vendored** kit needs the consumer to declare `bazel_dep(name="rules_cc")`
  directly, because its `load("@rules_cc//...")` resolves through the root
  module's repo mapping. The harness injects it when absent; a real vendoring
  consumer has to add it by hand.
