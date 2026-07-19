# Bzlmod module extension that builds Clang/LLVM from source using LLVM's own
# Bazel overlay (`utils/bazel`).  The Bazel Central Registry only publishes
# llvm-project up to 17.0.4, so to track a newer Clang (19.1.7 here) we fetch
# the monorepo source archive and run the upstream `llvm_configure` overlay
# rule, mirroring the WORKSPACE example in utils/bazel/examples/http_archive.
load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
load("//third_party/llvm:configure.bzl", "llvm_configure")

# Pin to a released tag so builds are reproducible.
LLVM_VERSION = "19.1.7"
LLVM_SHA256 = "59abea1c22e64933fad4de1671a61cdb934098793c7a31b333ff58dc41bff36c"

def _llvm_impl(_module_ctx):
    http_archive(
        name = "llvm-raw",
        build_file_content = "# empty",
        patch_args = ["-p1"],
        patches = ["//patches:llvm_zstd_no_asm_on_windows.patch"],
        sha256 = LLVM_SHA256,
        strip_prefix = "llvm-project-llvmorg-" + LLVM_VERSION,
        urls = ["https://github.com/llvm/llvm-project/archive/refs/tags/llvmorg-{v}.tar.gz".format(v = LLVM_VERSION)],
    )

    http_archive(
        name = "llvm_zlib",
        build_file = "@llvm-raw//utils/bazel/third_party_build:zlib-ng.BUILD",
        sha256 = "e36bb346c00472a1f9ff2a0a4643e590a254be6379da7cddd9daeb9a7f296731",
        strip_prefix = "zlib-ng-2.0.7",
        urls = ["https://github.com/zlib-ng/zlib-ng/archive/refs/tags/2.0.7.zip"],
    )

    http_archive(
        name = "llvm_zstd",
        build_file = "@llvm-raw//utils/bazel/third_party_build:zstd.BUILD",
        sha256 = "7c42d56fac126929a6a85dbc73ff1db2411d04f104fae9bdea51305663a83fd0",
        strip_prefix = "zstd-1.5.2",
        urls = ["https://github.com/facebook/zstd/releases/download/v1.5.2/zstd-1.5.2.tar.gz"],
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
