"""The protobuf producer of the symbol index, as an aspect on `proto_library`.

The kit's copy of `bazel/proto_index.bzl`, and identical to it but for how the
tool is named: the prebuilt `@cpp_format_bin//:cpp_format` a release provides
rather than one built from source.  Change the two together.

The index is one format with a producer per language.  `cpp_index_aspect` in
`cpp_format.bzl` is the C++ one; this is the second, and it lives in a file of
its own so that a repository without a single `.proto` never loads `@protobuf`
on the index's account: `cpp_format.sh` names this aspect only when its query
finds a `proto_library`.  It needs a release whose `cpp_format` knows
`--emit-proto-index`; with an older one these actions fail and the index is
built from the rest (`--keep_going`).

Per first-party `proto_library`:

  * one `protoc --cpp_out=annotate_headers:` action, for the `.pb.h.meta` next
    to each generated header: protoc's own account of which bytes of the
    header came from which message, field or enumerator.  It is the protoc
    `cc_proto_library` runs, with the same import paths, and `annotate_headers`
    does not change the header, so the byte ranges are those of the header the
    C++ compiler (and the C++ index) sees.  The header and the `.pb.cc` this
    run writes are not declared outputs and are dropped;
  * one `cpp_format --emit-proto-index` action per `.proto`: its definitions
    and references, plus those ranges as `GENERATES` anchors on the path a
    C++ compile opens that header under.

The merge (`cpp_format --merge-index`) joins anchors with the C++ symbols
declared on them; nothing here, and nothing in the C++ aspect, knows about the
other language.

The index names files by their real, exec-root-relative path.  A
`proto_library` with `strip_import_prefix`/`import_prefix` hands out
`_virtual_imports` symlinks instead, so every target contributes (import name,
real path) pairs -- the path map -- and the sources themselves, transitively;
external repositories included, which get no unit of their own but are where
an imported `google.protobuf.Timestamp` is declared.
"""

load("@protobuf//bazel/common:proto_common.bzl", "proto_common")
load("@protobuf//bazel/common:proto_info.bzl", "ProtoInfo")

ProtoIndexInfo = provider(
    doc = "Per-.proto index units for a proto_library and its transitive deps.",
    fields = {
        "units": "depset of per-.proto index unit files (serialized IndexUnit " +
                 "protos from --emit-proto-index), transitively",
        "path_map": "depset of `import name<TAB>real path` lines, transitively",
        "sources": "depset of the real .proto Files those lines name",
    },
)

def _dep_infos(ctx):
    deps = getattr(ctx.rule.attr, "deps", []) + getattr(ctx.rule.attr, "exports", [])
    return [d[ProtoIndexInfo] for d in deps if ProtoIndexInfo in d]

# The three passes proto_common.compile makes over transitive_proto_path, which
# it keeps private: protoc takes the first -I a file is under, so the most
# specific roots have to come first.
def _virtual_proto_path(path):
    return "-I" + path if path.count("/") > 4 else None

def _repo_proto_path(path):
    return "-I" + path if 2 < path.count("/") and path.count("/") <= 4 else None

def _main_output_proto_path(path):
    return "-I" + path if path.count("/") <= 2 and path != "." else None

def _generated_header_path(ctx, proto_info, src):
    """The path a C++ compile opens the header generated from `src` under.

    cc_proto_library declares it as the sibling of `src` in bin.  When the
    proto_library has an import prefix to strip or add, `src` is a
    `_virtual_imports` symlink and the header is *reached* through the
    `_virtual_includes/<name>` tree that cc_proto_library's
    strip_include_prefix sets up -- the name Clang opens, so the name the C++
    index has.
    """
    root = proto_info.proto_source_root
    if root != "." and root != ctx.label.workspace_root:
        stem = proto_common.get_import_path(src)[:-len(src.extension) - 1]
        package = "/".join([p for p in [ctx.label.workspace_root, ctx.label.package] if p])
        return "%s/%s/_virtual_includes/%s/%s.pb.h" % (ctx.bin_dir.path, package, ctx.label.name, stem)
    stem = src.path[:-len(src.extension) - 1]
    if src.is_source:
        stem = ctx.bin_dir.path + "/" + stem
    return stem + ".pb.h"

