#!/usr/bin/env bash
# Target-workspace management for the e2e harness: build/stage the tool, fetch
# the target repo, and wire the vendored cpp_format kit into it.
#
# Sourced by run_e2e.sh.  Expects assert.sh to be sourced first, and these
# globals to be set by the driver: REPO_ROOT, WORK, TOOL_SPEC, TOOL_CONFIG.
#
# Exports on success: TOOL_BIN, TOOL_VERSION, TOOL_BASE_URL, TOOL_SHA256, TOOL_ASSET.

# The release asset name for this host -- must match the _ASSETS table in
# bazel/integration/extensions.bzl, since that is what the module extension
# appends to base_url.
_host_asset() {
  local os arch
  case "$(uname -s)" in
    Linux)  os=linux ;;
    Darwin) os=darwin ;;
    *) die "unsupported host OS for the e2e harness: $(uname -s)" ;;
  esac
  case "$(uname -m)" in
    x86_64|amd64)  arch=x86_64 ;;
    aarch64|arm64) arch=aarch64 ;;
    *) die "unsupported host arch for the e2e harness: $(uname -m)" ;;
  esac
  # There is no darwin-x86_64 asset; Intel Macs run the arm64 build.
  [[ "$os" == darwin ]] && arch=aarch64
  printf 'cpp_format-%s-%s' "$os" "$arch"
}

