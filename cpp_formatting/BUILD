load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("@rules_shell//shell:sh_test.bzl", "sh_test")

cc_library(
    name = "output_mode",
    hdrs = ["output_mode.h"],
)

# Shared library: the rewrite callback + the test-friendly rewriteToTrailingReturnTypes() helper.
cc_library(
    name = "trailing_return_types_lib",
    srcs = ["trailing_return_types_lib.cpp"],
    hdrs = ["trailing_return_types_lib.h"],
    deps = [
        ":output_mode",
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

cc_library(
    name = "naming_convention",
    srcs = ["naming_convention.cpp"],
    hdrs = ["naming_convention.h"],
)

cc_test(
    name = "naming_convention_test",
    srcs = ["naming_convention_test.cpp"],
    copts = [
        "-DGTEST_HAS_RTTI=0",
    ],
    deps = [
        ":naming_convention",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)

cc_library(
    name = "rename_variables_lib",
    srcs = ["rename_variables_lib.cpp"],
    hdrs = ["rename_variables_lib.h"],
    deps = [
        ":output_mode",
        "@llvm-project//clang:ast",
        "@llvm-project//clang:frontend",
        "@llvm-project//clang:rewrite",
        "@llvm-project//clang:tooling",
        "@llvm-project//llvm:Support",
    ],
)

cc_binary(
    name = "normalize_variables",
    srcs = ["normalize_variables.cpp"],
    linkstatic = True,
    deps = [
        ":naming_convention",
        ":rename_variables_lib",
        "@llvm-project//clang:frontend",
        "@llvm-project//clang:tooling",
        "@llvm-project//llvm:Support",
    ],
)

sh_test(
    name = "normalize_variables_integration_test",
    srcs = ["normalize_variables_integration_test.sh"],
    args = [
        "$(location //cpp_formatting:normalize_variables)",
        "$(location testdata/normalize_multi_input.h)",
        "$(location testdata/normalize_multi_input.cpp)",
        "$(location testdata/normalize_multi_expected.h)",
        "$(location testdata/normalize_multi_expected.cpp)",
        "$(location testdata/normalize_shadow_input.cpp)",
        "$(location testdata/normalize_shadow_expected.cpp)",
        "$(location testdata/normalize_order_input.h)",
        "$(location testdata/normalize_order_impl_input.cpp)",
        "$(location testdata/normalize_order_test_input.cpp)",
        "$(location testdata/normalize_order_expected.h)",
        "$(location testdata/normalize_order_impl_expected.cpp)",
        "$(location testdata/normalize_order_test_expected.cpp)",
    ],
    data = [
        "testdata/normalize_multi_expected.cpp",
        "testdata/normalize_multi_expected.h",
        "testdata/normalize_multi_input.cpp",
        "testdata/normalize_multi_input.h",
        "testdata/normalize_order_expected.h",
        "testdata/normalize_order_impl_expected.cpp",
        "testdata/normalize_order_impl_input.cpp",
        "testdata/normalize_order_input.h",
        "testdata/normalize_order_test_expected.cpp",
        "testdata/normalize_order_test_input.cpp",
        "testdata/normalize_shadow_expected.cpp",
        "testdata/normalize_shadow_input.cpp",
        "//cpp_formatting:normalize_variables",
    ],
)

cc_binary(
    name = "cpp_format",
    srcs = ["cpp_format.cpp"],
    linkstatic = True,
    deps = [
        ":naming_convention",
        ":rename_variables_lib",
        ":trailing_return_types_lib",
        "@llvm-project//clang:frontend",
        "@llvm-project//clang:tooling",
        "@llvm-project//llvm:Support",
    ],
)

cc_test(
    name = "rename_variables_test",
    srcs = ["rename_variables_test.cpp"],
    copts = [
        "-DGTEST_HAS_RTTI=0",
    ],
    deps = [
        ":rename_variables_lib",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
