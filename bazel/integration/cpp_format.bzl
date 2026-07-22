"""cpp_format Bazel integration driven by the prebuilt release binary.

Vendored integration kit — see `extensions.bzl` for how the binary is fetched.

A per-target **aspect** derives each `cc_*` target's compile flags from its
`CcInfo.compilation_context` + the C++ toolchain and runs `cpp_format
--emit-edits`, declaring the target's sources **and its transitive headers** as
action inputs.  Declaring the transitive headers is what makes every include
reachable under sandboxing — no `compile_commands.json` needed.  The prebuilt
binary embeds and self-extracts the Clang builtin headers, so unlike the
from-source build this kit stages no resource directory of its own.

Each action writes a structured edit-record JSON file (offset-level edits plus a
template-dependent-token resolution sidecar).  `cpp_format --aggregate` merges
the per-target records into one repository change, resolving dependent tokens
across targets.  `cpp_format_targets(name, deps)` generates three targets:

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
    # deps but produces nothing there.
    if ctx.label.workspace_name != "" or CcInfo not in target:
        return [CppFormatEditsInfo(records = depset(transitive = transitive))]
    srcs = _own_sources(ctx)
    if not srcs:
        return [CppFormatEditsInfo(records = depset(transitive = transitive))]

    cc_toolchain = find_cc_toolchain(ctx)
    cc_ctx = target[CcInfo].compilation_context
    flags = _compile_flags(ctx, cc_toolchain, cc_ctx)
    binary = ctx.file._cpp_format

    records = ctx.actions.declare_file(ctx.label.name + ".cpp_format.json")
    args = ctx.actions.args()
    args.add("--config", ctx.file._config)
    args.add("--emit-edits", records)
    args.add_all(srcs)
    args.add("--")
    # Force C++ so headers (.h) parse as C++ rather than C, and carry the
    # derived compile command.  cpp_format drops -fno-canonical-system-headers
    # itself and supplies its own (self-extracted) -resource-dir.
    args.add("-x")
    args.add("c++")
    args.add_all(flags)

    ctx.actions.run(
        executable = binary,
        arguments = [args],
        tools = [binary],
        inputs = depset(
            direct = srcs + [ctx.file._config],
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
            default = Label("@cpp_format_bin//:cpp_format"),
            allow_single_file = True,
        ),
        "_config": attr.label(
            default = Label("@@//:cpp_format.yaml"),
            allow_single_file = True,
        ),
    },
)

# ---------------------------------------------------------------------------
# Aggregation rules (check / diff / fix)
# ---------------------------------------------------------------------------

# Bash runfiles library bootstrap (Bazel v3 snippet) so `rlocation` resolves the
# cpp_format binary and the per-target record files at run/test time.
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

def _rlocation_path(f):
    # runfiles key for `rlocation`: external-repo files carry a "../" prefix in
    # short_path; main-repo files are addressed under the root module name.
    if f.short_path.startswith("../"):
        return f.short_path[3:]
    return "_main/" + f.short_path

def _records_of(ctx):
    return depset(transitive = [
        d[CppFormatEditsInfo].records
        for d in ctx.attr.deps
        if CppFormatEditsInfo in d
    ]).to_list()

def _aggregator_impl(ctx):
    recs = _records_of(ctx)
    binary = ctx.file._cpp_format
    rec_lines = "".join([
        'records+=("$(rlocation "' + _rlocation_path(f) + '")")\n'
        for f in recs
    ])
    script = ctx.actions.declare_file(ctx.label.name + ".sh")
    ctx.actions.write(
        output = script,
        is_executable = True,
        content = (
            _RUNFILES_PREAMBLE + rec_lines +
            'BIN="$(rlocation "' + _rlocation_path(binary) + '")"\n' +
            'exec "$BIN" --aggregate ' + ctx.attr.mode_flags +
            ' --root="${BUILD_WORKSPACE_DIRECTORY:-$PWD}" "${records[@]}"\n'
        ),
    )
    runfiles = ctx.runfiles(files = recs + [binary])
    runfiles = runfiles.merge(ctx.attr._bash_runfiles[DefaultInfo].default_runfiles)
    return [DefaultInfo(executable = script, runfiles = runfiles)]

_AGG_ATTRS = {
    "deps": attr.label_list(
        aspects = [cpp_format_aspect],
        providers = [CcInfo],
        doc = "cc_* targets to format (transitively).",
    ),
    "mode_flags": attr.string(default = ""),
    "_cpp_format": attr.label(
        default = Label("@cpp_format_bin//:cpp_format"),
        allow_single_file = True,
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
