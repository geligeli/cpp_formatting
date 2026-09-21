#!/usr/bin/env bash
# The prebuilt kit's `cpp_format.sh browse`, in a consumer workspace.
#
#   e2e/kit_browse_test.sh [--tool=PATH] [--browser=PATH] [--work-dir=DIR] [--keep]
#
# A consumer of bazel/integration/ gets the code browser as a release asset,
# through the kit's module extension.  That path cannot be a `bazel test` here:
# it has to drive Bazel inside a *second* workspace (nesting one server in
# another deadlocks on the workspace lock -- the reason run_e2e.sh is a plain
# script too), and the assets it downloads must be the ones built from this
# checkout, which no published release contains yet.  So, like the e2e harness,
# this stages locally built binaries as a release on a file:// URL and points
# the kit's own `release()` tag at it.
#
# Both ways of consuming the kit are exercised over a copy of
# e2e/testdata/mini_repo:
#   import   bazel_dep + local_path_override (what `archive_override` of a tag
#            is for a consumer), the script placed by `bazel run
#            @cpp_formatting//bazel/integration:install`.  The consumer's
#            use_repo() lists *only* cpp_format_bin, as the README's quick start
#            has it: `@code_browser_bin` is not visible from its command line,
#            and `browse` works through the canonical label `install` baked in.
#   vendor   bazel/integration/ copied to //third_party/cpp_format and the
#            script run from there; the consumer names both repositories in its
#            own use_repo(), which is where the script's default labels resolve
#
# Asserted for each:
#   * `bazel build //...` succeeds and does NOT fetch the browser -- a consumer
#     who only formats never downloads it;
#   * `cpp_format.sh browse --check` indexes the whole repository (no target
#     named anywhere), fetches the browser, prints its binary, its command line
#     and how to update the index, and the browser opens the database;
#   * a second run re-imports nothing: the merged index did not change, so
#     index.pb is left alone and its database stays current;
#   * the launched server serves *the consumer's checkout*: /api/repo names it
#     as root, /api/file returns its bytes, and a cross-target reference
#     (geometry.cpp -> a member declared in shapes.h) is in the index;
#   * a release without the browser asset fails with a message that says so;
#   * with no release() tag at all, `bazel mod deps` still works, and the first
#     thing that needs the binary says which line to add.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOL="" BROWSER="" WORK="" KEEP=0
for arg in "$@"; do
  case "$arg" in
    --tool=*) TOOL="${arg#*=}" ;;
    --browser=*) BROWSER="${arg#*=}" ;;
    --work-dir=*) WORK="${arg#*=}" ;;
    --keep) KEEP=1 ;;
    -h|--help) sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $arg" >&2; exit 2 ;;
  esac
done

fail() { echo "FAIL: $*" >&2; exit 1; }
note() { echo "--- $*"; }

WORK="${WORK:-$(mktemp -d "${TMPDIR:-/tmp}/kit_browse.XXXXXX")}"
mkdir -p "$WORK"
WORK="$(cd "$WORK" && pwd)"
server=""
cleanup() {
  [[ -n "$server" ]] && kill "$server" 2>/dev/null || true
  if [[ "$KEEP" -eq 0 ]]; then
    for ws in "$WORK"/import "$WORK"/vendor "$WORK"/stale "$WORK"/untagged; do
      [[ -d "$ws" ]] && (cd "$ws" && consumer_bazel clean --expunge >/dev/null 2>&1) || true
    done
    chmod -R u+w "$WORK" 2>/dev/null || true
    rm -rf "$WORK"
  else
    echo "kept: $WORK"
  fi
}
trap cleanup EXIT

# Direct children of a process.  pgrep where there is one (macOS, most Linux);
# the devcontainer image has no procps, and /proc answers the same question.
children_of() {
  pgrep -P "$1" 2>/dev/null || cat "/proc/$1/task/$1/children" 2>/dev/null || true
}

# The consumer's Bazel must not inherit this machine's rc files: a home rc that
# turns on remote execution would send the index actions to a worker that has
# neither the consumer's (non-hermetic) toolchain nor its system headers.
# cpp_format.sh runs "$BAZEL" as one word, so the startup flags also live in a
# wrapper it is pointed at.
consumer_bazel() {
  bazel --nohome_rc --nosystem_rc --output_user_root="$WORK/bazel_root" "$@"
}
printf '#!/usr/bin/env bash\nexec bazel --nohome_rc --nosystem_rc --output_user_root="%s" "$@"\n' \
  "$WORK/bazel_root" > "$WORK/bazel"
