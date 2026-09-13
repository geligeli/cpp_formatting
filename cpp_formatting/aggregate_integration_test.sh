#!/usr/bin/env bash
# Integration tests for the per-TU emit + aggregate pipeline:
#   * `cpp_format --emit-edits=<records.json> ...` (per-TU record emission)
#   * `cpp_format --aggregate [--check|--apply] <records.json>...`
#   * equivalence with the standalone `aggregate_edits` binary
#   * the per-source-file shape the Bazel aspect runs: one emit invocation per
#     file, told via --owned-files that the target's other files are
#     renameable, and record lists passed with --records-from.
#
# Exercises the cross-TU template-dependent-token path: a header-only template
# whose member token is resolved from the instantiating .cpp's records.
#
# Arguments (Bazel $(location ...) expansions):
#   $1  cpp_format binary
#   $2  aggregate_edits binary

set -euo pipefail

cpp_format="$(realpath "$1")"
aggregate_edits="$(realpath "$2")"

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

cat > "$tmpdir/cpp_format.yaml" <<'EOF'
normalize_variables:
  - scope: member
    style: snake_case
EOF

# Header-only template with a dependent member token, instantiated in the .cpp.
cat > "$tmpdir/widget.h" <<'EOF'
#ifndef WIDGET_H_
#define WIDGET_H_
struct Widget {
  int itemCount;
};
template <class T>
int total_of(T& w) {
  return w.itemCount;
}
#endif
EOF
cat > "$tmpdir/widget.cpp" <<'EOF'
#include "widget.h"
int use() {
  Widget w;
  w.itemCount = 3;
  return total_of(w);
}
EOF

cd "$tmpdir"

# Phase 1 — emit per-TU records (the parallelizable step under Bazel).
"$cpp_format" --config=cpp_format.yaml --emit-edits=widget_cpp.json \
  widget.cpp widget.h -- -x c++ -std=c++17 -I.
"$cpp_format" --config=cpp_format.yaml --emit-edits=widget_h.json \
  widget.h -- -x c++ -std=c++17 -I.

# The header TU alone cannot resolve the dependent token; the .cpp's records
# must carry the resolution.
grep -q '"resolutions"' widget_cpp.json \
  || fail "emit: widget.cpp records missing resolutions sidecar"

# Records are emitted per TU and merged in source order, so the file is byte
# for byte the same whether the TUs were parsed on one thread or several.
"$cpp_format" --config=cpp_format.yaml --emit-edits=widget_cpp_j1.json --jobs=1 \
  widget.cpp widget.h -- -x c++ -std=c++17 -I.
"$cpp_format" --config=cpp_format.yaml --emit-edits=widget_cpp_j4.json --jobs=4 \
  widget.cpp widget.h -- -x c++ -std=c++17 -I.
cmp widget_cpp_j1.json widget_cpp_j4.json \
  || fail "emit: records differ between --jobs=1 and --jobs=4"
echo "PASS: --emit-edits records are identical with --jobs=1 and --jobs=4"

# ---------------------------------------------------------------------------
# Test 1 — folded `cpp_format --aggregate` diff == standalone aggregate_edits
# ---------------------------------------------------------------------------
"$aggregate_edits" --root="$tmpdir" widget_cpp.json widget_h.json > diff_standalone.patch
"$cpp_format" --aggregate --root="$tmpdir" widget_cpp.json widget_h.json > diff_folded.patch
diff -u diff_standalone.patch diff_folded.patch \
  || fail "aggregate: folded diff differs from standalone aggregate_edits"
grep -q '+  int item_count;' diff_folded.patch \
  || fail "aggregate: member decl not renamed in diff"
grep -q '+  return w.item_count;' diff_folded.patch \
  || fail "aggregate: cross-TU dependent token not rewritten in diff"
echo "PASS: cpp_format --aggregate diff matches aggregate_edits (incl. dependent token)"

