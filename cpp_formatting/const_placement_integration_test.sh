#!/usr/bin/env bash
# Integration tests for the east-const / west-const qualifier move.
# Arguments (all Bazel $(location ...) expansions):
#   $1   const_placement binary
#   $2   cpp_format binary

set -euo pipefail

const_placement="$(realpath "$1")"
cpp_format="$(realpath "$2")"

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

# Runs a lint command expecting exit code 1 (violations found).
expect_violations() {
  set +e
  "$@"
  local rc=$?
  set -e
  [[ $rc -eq 1 ]] || fail "expected exit 1 (violations), got $rc: $*"
}

# A fixture exercising the cases the move has to get right: the qualifier of a
# pointee moves while the qualifier *of* a pointer does not, a member
# function's own `const` is not a type qualifier at all, and nested template
# arguments (including a list that closes with `>>`) move with their outer
# type.
write_west() {
  cat >"$1" <<'EOF'
template <class T, class U>
struct P {};
struct W {};

const int kCount = 1;
static const char* const kName = "x";
int* const kSlot = nullptr;
const P<const W, const W*>* gPair = nullptr;
const P<int, const P<int, const W>>* gNested = nullptr;

struct Holder {
  const int id;
  int* const slot;
  const W* Peek() const { return nullptr; }
  const int Count() const { return id; }
};

void Take(const int a, const W& w, const P<const W, int>& p);
EOF
}

write_east() {
  cat >"$1" <<'EOF'
template <class T, class U>
struct P {};
struct W {};

int const kCount = 1;
static char const* const kName = "x";
int* const kSlot = nullptr;
P<W const, W const*> const* gPair = nullptr;
P<int, P<int, W const> const> const* gNested = nullptr;

struct Holder {
  int const id;
  int* const slot;
  W const* Peek() const { return nullptr; }
  int const Count() const { return id; }
};

void Take(int const a, W const& w, P<W const, int> const& p);
EOF
}

write_west "$tmpdir/west.cpp"
write_east "$tmpdir/east.cpp"

# ---------------------------------------------------------------------------
# Test 1 — in-place east: west source becomes the expected east source
# ---------------------------------------------------------------------------
cp "$tmpdir/west.cpp" "$tmpdir/t1.cpp"
"$const_placement" --style=east -i "$tmpdir/t1.cpp" -- -std=c++17 >/dev/null
diff -u "$tmpdir/east.cpp" "$tmpdir/t1.cpp" \
  || fail "east: in-place output does not match the expected east source"
echo "PASS: --style=east rewrites west-const source in place"

# ---------------------------------------------------------------------------
# Test 2 — in-place west: the reverse, and a byte-identical round trip
# ---------------------------------------------------------------------------
cp "$tmpdir/east.cpp" "$tmpdir/t2.cpp"
"$const_placement" --style=west -i "$tmpdir/t2.cpp" -- -std=c++17 >/dev/null
diff -u "$tmpdir/west.cpp" "$tmpdir/t2.cpp" \
  || fail "west: in-place output does not match the expected west source"
echo "PASS: --style=west rewrites east-const source in place (round trip)"

# ---------------------------------------------------------------------------
# Test 3 — both directions are fixpoints
# ---------------------------------------------------------------------------
"$const_placement" --style=east -i "$tmpdir/t1.cpp" -- -std=c++17 >/dev/null
diff -u "$tmpdir/east.cpp" "$tmpdir/t1.cpp" || fail "east is not a fixpoint"
"$const_placement" --style=west -i "$tmpdir/t2.cpp" -- -std=c++17 >/dev/null
diff -u "$tmpdir/west.cpp" "$tmpdir/t2.cpp" || fail "west is not a fixpoint"
echo "PASS: a second pass in the same direction changes nothing"

# ---------------------------------------------------------------------------
# Test 4 — the qualifier of a pointer is never moved
#
# `int* const p` is a const pointer and has no west spelling; rewriting it to
# `const int* p` would silently change the type.  Test 1 already pins that
# through the fixture, this asserts it directly on the line in isolation.
# ---------------------------------------------------------------------------
printf 'int* const p = nullptr;\n' >"$tmpdir/ptr.cpp"
cp "$tmpdir/ptr.cpp" "$tmpdir/ptr_out.cpp"
"$const_placement" --style=west -i "$tmpdir/ptr_out.cpp" -- -std=c++17 >/dev/null
diff -u "$tmpdir/ptr.cpp" "$tmpdir/ptr_out.cpp" \
  || fail "west moved the qualifier of a pointer"
echo "PASS: a pointer's own qualifier is left alone"

