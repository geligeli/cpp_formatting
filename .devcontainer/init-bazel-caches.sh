#!/bin/sh
# Prepare the host-side Bazel cache directories that devcontainer.json binds in.
# Runs on the *host*, before the container is created (initializeCommand).
#
# The devcontainer CLI mounts with `docker --mount type=bind`, which -- unlike
# `-v` -- refuses to create a missing source ("bind source path does not
# exist") and fails before the container starts.  So the mount sources must be
# paths that exist on every host: <workspace>/.disk_cache and
# <workspace>/.bazel_cache, created here (they are the same directories CI
# already persists with actions/cache).
#
# To share one cache across checkouts -- an NFS cache, say -- point
# BAZEL_DISK_CACHE / BAZEL_REPO_CACHE at it in the host environment:
#
#   export BAZEL_DISK_CACHE=/large_nfs/bazel-cache/disk
#   export BAZEL_REPO_CACHE=/large_nfs/bazel-cache/repo
#
# The workspace entry then becomes a symlink to it, which Docker resolves when
# it binds the source.  (A symlink rather than a mount source of its own:
# devcontainer.json substitutes variables in a single non-greedy pass, so
# ${localEnv:VAR:${localWorkspaceFolder}/...} does not parse -- the default
# value would come out as the literal "${localWorkspaceFolder".)
set -eu

ws=${1:-$PWD}

# prepare <entry name> <host path to share, or empty>
prepare() {
    entry=$ws/$1
    shared=$2

    if [ -n "$shared" ]; then
        mkdir -p "$shared"
        if [ -e "$entry" ] && [ ! -L "$entry" ]; then
            echo "init-bazel-caches: $entry is a real directory; remove it to" \
                 "use $shared instead" >&2
            exit 1
        fi
        ln -sfn "$shared" "$entry"
    else
        # A leftover symlink from a previous override: the variable is gone, so
        # go back to a workspace-local cache.
        [ ! -L "$entry" ] || rm "$entry"
        mkdir -p "$entry"
    fi
}

prepare .disk_cache "${BAZEL_DISK_CACHE-}"
prepare .bazel_cache "${BAZEL_REPO_CACHE-}"