# ---------------------------------------------------------------------------
# Test 1b — one emit action per source file, as the Bazel aspect runs them
#
# Each invocation parses exactly one file and is told via --owned-files that
# every file of the target is renameable.  The records must merge to the same
# change as one invocation over the whole target: the .cpp's records carry the
# dependent-token resolution, both actions rewrite the declaration, and
# aggregation joins them.  The list of records goes through --records-from,
# which is how the rules pass a repository's worth of them.
#
# In Emit mode every action edits every file it owns, not just its own main
# file, because a header is not a translation unit: the aspect gives it no
# action of its own and it is parsed wherever it is included.  So the .cpp's
# records cover the header as well, and aggregation dedups the byte-identical
# duplicates -- which is what the final diff comparison below proves.
# ---------------------------------------------------------------------------
realpath widget.cpp > owned.txt
realpath widget.h >> owned.txt
"$cpp_format" --config=cpp_format.yaml --owned-files=owned.txt \
  --emit-edits=pf_widget_cpp.json widget.cpp -- -x c++ -std=c++17 -I.
"$cpp_format" --config=cpp_format.yaml --owned-files=owned.txt \
  --emit-edits=pf_widget_h.json widget.h -- -x c++ -std=c++17 -I.
# The .cpp's action edits the header too -- it owns it, and nothing else would
# rewrite it if the header had no action of its own.
sed -n '/"edits"/,/"resolutions"/p' pf_widget_cpp.json | grep -q '"file": ".*widget\.h"' \
  || fail "per-file: the .cpp's action did not edit the header it owns"
# The dependent token is still carried as a *resolution*, not an edit: which
# member it names is only known from an instantiation, so aggregation decides.
if sed -n '/"edits"/,/"resolutions"/p' pf_widget_cpp.json | grep -q '"old": "val"'; then
  fail "per-file: the dependent token was emitted as an edit, not a resolution"
fi
sed -n '/"resolutions"/,/"vetoes"/p' pf_widget_cpp.json | grep -q '"file": ".*widget\.h"' \
  || fail "per-file: the .cpp's action recorded no dependent-token resolution"
printf '%s\n\n  %s  \n' pf_widget_cpp.json pf_widget_h.json > records.txt
"$cpp_format" --aggregate --root="$tmpdir" --records-from=records.txt > diff_perfile.patch
diff -u diff_folded.patch diff_perfile.patch \
  || fail "per-file: records from one action per file merge differently than one action per target"
"$aggregate_edits" --root="$tmpdir" --records-from=records.txt > diff_perfile_standalone.patch
diff -u diff_folded.patch diff_perfile_standalone.patch \
  || fail "per-file: aggregate_edits --records-from disagrees"
"$cpp_format" --aggregate --root="$tmpdir" --records-from records.txt pf_widget_h.json \
  > diff_perfile_dup.patch
diff -u diff_folded.patch diff_perfile_dup.patch \
  || fail "per-file: a record listed twice should dedup, not conflict"
set +e
"$cpp_format" --aggregate --root="$tmpdir" --records-from=missing.txt >/dev/null 2>&1
rc=$?
set -e
[[ $rc -eq 2 ]] || fail "per-file: a missing record list should exit 2, got $rc"
echo "PASS: one emit action per source file merges to the same change (via --records-from)"

# ---------------------------------------------------------------------------
# Test 2 — `--aggregate --check` reports violations and exits 1
# ---------------------------------------------------------------------------
set +e
check_out="$("$cpp_format" --aggregate --check --root="$tmpdir" \
  widget_cpp.json widget_h.json 2>/dev/null)"
rc=$?
set -e
[[ $rc -eq 1 ]] || fail "check: expected exit 1 on violations, got $rc"
grep -q 'widget.h' <<<"$check_out" || fail "check: missing per-file count"
echo "PASS: cpp_format --aggregate --check exits 1 with per-file counts"

# ---------------------------------------------------------------------------
# Test 3 — `--aggregate --apply` rewrites files; result is consistent + clean
# ---------------------------------------------------------------------------
"$cpp_format" --aggregate --apply --root="$tmpdir" widget_cpp.json widget_h.json
grep -q 'int item_count;' widget.h || fail "apply: member decl not renamed on disk"
grep -q 'return w.item_count;' widget.h \
  || fail "apply: dependent token not renamed on disk"
