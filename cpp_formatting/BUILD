load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("@rules_shell//shell:sh_test.bzl", "sh_test")

# Shared library: the rewrite callback + the test-friendly rewriteToTrailingReturnTypes() helper.
cc_library(
    name = "trailing_return_types_lib",
    srcs = ["trailing_return_types_lib.cpp"],
    hdrs = ["trailing_return_types_lib.h"],
    deps = [
        "@llvm-project//clang:ast_matchers",
        "@llvm-project//clang:frontend",
        "@llvm-project//clang:lex",
        "@llvm-project//clang:rewrite",
        "@llvm-project//clang:tooling",
        "@llvm-project//llvm:Support",
    ],
)

cc_binary(
    name = "trailing_return_types",
    srcs = ["trailing_return_types.cpp"],
    linkstatic = True,
    deps = [
        ":trailing_return_types_lib",
        "@llvm-project//clang:ast_matchers",
        "@llvm-project//clang:frontend",
        "@llvm-project//clang:lex",
        "@llvm-project//clang:rewrite",
        "@llvm-project//clang:tooling",
        "@llvm-project//llvm:Support",
    ],
)

cc_test(
    name = "trailing_return_types_test",
    srcs = ["trailing_return_types_test.cpp"],
    copts = [
        "-DGTEST_HAS_RTTI=0",
    ],
    deps = [
        ":trailing_return_types_lib",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)

sh_test(
    name = "trailing_return_types_integration_test",
    srcs = ["integration_test.sh"],
    args = [
        "$(location //cpp_formatting:trailing_return_types)",
        "$(location testdata/input.cpp)",
        "$(location testdata/expected.cpp)",
        "$(location testdata/input2.cpp)",
        "$(location testdata/expected2.cpp)",
        "$(location testdata/input3.cpp)",
        "$(location testdata/expected3.cpp)",
    ],
    data = [
        "testdata/expected.cpp",
        "testdata/expected2.cpp",
        "testdata/expected3.cpp",
        "testdata/input.cpp",
        "testdata/input2.cpp",
        "testdata/input3.cpp",
        "//cpp_formatting:trailing_return_types",
    ],
)
