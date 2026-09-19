#!/usr/bin/env bash
# iwyu.sh -- include-what-you-use (https://include-what-you-use.org/) over this
# repository's C++ targets.
#
#   Usage: tools/iwyu/iwyu.sh <check|report|fix> [target-pattern...]
#
#     check    print the files IWYU would change and what it would do to them;
#              exit 1 if there are any
#     report   print IWYU's full output for every file (what `fix` consumes)
#     fix      apply the suggestions to the sources in place with IWYU's
#              fix_includes.py, then clang-format the files it touched.
#              Review the diff: IWYU is a good advisor and a poor authority.
#
#   target-pattern defaults to //cpp_formatting/... //code_browser/... (the
#   fixtures under bazel/testdata are inputs to tests, not code to tidy).
#   Tag a target `no-iwyu` to exclude it.
#
# IWYU runs as one Bazel action per file through //tools/iwyu:iwyu.bzl, with the
# compile command Bazel derives for that file and the IWYU binary built from
# source against the repo's own Clang (@iwyu) -- so the first run builds that
# binary, and every later one re-analyses only the files that changed.  The
# mappings that keep IWYU from suggesting private headers are in
# tools/iwyu/mappings.imp.
#
# Like bazel/integration/cpp_format.sh this is a plain script, not a `bazel run`
# target: it has to call `bazel build`, and a Bazel server cannot be nested
# inside a running one.
#
# After `fix`, run `bazel run //tools/gazelle` -- the BUILD files' deps are
# derived from the includes, so they follow what IWYU just changed.

set -euo pipefail

ASPECT="//tools/iwyu:iwyu.bzl%iwyu_aspect"
BAZEL="${BAZEL:-bazel}"
PYTHON="${PYTHON:-python3}"

usage() {
  echo "usage: $0 <check|report|fix> [target-pattern...]" >&2
  exit 2
}

mode="${1:-}"
case "$mode" in
  check | report | fix) shift ;;
  *) usage ;;
esac
patterns=("$@")
[[ ${#patterns[@]} -gt 0 ]] || patterns=(//cpp_formatting/... //code_browser/...)

root="$("$BAZEL" info workspace)"
cd "$root"

query="kind('cc_(library|binary|test) rule', set(${patterns[*]})) except attr(tags, 'no-iwyu', set(${patterns[*]}))"
mapfile -t targets < <("$BAZEL" query "$query" 2>/dev/null)
if [[ ${#targets[@]} -eq 0 ]]; then
  echo "iwyu: no cc_* targets match ${patterns[*]}" >&2
  exit 2
fi

"$BAZEL" build --aspects="$ASPECT" --output_groups=iwyu -- "${targets[@]}" >&2

# Each target's manifest is at a deterministic path (//pkg:name ->
# <bazel-bin>/pkg/name.iwyu.manifest) and lists the target's *current* reports,
# exec-root relative; a report left behind by a file that has since left the
# target is never read.
bazel_bin="$("$BAZEL" info bazel-bin)"
exec_root="$("$BAZEL" info execution_root)"
reports=()
for t in "${targets[@]}"; do
  pkg="${t#//}"
  pkg="${pkg%%:*}"
  name="${t##*:}"
  manifest="$bazel_bin/$pkg/$name.iwyu.manifest"
  [[ -f "$manifest" ]] || continue
  while IFS= read -r r; do
    [[ -n "$r" ]] && reports+=("$exec_root/$r")
  done < "$manifest"
done
if [[ ${#reports[@]} -eq 0 ]]; then
  echo "iwyu: nothing to analyse under ${patterns[*]}" >&2
  exit 0
fi

case "$mode" in
  report)
    cat "${reports[@]}"
    ;;

  check)
    # A file with nothing to change is reported as "(<file> has correct
    # #includes/fwd-decls)"; everything else is a suggestion.  The same header
    # is reported by every TU it is associated with, so dedup by section.
    out="$(cat "${reports[@]}" | awk '
      / should (add|remove) these lines:$/ { keep = 1; section = $0 ORS; next }
      /^The full include-list for / { keep = 0 }
      keep && /^$/ {
        # An empty add/remove section is just its header.
        if (section ~ /\n./ && !(section in seen)) { seen[section] = 1; printf "%s\n", section }
        keep = 0; section = ""
        next
      }
      keep { section = section $0 ORS }
    ')"
    if [[ -n "$out" ]]; then
      echo "$out"
      echo "iwyu: includes need attention; run tools/iwyu/iwyu.sh fix" >&2
      exit 1
    fi
    echo "iwyu: ${#reports[@]} files clean" >&2
    ;;

  fix)
    fix_includes="$exec_root/$("$BAZEL" cquery --output=files @iwyu//:fix_includes.py 2>/dev/null | tail -1)"
    # The files IWYU has a non-empty add or remove section for.
    mapfile -t changed < <(cat "${reports[@]}" | awk '
      / should (add|remove) these lines:$/ { file = $1; next }
      file != "" && /^$/ { file = "" }
      file != "" && !(file in seen) { seen[file] = 1; print file }
    ')
    # --nosafe_headers: by default fix_includes will not remove an include from
    # a header, because a file that includes the header may have been relying on
    # it.  Every such file here is analysed by this same run and gets the
    # include itself, so the removal is safe -- and leaving it would just move
    # the dependency graph's dead edges into the headers.
    cat "${reports[@]}" |
      "$PYTHON" "$fix_includes" --nocomments --nosafe_headers --basedir="$root" >&2 || true

    # One thing IWYU cannot be talked out of with a mapping: it prefers the C
    # spelling of a C library header (<stddef.h> for size_t).  The code base
    # uses the C++ one, which IWYU accepts just as well once it is there -- so
    # this is a fixpoint, not a fight.  clang-format then regroups and sorts.
    for f in "${changed[@]}"; do
      sed -E -i \
        -e 's@^(#[[:space:]]*include[[:space:]]+)<(assert|ctype|errno|limits|math|signal|stddef|stdint|stdio|stdlib|string|time)\.h>@\1<c\2>@' \
        "$f"
    done
    if [[ ${#changed[@]} -gt 0 ]] && command -v clang-format > /dev/null; then
      clang-format -i -style=file "${changed[@]}"
    fi
    echo "iwyu: rewrote ${#changed[@]} files; review with git diff, then run: bazel run //tools/gazelle" >&2
    ;;
esac
