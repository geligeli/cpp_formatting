# Bzlmod module extension that builds Clang/LLVM from source using LLVM's own
# Bazel overlay (`utils/bazel`).  The Bazel Central Registry only publishes
# llvm-project up to 17.0.4, so to track a newer Clang (21.1.8 here) we fetch
# the monorepo source archive and run the upstream `llvm_configure` overlay
# rule, mirroring the WORKSPACE example in utils/bazel/examples/http_archive.
load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
load("//third_party/llvm:configure.bzl", "llvm_configure")

# Pin to a released tag so builds are reproducible.
LLVM_VERSION = "21.1.8"
LLVM_SHA256 = "7ba3f2a8d8fda88be18a31d011e8195d3b7f87f9fa92b20c94cba2d7f65b0e3f"

# include-what-you-use is built against the Clang above and only ever supports
# one Clang release per IWYU release (0.25 <-> Clang 21, the `clang_21`
# branch), so the two pins move together: bump this whenever LLVM_VERSION
# changes major version.  See https://include-what-you-use.org/ and
# //third_party/iwyu.
IWYU_VERSION = "0.25"
IWYU_SHA256 = "2e8381368ec0a6ecb770834bce00fc62efa09a2b2f9710ed569acbb823ead9cc"

def _zlib_alias_impl(rctx):
    rctx.file("BUILD.bazel", """\
alias(
    name = "zlib",
    actual = "@zlib",
    visibility = ["//visibility:public"],
)
""")

# `@zlib` resolves through this module's repo mapping (an extension's repos see
# what the module hosting the extension sees), hence the `bazel_dep(name =
# "zlib")` in MODULE.bazel.
_zlib_alias = repository_rule(implementation = _zlib_alias_impl)

def _llvm_impl(_module_ctx):
    http_archive(
        name = "llvm-raw",
        build_file_content = "# empty",
        patch_args = ["-p1"],
        patches = [
            "//patches:llvm_blake3_no_asm_on_windows.patch",
            "//patches:llvm_enable_zlib_define.patch",
            "//patches:llvm_rdf_std_specializations.patch",
            "//patches:llvm_zstd_no_asm_on_windows.patch",
        ],
        sha256 = LLVM_SHA256,
        strip_prefix = "llvm-project-llvmorg-" + LLVM_VERSION,
        urls = ["https://github.com/llvm/llvm-project/archive/refs/tags/llvmorg-{v}.tar.gz".format(v = LLVM_VERSION)],
    )

    # The overlay links `@llvm_zlib//:zlib` into llvm:Support.  Upstream's
    # recipe for that repo is a private copy of zlib-ng in zlib-compat mode --
    # but protobuf already links the BCR `zlib` into the same binaries, and the
    # two export the same symbols: one program, two zlibs, the winner of each
    # symbol decided by link order.  AddressSanitizer reports it as an ODR
    # violation on `deflate_copyright` before main() runs.  So `llvm_zlib` is
    # the one zlib the module graph already has.
    #
    # An alias, not a cc_library wrapping it: llvm:Support includes <zlib.h>,
    # and under Clang's layering_check (every release build) the target it
    # depends on must be the one that owns the header -- a wrapper fails with
    # "does not directly depend on a module exporting 'zlib.h'".  An alias
    # cannot carry the LLVM_ENABLE_ZLIB=1 define upstream's target exported,
    # so //patches:llvm_enable_zlib_define.patch puts it in the overlay's
    # llvm_config_defines instead.
    _zlib_alias(name = "llvm_zlib")

    http_archive(
        name = "llvm_zstd",
        build_file = "@llvm-raw//utils/bazel/third_party_build:zstd.BUILD",
        sha256 = "7c42d56fac126929a6a85dbc73ff1db2411d04f104fae9bdea51305663a83fd0",
        strip_prefix = "zstd-1.5.2",
        urls = ["https://github.com/facebook/zstd/releases/download/v1.5.2/zstd-1.5.2.tar.gz"],
    )

    # Developer tooling (`tools/iwyu.sh`), never part of a shipped binary.  The
    # repo is fetched lazily, so nothing is downloaded until something asks
    # for @iwyu.
    http_archive(
        name = "iwyu",
        build_file = "//third_party/iwyu:iwyu.BUILD",
        sha256 = IWYU_SHA256,
        strip_prefix = "include-what-you-use-" + IWYU_VERSION,
        urls = ["https://github.com/include-what-you-use/include-what-you-use/archive/refs/tags/{v}.tar.gz".format(v = IWYU_VERSION)],
    )

    # Only the host backends are needed: the tools use Clang's AST/tooling
    # layer and just require the host target to be registered.  The release
    # workflow builds on both x86_64 and aarch64 runners, and LLVM's
    # llvm-c/Target.h references LLVMInitialize<Host>Target* for the host
    # architecture, so both X86 and AArch64 must be registered.  Restricting
    # targets keeps the from-source build small.
    llvm_configure(
        name = "llvm-project",
        targets = ["X86", "AArch64"],
    )

llvm = module_extension(implementation = _llvm_impl)