# ---------------------------------------------------------------------------
# Test 5 — lint: text, SARIF and a diff that git-applies to the in-place result
# ---------------------------------------------------------------------------
cp "$tmpdir/west.cpp" "$tmpdir/lint.cpp"
out="$(
  cd "$tmpdir"
  expect_violations "$const_placement" --style=east --lint lint.cpp -- -std=c++17
)"
grep -q "lint.cpp:5:1: warning: cv-qualifier should follow the type it qualifies \[east_const\]" <<<"$out" \
  || fail "text lint: missing expected diagnostic, got: $out"
diff -u "$tmpdir/west.cpp" "$tmpdir/lint.cpp" \
  || fail "text lint: input file was modified"

out="$(
  cd "$tmpdir"
  expect_violations "$const_placement" --style=east --format=sarif lint.cpp \
    -- -std=c++17
)"
grep -q '"version": "2.1.0"' <<<"$out" || fail "sarif lint: missing version"
grep -q '"ruleId": "east_const"' <<<"$out" || fail "sarif lint: missing ruleId"
grep -q '"uri": "lint.cpp"' <<<"$out" \
  || fail "sarif lint: artifact URI is not cwd-relative, got: $out"

(
  cd "$tmpdir"
  expect_violations "$const_placement" --style=east --format=diff lint.cpp \
    -- -std=c++17
) >"$tmpdir/east.patch"
(cd "$tmpdir" && git apply east.patch) \
  || fail "diff lint: emitted patch does not apply with git apply"
diff -u "$tmpdir/east.cpp" "$tmpdir/lint.cpp" \
  || fail "diff lint: patched file differs from the --in-place result"
echo "PASS: lint text/SARIF/diff agree with the in-place rewrite"

# A file already in the requested style reports nothing and exits 0.
cp "$tmpdir/east.cpp" "$tmpdir/clean.cpp"
out="$("$const_placement" --style=east --lint "$tmpdir/clean.cpp" -- -std=c++17)"
[[ -z "$out" ]] || fail "clean file: expected no diagnostics, got: $out"
echo "PASS: a file already in the requested style is clean"

# ---------------------------------------------------------------------------
# Test 6 — --jobs does not change the result
# ---------------------------------------------------------------------------
write_west "$tmpdir/j1_a.cpp"; write_west "$tmpdir/j1_b.cpp"
write_west "$tmpdir/j4_a.cpp"; write_west "$tmpdir/j4_b.cpp"
"$const_placement" --style=east -i -j1 "$tmpdir/j1_a.cpp" "$tmpdir/j1_b.cpp" \
  -- -std=c++17 >/dev/null
"$const_placement" --style=east -i -j4 "$tmpdir/j4_a.cpp" "$tmpdir/j4_b.cpp" \
  -- -std=c++17 >/dev/null
diff -u "$tmpdir/j1_a.cpp" "$tmpdir/j4_a.cpp" || fail "-j1 and -j4 differ"
diff -u "$tmpdir/j1_b.cpp" "$tmpdir/j4_b.cpp" || fail "-j1 and -j4 differ"
diff -u "$tmpdir/east.cpp" "$tmpdir/j4_b.cpp" || fail "-j4 result is wrong"
echo "PASS: --jobs does not change the result"

# ---------------------------------------------------------------------------
# Test 7 — cpp_format runs the same pass from its config, and composes with
# the return-type pass: a qualifier moved east has to ride along into the
# `-> type` that trailing_return_types lifts, not be left on the `auto`.
# ---------------------------------------------------------------------------
cat >"$tmpdir/cfg.yaml" <<'EOF'
const_placement: east
return_types: trailing
EOF
cat >"$tmpdir/combo.cpp" <<'EOF'
struct S {
  const int Get() const { return 0; }
  const int* Ptr() const { return nullptr; }
};
const int Free() { return 0; }
EOF
cp "$tmpdir/combo.cpp" "$tmpdir/combo_out.cpp"
"$cpp_format" --config="$tmpdir/cfg.yaml" -i "$tmpdir/combo_out.cpp" \
  -- -std=c++17 >/dev/null
cat >"$tmpdir/combo_expected.cpp" <<'EOF'
struct S {
  auto Get() const -> int const { return 0; }
  auto Ptr() const -> int const* { return nullptr; }
};
auto Free() -> int const { return 0; }
EOF
diff -u "$tmpdir/combo_expected.cpp" "$tmpdir/combo_out.cpp" \
  || fail "cpp_format: const_placement + return_types produced the wrong output"
echo "PASS: cpp_format composes const_placement with return_types"

