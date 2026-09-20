#!/usr/bin/env bash
# run_e2e.sh -- end-to-end corpus tests: build a real Bazel C++ repo, run a
# cpp_format transformation over all of it, and build it again.
#
#   Usage: e2e/run_e2e.sh [options] [scenario ...]      (no scenario = all)
#
# A rebuild that fails is the signal this harness exists for: a rename that
# missed a reference, or a rewrite that produced invalid code, shows up as a
# compile error exactly as it would for a user.
#
# This is deliberately NOT a Bazel target.  It drives `bazel` inside a second
# workspace, and nesting a Bazel server inside a running one deadlocks on the
# workspace lock -- the same reason bazel/integration/cpp_format.sh is a plain
# script.  Run it directly from a shell.
#
# Exit codes:  0 every scenario met its expectation
#              1 an expectation was violated (a real regression, or an XPASS)
#              2 setup/environment failure -- nothing was learned about the tool

set -euo pipefail

E2E_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$E2E_DIR/.." && pwd)"

# shellcheck source=lib/assert.sh
source "$E2E_DIR/lib/assert.sh"
# shellcheck source=lib/workspace.sh
source "$E2E_DIR/lib/workspace.sh"

# A stray Bazel test/run context would corrupt the inner invocations.
unset TEST_TMPDIR TEST_SRCDIR RUNFILES_DIR RUNFILES_MANIFEST_FILE \
      JAVA_RUNFILES BUILD_WORKSPACE_DIRECTORY BUILD_WORKING_DIRECTORY 2>/dev/null || true

WORK="${E2E_WORK_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/cpp_format_e2e}"
OUT=""
TOOL_SPEC="source"
PHASE_TIMEOUT="${E2E_PHASE_TIMEOUT:-7200}"
JOBS=""
FRESH=""
KEEP=""
DRY_RUN=""
LIST=""

usage() {
  sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'
  cat <<'EOF'

Options:
  --list                 list the available scenarios and exit
  --tool=SPEC            source (default) | release:<tag> | path:<abs path>
  --work-dir=DIR         checkouts, caches and staged binaries (default
                         $XDG_CACHE_HOME/cpp_format_e2e; must be outside the repo)
  --out-dir=DIR          artifacts: logs, patches, summaries (default <work>/artifacts)
  --jobs=N               passed to the target repo's builds
  --phase-timeout=SECS   per-phase timeout (default 7200)
  --fresh                re-fetch the target repo instead of reusing the checkout
  --keep                 keep the target workspace on success (it is kept on failure)
  --dry-run              print what each scenario would do, touch nothing
  -v, --verbose          stream command output as well as logging it
EOF
}

for arg in "$@"; do
  case "$arg" in
    --help|-h) usage; exit 0 ;;
  esac
done

SCENARIOS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --list) LIST=1 ;;
    --tool=*) TOOL_SPEC="${1#*=}" ;;
    --work-dir=*) WORK="${1#*=}" ;;
    --out-dir=*) OUT="${1#*=}" ;;
    --jobs=*) JOBS="${1#*=}" ;;
    --phase-timeout=*) PHASE_TIMEOUT="${1#*=}" ;;
    --fresh) FRESH=1 ;;
    --keep) KEEP=1 ;;
    --dry-run) DRY_RUN=1 ;;
    -v|--verbose) E2E_VERBOSE=1 ;;
    -*) die "unknown option: $1  (try --help)" ;;
    *) SCENARIOS+=("$1") ;;
  esac
  shift
done
export E2E_VERBOSE="${E2E_VERBOSE:-}"

case "$TOOL_SPEC" in
  source|release:*|path:*) ;;
  *) die "--tool must be source, release:<tag> or path:<abs path>; got '$TOOL_SPEC'" ;;
esac

: "${OUT:=$WORK/artifacts}"