chmod +x "$WORK/bazel"

# ---------------------------------------------------------------------------
# 1. The two binaries, from this checkout unless given.
# ---------------------------------------------------------------------------
if [[ -z "$TOOL" || -z "$BROWSER" ]]; then
  note "building cpp_format and code_browser from $REPO_ROOT"
  # E2E_TOOL_BAZEL_FLAGS: the same knob run_e2e.sh has, for CI's cache flags.
  # shellcheck disable=SC2086
  (cd "$REPO_ROOT" && bazel build --remote_download_outputs=toplevel ${E2E_TOOL_BAZEL_FLAGS:-} \
      //cpp_formatting:cpp_format //code_browser:code_browser) >"$WORK/build.log" 2>&1 \
    || { tail -30 "$WORK/build.log" >&2; fail "could not build the binaries"; }
  TOOL="${TOOL:-$REPO_ROOT/bazel-bin/cpp_formatting/cpp_format}"
  BROWSER="${BROWSER:-$REPO_ROOT/bazel-bin/code_browser/code_browser}"
fi
[[ -x "$TOOL" && -x "$BROWSER" ]] || fail "not executable: $TOOL / $BROWSER"

case "$(uname -s)-$(uname -m)" in
  Linux-x86_64) plat=linux-x86_64 ;;
  Linux-aarch64) plat=linux-aarch64 ;;
  Darwin-*) plat=darwin-aarch64 ;;
  *) fail "no release asset name for $(uname -s)-$(uname -m)" ;;
esac

# The version string is the URL path component *and* the repository rule's
# refetch key, so it carries the binaries' hash: a stable one would silently
# test a stale download.
digest="$(cat "$TOOL" "$BROWSER" | sha256sum | cut -c1-12)"
VERSION="kit-browse-$digest"
stage="$WORK/release/$VERSION"
mkdir -p "$stage"
install -m 0755 "$TOOL" "$stage/cpp_format-$plat"
install -m 0755 "$BROWSER" "$stage/code_browser-$plat"
BASE_URL="file://$WORK/release"

# A second "release" with no browser in it, as every release before this one.
mkdir -p "$WORK/release/$VERSION-nobrowser"
install -m 0755 "$TOOL" "$WORK/release/$VERSION-nobrowser/cpp_format-$plat"

# ---------------------------------------------------------------------------
# 2. Consumer workspaces.
# ---------------------------------------------------------------------------
make_consumer() {  # <flavor> <version>
  local flavor="$1" version="$2" ws="$WORK/$1"
  rm -rf "$ws"
  cp -r "$REPO_ROOT/e2e/testdata/mini_repo" "$ws"
  # Without a .bazelversion bazelisk takes the newest Bazel, and the result of
  # this test would change with it.  BAZELVERSION=latest is how to ask for that
  # on purpose (it is how the kit's Bazel 9 breakage was found).
  printf '%s\n' "${BAZELVERSION:-$(cat "$REPO_ROOT/.bazelversion")}" > "$ws/.bazelversion"
  printf 'normalize_variables: []\n' > "$ws/cpp_format.yaml"
  # version "none": the consumer uses the extension but issues no release() tag.
  local release="cpp_format.release(version = \"$version\", base_url = \"$BASE_URL\")"
  [[ "$version" == none ]] && release="# (no cpp_format.release() tag)"
  if [[ "$flavor" == vendor ]]; then
    mkdir -p "$ws/third_party/cpp_format"
    cp "$REPO_ROOT/bazel/integration"/{cpp_format.bzl,extensions.bzl,BUILD.bazel,cpp_format.sh} \
       "$ws/third_party/cpp_format/"
    cat >> "$ws/MODULE.bazel" <<EOF

cpp_format = use_extension("//third_party/cpp_format:extensions.bzl", "cpp_format")
$release
use_repo(cpp_format, "code_browser_bin", "cpp_format_bin")
EOF
  else
    cat >> "$ws/MODULE.bazel" <<EOF

bazel_dep(name = "cpp_formatting", version = "0.1.0", dev_dependency = True)
local_path_override(module_name = "cpp_formatting", path = "$REPO_ROOT")

cpp_format = use_extension("@cpp_formatting//bazel/integration:extensions.bzl", "cpp_format", dev_dependency = True)
$release
EOF
    # Only the formatter's repository, as the README's quick start has it.
    [[ "$version" == none ]] || printf 'use_repo(cpp_format, "cpp_format_bin")\n' >> "$ws/MODULE.bazel"
  fi
  printf '\nexports_files(["cpp_format.yaml"])\n' >> "$ws/BUILD.bazel"

}