# ---------------------------------------------------------------------------
# Test 8 — the Bazel path: --emit-edits + --aggregate --apply reproduces the
# in-place result exactly, including the records the return-type pass subsumes.
# ---------------------------------------------------------------------------
cp "$tmpdir/combo.cpp" "$tmpdir/emit.cpp"
"$cpp_format" --config="$tmpdir/cfg.yaml" --emit-edits="$tmpdir/emit.json" \
  "$tmpdir/emit.cpp" -- -std=c++17 >/dev/null
"$cpp_format" --aggregate --apply "$tmpdir/emit.json" >/dev/null
diff -u "$tmpdir/combo_expected.cpp" "$tmpdir/emit.cpp" \
  || fail "emit+aggregate does not match the --in-place result"
echo "PASS: --emit-edits + --aggregate matches --in-place"

# ---------------------------------------------------------------------------
# Test 9 — an unknown style is refused, by both entry points
# ---------------------------------------------------------------------------
set +e
out="$("$const_placement" --style=middle "$tmpdir/west.cpp" -- -std=c++17 2>&1)"
rc=$?
set -e
[[ $rc -eq 1 ]] || fail "unknown style: expected exit 1, got $rc"
grep -q "Valid styles: east, west" <<<"$out" \
  || fail "unknown style: missing explanation, got: $out"

set +e
out="$("$cpp_format" --const-placement=middle "$tmpdir/west.cpp" -- -std=c++17 2>&1)"
rc=$?
set -e
[[ $rc -eq 1 ]] || fail "cpp_format unknown style: expected exit 1, got $rc"
grep -q "Valid values: east, west" <<<"$out" \
  || fail "cpp_format unknown style: missing explanation, got: $out"
echo "PASS: an unknown style is refused with an explanation"

# ---------------------------------------------------------------------------
# Test 10 — nested qualified types, with a rename landing *inside* one of the
# type specifiers the move re-emits.  The moved specifier is taken through
# Rewriter::getRewrittenText, so the rename has to ride along rather than be
# clobbered; under --emit-edits the subsumed rename record has to be dropped,
# or aggregation would apply it twice.
# ---------------------------------------------------------------------------
cat >"$tmpdir/nested_cfg.yaml" <<'EOF'
const_placement: east
normalize_variables:
  - scope: member
    style: m_prefix
EOF
cat >"$tmpdir/nested.cpp" <<'EOF'
template <class A, class B>
struct P {};
struct W {};

const P<const W, const P<int, const W>>* gDeep = nullptr;

struct Holder {
  const P<const W, const W*> pairs;
  const int count_;
  const decltype(count_) mirror_;
};
EOF
cat >"$tmpdir/nested_expected.cpp" <<'EOF'
template <class A, class B>
struct P {};
struct W {};

P<W const, P<int, W const> const> const* gDeep = nullptr;

struct Holder {
  P<W const, W const*> const m_pairs;
  int const m_count;
  decltype(m_count) const m_mirror;
};
EOF

cp "$tmpdir/nested.cpp" "$tmpdir/nested_ip.cpp"
"$cpp_format" --config="$tmpdir/nested_cfg.yaml" -i "$tmpdir/nested_ip.cpp" \
  -- -std=c++17 >/dev/null
diff -u "$tmpdir/nested_expected.cpp" "$tmpdir/nested_ip.cpp" \
  || fail "nested: in-place output is wrong"

cp "$tmpdir/nested.cpp" "$tmpdir/nested_em.cpp"
"$cpp_format" --config="$tmpdir/nested_cfg.yaml" \
  --emit-edits="$tmpdir/nested.json" "$tmpdir/nested_em.cpp" -- -std=c++17 >/dev/null
"$cpp_format" --aggregate --apply "$tmpdir/nested.json" >/dev/null
diff -u "$tmpdir/nested_expected.cpp" "$tmpdir/nested_em.cpp" \
  || fail "nested: emit+aggregate does not match the --in-place result"

# ...and the whole thing still comes back byte-identical the other way.
cp "$tmpdir/nested_ip.cpp" "$tmpdir/nested_rt.cpp"
"$const_placement" --style=west -i "$tmpdir/nested_rt.cpp" -- -std=c++17 >/dev/null
sed -e 's/m_pairs/pairs/; s/m_count/count_/g; s/m_mirror/mirror_/' \
  "$tmpdir/nested_rt.cpp" >"$tmpdir/nested_rt_unrenamed.cpp"
diff -u "$tmpdir/nested.cpp" "$tmpdir/nested_rt_unrenamed.cpp" \
  || fail "nested: west does not undo east"
echo "PASS: nested qualifiers carry a rename made inside the specifier"

echo "All const_placement integration tests passed"