all_scenarios() {
  local f
  for f in "$E2E_DIR"/scenarios/*.scenario; do
    [[ -e "$f" ]] || continue
    basename "$f" .scenario
  done
}

if [[ -n "$LIST" ]]; then
  for s in $(all_scenarios); do
    # shellcheck disable=SC1090
    ( EXPECT=pass; RULESET_THEN=""; source "$E2E_DIR/scenarios/$s.scenario"
      printf '  %-40s %s -> %s%s  [%s]\n' "$s" "$REPO" "$RULESET" \
        "$([[ -n "$RULESET_THEN" ]] && printf ', then %s' "$RULESET_THEN")" "$EXPECT" )
  done
  exit 0
fi

[[ ${#SCENARIOS[@]} -gt 0 ]] || mapfile -t SCENARIOS < <(all_scenarios)
[[ ${#SCENARIOS[@]} -gt 0 ]] || die "no scenarios found in $E2E_DIR/scenarios"

# ---------------------------------------------------------------------------
# Phases.  Each returns 0, or calls fail() and returns 1.  The driver stops the
# scenario at the first failure and records which phase it was.
# ---------------------------------------------------------------------------

# Bazel flags for the *target* repo's builds.  The repo's own .bazelrc (which
# wire() appended to) carries the cache and strategy settings.
target_build_flags() {
  local f=()
  [[ -n "$JOBS" ]] && f+=("--jobs=$JOBS")
  [[ ${#BUILD_FLAGS[@]} -gt 0 ]] && f+=("${BUILD_FLAGS[@]}")
  # Printing an empty array would emit one blank line, which mapfile turns into
  # an empty argument -- Bazel then rejects it as an invalid target name.
  [[ ${#f[@]} -gt 0 ]] && printf '%s\n' "${f[@]}"
  return 0
}

phase_tool() { stage_tool "$LOGDIR"; }

phase_materialize() { materialize "$SRC" "$LOGDIR"; }

phase_wire() {
  wire "$SRC" "$E2E_DIR/rulesets/$RULESET.yaml" "$DISK_CACHE" || return 1
  if ! run_logged "$LOGDIR/wire-resolve.log" \
         env -C "$SRC" "$TARGET_BAZEL" query "@cpp_format_bin//:cpp_format"; then
    log_tail "$LOGDIR/wire-resolve.log"
    fail "@cpp_format_bin//:cpp_format does not resolve after wiring"
    return 1
  fi
  return 0
}

# The wrapper swallows its query's stderr, so a broken query would look like
# "no cc targets" and exit 0 -- which the vacuity check below would then
# misreport as "the ruleset produced no edits".  Run the same query visibly.
phase_enumerate() {
  local q="kind('cc_(library|binary|test) rule', $TARGET_PATTERN) except attr(tags, 'no-cpp-format', $TARGET_PATTERN)"
  if ! run_logged "$LOGDIR/enumerate.log" env -C "$SRC" "$TARGET_BAZEL" query "$q"; then
    log_tail "$LOGDIR/enumerate.log"
    fail "target enumeration query failed"
    return 1
  fi
  N_TARGETS="$(grep -c '^//' "$LOGDIR/enumerate.log" || true)"
  [[ "$N_TARGETS" -gt 0 ]] || { fail "no cc_* targets under $TARGET_PATTERN"; return 1; }
  pass "$N_TARGETS cc_* targets under $TARGET_PATTERN"
}

_target_build() {
  local log="$1" verb="$2"; shift 2
  local flags=(); mapfile -t flags < <(target_build_flags)
  run_logged_timeout "$PHASE_TIMEOUT" "$log" \
    env -C "$SRC" "$TARGET_BAZEL" "$verb" "${flags[@]}" "$@" "$TARGET_PATTERN"
}

phase_baseline() {
  local verb=build; [[ "$RUN_TESTS" == 1 ]] && verb=test
  if ! _target_build "$LOGDIR/baseline.log" "$verb"; then
    log_tail "$LOGDIR/baseline.log" 40
    fail "the unmodified repo does not $verb cleanly -- check the pin and flags"
    return 1
  fi
  pass "bazel $verb $TARGET_PATTERN"
}

# Runs the vendored wrapper exactly as a user would.
_cpp_format_sh() {
  local mode="$1" log_out="$2" log_err="$3"
  env -C "$SRC" \
      CPP_FORMAT_ASPECT="//third_party/cpp_format:cpp_format.bzl%cpp_format_aspect" \
      CPP_FORMAT_BIN_LABEL="@cpp_format_bin//:cpp_format" \
      timeout --foreground "$PHASE_TIMEOUT" ./tools/cpp_format.sh "$mode" "$TARGET_PATTERN" \
      >"$log_out" 2>"$log_err"
}

# Parses `<abs path>: N edit(s)` lines (stdout) into REPORTED_FILES/EDITS_TOTAL,
# and `conflict: ...` lines (stderr) into N_CONFLICTS.
_parse_check() {
  local out="$1" err="$2"
  REPORTED_FILES=(); EDITS_TOTAL=0; OUTSIDE_SRC=()
  local line f n
  while IFS= read -r line; do
    [[ "$line" =~ ^(.+):\ ([0-9]+)\ edit\(s\)$ ]] || continue
    f="${BASH_REMATCH[1]}"; n="${BASH_REMATCH[2]}"
    if [[ "$f" == "$SRC"/* ]]; then
      REPORTED_FILES+=("${f#"$SRC"/}")
    else
      OUTSIDE_SRC+=("$f")
    fi
    EDITS_TOTAL=$(( EDITS_TOTAL + n ))
  done < "$out"
  N_CONFLICTS="$(grep -c '^conflict: ' "$err" || true)"
}

phase_check() {
  local rc=0
  _cpp_format_sh check "$LOGDIR/check.out" "$LOGDIR/check.err" || rc=$?
  _parse_check "$LOGDIR/check.out" "$LOGDIR/check.err"

  if [[ ${#OUTSIDE_SRC[@]} -gt 0 ]]; then
    note "  first offending path: ${OUTSIDE_SRC[0]}"
    fail "records name ${#OUTSIDE_SRC[@]} path(s) outside the target tree (stale disk cache?)"
    return 1
  fi
  if [[ "$rc" -ne 1 ]]; then
    log_tail "$LOGDIR/check.err"
    fail "check exited $rc, expected 1 (violations found)"
    return 1
  fi
  # A ruleset that changes nothing proves nothing.
  if [[ "$EDITS_TOTAL" -eq 0 ]]; then
    fail "vacuous scenario: the ruleset produced no edits"
    return 1
  fi
  if [[ "$N_CONFLICTS" -ne "$EXPECT_CONFLICTS" ]]; then
    log_tail "$LOGDIR/check.err"
    fail "$N_CONFLICTS overlapping-edit conflict(s), expected $EXPECT_CONFLICTS"
    return 1
  fi
  pass "$EDITS_TOTAL edit(s) across ${#REPORTED_FILES[@]} file(s), $N_CONFLICTS conflict(s)"
}

phase_diff() {
  local rc=0 patch="$ARTDIR/$RULESET.patch"
  _cpp_format_sh diff "$patch" "$LOGDIR/diff.err" || rc=$?
  [[ "$rc" -eq 0 ]] || { log_tail "$LOGDIR/diff.err"; fail "diff exited $rc"; return 1; }
  [[ -s "$patch" ]] || { fail "diff produced an empty patch"; return 1; }
  pass "$(wc -l < "$patch") line patch -> ${ARTDIR##*/}/$RULESET.patch"
}

