"""Bazel integration for cpp_format.

A per-target **aspect** derives each cc_* target's compile flags from its
`CcInfo.compilation_context` + the C++ toolchain and runs `cpp_format
--emit-edits`, declaring the target's sources **and its transitive headers** as
action inputs.  Declaring the transitive headers is what makes the tool
reachable-header-correct under sandboxing — no `compile_commands.json` needed.

Each action writes a structured edit-record JSON file (offset-level edits plus a
template-dependent-token resolution sidecar).  The `aggregate_edits` tool merges
the per-target records into one repository change, resolving dependent tokens
across targets.  Three rules drive it:

  * `<name>.check` — a test that fails when any edit would be applied (lint gate),
  * `<name>.diff`  — `bazel run` prints the merged unified diff (review),
  * `<name>.fix`   — `bazel run` applies the edits in $BUILD_WORKSPACE_DIRECTORY.

Because Bazel actions cannot mutate workspace sources, `.fix` runs outside the
action graph via `bazel run`, consuming the same records the aspect produced.
"""

load("@rules_cc//cc:action_names.bzl", "CPP_COMPILE_ACTION_NAME")
load("@rules_cc//cc:find_cc_toolchain.bzl", "find_cc_toolchain", "use_cc_toolchain")

CppFormatEditsInfo = provider(
    doc = "Transitive set of per-target cpp_format edit-record JSON files.",
    fields = {"records": "depset of .cpp_format.json files"},
)

_SRC_EXTS = ["cc", "cpp", "cxx", "c++", "h", "hh", "hpp", "hxx", "h++", "inc", "ipp"]

def _own_sources(ctx):
    out = []
    for attr in ("srcs", "hdrs"):
        for t in getattr(ctx.rule.attr, attr, []):
            for f in t.files.to_list():
                if f.is_source and f.extension in _SRC_EXTS:
                    out.append(f)
    return out

def _resource_dir(builtin_headers):
    # The staged Clang builtin headers live under ".../staging/include"; the
    # resource dir is the "staging" parent.  Derived from the actual file paths
    # so it survives a module-extension rename (unlike a hardcoded path).
    for f in builtin_headers:
        idx = f.path.find("/staging/")
        if idx != -1:
            return f.path[:idx] + "/staging"
    return None

def _compile_flags(ctx, cc_toolchain, cc_ctx):
    feature_config = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
        requested_features = ctx.features,
        unsupported_features = ctx.disabled_features,
    )
    variables = cc_common.create_compile_variables(
        feature_configuration = feature_config,
        cc_toolchain = cc_toolchain,
        user_compile_flags = ctx.fragments.cpp.copts + ctx.fragments.cpp.cxxopts,
        include_directories = cc_ctx.includes,
        quote_include_directories = cc_ctx.quote_includes,
        system_include_directories = cc_ctx.system_includes,
        framework_include_directories = cc_ctx.framework_includes,
        preprocessor_defines = depset(transitive = [cc_ctx.defines, cc_ctx.local_defines]),
    )
    return cc_common.get_memory_inefficient_command_line(
        feature_configuration = feature_config,
        action_name = CPP_COMPILE_ACTION_NAME,
        variables = variables,
    )

def _aspect_impl(target, ctx):
    transitive = [
        d[CppFormatEditsInfo].records
        for d in getattr(ctx.rule.attr, "deps", [])
        if CppFormatEditsInfo in d
    ]

    # First-party cc_* targets only. The aspect still propagates into external
    # deps (e.g. @llvm-project) but produces nothing there.
    if ctx.label.workspace_name != "" or CcInfo not in target:
        return [CppFormatEditsInfo(records = depset(transitive = transitive))]
    srcs = _own_sources(ctx)
    if not srcs:
        return [CppFormatEditsInfo(records = depset(transitive = transitive))]

    cc_toolchain = find_cc_toolchain(ctx)
    cc_ctx = target[CcInfo].compilation_context
    flags = _compile_flags(ctx, cc_toolchain, cc_ctx)
    builtin = ctx.files._builtin_headers
    res_dir = _resource_dir(builtin)

    records = ctx.actions.declare_file(ctx.label.name + ".cpp_format.json")
    args = ctx.actions.args()
    args.add("--config", ctx.file._config)
    args.add("--emit-edits", records)
    args.add_all(srcs)
    args.add("--")
    # Force C++ so headers (.h) parse as C++ rather than C, and carry the
    # derived compile command.  cpp_format itself drops -fno-canonical-system-headers.
    args.add("-x")
    args.add("c++")
    args.add_all(flags)
    if res_dir:
        args.add("-resource-dir=" + res_dir)

    ctx.actions.run(
        executable = ctx.executable._cpp_format,
        arguments = [args],
        inputs = depset(
            direct = srcs + [ctx.file._config] + builtin,
            transitive = [cc_ctx.headers, cc_toolchain.all_files],
        ),
        outputs = [records],
        mnemonic = "CppFormatEmit",
        progress_message = "cpp_format: emitting edits for %{label}",
    )
    mine = depset(direct = [records], transitive = transitive)
    return [
        CppFormatEditsInfo(records = mine),
        OutputGroupInfo(cpp_format_edits = mine),
    ]