grep -q 'w.item_count = 3;' widget.cpp || fail "apply: use not renamed on disk"

# Re-emitting against the fixed sources yields no edits -> check exits 0.
"$cpp_format" --config=cpp_format.yaml --emit-edits=fixed.json \
  widget.cpp widget.h -- -x c++ -std=c++17 -I.
"$cpp_format" --aggregate --check --root="$tmpdir" fixed.json \
  || fail "check: clean tree should exit 0"
echo "PASS: cpp_format --aggregate --apply rewrites files; re-check is clean"

# ---------------------------------------------------------------------------
# Test 4 — --owned-files: a dependent target renames uses of a dep's declaration
#
# Mirrors how the Bazel aspect works: one emit action per target, each parsing
# only its own sources.  main.cpp uses a member declared in the library's
# header, so its action must be told that header is renameable — otherwise the
# member gets renamed in the library and the use site here is left behind.
# ---------------------------------------------------------------------------
mkdir -p "$tmpdir/xtarget"
cd "$tmpdir/xtarget"
cp ../cpp_format.yaml .

cat > lib.h <<'EOF'
#ifndef LIB_H_
#define LIB_H_
struct Gadget {
  int partCount;
};
#endif
EOF
cat > lib.cpp <<'EOF'
#include "lib.h"
int lib_use() {
  Gadget g;
  g.partCount = 1;
  return g.partCount;
}
EOF
cat > main.cpp <<'EOF'
#include "lib.h"
int main() {
  Gadget g;
  g.partCount = 2;
  return g.partCount;
}
EOF

# The library's action: owns both its own files.
"$cpp_format" --config=cpp_format.yaml --emit-edits=lib.json \
  lib.cpp lib.h -- -x c++ -std=c++17 -I.

# The dependent's action *without* the dep's headers: nothing to rename here,
# which is exactly the half-applied rename this flag exists to prevent.
"$cpp_format" --config=cpp_format.yaml --emit-edits=main_unowned.json \
  main.cpp -- -x c++ -std=c++17 -I.
if grep -q 'partCount' main_unowned.json; then
  fail "owned-files: expected no edits without the dep's headers"
fi

# The dependent's action *with* them, as the aspect passes them.
realpath lib.h > owned.txt
"$cpp_format" --config=cpp_format.yaml --owned-files=owned.txt \
  --emit-edits=main.json main.cpp -- -x c++ -std=c++17 -I.
grep -q 'partCount' main.json \
  || fail "owned-files: dep member use not renamed in the dependent's records"

# The dependent edits the dep's header too.  It has to: the dep may be a
# header-only target, which has no translation unit and therefore no action of
# its own, so its dependents are the only thing that ever rewrites it.  Several
# dependents emitting the same edit is what aggregation's byte-identical dedup
# is for -- the merged result below is the proof.
grep -q '"file": .*lib\.h' main.json \
  || fail "owned-files: dependent did not edit the dep's header it owns"
# The edits still name lib.h as their "owner_file" -- the declaration they
# belong to -- which is how a veto raised in one target suppresses another's.
grep -q '"owner_file": .*lib\.h' main.json \
  || fail "owned-files: dependent's edits do not name the dep's decl as owner"

"$cpp_format" --aggregate --apply --root="$tmpdir/xtarget" lib.json main.json
grep -q 'int part_count;' lib.h || fail "owned-files: decl not renamed on disk"
grep -q 'g.part_count = 1;' lib.cpp || fail "owned-files: dep use not renamed on disk"
grep -q 'g.part_count = 2;' main.cpp \
  || fail "owned-files: dependent use not renamed on disk"
echo "PASS: --owned-files renames uses of a dependency's declarations"

echo "All aggregate integration tests passed."

