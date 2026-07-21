"""Fetch a prebuilt `cpp_format` release binary for the host platform.

This is a **vendored integration kit**: copy the whole `bazel/integration/`
directory into your own repository (e.g. as `third_party/cpp_format/`).  It
wires a single `@cpp_format_bin//:cpp_format` target to the platform-appropriate
asset published at

    https://github.com/geligeli/cpp_formatting/releases

so you get the Bazel lint/fix integration (see `cpp_format.bzl`) **without**
building Clang/LLVM from source.  The binary embeds the Clang builtin headers
and self-extracts them at runtime, so nothing else needs fetching.
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

_binary_repo = repository_rule(
    implementation = _binary_repo_impl,
    attrs = {
        "version": attr.string(mandatory = True),
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
    version = None
    base_url = "https://github.com/geligeli/cpp_formatting/releases/download"
    sha = {}
    for mod in mctx.modules:
        for r in mod.tags.release:
            version = r.version
            base_url = r.base_url
            sha = r.sha256
    if not version:
        fail("cpp_format: add `cpp_format.release(version = ...)` to MODULE.bazel")
    _binary_repo(
        name = "cpp_format_bin",
        version = version,
        base_url = base_url,
        sha256 = sha,
    )

# use_extension(...) target in the consumer's MODULE.bazel.
cpp_format = module_extension(
    implementation = _ext_impl,
    tag_classes = {"release": _release},
)
