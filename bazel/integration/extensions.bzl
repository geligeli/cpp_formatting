"""Fetch a prebuilt `cpp_format` release binary for the host platform.

This is a **vendored integration kit**: copy the whole `bazel/integration/`
directory into your own repository (e.g. as `third_party/cpp_format/`).  It
wires a single `@cpp_format_bin//:cpp_format` target to the platform-appropriate
asset published at

    https://github.com/geligeli/cpp_formatting/releases

so you get the Bazel lint/fix integration (see `cpp_format.bzl`) **without**
building Clang/LLVM from source.  The binary embeds the Clang builtin headers
and self-extracts them at runtime, so nothing else needs fetching.

A second repository, `@code_browser_bin//:code_browser`, is the code browser of
the same release -- what `<name>.db` and `<name>.browse` of `cpp_index_targets`
run.  Repositories are fetched on demand, so it is downloaded only when one of
those two targets is built; a consumer who only formats never pays for it.
"""

# (os, arch) -> release asset filename.  `rctx.os.arch` reports the JVM arch
# name ("amd64", "aarch64", "x86_64"), which varies by platform, so both
# spellings are mapped.
_ASSETS = {
    ("linux", "x86_64"): "cpp_format-linux-x86_64",
    ("linux", "amd64"): "cpp_format-linux-x86_64",
    ("linux", "aarch64"): "cpp_format-linux-aarch64",
    ("linux", "arm64"): "cpp_format-linux-aarch64",
    ("mac", "aarch64"): "cpp_format-darwin-aarch64",
    ("mac", "arm64"): "cpp_format-darwin-aarch64",
    ("mac", "x86_64"): "cpp_format-darwin-aarch64",  # runs under Rosetta
    ("windows", "x86_64"): "cpp_format-windows-x86_64.exe",
    ("windows", "amd64"): "cpp_format-windows-x86_64.exe",
}

# The code browser, published next to cpp_format from the same commit.  There is
# no Windows build of it yet: the release workflow builds it where the code has
# been compiled and run, and that is not there.
_BROWSER_ASSETS = {
    ("linux", "x86_64"): "code_browser-linux-x86_64",
    ("linux", "amd64"): "code_browser-linux-x86_64",
    ("linux", "aarch64"): "code_browser-linux-aarch64",
    ("linux", "arm64"): "code_browser-linux-aarch64",
    ("mac", "aarch64"): "code_browser-darwin-aarch64",
    ("mac", "arm64"): "code_browser-darwin-aarch64",
    ("mac", "x86_64"): "code_browser-darwin-aarch64",  # runs under Rosetta
}

# Raised when a repository is *fetched*, not when the extension is evaluated:
# `bazel mod deps`, `bazel mod tidy`, `bazel vendor` and `bazel fetch --all`
# evaluate every extension, and a module that uses this one without needing the
# binary -- cpp_formatting itself, whose own targets all build from source --
# must not have to invent a pin to keep them working.
_NO_RELEASE = ("cpp_format: add `cpp_format.release(version = ...)` to " +
               "MODULE.bazel (see the README quick start)")

def _host_key(rctx):
    name = rctx.os.name.lower()
    if name.startswith("linux"):
        os = "linux"
    elif name.startswith("mac os") or name.startswith("darwin"):
        os = "mac"
    elif name.startswith("windows"):
        os = "windows"
    else:
        fail("cpp_format: unsupported host OS %r" % rctx.os.name)
    return (os, rctx.os.arch.lower())

def _binary_repo_impl(rctx):
    if not rctx.attr.version:
        fail(_NO_RELEASE)
    key = _host_key(rctx)
    asset = _ASSETS.get(key)
    if not asset:
        fail(("cpp_format: no prebuilt release asset for host %r; " +
              "build //cpp_formatting:cpp_format from source instead") % (key,))

    # Download into a subdirectory so the file path never collides with the
    # `cpp_format` filegroup target name (a same-name src would be a self-edge).
    out = "bin/cpp_format.exe" if key[0] == "windows" else "bin/cpp_format"
    url = "{base}/{version}/{asset}".format(
        base = rctx.attr.base_url.rstrip("/"),
        version = rctx.attr.version,
        asset = asset,
    )
    sha = rctx.attr.sha256.get(asset, "")
    rctx.download(url = url, output = out, executable = True, sha256 = sha)

    # A stable, platform-independent label: @cpp_format_bin//:cpp_format.
    rctx.file(
        "BUILD.bazel",
        'filegroup(name = "cpp_format", srcs = ["%s"], ' % out +
        'visibility = ["//visibility:public"])\n',
    )