phase_fix() {
  local rc=0
  _cpp_format_sh fix "$LOGDIR/fix.out" "$LOGDIR/fix.err" || rc=$?
  [[ "$rc" -eq 0 ]] || { log_tail "$LOGDIR/fix.err"; fail "fix exited $rc"; return 1; }
  pass "applied"
}

# The set of files that actually changed on disk must equal the set `check`
# reported.  A short set is how a silently-failed write shows up: flush() uses
# std::ofstream without checking the failbit, so an unwritable file is skipped
# with exit code 0.
phase_applied() {
  local changed reported
  changed="$(git -C "$SRC" diff --name-only | sort)"
  reported="$(printf '%s\n' "${REPORTED_FILES[@]}" | sort)"
  if [[ "$changed" != "$reported" ]]; then
    diff <(printf '%s\n' "$reported") <(printf '%s\n' "$changed") \
      | sed 's/^/  /' | head -30
    fail "files changed on disk differ from the files check reported (< reported, > changed)"
    return 1
  fi
  pass "$(printf '%s\n' "$changed" | grep -c . ) file(s) changed, matching the report"
}

phase_rebuild() {
  local verb=build; [[ "$RUN_TESTS" == 1 ]] && verb=test
  if ! _target_build "$LOGDIR/rebuild.log" "$verb" --keep_going; then
    local errs files
    errs="$(grep -c ' error: ' "$LOGDIR/rebuild.log" || true)"
    files="$(grep -o '^[^ :]*:[0-9]*:[0-9]*: error: ' "$LOGDIR/rebuild.log" \
             | cut -d: -f1 | sort -u | grep -c . || true)"
    log_tail "$LOGDIR/rebuild.log" 40
    fail "$errs error(s) in $files file(s) after the transform"
    return 1
  fi
  pass "bazel $verb $TARGET_PATTERN"
}

