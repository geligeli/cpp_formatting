#!/usr/bin/env bash
# This repository's own cpp_format.sh: bazel/integration/cpp_format.sh with
# everything built from source.
#
#   tools/cpp_format.sh browse            # index the whole repo and browse it
#   tools/cpp_format.sh browse --check    # ... stop after the index's stats
#   tools/cpp_format.sh coverage          # ... with the tests' coverage overlaid
#   tools/cpp_format.sh <check|diff|fix|compile_commands|index> [pattern] [flags]
#
# The kit's script is written for a consumer of the prebuilt release -- its
# defaults name the release binaries, and `install` places it as
# tools/cpp_format.sh over there -- and this repository pins no release of its
# own.  This points the same script at the from-source aspect and at
# //cpp_formatting:cpp_format and //code_browser:code_browser instead; see its
# header for the modes.
#
# A plain script, not a `bazel run` target: it calls bazel, and nesting that
# deadlocks on the workspace lock.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export CPP_FORMAT_ASPECT="${CPP_FORMAT_ASPECT:-//bazel:cpp_format.bzl%cpp_format_aspect}"
export CPP_FORMAT_BIN_LABEL="${CPP_FORMAT_BIN_LABEL:-//cpp_formatting:cpp_format}"
export CODE_BROWSER_LABEL="${CODE_BROWSER_LABEL:-//code_browser:code_browser}"
# What the script calls itself in the instructions it prints.
export CPP_FORMAT_SH_NAME="${CPP_FORMAT_SH_NAME:-$0}"
# `coverage`: .bazelrc's own `coverage` lines instrument this repository (its
# two packages, not their test helpers), so the kit's toolchain-derived flags
# stay out of the way.  CPP_FORMAT_COVERAGE_AUTO=1 tries the kit's instead.
export CPP_FORMAT_COVERAGE_AUTO="${CPP_FORMAT_COVERAGE_AUTO:-0}"
cd "$here/.."
exec bazel/integration/cpp_format.sh "$@"