# ---------------------------------------------------------------------------
# Test 5 — a veto raised by one target suppresses another target's edits
#
# The cross-target shape of the all-or-nothing rule.  The library's action only
# ever parses its own sources, so it never sees BUMP expanded and happily emits
# a rename for `itemCount`.  The binary's action expands the macro, cannot
# rewrite the body token, and emits a veto instead.  Aggregation has to drop the
# library's edits too -- otherwise the declaration is renamed and the macro body
# is left spelling the old name, i.e. a broken build.  `otherCount`, which no
# macro names, must survive.
#
# BUMP is defined in main.cpp, not counter.h, on purpose: the spelling audit
# lexes each main file for every spelling of a renamed name, so a macro body in
# counter.h that names itemCount would make the *library's* action decline the
# rename on its own -- correct, and asserted at the end of this test, but it
# would leave nothing for the cross-target propagation to prove.
# ---------------------------------------------------------------------------
mkdir -p "$tmpdir/macro"
cd "$tmpdir/macro"
cp ../cpp_format.yaml .

cat > counter.h <<'EOF'
#ifndef COUNTER_H_
#define COUNTER_H_
struct Counter {
  int itemCount;
  int otherCount;
};
int total(const Counter& c);
#endif
EOF
cat > counter.cpp <<'EOF'
#include "counter.h"
int total(const Counter& c) { return c.itemCount + c.otherCount; }
EOF
cat > main.cpp <<'EOF'
#include "counter.h"
#define BUMP(c) ((c).itemCount += 1)
int main() {
  Counter c{0, 0};
  BUMP(c);
  return total(c);
}
EOF

# The library's action: owns both its files, sees no expansion of BUMP.
"$cpp_format" --config=cpp_format.yaml --emit-edits=counter.json \
  counter.cpp counter.h -- -x c++ -std=c++17 -I.
grep -q '"old": "itemCount"' counter.json \
  || fail "veto: the library's action should emit the rename it cannot know is unsafe"
grep -q '"vetoes": \[\]' counter.json \
  || fail "veto: the library's action has no expansion to veto from"
"$cpp_format" --config=cpp_format.yaml --emit-edits=counter_j1.json --jobs=1 \
  counter.cpp counter.h -- -x c++ -std=c++17 -I.
cmp counter.json counter_j1.json \
  || fail "veto: the library's records differ between --jobs=1 and the default"

# The dependent's action: expands BUMP, so it vetoes instead of editing.
realpath counter.h > owned.txt
"$cpp_format" --config=cpp_format.yaml --owned-files=owned.txt \
  --emit-edits=main.json main.cpp -- -x c++ -std=c++17 -I.
grep -q 'macro body' main.json \
  || fail "veto: the dependent's action did not emit a veto for itemCount"

# Aggregating both drops every itemCount edit and keeps otherCount.
"$cpp_format" --aggregate --root="$tmpdir/macro" counter.json main.json \
  > veto.patch
if grep -q 'item_count' veto.patch; then
  fail "veto: a vetoed rename survived aggregation"
fi
grep -q '+  int other_count;' veto.patch \
  || fail "veto: the unaffected member should still be renamed"

# The standalone aggregator applies the same rule.
"$aggregate_edits" --root="$tmpdir/macro" counter.json main.json \
  > veto_standalone.patch
diff -u veto.patch veto_standalone.patch \
  || fail "veto: standalone aggregate_edits disagrees with --aggregate"
echo "PASS: a veto from one target suppresses another target's edits"

# The same with the library split into one action per file, as the aspect runs
# it: the header's action renames the declaration, counter.cpp's action renames
# its use, main.cpp's action vetoes, and aggregation still drops all of it.
realpath counter.cpp > lib_owned.txt
realpath counter.h >> lib_owned.txt
"$cpp_format" --config=cpp_format.yaml --owned-files=lib_owned.txt \
  --emit-edits=pf_counter_cpp.json counter.cpp -- -x c++ -std=c++17 -I.
"$cpp_format" --config=cpp_format.yaml --owned-files=lib_owned.txt \
  --emit-edits=pf_counter_h.json counter.h -- -x c++ -std=c++17 -I.
"$cpp_format" --aggregate --root="$tmpdir/macro" \
  pf_counter_cpp.json pf_counter_h.json main.json > veto_perfile.patch
diff -u veto.patch veto_perfile.patch \
  || fail "veto: per-file records merge differently than per-target records"
echo "PASS: the cross-target veto holds with one action per file"