def _browser_repo_impl(rctx):
    if not rctx.attr.version:
        fail(_NO_RELEASE)
    key = _host_key(rctx)
    asset = _BROWSER_ASSETS.get(key)
    if not asset:
        fail(("cpp_format: there is no prebuilt code_browser for host %r, so " +
              "<name>.db and <name>.browse of cpp_index_targets are not " +
              "available here (<name>.index is); build " +
              "//code_browser:code_browser from source instead") % (key,))
    out = "bin/code_browser"
    url = "{base}/{version}/{asset}".format(
        base = rctx.attr.base_url.rstrip("/"),
        version = rctx.attr.version,
        asset = asset,
    )
    sha = rctx.attr.sha256.get(asset, "")
    result = rctx.download(
        url = url,
        output = out,
        executable = True,
        sha256 = sha,
        allow_fail = True,
    )
    if not result.success:
        # Releases before the browser was published have no such asset, and a
        # bare 404 would not say so.
        fail(("cpp_format: could not download %s.  Release %r may predate " +
              "the prebuilt code browser; pin a newer one in " +
              "cpp_format.release(version = ...)") % (url, rctx.attr.version))
    rctx.file(
        "BUILD.bazel",
        'filegroup(name = "code_browser", srcs = ["%s"], ' % out +
        'visibility = ["//visibility:public"])\n',
    )

_browser_repo = repository_rule(
    implementation = _browser_repo_impl,
    attrs = {
        "version": attr.string(
            mandatory = True,
            doc = "Release tag; empty when no module issued a release() tag.",
        ),
        "base_url": attr.string(
            default = "https://github.com/geligeli/cpp_formatting/releases/download",
        ),
        "sha256": attr.string_dict(
            doc = "Optional map of asset filename -> sha256 for pinning.",
        ),
    },
)

_binary_repo = repository_rule(
    implementation = _binary_repo_impl,
    attrs = {
        "version": attr.string(
            mandatory = True,
            doc = "Release tag; empty when no module issued a release() tag.",
        ),
        "base_url": attr.string(
            default = "https://github.com/geligeli/cpp_formatting/releases/download",
        ),
        "sha256": attr.string_dict(
            doc = "Optional map of asset filename -> sha256 for pinning.",
        ),
    },
)

_release = tag_class(attrs = {
    "version": attr.string(
        mandatory = True,
        doc = "Release tag to download, e.g. \"20260720-39c5de9\".",
    ),
    "base_url": attr.string(
        default = "https://github.com/geligeli/cpp_formatting/releases/download",
    ),
    "sha256": attr.string_dict(
        doc = "Optional {asset-filename: sha256} pins (recommended for CI).",
    ),
})

def _ext_impl(mctx):
    # `@cpp_format_bin` is one repo shared by every module that uses this
    # extension, so exactly one `release()` tag can win -- and it must be the
    # **root** module's.  When the kit is imported by URL, cpp_formatting itself
    # is a dependency module whose own MODULE.bazel also calls `release()`; a
    # last-tag-wins loop lets that stale pin silently replace the consumer's,
    # which then downloads a binary older than the aspect that drives it (an
    # older binary rejects `--owned-files`).  Same root-vs-dependency trap as
    # `_config`'s `@@//:cpp_format.yaml` default in cpp_format.bzl.
    # cpp_formatting itself issues no tag at all (nothing in it uses the
    # prebuilt binary); the root-wins rule below is for any other module in the
    # graph that does.
    root = None
    dep = None
    for mod in mctx.modules:
        for r in mod.tags.release:
            if mod.is_root:
                root = r
            elif dep == None:
                dep = r
    rel = root or dep

    # No tag anywhere is not an error *here* (see _NO_RELEASE): the repositories
    # are declared with an empty version and say what is missing if fetched.
    version = rel.version if rel else ""
    base_url = rel.base_url if rel else None
    sha256 = rel.sha256 if rel else {}
    _binary_repo(
        name = "cpp_format_bin",
        version = version,
        base_url = base_url,
        sha256 = sha256,
    )

    # Same release, same pins (`sha256` is keyed by asset file name, so one
    # dict serves both).  Declaring the repository costs nothing: it is only
    # fetched when something builds a target in it.
    _browser_repo(
        name = "code_browser_bin",
        version = version,
        base_url = base_url,
        sha256 = sha256,
    )

# use_extension(...) target in the consumer's MODULE.bazel.
cpp_format = module_extension(
    implementation = _ext_impl,
    tag_classes = {"release": _release},
)
