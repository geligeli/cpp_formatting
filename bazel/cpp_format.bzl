"""Bazel integration for cpp_format.

An **aspect** derives each cc_* target's compile flags from its
`CcInfo.compilation_context` + the C++ toolchain and runs one `cpp_format
--emit-edits` action per source file, declaring that file **and the target's
transitive headers** as action inputs.  Declaring the transitive headers is
what makes the tool reachable-header-correct under sandboxing — no
`compile_commands.json` needed.

One action per source file -- like CppCompile, so Bazel parallelises, caches
and remotely executes at file granularity -- writes a structured edit-record
JSON file (offset-level edits plus a template-dependent-token resolution
sidecar).  The `aggregate_edits` tool merges the per-file records into one
repository change, resolving dependent tokens across files and targets.  Three
rules drive it:

  * `<name>.check` — a test that fails when any edit would be applied (lint gate),
  * `<name>.diff`  — `bazel run` prints the merged unified diff (review),
  * `<name>.fix`   — `bazel run` applies the edits in $BUILD_WORKSPACE_DIRECTORY.

Because Bazel actions cannot mutate workspace sources, `.fix` runs outside the
action graph via `bazel run`, consuming the same records the aspect produced.
"""

load("@rules_cc//cc:action_names.bzl", "CPP_COMPILE_ACTION_NAME")
load("@rules_cc//cc:find_cc_toolchain.bzl", "find_cc_toolchain", "use_cc_toolchain")

CppFormatEditsInfo = provider(
    doc = "Transitive cpp_format state: per-source-file edit-record JSON files, and " +
          "the first-party headers the dep closure owns (see `_owned_headers`).",
    fields = {
        "records": "depset of per-source-file edit-record JSON files",
        "headers": "depset of first-party header source files in the dep closure",
    },
)

# .inc / .ipp are deliberately absent. They are textual fragments -- #include'd
# into the middle of another file -- so they do not parse as translation units
# of their own: abseil's spinlock_posix.inc references declarations from its
# includer, and spinlock_win32.inc includes <windows.h>. Passing them as sources
# fails the whole emit action. They are not propagated as owned headers either:
# a file the tool never rewrites must not have its declarations renamed at use
# sites, which would half-apply the rename. The cost is that declarations
# living in a textual fragment are not formatted.
_HDR_EXTS = ["h", "hh", "hpp", "hxx", "h++"]
_SRC_EXTS = ["cc", "cpp", "cxx", "c++"] + _HDR_EXTS

def _own_files(ctx, exts):
    out = []
    seen = {}
    for attr in ("srcs", "hdrs"):
        for t in getattr(ctx.rule.attr, attr, []):
            for f in t.files.to_list():
                # Deduplicated: a glob can match the same header in both srcs
                # and hdrs (googletest does exactly that), and listing a file
                # twice makes cpp_format parse it as a main file twice.  The
                # second pass then reads back the first pass's rewrite, so the
                # target emits duplicate -- and mutually conflicting -- records
                # for one source location.
                # A first-party target may list a file from an external repo;
                # it is not ours to rewrite, and its `../` short_path could not
                # name a record file anyway.
                if (f.is_source and f.extension in exts and f.path not in seen and
                    not f.short_path.startswith("../")):
                    seen[f.path] = True
                    out.append(f)
    return out

def _own_sources(ctx):
    return _own_files(ctx, _SRC_EXTS)

def _record_path(ctx, src):
    # Under <name>.cpp_format/ so two targets' records never collide, and keyed
    # by the source's workspace-relative path so two sources sharing a basename
    # in different packages do not either.
    return ctx.label.name + ".cpp_format/" + src.short_path + ".json"