# The transform must be a fixpoint: a second pass has nothing left to do.
phase_converge() {
  local rc=0
  _cpp_format_sh check "$LOGDIR/converge.out" "$LOGDIR/converge.err" || rc=$?
  if [[ "$rc" -ne 0 ]]; then
    _parse_check "$LOGDIR/converge.out" "$LOGDIR/converge.err"
    note "  $EDITS_TOTAL edit(s) still pending across ${#REPORTED_FILES[@]} file(s)"
    fail "did not converge: a second pass still wants changes"
    return 1
  fi
  pass "fixpoint reached"
}

# A scenario with RULESET_THEN runs the whole transform twice: the second pass
# starts from the first one's *output*, which is how "forward, then back"
# (trailing return types, then leading) gets a rebuild of its own.  Each leg
# reuses the same phase bodies; only the ruleset in place differs.
phase_swap() {
  # Swap the ruleset first, then commit: the commit has to include the new
  # cpp_format.yaml, or the second pass's `applied` check would see it as a
  # file the tool changed but did not report.
  cp "$E2E_DIR/rulesets/$RULESET_THEN.yaml" "$SRC/cpp_format.yaml"
  git -C "$SRC" add -A
  _git_commit "$SRC" "cpp_format e2e: after $RULESET"
  RULESET="$RULESET_THEN"
  pass "pass 1 committed; ruleset now $RULESET_THEN"
}
phase_check2() { phase_check; }
phase_diff2() { phase_diff; }
phase_fix2() { phase_fix; }
phase_applied2() { phase_applied; }
phase_rebuild2() { phase_rebuild; }
phase_converge2() { phase_converge; }

# Opt-in, for a corpus where the second ruleset is known to undo the first
# exactly: the sources must come back byte-identical to what they were before
# either pass ran.  A weaker round trip still rebuilds and converges, so this
# is the assertion that catches the reverse quietly recovering *less* than the
# forward direction moved.  e2e-wired is the commit wire() made, so the diff is
# only ever the tool's own writes; the ruleset itself changed at swap.
phase_roundtrip() {
  local changed
  changed="$(git -C "$SRC" diff --name-only e2e-wired -- \
               . ':(exclude)cpp_format.yaml')"
  if [[ -n "$changed" ]]; then
    printf '%s\n' "$changed" | sed 's/^/  /' | head -20
    fail "$(printf '%s\n' "$changed" | grep -c .) file(s) did not come back to their original content"
    return 1
  fi
  pass "sources are byte-identical to the pre-transform tree"
}

PHASES=(tool materialize wire enumerate baseline check diff fix applied rebuild converge)
SECOND_PASS_PHASES=(swap check2 diff2 fix2 applied2 rebuild2 converge2 roundtrip)
# A failure in any of these says nothing about the tool -- the environment, the
# pin or the injected flags are wrong.
SETUP_PHASES=" tool materialize wire enumerate baseline "

# ---------------------------------------------------------------------------

