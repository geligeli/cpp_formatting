#!/usr/bin/env bash
# The `bazel coverage` flags `cpp_format.sh coverage` derives from a
# workspace's C++ toolchain -- the script is *sourced* for its helpers, since
# the rest of it drives Bazel.  What the helpers are given is what the script
# gets from
#   bazel cquery 'filter("bin/llvm-(cov|profdata)$", deps(<cc toolchain>))' \
#     --output=files
# and from `llvm-profdata --version`.  The flags themselves are checked end to
# end by `CPP_FORMAT_COVERAGE_AUTO=1 tools/cpp_format.sh coverage` (a Clang
# toolchain) and by e2e/kit_browse_test.sh (a consumer on g++).
set -euo pipefail

script="$1"  # bazel/integration/cpp_format.sh

fail() { echo "FAIL: $*" >&2; exit 1; }
lines() { printf '%s\n' "$@"; }

# Sourcing defines the helpers and does nothing else: no usage error, no Bazel.
# shellcheck source=/dev/null
source "$script"
for f in llvm_coverage_tools llvm_major_version coverage_flags; do
  declare -F "$f" > /dev/null || fail "sourcing did not define $f"
done

# 1. The tools of one toolchain, llvm-cov first.
hermetic=external/llvm++llvm_toolchain_minimal+llvm-toolchain-minimal-linux-amd64/bin
got="$(lines "$hermetic/llvm-profdata" "$hermetic/llvm-cov" | llvm_coverage_tools)"
[[ "$got" == "$(lines "$hermetic/llvm-cov" "$hermetic/llvm-profdata")" ]] \
  || fail "hermetic-llvm's tools: got '$got'"
# A toolchain without them (autodetected g++ has gcov, which the query leaves
# out anyway), or with only one of the two: nothing.
[[ -z "$(printf '' | llvm_coverage_tools)" ]] || fail "no tools must print nothing"
[[ -z "$(lines "$hermetic/llvm-cov" | llvm_coverage_tools)" ]] \
  || fail "llvm-cov alone must print nothing"
# Two toolchains' files: the first directory that has both, never a mix.
got="$(lines a/bin/llvm-cov b/bin/llvm-cov b/bin/llvm-profdata | llvm_coverage_tools)"
[[ "$got" == "$(lines b/bin/llvm-cov b/bin/llvm-profdata)" ]] || fail "two toolchains: got '$got'"
got="$(lines bin/llvm-profdata bin/llvm-cov | llvm_coverage_tools)"
[[ "$got" == "$(lines bin/llvm-cov bin/llvm-profdata)" ]] || fail "a main-repository toolchain: got '$got'"

# 2. The version.
[[ "$(printf 'Ubuntu LLVM version 23.1.0\n  Optimized build.\n' | llvm_major_version)" == 23 ]] \
  || fail "version 23"
[[ "$(printf 'LLVM (http://llvm.org/):\n  LLVM version 19.1.7\n' | llvm_major_version)" == 19 ]] \
  || fail "version 19"
[[ -z "$(printf 'something else\n' | llvm_major_version)" ]] || fail "no version"

# 3. The flags.
generic="$(lines --keep_going --build_tests_only --combined_report=lcov)"
[[ "$(coverage_flags 0 "" "" 0)" == "$generic" ]] \
  || fail "CPP_FORMAT_COVERAGE_AUTO=0 must add nothing to the generic flags"
# The same for a toolchain with the tools: the workspace's rc decides.
[[ "$(coverage_flags 0 "$hermetic/llvm-cov" "$hermetic/llvm-profdata" 1)" == "$generic" ]] \
  || fail "CPP_FORMAT_COVERAGE_AUTO=0 must ignore the tools"
[[ "$(coverage_flags 1 "" "" 0)" == "$(lines "$generic" '--instrumentation_filter=^//')" ]] \
  || fail "not Clang: the generic flags and the main repository"

clang="$(coverage_flags 1 "$hermetic/llvm-cov" "$hermetic/llvm-profdata" 1)"
want="$(lines "$generic" '--instrumentation_filter=^//' \
  --experimental_use_llvm_covmap --experimental_generate_llvm_lcov \
  --features=-llvm_coverage_map_format \
  '--per_file_copt=^//@-fprofile-instr-generate,-fcoverage-mapping,-fprofile-update=atomic,-fprofile-continuous' \
  --linkopt=-fprofile-instr-generate \
  "--test_env=COVERAGE_GCOV_PATH=$hermetic/llvm-profdata" \
  "--test_env=LLVM_COV=$hermetic/llvm-cov" \
  --test_env=LLVM_PROFILE_CONTINUOUS_MODE=1)"
[[ "$clang" == "$want" ]] || fail "Clang flags:
$clang
--- wanted:
$want"
# Before Clang 20: no continuous mode, and nothing that asks for it.
old="$(coverage_flags 1 "$hermetic/llvm-cov" "$hermetic/llvm-profdata" 0)"
[[ "$old" != *continuous* && "$old" != *CONTINUOUS* ]] || fail "continuous mode without Clang 20"
[[ "$old" == *'-fprofile-update=atomic'* ]] || fail "atomic counters regardless"
# The per-file filter is the part before the first `@` (Bazel splits there):
# exactly `^//`, main-repository labels only.
copt="$(grep -- '^--per_file_copt=' <<< "$clang")"
copt="${copt#--per_file_copt=}"
[[ "${copt%%@*}" == '^//' ]] || fail "the per-file filter is '${copt%%@*}'"

echo "PASS"
