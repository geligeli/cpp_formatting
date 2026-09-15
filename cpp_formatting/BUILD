load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("@rules_shell//shell:sh_test.bzl", "sh_test")
load("@rules_pkg//pkg:tar.bzl", "pkg_tar")
load("@protobuf//bazel:cc_proto_library.bzl", "cc_proto_library")
load("@protobuf//bazel:proto_library.bzl", "proto_library")

cc_library(
    name = "output_mode",
    hdrs = ["output_mode.h"],
)

cc_library(
    name = "lint_lib",
    srcs = ["lint_lib.cpp"],
    hdrs = ["lint_lib.h"],
    deps = [
        "@llvm-project//llvm:Support",
    ],
)

cc_test(
    name = "lint_lib_test",
    srcs = ["lint_lib_test.cpp"],
    copts = [
        "-DGTEST_HAS_RTTI=0",
    ],
    deps = [
        ":lint_lib",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)

# The state a rename pass shares across translation units (dependent-token
# resolutions, vetoes, reported conflicts) and the merge semantics the TU
# driver relies on.  No Clang dependency.
cc_library(
    name = "rename_state",
    srcs = ["rename_state.cpp"],
    hdrs = ["rename_state.h"],
    deps = [
        ":lint_lib",
        "@llvm-project//llvm:Support",
    ],
)

cc_test(
    name = "rename_state_test",
    srcs = ["rename_state_test.cpp"],
    copts = [
        "-DGTEST_HAS_RTTI=0",
    ],
    deps = [
        ":rename_state",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)

# Runs a tool's per-TU action over every source on a pool of worker threads
# (one ClangTool per TU), exchanging cross-TU state at barriers so the result
# is deterministic and independent of the thread count.  Used by all three
# binaries.
cc_library(
    name = "tu_driver",
    srcs = ["tu_driver.cpp"],
    hdrs = ["tu_driver.h"],
    deps = [
        ":lint_lib",
        ":rename_state",
        "@llvm-project//clang:basic",
        "@llvm-project//clang:frontend",
        "@llvm-project//clang:tooling",
        "@llvm-project//llvm:Support",
    ],
)

cc_test(
    name = "tu_driver_test",
    srcs = ["tu_driver_test.cpp"],
    copts = [
        "-DGTEST_HAS_RTTI=0",
    ],
    deps = [
        ":tu_driver",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
        "@llvm-project//clang:tooling",
        "@llvm-project//llvm:Support",
    ],
)

# Shared library: the rewrite callback + the test-friendly rewriteToTrailingReturnTypes() helper.
cc_library(
    name = "trailing_return_types_lib",
    srcs = ["trailing_return_types_lib.cpp"],
    hdrs = ["trailing_return_types_lib.h"],
    deps = [
        ":lint_lib",
        ":output_mode",
        ":tu_driver",
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
        ":embedded_clang_resource",
        ":lint_lib",
        ":trailing_return_types_lib",
        ":tu_driver",
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

# The east-const / west-const rewrite: moves a type specifier's cv-qualifiers
# to the configured side.  Purely local to one TU -- no cross-TU state.
cc_library(
    name = "const_placement_lib",
    srcs = ["const_placement_lib.cpp"],
    hdrs = ["const_placement_lib.h"],
    deps = [
        ":lint_lib",
        ":output_mode",
        ":tu_driver",
        "@llvm-project//clang:ast",
        "@llvm-project//clang:basic",
        "@llvm-project//clang:frontend",
        "@llvm-project//clang:lex",
        "@llvm-project//clang:rewrite",
        "@llvm-project//clang:tooling",
        "@llvm-project//llvm:Support",
    ],
)

cc_test(
    name = "const_placement_test",
    srcs = ["const_placement_test.cpp"],
    copts = [
        "-DGTEST_HAS_RTTI=0",
    ],
    deps = [
        ":const_placement_lib",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)

cc_binary(
    name = "const_placement",
    srcs = ["const_placement.cpp"],
    linkstatic = True,
    deps = [
        ":const_placement_lib",
        ":embedded_clang_resource",
        ":lint_lib",
        ":tu_driver",
        "@llvm-project//clang:frontend",
        "@llvm-project//clang:tooling",
        "@llvm-project//llvm:Support",
    ],
)

sh_test(
    name = "const_placement_integration_test",
    srcs = ["const_placement_integration_test.sh"],
    args = [
        "$(location //cpp_formatting:const_placement)",
        "$(location //cpp_formatting:cpp_format)",
    ],
    data = [
        "//cpp_formatting:const_placement",
        "//cpp_formatting:cpp_format",
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
        ":lint_lib",
        ":output_mode",
        ":rename_state",
        ":tu_driver",
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
        ":embedded_clang_resource",
        ":lint_lib",
        ":naming_convention",
        ":rename_variables_lib",
        ":tu_driver",
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
        "$(location testdata/normalize_method_input.h)",
        "$(location testdata/normalize_method_input.cpp)",
        "$(location testdata/normalize_method_expected.h)",
        "$(location testdata/normalize_method_expected.cpp)",
    ],
    data = [
        "testdata/normalize_method_expected.cpp",
        "testdata/normalize_method_expected.h",
        "testdata/normalize_method_input.cpp",
        "testdata/normalize_method_input.h",
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

sh_test(
    name = "lint_integration_test",
    srcs = ["lint_integration_test.sh"],
    args = [
        "$(location //cpp_formatting:normalize_variables)",
        "$(location //cpp_formatting:trailing_return_types)",
        "$(location //cpp_formatting:cpp_format)",
        "$(location testdata/normalize_shadow_input.cpp)",
        "$(location testdata/normalize_shadow_expected.cpp)",
        "$(location testdata/input.cpp)",
        "$(location testdata/expected.cpp)",
    ],
    data = [
        "testdata/expected.cpp",
        "testdata/input.cpp",
        "testdata/normalize_shadow_expected.cpp",
        "testdata/normalize_shadow_input.cpp",
        "//cpp_formatting:cpp_format",
        "//cpp_formatting:normalize_variables",
        "//cpp_formatting:trailing_return_types",
    ],
)

# Combined single-pass library used by the cpp_format binary: runs every
# normalize_variables rule plus the trailing-return-type rewrite on one AST.
cc_library(
    name = "cpp_format_lib",
    srcs = ["cpp_format_lib.cpp"],
    hdrs = ["cpp_format_lib.h"],
    deps = [
        ":const_placement_lib",
        ":lint_lib",
        ":output_mode",
        ":rename_variables_lib",
        ":trailing_return_types_lib",
        ":tu_driver",
        "@llvm-project//clang:ast_matchers",
        "@llvm-project//clang:frontend",
        "@llvm-project//clang:rewrite",
        "@llvm-project//clang:tooling",
        "@llvm-project//llvm:Support",
    ],
)

cc_binary(
    name = "cpp_format",
    srcs = ["cpp_format.cpp"],
    linkstatic = True,
    visibility = ["//visibility:public"],
    deps = [
        ":const_placement_lib",
        ":cpp_format_lib",
        ":cpp_index_lib",
        ":cpp_index_merge",
        ":embedded_clang_resource",
        ":lint_lib",
        ":naming_convention",
        ":rename_variables_lib",
        ":tu_driver",
        "@llvm-project//clang:frontend",
        "@llvm-project//clang:tooling",
        "@llvm-project//llvm:Support",
    ],
)

# The symbol index schema: one IndexUnit per translation unit
# (`cpp_format --emit-index`), merged into one Index for the repository
# (`cpp_format --merge-index`).  Public so a consumer can read the files.
proto_library(
    name = "index_proto",
    srcs = ["index.proto"],
    visibility = ["//visibility:public"],
)

cc_proto_library(
    name = "index_cc_proto",
    visibility = ["//visibility:public"],
    deps = [":index_proto"],
)

# The Clang-free half of the index: normalizing, merging and serializing the
# messages.  `cpp_format --merge-index` / `--dump-index` run on this alone.
cc_library(
    name = "cpp_index_merge",
    srcs = ["cpp_index_merge.cpp"],
    hdrs = ["cpp_index_merge.h"],
    visibility = ["//code_browser:__pkg__"],
    deps = [
        ":index_cc_proto",
        "@llvm-project//llvm:Support",
        "@protobuf//:protobuf",
    ],
)

cc_test(
    name = "cpp_index_merge_test",
    srcs = ["cpp_index_merge_test.cpp"],
    copts = [
        "-DGTEST_HAS_RTTI=0",
    ],
    deps = [
        ":cpp_index_merge",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
        "@llvm-project//llvm:Support",
    ],
)

# The per-TU half of the index: an IndexDataConsumer for Clang's own indexing
# library, the TU driver client behind `cpp_format --emit-index`, and the
# in-memory test helper.
cc_library(
    name = "cpp_index_lib",
    srcs = ["cpp_index_lib.cpp"],
    hdrs = ["cpp_index_lib.h"],
    deps = [
        ":cpp_index_merge",
        ":index_cc_proto",
        ":lint_lib",
        ":rename_variables_lib",
        ":tu_driver",
        "@llvm-project//clang:ast",
        "@llvm-project//clang:basic",
        "@llvm-project//clang:frontend",
        "@llvm-project//clang:index",
        "@llvm-project//clang:lex",
        "@llvm-project//clang:tooling",
        "@llvm-project//llvm:Support",
        "@protobuf//:protobuf",
    ],
)

cc_test(
    name = "cpp_index_test",
    srcs = ["cpp_index_test.cpp"],
    copts = [
        "-DGTEST_HAS_RTTI=0",
    ],
    deps = [
        ":cpp_index_lib",
        ":cpp_index_merge",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
        "@llvm-project//clang:tooling",
    ],
)

# Exercises `cpp_format --emit-index` (per-file, with --owned-files, as the
# aspect runs it), `--merge-index` (incl. --records-from and an index as an
# input) and `--dump-index` (text, JSON, --lookup across files).
sh_test(
    name = "index_integration_test",
    srcs = ["index_integration_test.sh"],
    args = [
        "$(location //cpp_formatting:cpp_format)",
    ],
    data = [
        "//cpp_formatting:cpp_format",
    ],
)

# Aggregates per-TU edit-record files (from --emit-edits) into one repository
# change: resolves template-dependent tokens across TUs, merges edits per file,
# and prints a unified diff or applies in place.  No Clang/LLVM tooling needed —
# it only reads records and files.
cc_binary(
    name = "aggregate_edits",
    srcs = ["aggregate_edits.cpp"],
    linkstatic = True,
    visibility = ["//visibility:public"],
    deps = [
        ":lint_lib",
        "@llvm-project//llvm:Support",
    ],
)

# Exercises `cpp_format --emit-edits` + `cpp_format --aggregate` (the single
# published binary's aggregation mode) and its equivalence with the standalone
# aggregate_edits binary, including the cross-TU dependent-token path.
sh_test(
    name = "aggregate_integration_test",
    srcs = ["aggregate_integration_test.sh"],
    args = [
        "$(location //cpp_formatting:cpp_format)",
        "$(location //cpp_formatting:aggregate_edits)",
    ],
    data = [
        "//cpp_formatting:aggregate_edits",
        "//cpp_formatting:cpp_format",
    ],
)

cc_test(
    name = "rename_variables_test",
    srcs = ["rename_variables_test.cpp"],
    copts = [
        "-DGTEST_HAS_RTTI=0",
    ],
    deps = [
        ":naming_convention",
        ":rename_variables_lib",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)

pkg_tar(
    name = "clang_include_headers",
    srcs = ["@llvm-project//clang:builtin_headers_gen"],
    strip_prefix = "/external/+llvm+llvm-project/clang/staging",
    extension = "tar.gz",
)

# Host tool that converts a binary file into a C++ source file embedding its
# bytes as a const array.  Used instead of `xxd -i` so the genrule below works
# on any platform.
cc_binary(
    name = "embed_file",
    srcs = ["embed_file.cpp"],
    visibility = ["//code_browser:__pkg__"],
)

# Convert clang_include_headers.tar.gz into a C++ source file embedding the
# bytes as a const array.  Built into binaries so they can extract the headers
# at runtime without depending on a system-installed clang.
genrule(
    name = "clang_include_headers_embed_cc",
    srcs = [":clang_include_headers"],
    outs = ["clang_include_headers_embed.cc"],
    cmd = "$(execpath :embed_file) $(location :clang_include_headers) $@ kClangIncludeHeadersTarGz",
    tools = [":embed_file"],
)

cc_library(
    name = "embedded_clang_resource",
    srcs = [
        "embedded_clang_resource.cpp",
        ":clang_include_headers_embed_cc",
    ],
    hdrs = ["embedded_clang_resource.h"],
    deps = [
        "@llvm_zlib//:zlib",
    ],
)