# Headers this target owns, propagated to dependents as `--owned-files`.  A
# target's action parses only its own sources, so without this a declaration in
# a dependency's header is invisible to the file set and its *uses* in this
# target go unrenamed while the declaration itself is renamed by the dep's own
# action — a half-applied rename that breaks the build.  Only headers are
# propagated: a dep's .cpp is never reachable from this TU.
def _owned_headers(ctx):
    return _own_files(ctx, _HDR_EXTS)

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
    dep_infos = [
        d[CppFormatEditsInfo]
        for d in getattr(ctx.rule.attr, "deps", [])
        if CppFormatEditsInfo in d
    ]
    transitive = [i.records for i in dep_infos]
    dep_headers = depset(transitive = [i.headers for i in dep_infos])

    # First-party cc_* targets only. The aspect still propagates into external
    # deps (e.g. @llvm-project) but produces nothing there.  A `no-cpp-format`
    # target contributes no owned headers either: its declarations are never
    # renamed, so a dependent must not rename their uses.
    if ("no-cpp-format" in getattr(ctx.rule.attr, "tags", []) or
        ctx.label.workspace_name != "" or CcInfo not in target):
        return [CppFormatEditsInfo(
            records = depset(transitive = transitive),
            headers = dep_headers,
        )]
    srcs = _own_sources(ctx)
    mine_headers = depset(direct = _owned_headers(ctx), transitive = [dep_headers])
    if not srcs:
        return [CppFormatEditsInfo(
            records = depset(transitive = transitive),
            headers = mine_headers,
        )]

    cc_toolchain = find_cc_toolchain(ctx)
    cc_ctx = target[CcInfo].compilation_context
    flags = _compile_flags(ctx, cc_toolchain, cc_ctx)
    builtin = ctx.files._builtin_headers
    res_dir = _resource_dir(builtin)

    # One action per source file, like CppCompile: Bazel then parallelises and
    # caches at file granularity -- editing one .cpp re-parses one file, not
    # its whole target -- and remote execution spreads the parses across
    # workers.  Each action parses exactly one file and is told, via
    # --owned-files, that every file of this target and every header of its dep
    # closure is renameable, so a declaration anywhere in that set is rewritten
    # at its use sites here, while the declaration itself is rewritten only by
    # the action for the file that holds it.  Splitting a target's TUs across
    # processes changes nothing else: the records were always merged by
    # aggregation, which resolves dependent tokens and vetoes across processes.
    # The owned list is written once per target (the closure can be large) and
    # shared by every action.
    owned_list = ctx.actions.declare_file(ctx.label.name + ".cpp_format/owned-files.txt")
    owned = ctx.actions.args()
    owned.add_all(depset(direct = srcs, transitive = [dep_headers]))
    owned.set_param_file_format("multiline")
    ctx.actions.write(owned_list, owned)

    # Force C++ so headers (.h) parse as C++ rather than C, and carry the
    # derived compile command.  cpp_format itself drops
    # -fno-canonical-system-headers, and adds -w: the flags below are the
    # project's, and a project that builds with -Werror would otherwise have a
    # warning in a file this action only parses fail the action.
    compile_args = ctx.actions.args()
    compile_args.add("--")
    compile_args.add("-x")
    compile_args.add("c++")
    compile_args.add_all(flags)
    if res_dir:
        compile_args.add("-resource-dir=" + res_dir)

    records = []
    for src in srcs:
        rec = ctx.actions.declare_file(_record_path(ctx, src))
        args = ctx.actions.args()
        args.add("--config", ctx.file._config)
        args.add("--emit-edits", rec)
        args.add("--owned-files", owned_list)
        args.add(src)
        ctx.actions.run(
            executable = ctx.executable._cpp_format,
            arguments = [args, compile_args],
            inputs = depset(
                direct = [src, ctx.file._config, owned_list] + builtin,
                transitive = [cc_ctx.headers, cc_toolchain.all_files],
            ),
            outputs = [rec],
            mnemonic = "CppFormatEmit",
            progress_message = "cpp_format: emitting edits for " + src.short_path,
        )
        records.append(rec)

    # cpp_format.sh finds a target's records through this manifest rather than
    # by globbing the records directory: Bazel never deletes the record of a
    # source that was since removed from the target, and a stale record would
    # apply stale edits.  Paths are exec-root relative, as File.path is.
    manifest = ctx.actions.declare_file(ctx.label.name + ".cpp_format.manifest")
    ctx.actions.write(manifest, "".join([r.path + "\n" for r in records]))

    mine = depset(direct = records, transitive = transitive)
    return [
        CppFormatEditsInfo(records = mine, headers = mine_headers),
        OutputGroupInfo(cpp_format_edits = depset(direct = [manifest], transitive = [mine])),
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
        # The *root module's* ruleset, like the prebuilt kit's aspect.  A bare
        # `//:cpp_format.yaml` would resolve to cpp_formatting's own config when
        # this .bzl is loaded from a consumer repo — silently the wrong ruleset.
        # In-repo (//bazel/testdata) cpp_formatting is the root module, so the
        # two spellings are equivalent there.
        "_config": attr.label(
            default = Label("@@//:cpp_format.yaml"),
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
# aggregate_edits binary and the per-file record files at run/test time.
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

# A repository's worth of per-file records does not fit on a command line, so
# the generated scripts hand the aggregator a list file.
_RECORD_LIST_SNIPPET = """list="$(mktemp "${TEST_TMPDIR:-${TMPDIR:-/tmp}}/cpp_format_records.XXXXXX")"
printf '%s\\n' "${records[@]}" > "$list"
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
            _RECORD_LIST_SNIPPET +
            "rc=0\n" +
            '"$AGG" ' + ctx.attr.mode_flags +
            ' --root="${BUILD_WORKSPACE_DIRECTORY:-$PWD}" --records-from="$list" || rc=$?\n' +
            'rm -f "$list"\n' +
            "exit $rc\n"
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
