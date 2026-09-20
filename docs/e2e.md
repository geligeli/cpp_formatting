# End-to-end corpus tests (e2e/)

Part of the maintainer notes indexed by [AGENTS.md](../AGENTS.md).

How to run the harness and add scenarios: [e2e/README.md](../e2e/README.md).


Not part of `bazel test //...`, and **not a Bazel target** — `e2e/run_e2e.sh`
drives `bazel` inside a *second* workspace, which a Bazel action cannot do
(nesting a server inside a running one deadlocks on the workspace lock, the same
constraint that keeps `bazel/integration/cpp_format.sh` a plain script). CI runs
it nightly / on dispatch via [.github/workflows/e2e.yml](../.github/workflows/e2e.yml).

Each scenario pairs a pinned repo ([e2e/repos/](../e2e/repos/)) with a ruleset
([e2e/rulesets/](../e2e/rulesets/)) and runs twelve phases: build the unmodified
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

**naming_repo** (`naming_repo-google_style`, a checked-in fixture like
`mini_repo`) is the corpus entry for the `type` and `namespace` scopes: a
namespace, a nested namespace, an aggregate with a nested struct, an enum, a
typedef, an alias, a class template with STL protocol members, a class with
out-of-line constructor and destructor, a namespace alias and a
using-directive, all spelled against the Google style across three targets,
plus a dependent type name (`typename R::dims`) that only the instantiating
TUs can resolve. `RUN_TESTS=1`, so the renamed code runs, not just compiles.

**re2** widens the shapes rather than the size, and is the cheap gate --
minutes, not abseil's half hour, so it is the one to run while iterating. It
found the anonymous-union lookup-home bug above on its first run (1069 errors
from 7 files); most of what it declines is capture, since re2 names a parameter
after the member it initialises throughout.

**game_arena** (`game_arena-google_style`) is the dogfooding entry: the repo
the tool is used on, under its real ruleset -- nine rename scopes plus both
rewrite passes in one run. It is also the only corpus
member that already *consumes* cpp_format, so `wire()` strips the repo's own
`bazel_dep`/`archive_override`/`release()` wiring out of `MODULE.bazel` before
injecting the harness's (`_strip_cpp_format_wiring`): left in place it would
collide on `@cpp_format_bin`, and if it did not, the scenario would test the
repo's pinned release rather than the binary under test. The harness's
`--disk_cache` is appended as `common`, not `build`, for the same repo -- its
rc points every command at a shared cache that need not exist elsewhere. Its
first run found the **class-scope capture** bug, which `highway-member_snake_case`
hit in the other direction: `collides()` asked whether a new name is *taken*
in the class, never whether it is *used unqualified* inside the class (or a
derived class) to mean an enclosing-scope entity that the renamed member would
then hide -- `capabilities()` → `Capabilities()` next to `struct Capabilities`
in the namespace, `free_` → `free` next to a static member calling `::free`.
Both rebuild now that scan() has that check (see "The new name must not hide
what the scope already means by it"). The same run also showed the spelling
audit over-declining: an `ABSL_FLAG` name is only ever pasted or stringized, so
a flag spelled like a member of the struct it is copied into (`docker_image`,
`repo_dir`, ~20 of them) tripped the audit and the member kept its bare name;
the audit now skips the macro arguments the preprocessor consumed, and the
scenario went from 1379 to 1522 edits (1538 with the type and namespace rules) -- and then to **175**, once the ruleset could state its exceptions (`const_local`, `public_member`, `const_member`, most specific rule wins): most of the old edits were the ruleset mis-formatting a repository that already follows the style, underscores on struct fields and the k taken off function-local constants. `protobuf-member_snake_case` found the
third fix in the same batch, the dependent-token all-or-nothing rule (16695
edits, 307 files; `T::_table_` and `T::kInlineCapacity`, see the "Known
non-obvious behaviours").

**A target workspace does not inherit the machine's Bazel rc files.** A corpus repository compiles with whatever toolchain Bazel finds on the machine, and a home rc that turns on remote execution -- reasonable once *this* repository's toolchain is hermetic -- sent those compiles to workers with no compiler: `baseline` failed with "Remote Execution Failure", or passed, depending on which side of a dynamic-execution race won. Every Bazel run in a target workspace goes through a shim (`$WORK/bin/bazel`, `--nohome_rc --nosystem_rc`): startup options cannot come from the target's own `.bazelrc` (the home rc is read after it), and a shim rather than a flag per call site because `tools/cpp_format.sh` runs Bazel too (it honours `$BAZEL`). The tool's own build, in this repository, still uses the machine's configuration. The price is speed for a target that *is* hermetic: game_arena's baseline builds locally (about six minutes).

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
workspaces sharing one can restore each other's records. [.bazelignore](../.bazelignore)
keeps `e2e/testdata` (self-contained workspaces) and `.e2e` out of `//...`.