# And the local counterpart: a macro body in the library's own header that
# spells itemCount is caught by the spelling audit of that header's action --
# nothing expands the macro there, so no AST node accounts for the token -- and
# the library declines the rename itself, before any dependent is consulted.
cat > counter_macro.h <<'EOF'
#ifndef COUNTER_MACRO_H_
#define COUNTER_MACRO_H_
struct Counter { int itemCount; int otherCount; };
#define BUMP(c) ((c).itemCount += 1)
#endif
EOF
"$cpp_format" --config=cpp_format.yaml --emit-edits=counter_macro.json \
  counter_macro.h -- -x c++ -std=c++17 -I.
grep -q '"name": "itemCount"' counter_macro.json \
  || fail "audit: an unexpanded macro body naming the member should decline it"
grep -q 'spelled where no reference the tool understands accounts for it' counter_macro.json \
  || fail "audit: unexpected veto reason: $(cat counter_macro.json)"
if grep -q '"old": "itemCount"' counter_macro.json; then
  fail "audit: the declined member must not be emitted as an edit"
fi
grep -q '"old": "otherCount"' counter_macro.json \
  || fail "audit: the sibling member should still be renamed"
echo "PASS: an unexpanded macro body in the owning header declines the rename locally"

# ---------------------------------------------------------------------------
# Test 6 — two rename rules: one rule's veto must not cancel the other's
#          dependent-token resolution
# ---------------------------------------------------------------------------
# Every rule looks at every dependent token, and the rules disagree by
# construction: the rule owning the member the token resolves to records a
# name, and the others record a veto because it resolves to a member *they* are
# not renaming.  Merging is veto-absorbing, so the records have to be kept apart
# per rule -- pooled under one (file, offset) key, the veto wins, the
# declaration is renamed and the dependent use is left spelling the old name.
mkdir -p "$tmpdir/tworule"
cd "$tmpdir/tworule"

cat > m.h <<'EOF'
#ifndef M_H_
#define M_H_
template <typename T>
class MatcherBase {
 public:
  // Renamed by the *method* rule; the identically named VTable field below is
  // renamed by the *member* rule.
  bool MatchAndExplain(const T& x) const {
    return vtable_->match_and_explain(*this, x);
  }

 private:
  struct VTable {
    bool (*match_and_explain)(const MatcherBase&, const T&);
  };
  const VTable* vtable_;
};
#endif
EOF
cat > m.cpp <<'EOF'
#include "m.h"
bool f(MatcherBase<int>& m, int x) { return m.MatchAndExplain(x); }
EOF
cat > cpp_format.yaml <<'EOF'
normalize_variables:
  - scope: member
    style: trailing_
  - scope: method
    style: snake_case
EOF

realpath m.h > owned.txt
realpath m.cpp >> owned.txt
"$cpp_format" --config=cpp_format.yaml --owned-files=owned.txt \
  --emit-edits=m_cpp.json m.cpp -- -x c++ -std=c++17 -I.
"$cpp_format" --config=cpp_format.yaml --owned-files=owned.txt \
  --emit-edits=m_h.json m.h -- -x c++ -std=c++17 -I.

# The two rules must record the token under *different* rule indices.
grep -q '"rule": 1' m_cpp.json \
  || fail "two rules: the sidecar does not distinguish the rules"

"$cpp_format" --aggregate --root="$tmpdir/tworule" m_cpp.json m_h.json \
  > tworule.patch
"$cpp_format" --aggregate --apply --root="$tmpdir/tworule" m_cpp.json m_h.json

# The member rule owns the VTable field: declaration and dependent use both
# gain the trailing underscore.  The method rule owns MatchAndExplain.
grep -q 'bool (\*match_and_explain_)' m.h \
  || fail "two rules: the VTable field was not renamed"
grep -q 'vtable_->match_and_explain_(' m.h \
  || fail "two rules: the dependent use was left spelling the old name"
grep -q 'bool match_and_explain(const T& x)' m.h \
  || fail "two rules: the method was not renamed"
echo "PASS: two rename rules keep their dependent-token resolutions apart"