def _proto_index_aspect_impl(target, ctx):
    dep_infos = _dep_infos(ctx)
    proto_info = target[ProtoInfo]

    # The sources as the rule was given them, and as ProtoInfo hands them out
    # (the same files, or their _virtual_imports symlinks), in the same order.
    real = ctx.rule.files.srcs
    direct = proto_info.direct_sources
    if len(real) != len(direct):
        real = direct
    path_map = depset(
        direct = [
            proto_common.get_import_path(direct[i]) + "\t" + real[i].path
            for i in range(len(direct))
        ],
        transitive = [i.path_map for i in dep_infos],
    )
    sources = depset(direct = real, transitive = [i.sources for i in dep_infos])
    transitive_units = depset(transitive = [i.units for i in dep_infos])

    # First-party proto_library targets only.  `no-cpp-index` is the index's
    # opt-out whatever the language.
    if (ctx.label.workspace_name != "" or
        "no-cpp-index" in getattr(ctx.rule.attr, "tags", [])):
        return [ProtoIndexInfo(units = transitive_units, path_map = path_map, sources = sources)]

    # Read by cpp_format.sh, like the C++ aspect's: one unit path per line.
    # Always written, even empty: Bazel never deletes a stale output, so a
    # target whose srcs went away would otherwise keep listing its old units.
    manifest = ctx.actions.declare_file(ctx.label.name + ".proto_index.manifest")
    if not direct:
        ctx.actions.write(manifest, "")
        return [
            ProtoIndexInfo(units = transitive_units, path_map = path_map, sources = sources),
            OutputGroupInfo(proto_index = depset(direct = [manifest], transitive = [transitive_units])),
        ]

    out_dir = ctx.label.name + ".proto_index"
    import_paths = [proto_common.get_import_path(f) for f in direct]

    # protoc names its outputs after the import path, under the --cpp_out root.
    metas = [
        ctx.actions.declare_file("%s/gen/%s.pb.h.meta" % (out_dir, p[:-len(".proto")]))
        for p in import_paths
    ]
    gen_root = metas[0].path[:-len(import_paths[0][:-len(".proto")] + ".pb.h.meta") - 1]
    protoc_args = ctx.actions.args()
    protoc_args.add("--cpp_out=annotate_headers:" + gen_root)
    protoc_args.add_all(proto_info.transitive_proto_path, map_each = _virtual_proto_path)
    protoc_args.add_all(proto_info.transitive_proto_path, map_each = _repo_proto_path)
    protoc_args.add_all(proto_info.transitive_proto_path, map_each = _main_output_proto_path)
    protoc_args.add("-I.")
    protoc_args.add_all(direct)
    ctx.actions.run(
        executable = ctx.executable._protoc,
        arguments = [protoc_args],
        inputs = proto_info.transitive_sources,
        outputs = metas,
        mnemonic = "ProtoIndexAnnotate",
        progress_message = "cpp_format: annotating the C++ generated from %{label}",
    )

    path_map_file = ctx.actions.declare_file(out_dir + "/path-map.txt")
    path_map_args = ctx.actions.args()
    path_map_args.add_all(path_map)
    path_map_args.set_param_file_format("multiline")
    ctx.actions.write(path_map_file, path_map_args)

    binary = ctx.file._cpp_format
    units = []
    for i in range(len(direct)):
        unit = ctx.actions.declare_file("%s/%s.pb" % (out_dir, import_paths[i]))
        args = ctx.actions.args()
        args.add("--emit-proto-index", unit)
        args.add("--path-map", path_map_file)
        args.add("--anchors", "%s=%s" % (metas[i].path, _generated_header_path(ctx, proto_info, direct[i])))
        args.add(real[i])
        ctx.actions.run(
            executable = binary,
            tools = [binary],
            arguments = [args],
            inputs = depset(direct = [path_map_file, metas[i]], transitive = [sources]),
            outputs = [unit],
            mnemonic = "ProtoIndexEmit",
            progress_message = "cpp_format: indexing " + real[i].short_path,
        )
        units.append(unit)

    ctx.actions.write(manifest, "".join([u.path + "\n" for u in units]))

    mine = depset(direct = units, transitive = [transitive_units])
    return [
        ProtoIndexInfo(units = mine, path_map = path_map, sources = sources),
        OutputGroupInfo(proto_index = depset(direct = [manifest], transitive = [mine])),
    ]

proto_index_aspect = aspect(
    implementation = _proto_index_aspect_impl,
    attr_aspects = ["deps", "exports"],
    required_providers = [ProtoInfo],
    attrs = {
        "_cpp_format": attr.label(
            default = Label("@cpp_format_bin//:cpp_format"),
            allow_single_file = True,
        ),
        # What proto_lang_toolchain -- so cc_proto_library -- compiles with.
        "_protoc": attr.label(
            default = configuration_field(fragment = "proto", name = "proto_compiler"),
            executable = True,
            cfg = "exec",
        ),
    },
    fragments = ["proto"],
)
