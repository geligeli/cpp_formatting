#!/usr/bin/env bash
# The lint gate: merges the edit records the cpp_format aspect emitted (see
# aspect_outputs.bzl) with `aggregate_edits --check`, which counts the edits
# without reading a source file and exits 1 if there are any.
#
#   $1   aggregate_edits
#   $2.. the per-file edit records
set -euo pipefail
agg="$1"
shift
[[ $# -gt 0 ]] || { echo "FAIL: no edit records given" >&2; exit 1; }
exec "$agg" --check --root="$PWD" "$@"
