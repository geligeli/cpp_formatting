#!/usr/bin/env bash
# Output and assertion helpers for the e2e harness.
#
# Style follows cpp_formatting/*_integration_test.sh: one PASS: line per logical
# step, loud failures, no test framework.  Sourced by run_e2e.sh; not executable
# on its own.

if [[ -t 1 ]]; then
  _C_RED=$'\033[31m'; _C_GRN=$'\033[32m'; _C_YEL=$'\033[33m'
  _C_BLD=$'\033[1m'; _C_DIM=$'\033[2m'; _C_OFF=$'\033[0m'
else
  _C_RED=; _C_GRN=; _C_YEL=; _C_BLD=; _C_DIM=; _C_OFF=
fi

# Current phase name and the message explaining why it failed.  The driver reads
# both after a phase function returns non-zero.
PHASE=""
PHASE_FAIL_MSG=""
_phase_start=0

_elapsed() {
  local s=$(( SECONDS - _phase_start ))
  printf '%dm%02ds' $(( s / 60 )) $(( s % 60 ))
}

info() { printf '%s\n' "$*"; }
note() { printf '%s%s%s\n' "$_C_DIM" "$*" "$_C_OFF"; }
head1() { printf '\n%s=== %s ===%s\n' "$_C_BLD" "$*" "$_C_OFF"; }

begin_phase() { PHASE="$1"; PHASE_FAIL_MSG=""; _phase_start=$SECONDS; }

# pass <message...> — the current phase succeeded.
pass() {
  printf '%sPASS%s:  %-12s %s %s(%s)%s\n' \
    "$_C_GRN" "$_C_OFF" "$PHASE" "$*" "$_C_DIM" "$(_elapsed)" "$_C_OFF"
}

# fail <message...> — the current phase failed.  Always `return 1` from the
# phase function right after calling this (or `fail ... || return 1`).
fail() {
  PHASE_FAIL_MSG="$*"
  printf '%sFAIL%s:  %-12s %s %s(%s)%s\n' \
    "$_C_RED" "$_C_OFF" "$PHASE" "$*" "$_C_DIM" "$(_elapsed)" "$_C_OFF"
  return 1
}

# xfail / skip are printed by the driver once an expectation has been evaluated.
xfail() { printf '%sXFAIL%s: %-12s %s\n' "$_C_YEL" "$_C_OFF" "$1" "${*:2}"; }
skip()  { printf '%sSKIP%s:  %-12s %s\n' "$_C_DIM" "$_C_OFF" "$1" "${*:2}"; }

# die <message...> — environment/setup problem; never a tool regression.
die() { printf '%sSETUP-FAIL%s: %s\n' "$_C_RED" "$_C_OFF" "$*" >&2; exit 2; }

# run_logged <logfile> <cmd...> — run a command, capturing output to the log.
# Returns the command's exit status.  E2E_VERBOSE=1 also streams it.
run_logged() {
  local log="$1"; shift
  mkdir -p "${log%/*}"
  local rc=0
  if [[ -n "${E2E_VERBOSE:-}" ]]; then
    "$@" > >(tee "$log") 2>&1 || rc=$?
  else
    "$@" >"$log" 2>&1 || rc=$?
  fi
  return $rc
}

# run_logged_timeout <secs> <logfile> <cmd...>
run_logged_timeout() {
  local secs="$1" log="$2"; shift 2
  run_logged "$log" timeout --foreground "$secs" "$@"
}

# log_tail <logfile> [lines] — show the end of a log after a failure.
log_tail() {
  local log="$1" n="${2:-25}"
  [[ -f "$log" ]] || return 0
  note "  --- last $n lines of ${log##*/} ---"
  tail -n "$n" "$log" | sed 's/^/  /'
  note "  --- (full log: $log) ---"
}