cpp_format_aspect = aspect(
    implementation = _aspect_impl,
    attr_aspects = ["deps"],
    fragments = ["cpp"],
    toolchains = use_cc_toolchain(),
    attrs = {
        "_cpp_format": attr.label(
            default = Label("//cpp_formatting:cpp_format"),
            executable = True,
            cfg = "exec",
        ),
        "_config": attr.label(
            default = Label("//:cpp_format.yaml"),
            allow_single_file = True,
        ),
        "_builtin_headers": attr.label(
            default = Label("@llvm-project//clang:builtin_headers_gen"),
        ),
    },
)

# ---------------------------------------------------------------------------
# Aggregation rules (check / diff / fix)
# ---------------------------------------------------------------------------

# Bash runfiles library bootstrap (Bazel v3 snippet) so `rlocation` resolves the
# aggregate_edits binary and the per-target record files at run/test time.
_RUNFILES_PREAMBLE = """#!/usr/bin/env bash
# --- begin runfiles.bash initialization v3 ---
set -uo pipefail; set +e; f=bazel_tools/tools/bash/runfiles/runfiles.bash
source "${RUNFILES_DIR:-/dev/null}/$f" 2>/dev/null || \\
  source "$(grep -sm1 "^$f " "${RUNFILES_MANIFEST_FILE:-/dev/null}" | cut -f2- -d' ')" 2>/dev/null || \\
  source "$0.runfiles/$f" 2>/dev/null || \\
  source "$(grep -sm1 "^$f " "$0.runfiles_manifest" | cut -f2- -d' ')" 2>/dev/null || \\
  source "$(grep -sm1 "^$f " "$0.exe.runfiles_manifest" | cut -f2- -d' ')" 2>/dev/null || \\
  { echo>&2 "ERROR: cannot find $f"; exit 1; }; f=; set -e
# --- end runfiles.bash initialization v3 ---
records=()
"""

def _records_of(ctx):
    return depset(transitive = [
        d[CppFormatEditsInfo].records
        for d in ctx.attr.deps
        if CppFormatEditsInfo in d
    ]).to_list()

def _aggregator_impl(ctx):
    recs = _records_of(ctx)
    agg = ctx.executable._aggregate
    rec_lines = "".join([
        'records+=("$(rlocation "_main/' + f.short_path + '")")\n'
        for f in recs
    ])
    script = ctx.actions.declare_file(ctx.label.name + ".sh")
    ctx.actions.write(
        output = script,
        is_executable = True,
        content = (
            _RUNFILES_PREAMBLE + rec_lines +
            'AGG="$(rlocation "_main/' + agg.short_path + '")"\n' +
            'exec "$AGG" ' + ctx.attr.mode_flags +
            ' --root="${BUILD_WORKSPACE_DIRECTORY:-$PWD}" "${records[@]}"\n'
        ),
    )
    runfiles = ctx.runfiles(files = recs + [agg])
    runfiles = runfiles.merge(ctx.attr._aggregate[DefaultInfo].default_runfiles)
    runfiles = runfiles.merge(ctx.attr._bash_runfiles[DefaultInfo].default_runfiles)
    return [DefaultInfo(executable = script, runfiles = runfiles)]

_AGG_ATTRS = {
    "deps": attr.label_list(
        aspects = [cpp_format_aspect],
        providers = [CcInfo],
        doc = "cc_* targets to format (transitively).",
    ),
    "mode_flags": attr.string(default = ""),
    "_aggregate": attr.label(
        default = Label("//cpp_formatting:aggregate_edits"),
        executable = True,
        cfg = "target",
    ),
    "_bash_runfiles": attr.label(default = Label("@bazel_tools//tools/bash/runfiles")),
}

_cpp_format_run = rule(
    implementation = _aggregator_impl,
    executable = True,
    attrs = _AGG_ATTRS,
)

_cpp_format_test = rule(
    implementation = _aggregator_impl,
    test = True,
    attrs = _AGG_ATTRS,
)

def cpp_format_targets(name, deps, **kwargs):
    """Defines <name>.check (test), <name>.diff and <name>.fix (bazel run)."""
    _cpp_format_test(name = name + ".check", deps = deps, mode_flags = "--check", **kwargs)
    _cpp_format_run(name = name + ".diff", deps = deps, mode_flags = "", **kwargs)
    _cpp_format_run(name = name + ".fix", deps = deps, mode_flags = "--apply", **kwargs)