# stage_tool <logdir> -- resolve the cpp_format binary named by TOOL_SPEC and
# stage it where the target repo's module extension can download it.
stage_tool() {
  local logdir="$1"
  TOOL_ASSET="$(_host_asset)"

  if [[ "$TOOL_SPEC" == release:* ]]; then
    # Point the extension at GitHub, exactly as a real consumer would.
    TOOL_VERSION="${TOOL_SPEC#release:}"
    TOOL_BASE_URL="https://github.com/geligeli/cpp_formatting/releases/download"
    TOOL_SHA256=""
    TOOL_BIN=""
    pass "published release $TOOL_VERSION (no local build)"
    return 0
  fi

  local bin
  if [[ "$TOOL_SPEC" == path:* ]]; then
    bin="${TOOL_SPEC#path:}"
    [[ -x "$bin" ]] || { fail "not an executable: $bin"; return 1; }
  else
    # Build from source in this repo.  Deliberately a separate Bazel server from
    # the target repo's; shut it down afterwards so the LLVM build's memory is
    # released before the target's server starts.
    local cfg=()
    [[ "$TOOL_CONFIG" == opt ]] && cfg=(-c opt)
    # shellcheck disable=SC2086  # E2E_TOOL_BAZEL_FLAGS is an intentional word list
    if ! run_logged "$logdir/tool-build.log" \
           env -C "$REPO_ROOT" bazel build "${cfg[@]}" ${E2E_TOOL_BAZEL_FLAGS:-} \
             //cpp_formatting:cpp_format; then
      log_tail "$logdir/tool-build.log"
      fail "could not build //cpp_formatting:cpp_format"
      return 1
    fi
    bin="$(env -C "$REPO_ROOT" bazel cquery "${cfg[@]}" --output=files \
             //cpp_formatting:cpp_format 2>/dev/null | tail -1)"
    [[ -n "$bin" ]] || { fail "cquery did not report a binary path"; return 1; }
    [[ "$bin" = /* ]] || bin="$(env -C "$REPO_ROOT" bazel info execution_root 2>/dev/null)/$bin"
    env -C "$REPO_ROOT" bazel shutdown >/dev/null 2>&1 || true
  fi

  # The staged version string doubles as the URL path component *and* as the
  # @cpp_format_bin repo's cache key.  It must encode the binary's identity: a
  # stable version would make Bazel reuse a stale download after the tool is
  # rebuilt, and every scenario would silently test the old binary.
  TOOL_SHA256="$(sha256sum "$bin" | cut -d' ' -f1)"
  TOOL_VERSION="local-${TOOL_SHA256:0:12}"
  TOOL_BASE_URL="file://$WORK/bin"
  TOOL_BIN="$WORK/bin/$TOOL_VERSION/$TOOL_ASSET"
  install -D -m 0755 "$bin" "$TOOL_BIN"

  local desc="$TOOL_VERSION"
  if [[ "$TOOL_SPEC" == source ]]; then
    desc="$desc (HEAD $(git -C "$REPO_ROOT" rev-parse --short HEAD), $TOOL_CONFIG)"
  fi
  pass "cpp_format $desc"
}

# materialize <src-dir> <logdir> -- put the pinned target repo in <src-dir>,
# leaving it pristine and tagged e2e-pristine.
materialize() {
  local src="$1" logdir="$2"

  if [[ "$REPO_KIND" == local ]]; then
    local from="$REPO_ROOT/$REPO_PATH"
    [[ -d "$from" ]] || { fail "local fixture not found: $from"; return 1; }
    rm -rf "$src"
    mkdir -p "$src"
    # -r, not -a: preserving ownership fails on NFS, and the fixture has
    # nothing worth preserving beyond file contents.
    cp -r "$from"/. "$src"/
    _git_init_pristine "$src" || return 1
    pass "local fixture $REPO_PATH"
    return 0
  fi

  # A depth-1 fetch of a pinned SHA: immutable by construction, so no tarball
  # hash bookkeeping, and ~10-30 MB even for a large repo.
  if [[ -n "${FRESH:-}" ]]; then rm -rf "$src"; fi
  if [[ -d "$src/.git" ]] && git -C "$src" rev-parse -q --verify "$REPO_REV^{commit}" >/dev/null 2>&1; then
    # Already fetched: restoring is much cheaper than refetching.
    git -C "$src" reset -q --hard "$REPO_REV" || { fail "git reset failed"; return 1; }
    git -C "$src" clean -qxfd
    git -C "$src" tag -f e2e-pristine >/dev/null
    pass "$REPO_NAME @ ${REPO_REV:0:12} (cached)"
    return 0
  fi

  [[ -n "${E2E_OFFLINE:-}" ]] && { fail "E2E_OFFLINE=1 but $src is not materialized"; return 1; }
  rm -rf "$src"; mkdir -p "$src"
  git -C "$src" init -q
  git -C "$src" remote add origin "$REPO_URL"
  if ! run_logged "$logdir/fetch.log" \
         git -C "$src" -c protocol.version=2 fetch -q --depth 1 origin "$REPO_REV"; then
    log_tail "$logdir/fetch.log"
    fail "could not fetch $REPO_URL @ $REPO_REV"
    return 1
  fi
  git -C "$src" checkout -q FETCH_HEAD
  git -C "$src" tag -f e2e-pristine >/dev/null
  pass "$REPO_NAME @ ${REPO_REV:0:12} (fetched)"
}

_git_commit() {
  git -C "$1" -c user.name=cpp-format-e2e -c user.email=e2e@invalid \
    commit -q --allow-empty -m "$2"
}

_git_init_pristine() {
  local src="$1"
  rm -rf "$src/.git"
  git -C "$src" init -q
  git -C "$src" add -A
  _git_commit "$src" "e2e pristine fixture"
  git -C "$src" tag -f e2e-pristine >/dev/null
}

# wire <src-dir> <ruleset-yaml> <disk-cache-dir> -- inject the vendored kit, the
# ruleset, and the harness's Bazel settings, then commit so that everything
# `git diff` reports afterwards is exactly what the tool wrote.
wire() {
  local src="$1" ruleset="$2" disk_cache="$3"

  # 1. Vendor the integration kit.  A copy, never a symlink: Bazel would follow
  #    a symlink out of the workspace.  //third_party/cpp_format is the layout
  #    cpp_format.sh already defaults to, so no env is strictly required.
  mkdir -p "$src/third_party/cpp_format" "$src/tools"
  cp "$REPO_ROOT/bazel/integration"/{cpp_format.bzl,extensions.bzl,BUILD.bazel,cpp_format.sh} \
     "$src/third_party/cpp_format/"
  install -m 0755 "$REPO_ROOT/bazel/integration/cpp_format.sh" "$src/tools/cpp_format.sh"

  # 2. Wire the staged binary in through the kit's own release() tag class.
  [[ -f "$src/MODULE.bazel" ]] || printf 'module(name = "e2e_target")\n' > "$src/MODULE.bazel"

  # A *vendored* kit lives in the root module, so its `load("@rules_cc//...")`
  # resolves through the root module's repo mapping -- which means the consumer
  # must depend on rules_cc directly, even if it already gets it transitively.
  # (URL import is unaffected: there the load resolves through cpp_formatting's
  # own deps.) googletest, for one, does not declare it.
  if ! grep -q '"rules_cc"' "$src/MODULE.bazel"; then
    {
      printf '\n# ---- cpp_format e2e harness (injected) ----\n'
      printf '# Required by the vendored kit at //third_party/cpp_format.\n'
      printf 'bazel_dep(name = "rules_cc", version = "%s")\n' "$RULES_CC_VERSION"
    } >> "$src/MODULE.bazel"
  fi

  {
    printf '\n# ---- cpp_format e2e harness (injected, do not commit) ----\n'
    printf 'cpp_format = use_extension("//third_party/cpp_format:extensions.bzl", "cpp_format")\n'
    printf 'cpp_format.release(\n'
    printf '    version = "%s",\n' "$TOOL_VERSION"
    printf '    base_url = "%s",\n' "$TOOL_BASE_URL"
    if [[ -n "$TOOL_SHA256" ]]; then
      printf '    sha256 = {"%s": "%s"},\n' "$TOOL_ASSET" "$TOOL_SHA256"
    fi
    printf ')\nuse_repo(cpp_format, "cpp_format_bin")\n'
  } >> "$src/MODULE.bazel"

  # 3. The ruleset, read by the aspect as the root module's @@//:cpp_format.yaml
  #    -- so it must be exported from the root package.
  cp "$ruleset" "$src/cpp_format.yaml"
  local root_build="$src/BUILD.bazel"
  [[ -f "$src/BUILD" ]] && root_build="$src/BUILD"
  # Anchored, so a mention inside a comment does not count as "already exported".
  if ! grep -qE '^[[:space:]]*exports_files\(\["cpp_format\.yaml"\]\)' \
         "$root_build" 2>/dev/null; then
    printf '\n# ---- cpp_format e2e harness (injected) ----\nexports_files(["cpp_format.yaml"])\n' \
      >> "$root_build"
  fi

  # 4. Bazel settings.  The workspace rc is read after /etc/bazel.bazelrc, so
  #    these win over the devcontainer's machine-wide defaults -- which matters
  #    most for --disk_cache: edit records hold absolute paths, so two
  #    workspaces sharing a disk cache can restore each other's records.
  {
    printf '\n# ---- cpp_format e2e harness (appended, do not commit) ----\n'
    printf 'common --lockfile_mode=off\n'
    printf 'common --repository_cache=%s\n' "$WORK/repo-cache"
    printf 'build --disk_cache=%s\n' "$disk_cache"
    printf 'build --spawn_strategy=local\n'
    printf 'build --experimental_convenience_symlinks=ignore\n'
  } >> "$src/.bazelrc"

  # 5. Pin Bazel itself: a repo pinning 7.x under bazelisk would change the
  #    meaning of @@// and of bzlmod resolution generally.
  printf '%s\n' "$BAZELVERSION" > "$src/.bazelversion"

  git -C "$src" add -A
  _git_commit "$src" "e2e wiring"
  git -C "$src" tag -f e2e-wired >/dev/null
  pass "kit + ruleset $(basename "$ruleset" .yaml) + rc"
}