run_scenario() {
  local scen="$1"

  # Defaults, reset per scenario so nothing leaks between them.
  REPO=""; RULESET=""; RULESET_THEN=""; EXPECT="pass"
  EXPECT_FAIL_PHASE=""; EXPECT_FAIL_REASON=""
  EXPECT_CONFLICTS=0; EXPECT_CONVERGES=1; EXPECT_ROUNDTRIP_IDENTICAL=0
  REPO_KIND="git"; REPO_URL=""; REPO_REV=""; REPO_PATH=""
  TARGET_PATTERN="//..."; RUN_TESTS=0; BAZELVERSION=""; BUILD_FLAGS=()
  REPORTED_FILES=(); EDITS_TOTAL=0; N_CONFLICTS=0; N_TARGETS=0; OUTSIDE_SRC=()

  local sfile="$E2E_DIR/scenarios/$scen.scenario"
  [[ -f "$sfile" ]] || die "no such scenario: $scen"
  # shellcheck disable=SC1090
  source "$sfile"
  [[ -n "$REPO" ]] || die "$scen: REPO is not set"
  [[ -n "$RULESET" ]] || die "$scen: RULESET is not set"
  local rfile="$E2E_DIR/repos/$REPO.repo"
  [[ -f "$rfile" ]] || die "$scen: no such repo definition: $REPO"
  # shellcheck disable=SC1090
  source "$rfile"
  REPO_NAME="$REPO"
  [[ -f "$E2E_DIR/rulesets/$RULESET.yaml" ]] || die "$scen: no such ruleset: $RULESET"
  [[ -z "$RULESET_THEN" || -f "$E2E_DIR/rulesets/$RULESET_THEN.yaml" ]] \
    || die "$scen: no such ruleset: $RULESET_THEN"
  [[ -n "$BAZELVERSION" ]] || BAZELVERSION="$(cat "$REPO_ROOT/.bazelversion" 2>/dev/null || echo 8.6.0)"
  RULES_CC_VERSION="$(sed -n 's/.*bazel_dep(name = "rules_cc", version = "\([^"]*\)").*/\1/p' \
                        "$REPO_ROOT/MODULE.bazel" | head -1)"
  [[ -n "$RULES_CC_VERSION" ]] || RULES_CC_VERSION=0.2.17
  [[ "$EXPECT" != known_fail || -n "$EXPECT_FAIL_PHASE" ]] \
    || die "$scen: EXPECT=known_fail requires EXPECT_FAIL_PHASE"

  FIRST_RULESET="$RULESET"
  SRC="$WORK/$scen/src"
  DISK_CACHE="$WORK/$scen/disk"
  LOGDIR="$OUT/$scen"
  ARTDIR="$OUT/$scen"

  local hdr="$scen"
  [[ "$EXPECT" == known_fail ]] && hdr="$hdr  (expect: known_fail@$EXPECT_FAIL_PHASE)"
  head1 "$hdr"

  if [[ -n "$DRY_RUN" ]]; then
    info "  repo      $REPO ($REPO_KIND ${REPO_REV:-$REPO_PATH})"
    info "  ruleset   $RULESET${RULESET_THEN:+, then $RULESET_THEN}"
    info "  pattern   $TARGET_PATTERN   tests=$RUN_TESTS   bazel=$BAZELVERSION"
    info "  src       $SRC"
    info "  disk      $DISK_CACHE"
    info "  artifacts $ARTDIR"
    return 0
  fi

  mkdir -p "$LOGDIR" "$DISK_CACHE" "$WORK/repo-cache"

  FAILED_PHASE=""; FAIL_MSG=""
  local ph
  local -a phases=("${PHASES[@]}")
  [[ -n "$RULESET_THEN" ]] && phases+=("${SECOND_PASS_PHASES[@]}")
  for ph in "${phases[@]}"; do
    if [[ "$ph" == converge* && "$EXPECT_CONVERGES" != 1 ]]; then
      skip "$ph" "EXPECT_CONVERGES=0"
      continue
    fi
    if [[ "$ph" == roundtrip && "$EXPECT_ROUNDTRIP_IDENTICAL" != 1 ]]; then
      skip "$ph" "EXPECT_ROUNDTRIP_IDENTICAL=0"
      continue
    fi
    begin_phase "$ph"
    if ! "phase_$ph"; then
      FAILED_PHASE="$ph"; FAIL_MSG="$PHASE_FAIL_MSG"
      break
    fi
  done

  # Leave the workspace behind for triage unless it passed and --keep was not given.
  if [[ -z "$FAILED_PHASE" && -z "$KEEP" ]]; then
    env -C "$SRC" "$TARGET_BAZEL" shutdown >/dev/null 2>&1 || true
  fi

  _write_summary "$scen"
  _evaluate "$scen"
}

_write_summary() {
  local scen="$1"
  {
    printf 'scenario:   %s\n' "$scen"
    printf 'repo:       %s @ %s\n' "$REPO" "${REPO_REV:-$REPO_PATH}"
    printf 'ruleset:    %s%s\n' "$FIRST_RULESET" \
      "$([[ -n "$RULESET_THEN" ]] && printf ', then %s' "$RULESET_THEN")"
    printf 'tool:       %s %s\n' "$TOOL_SPEC" "${TOOL_VERSION:-}"
    printf 'targets:    %s\n' "$N_TARGETS"
    printf 'edits:      %s across %s file(s)\n' "$EDITS_TOTAL" "${#REPORTED_FILES[@]}"
    printf 'conflicts:  %s\n' "$N_CONFLICTS"
    printf 'expected:   %s%s\n' "$EXPECT" \
      "$([[ "$EXPECT" == known_fail ]] && printf '@%s' "$EXPECT_FAIL_PHASE")"
    printf 'failed at:  %s\n' "${FAILED_PHASE:-<none>}"
    [[ -n "$FAIL_MSG" ]] && printf 'message:    %s\n' "$FAIL_MSG"
    [[ -n "$EXPECT_FAIL_REASON" ]] && printf 'reason:     %s\n' "$EXPECT_FAIL_REASON"
  } > "$ARTDIR/summary.txt"
}

# Returns 0 if the scenario met its expectation, 1 if not, and exits 2 outright
# on a setup failure (which tells us nothing about the tool).
_evaluate() {
  local scen="$1"
  if [[ -n "$FAILED_PHASE" && "$SETUP_PHASES" == *" $FAILED_PHASE "* ]]; then
    printf '%sSETUP-FAIL%s: %s failed at %s -- %s\n' \
      "$_C_RED" "$_C_OFF" "$scen" "$FAILED_PHASE" "$FAIL_MSG" >&2
    exit 2
  fi

  if [[ -z "$FAILED_PHASE" ]]; then
    if [[ "$EXPECT" == known_fail ]]; then
      printf '%sXPASS%s: %s passed, but is declared known_fail@%s.\n' \
        "$_C_RED" "$_C_OFF" "$scen" "$EXPECT_FAIL_PHASE"
      printf '       The limitation no longer reproduces -- promote it to EXPECT=pass.\n'
      return 1
    fi
    printf '%sPASS scenario%s %s\n' "$_C_GRN" "$_C_OFF" "$scen"
    return 0
  fi

  if [[ "$EXPECT" == known_fail && "$FAILED_PHASE" == "$EXPECT_FAIL_PHASE" ]]; then
    xfail "$FAILED_PHASE" "$FAIL_MSG"
    [[ -n "$EXPECT_FAIL_REASON" ]] && note "       $EXPECT_FAIL_REASON"
    printf '%sXFAIL scenario%s %s (as declared)\n' "$_C_YEL" "$_C_OFF" "$scen"
    return 0
  fi

  if [[ "$EXPECT" == known_fail ]]; then
    printf '%sFAIL scenario%s %s: failed at %s, but the declared failure is %s\n' \
      "$_C_RED" "$_C_OFF" "$scen" "$FAILED_PHASE" "$EXPECT_FAIL_PHASE"
  else
    printf '%sFAIL scenario%s %s at %s: %s\n' \
      "$_C_RED" "$_C_OFF" "$scen" "$FAILED_PHASE" "$FAIL_MSG"
  fi
  return 1
}

# ---------------------------------------------------------------------------

mkdir -p "$WORK" "$OUT"

# Every Bazel that runs in a *target* workspace goes through this shim, which
# keeps the machine's rc files out of it.  A target repository compiles with
# whatever toolchain Bazel finds on the machine, and a home rc that turns on
# remote execution (as one does once this repository's own toolchain is
# hermetic) sends those compiles to workers that have no compiler: "Remote
# Execution Failure" at `baseline`, or not, depending on which side of a dynamic
# execution race won.  Startup options cannot come from the target's own
# .bazelrc -- the home rc is read after it -- so they go on the command line,
# and a shim rather than a flag at every call site because tools/cpp_format.sh
# runs Bazel too (it honours \$BAZEL).  The build of the tool itself, in this
# repository, still uses plain `bazel` and whatever the machine configures.
TARGET_BAZEL="$WORK/bin/bazel"
mkdir -p "$WORK/bin"
printf '#!/usr/bin/env bash\nexec %q --nohome_rc --nosystem_rc "$@"\n' "$(command -v bazel)" > "$TARGET_BAZEL"
chmod +x "$TARGET_BAZEL"
export TARGET_BAZEL BAZEL="$TARGET_BAZEL"
rc=0
declare -a RESULTS=()
for s in "${SCENARIOS[@]}"; do
  if run_scenario "$s"; then RESULTS+=("ok   $s"); else rc=1; RESULTS+=("FAIL $s"); fi
done

if [[ -z "$DRY_RUN" && ${#SCENARIOS[@]} -gt 1 ]]; then
  head1 "summary"
  printf '  %s\n' "${RESULTS[@]}"
  info "artifacts: $OUT"
fi
exit $rc
