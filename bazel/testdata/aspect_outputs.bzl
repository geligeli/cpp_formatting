"""Test-only rules that put the aspects' outputs in front of a Bazel test.

The driver of the aspects is `bazel/integration/cpp_format.sh`, which applies
them to a target pattern from the command line.  It runs Bazel, so it cannot
run inside a Bazel test -- and the aspects still have to be exercised by `bazel
test //...`, remotely executed like everything else.  Each rule here applies
one aspect to `deps` and hands back, as its files, one of the depsets the
aspect's provider carries; what a test does with them (aggregate, merge, read)
is an ordinary `sh_test` or `genrule` in the BUILD file.

Not part of the integration: a rule's `deps` can only name labels, never a
pattern such as `//...`, which is why nothing like this is offered to users.
"""

load("//bazel:cpp_format.bzl", "CppFormatEditsInfo", "CppIndexInfo", "cpp_format_aspect", "cpp_index_aspect")
load("//bazel:proto_index.bzl", "ProtoIndexInfo", "proto_index_aspect")

def _files_rule(aspect, provider, field, doc):
    def _impl(ctx):
        return [DefaultInfo(files = depset(transitive = [
            getattr(d[provider], field)
            for d in ctx.attr.deps
            if provider in d
        ]))]

    return rule(
        doc = doc,
        implementation = _impl,
        attrs = {"deps": attr.label_list(aspects = [aspect])},
    )

cpp_format_records = _files_rule(
    cpp_format_aspect,
    CppFormatEditsInfo,
    "records",
    "The per-file edit records of `deps`, transitively (what `--aggregate` merges).",
)

cpp_compile_commands_fragments = _files_rule(
    cpp_format_aspect,
    CppFormatEditsInfo,
    "compile_commands",
    "The per-target compile_commands fragments of `deps`, transitively.",
)

cpp_index_units = _files_rule(
    cpp_index_aspect,
    CppIndexInfo,
    "units",
    "The per-translation-unit index units of `deps`, transitively (what `--merge-index` merges).",
)

proto_index_units = _files_rule(
    proto_index_aspect,
    ProtoIndexInfo,
    "units",
    "The per-.proto index units of the proto_library `deps`, transitively.",
)