# The script as each flavor has it: copied in with the vendored kit, or placed
# by the kit's install target, which bakes the aspect's and the two binaries'
# canonical labels into it.  Prints the script's path.
consumer_script() {  # <workspace dir name> <flavor>
  local ws="$WORK/$1" flavor="$2"
  if [[ "$flavor" == vendor ]]; then
    echo "$ws/third_party/cpp_format/cpp_format.sh"
    return
  fi
  if [[ ! -x "$ws/tools/cpp_format.sh" ]]; then
    (cd "$ws" && consumer_bazel run @cpp_formatting//bazel/integration:install) >"$WORK/$1.install.log" 2>&1 \
      || { tail -30 "$WORK/$1.install.log" >&2; fail "[$1] bazel run ...:install failed"; }
  fi
  echo "$ws/tools/cpp_format.sh"
}

check_consumer() {  # <flavor>
  local flavor="$1" ws="$WORK/$1" log="$WORK/$1.log"
  note "[$flavor] bazel build //... must not fetch the browser"
  (cd "$ws" && consumer_bazel build //... ) >"$log" 2>&1 \
    || { tail -30 "$log" >&2; fail "[$flavor] bazel build //... failed"; }
  local ext_root
  ext_root="$(cd "$ws" && consumer_bazel info output_base 2>/dev/null)/external"
  if compgen -G "$ext_root/*code_browser_bin/bin/code_browser" >/dev/null; then
    fail "[$flavor] //... fetched the code browser; only browse may"
  fi

  local script
  script="$(consumer_script "$flavor" "$flavor")"
  if [[ "$flavor" == import ]]; then
    grep -q '^BROWSER_LABEL=.*:-@@[^/]*code_browser_bin//:code_browser}"' "$script" \
      || { grep -n '^BROWSER_LABEL=' "$script" >&2; fail "[$flavor] install did not bake a canonical browser label"; }
  fi

  note "[$flavor] cpp_format.sh browse --check"
  (cd "$ws" && BAZEL="$WORK/bazel" "$script" browse --check) >"$log" 2>&1 \
    || { tail -30 "$log" >&2; fail "[$flavor] cpp_format.sh browse --check failed"; }
  grep -Eq "index\.pb\.sqlite: [1-9][0-9]* files, [1-9][0-9]* symbols" "$log" \
    || { tail -20 "$log" >&2; fail "[$flavor] --check did not print the database stats"; }
  # The fixture has a target no platform can build.  The script names every
  # target explicitly, and Bazel refuses an explicitly requested incompatible
  # one, so "the whole repository" has to mean "what can be built here".
  grep -q "cpp_format: skipping //mini:other_platform (incompatible" "$log" \
    || { tail -20 "$log" >&2; fail "[$flavor] the incompatible target was not skipped"; }
  grep -q "cpp_format: wrote $ws/index.pb" "$log" || fail "[$flavor] the index was not written"
  grep -q "code_browser: importing" "$log" || fail "[$flavor] the first run did not import the index"
  # What the user is told before the server starts: which binary, the exact
  # command line (so it can be restarted by hand), and how to update the index.
  grep -q "cpp_format: code browser binary: /.*code_browser" "$log" \
    || { tail -20 "$log" >&2; fail "[$flavor] the browser binary was not printed"; }
  grep -q -- "--index=$ws/index.pb --root=$ws --exec-root=/.* --check" "$log" \
    || { tail -20 "$log" >&2; fail "[$flavor] the command line was not printed"; }
  { grep -q "To update it" "$log" && grep -q "cpp_format.sh index" "$log"; } \
    || { tail -20 "$log" >&2; fail "[$flavor] no instructions for updating the index"; }

  note "[$flavor] a second run leaves an unchanged index alone"
  (cd "$ws" && BAZEL="$WORK/bazel" "$script" browse --check) >"$log" 2>&1 \
    || { tail -30 "$log" >&2; fail "[$flavor] the second browse --check failed"; }
  grep -q "index.pb is up to date" "$log" || { tail -20 "$log" >&2; fail "[$flavor] the index was rewritten"; }
  if grep -q "code_browser: importing" "$log"; then
    fail "[$flavor] an unchanged index was re-imported"
  fi

  note "[$flavor] cpp_format.sh browse serves the checkout"
  local port="$WORK/$flavor.port"
  rm -f "$port"
  (cd "$ws" && BAZEL="$WORK/bazel" "$script" browse --port=0 --port-file="$port" \
      --log-requests=false) >"$log" 2>&1 &
  server=$!
  for _ in $(seq 1 600); do [[ -s "$port" ]] && break; sleep 0.1; done
  [[ -s "$port" ]] || { tail -30 "$log" >&2; fail "[$flavor] the server did not start"; }
  local url="http://127.0.0.1:$(cat "$port")"

  curl -fsS "$url/api/repo" > "$WORK/$flavor.repo.json" || fail "[$flavor] /api/repo"
  grep -q "\"root\": *\"$ws\"" "$WORK/$flavor.repo.json" \
    || { cat "$WORK/$flavor.repo.json" >&2; fail "[$flavor] root is not the consumer's workspace"; }
  curl -fsS "$url/api/file?path=mini/shapes.h" | cmp -s - "$ws/mini/shapes.h" \
    || fail "[$flavor] /api/file did not return the checkout's bytes"
  curl -fsS "$url/" | grep -qi "<html" || fail "[$flavor] the page is not served"
  # A use in geometry.cpp of something shapes.h declares: the cross-target
  # reference only exists if the dependency's header was indexed as owned.
  curl -fsS "$url/api/annotations?path=mini/geometry.cpp" > "$WORK/$flavor.ann.json" \
    || fail "[$flavor] /api/annotations"
  grep -q '"spans"\|"roles"\|"symbol' "$WORK/$flavor.ann.json" \
    || { head -c 400 "$WORK/$flavor.ann.json" >&2; fail "[$flavor] geometry.cpp has no annotations"; }

  # SIGINT is the browser's clean shutdown (exit 0).  The script execs the
  # browser once the index is built, so by now the browser is either `$server`
  # itself -- bash execs the last command of a subshell when it can -- or that
  # subshell's one child.
  local child signalled=0
  for child in $(children_of "$server"); do
    kill -INT "$child" 2>/dev/null && signalled=1
  done
  [[ "$signalled" -eq 1 ]] || kill -INT "$server" 2>/dev/null || true
  local rc=0
  wait "$server" || rc=$?
  server=""
  [[ "$rc" -eq 0 ]] || { tail -20 "$log" >&2; fail "[$flavor] the server exited $rc on SIGINT"; }
}

for flavor in import vendor; do
  make_consumer "$flavor" "$VERSION"
  check_consumer "$flavor"
done

# ---------------------------------------------------------------------------
# 3. A release that predates the browser says so.
# ---------------------------------------------------------------------------
note "[stale] a release without the asset fails with an explanation"
make_consumer import "$VERSION-nobrowser"
mv "$WORK/import" "$WORK/stale"
stale_script="$(consumer_script stale import)"
if (cd "$WORK/stale" && BAZEL="$WORK/bazel" "$stale_script" browse --check) >"$WORK/stale.log" 2>&1; then
  fail "[stale] browse succeeded without a browser asset"
fi
grep -q "may predate the prebuilt code browser" "$WORK/stale.log" \
  || { tail -20 "$WORK/stale.log" >&2; fail "[stale] the failure does not explain itself"; }
(cd "$WORK/stale" && consumer_bazel build //...) >"$WORK/stale.log" 2>&1 \
  || { tail -20 "$WORK/stale.log" >&2; fail "[stale] //... must still build: formatting does not need the browser"; }

# ---------------------------------------------------------------------------
# 4. No release() tag at all.
# ---------------------------------------------------------------------------
# The extension must not fail when merely *evaluated* -- `bazel mod deps` and
# `bazel mod tidy` evaluate every extension, and cpp_formatting itself uses this
# one without a tag, since nothing in it needs the prebuilt binary.  What is
# missing is said when a repository is fetched, i.e. when something needs it.
note "[untagged] no release(): mod deps works, the first use says what to add"
make_consumer import none
rm -rf "$WORK/untagged" && mv "$WORK/import" "$WORK/untagged"
(cd "$WORK/untagged" && consumer_bazel mod deps) >"$WORK/untagged.log" 2>&1 \
  || { tail -20 "$WORK/untagged.log" >&2; fail "[untagged] bazel mod deps must not need a release() tag"; }
untagged_script="$(consumer_script untagged import)"
if (cd "$WORK/untagged" && BAZEL="$WORK/bazel" "$untagged_script" index) >"$WORK/untagged.log" 2>&1; then
  fail "[untagged] the index built without a cpp_format binary"
fi
grep -q 'add `cpp_format.release(version = ...)`' "$WORK/untagged.log" \
  || { tail -20 "$WORK/untagged.log" >&2; fail "[untagged] the failure does not say what to add"; }

echo "PASS"
