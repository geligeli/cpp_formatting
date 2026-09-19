# BUILD file for the include-what-you-use source archive (`@iwyu`), fetched by
# the `llvm` module extension in //third_party/llvm:extensions.bzl.
#
# IWYU ships a CMake build only.  This is the part of it the repo needs: the
# one executable, linked against the same from-source Clang the formatting
# tools use (IWYU is tied to a Clang release -- see IWYU_VERSION there), plus
# the scripts and mapping files it installs next to it.
load("@rules_cc//cc:cc_binary.bzl", "cc_binary")

package(default_visibility = ["//visibility:public"])

licenses(["notice"])  # LLVM-style, see LICENSE.TXT

cc_binary(
    name = "include-what-you-use",
    srcs = glob([
        "*.cc",
        "*.h",
    ]),
    # The same flag the rest of the build gets from .bazelrc, repeated because
    # the aspect builds this in the exec configuration, where --cxxopt does not
    # reach: Clang is built without RTTI and IWYU derives from its classes.
    copts = select({
        "@platforms//os:windows": ["/GR-"],
        "//conditions:default": ["-fno-rtti"],
    }),
    local_defines = [
        # CMake takes this from `git rev-parse`; an archive has no .git.
        "IWYU_GIT_REV=\\\"bazel\\\"",
        # Both empty: no resource dir is compiled in (case 1 of
        # ComputeCustomResourceDir).  Every caller passes -resource-dir
        # pointing at the staged @llvm-project//clang:builtin_headers_gen,
        # exactly as the cpp_format aspect does.
        "IWYU_RESOURCE_BINARY_PATH=\\\"\\\"",
        "IWYU_RESOURCE_DIR=\\\"\\\"",
    ],
    deps = [
        "@llvm-project//clang:ast",
        "@llvm-project//clang:basic",
        "@llvm-project//clang:driver",
        "@llvm-project//clang:frontend",
        "@llvm-project//clang:frontend_tool",
        "@llvm-project//clang:lex",
        "@llvm-project//clang:sema",
        "@llvm-project//clang:serialization",
        "@llvm-project//clang:tooling_inclusions",
        "@llvm-project//llvm:AllTargetsAsmParsers",
        "@llvm-project//llvm:AllTargetsCodeGens",
        "@llvm-project//llvm:Option",
        "@llvm-project//llvm:Support",
        "@llvm-project//llvm:TargetParser",
    ],
)

# Applies IWYU's suggestions to the sources.  Plain Python 3, no dependencies;
# exported as a file so the driver script can run it with the host python.
exports_files([
    "fix_includes.py",
    "iwyu_tool.py",
])

# The mapping files IWYU ships (Boost, Qt, intrinsics, ...).
filegroup(
    name = "mappings",
    srcs = glob(["*.imp"]),
)
