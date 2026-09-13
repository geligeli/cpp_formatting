"""cpp_format Bazel integration driven by the prebuilt release binary.

Vendored integration kit — see `extensions.bzl` for how the binary is fetched.

An **aspect** derives each `cc_*` target's compile flags from its
`CcInfo.compilation_context` + the C++ toolchain and runs one `cpp_format
--emit-edits` action per source file, declaring that file **and the target's
transitive headers** as action inputs.  Declaring the transitive headers is
what makes every include reachable under sandboxing — no
`compile_commands.json` needed.  The prebuilt
binary embeds and self-extracts the Clang builtin headers, so unlike the
from-source build this kit stages no resource directory of its own.

One action per source file -- like CppCompile, so Bazel parallelises, caches
and remotely executes at file granularity -- writes a structured edit-record
JSON file (offset-level edits plus a template-dependent-token resolution
sidecar).  `cpp_format --aggregate` merges the per-file records into one
repository change, resolving dependent tokens across files and targets.
`cpp_format_targets(name, deps)` generates four targets:

  * `<name>.check` — a test that fails when any edit would be applied (lint gate),
  * `<name>.diff`  — `bazel run` prints the merged unified diff (review),
  * `<name>.fix`   — `bazel run` applies the edits in $BUILD_WORKSPACE_DIRECTORY,
  * `<name>.compile_commands` — `bazel run` writes a compile_commands.json there.

Because Bazel actions cannot mutate workspace sources, `.fix` runs outside the
action graph via `bazel run`, consuming the same records the aspect produced.

The compile command the aspect derives for a target is also what an editor
wants, so the aspect writes it out as a `<name>.compile_commands.jsonl`
fragment per target (a plain `ctx.actions.write`, no tool run, nothing
compiled), and `.compile_commands` / `cpp_format.sh compile_commands` merge the
fragments into one file -- no second Bazel dependency needed for a
compilation database.
"""

load("@rules_cc//cc:action_names.bzl", "CPP_COMPILE_ACTION_NAME")
load("@rules_cc//cc:find_cc_toolchain.bzl", "find_cc_toolchain", "use_cc_toolchain")

CppFormatEditsInfo = provider(
    doc = "Transitive cpp_format state: per-source-file edit-record JSON files, and " +
          "the first-party headers the dep closure owns (see `_owned_headers`).",
    fields = {
        "records": "depset of per-source-file edit-record JSON files",
        "headers": "depset of first-party header source files in the dep closure",
        "compile_commands": "depset of per-target compile_commands fragments (see `_compile_commands_fragment`)",
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
    flags = cc_common.get_memory_inefficient_command_line(
        feature_configuration = feature_config,
        action_name = CPP_COMPILE_ACTION_NAME,
        variables = variables,
    )
    compiler = cc_common.get_tool_for_action(
        feature_configuration = feature_config,
        action_name = CPP_COMPILE_ACTION_NAME,
    )
    return compiler, flags

# The compile command the aspect derives is exactly what a compilation database
# wants, so every first-party target with sources also gets a
# `<name>.compile_commands.jsonl` fragment: one JSON object per source file,
# written by `ctx.actions.write` (no tool runs, nothing is compiled).  The
# `.compile_commands` run target and `cpp_format.sh compile_commands` merge the
# fragments into a `compile_commands.json`.  Two things are only known at run
# time and are left as placeholders for the merger to fill in:
#
#   * `directory` is the execution root, which is where every relative flag
#     (`-iquote .`, `bazel-out/.../bin`, `external/...`) resolves -- no
#     `external` or `bazel-out` symlink has to be planted in the workspace.
#   * `file` is the source's absolute *workspace* path, which is the path an
#     editor opens (clangd matches entries by that path, not by realpath).
#     Includes resolve through the exec root's source symlinks and realpath
#     back into the workspace, so navigation lands on the real files.
#
# `-fno-canonical-system-headers` is dropped: it is a gcc-only flag that clang
# rejects (the tool itself drops it the same way), and everything else is
# passed through -- an IDE gets the same view of the target as the tool does.
def _compile_commands_fragment(ctx, srcs, compiler, flags):
    args = [compiler, "-x", "c++"] + [f for f in flags if f != "-fno-canonical-system-headers"]
    lines = [
        json.encode({
            "file": "__WORKSPACE__/" + src.short_path,
            "directory": "__EXEC_ROOT__",
            "arguments": args + ["-c", src.path],
        }) + "\n"
        for src in srcs
    ]
    frag = ctx.actions.declare_file(ctx.label.name + ".compile_commands.jsonl")
    ctx.actions.write(frag, "".join(lines))
    return frag

def _aspect_impl(target, ctx):
    dep_infos = [
        d[CppFormatEditsInfo]
        for d in getattr(ctx.rule.attr, "deps", [])
        if CppFormatEditsInfo in d
    ]
    transitive = [i.records for i in dep_infos]
    dep_headers = depset(transitive = [i.headers for i in dep_infos])
    dep_cc = [i.compile_commands for i in dep_infos]

    # First-party cc_* targets only. The aspect still propagates into external
    # deps but produces nothing there.  A `no-cpp-format` target contributes no
    # owned headers either: its declarations are never renamed, so a dependent
    # must not rename their uses.
    if ctx.label.workspace_name != "" or CcInfo not in target:
        return [CppFormatEditsInfo(
            records = depset(transitive = transitive),
            headers = dep_headers,
            compile_commands = depset(transitive = dep_cc),
        )]
    srcs = _own_sources(ctx)
    if not srcs:
        return [CppFormatEditsInfo(
            records = depset(transitive = transitive),
            headers = depset(direct = _owned_headers(ctx), transitive = [dep_headers]),
            compile_commands = depset(transitive = dep_cc),
        )]

    cc_toolchain = find_cc_toolchain(ctx)
    cc_ctx = target[CcInfo].compilation_context
    compiler, flags = _compile_flags(ctx, cc_toolchain, cc_ctx)

    # The compilation-database fragment is written for every first-party target
    # with sources, `no-cpp-format` or not: an IDE wants the whole repo.  Its
    # output group also carries the target's transitive headers, so building it
    # materialises every generated header the entries include -- what an editor
    # needs to resolve them -- while still compiling nothing.
    frag = _compile_commands_fragment(ctx, srcs, compiler, flags)
    mine_cc = depset(direct = [frag], transitive = dep_cc)
    cc_group = depset(direct = [frag], transitive = dep_cc + [cc_ctx.headers])

    # A `no-cpp-format` target is not formatted and contributes no owned headers
    # either: its declarations are never renamed, so a dependent must not
    # rename their uses.
    if "no-cpp-format" in getattr(ctx.rule.attr, "tags", []):
        return [
            CppFormatEditsInfo(
                records = depset(transitive = transitive),
                headers = dep_headers,
                compile_commands = mine_cc,
            ),
            OutputGroupInfo(cpp_format_compile_commands = cc_group),
        ]
    mine_headers = depset(direct = _owned_headers(ctx), transitive = [dep_headers])
    binary = ctx.file._cpp_format

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
    # derived compile command.  cpp_format drops -fno-canonical-system-headers
    # itself, supplies its own (self-extracted) -resource-dir, and adds -w: the
    # flags below are the project's, and a project that builds with -Werror
    # would otherwise have a warning in a file this action only parses fail the
    # action.
    compile_args = ctx.actions.args()
    compile_args.add("--")
    compile_args.add("-x")
    compile_args.add("c++")
    compile_args.add_all(flags)

    records = []
    for src in srcs:
        rec = ctx.actions.declare_file(_record_path(ctx, src))
        args = ctx.actions.args()
        args.add("--config", ctx.file._config)
        args.add("--emit-edits", rec)
        args.add("--owned-files", owned_list)
        args.add(src)
        ctx.actions.run(
            executable = binary,
            tools = [binary],
            arguments = [args, compile_args],
            inputs = depset(
                direct = [src, ctx.file._config, owned_list],
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
        CppFormatEditsInfo(records = mine, headers = mine_headers, compile_commands = mine_cc),
        OutputGroupInfo(
            cpp_format_edits = depset(direct = [manifest], transitive = [mine]),
            cpp_format_compile_commands = cc_group,
        ),
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
# cpp_format binary and the per-file record files at run/test time.
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
            _RECORD_LIST_SNIPPET +
            "rc=0\n" +
            '"$BIN" --aggregate ' + ctx.attr.mode_flags +
            ' --root="${BUILD_WORKSPACE_DIRECTORY:-$PWD}" --records-from="$list" || rc=$?\n' +
            'rm -f "$list"\n' +
            "exit $rc\n"
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

# ---------------------------------------------------------------------------
# compile_commands.json
# ---------------------------------------------------------------------------

# Merges the aspect's per-target `.compile_commands.jsonl` fragments into one
# `compile_commands.json`, filling in the two run-time placeholders (see
# `_compile_commands_fragment`).  cpp_format.sh carries the same two functions
# and runs the merge outside Bazel over the fragments at their deterministic
# bazel-bin paths.  Args: <exec root> <workspace> <output> <fragment>...
# A file listed by several targets keeps the first entry seen.
_MERGE_COMPILE_COMMANDS_SNIPPET = """
json_escape() { local s="$1" bs='\\'; s="${s//"$bs"/"$bs$bs"}"; s="${s//\\"/$bs\\"}"; printf '%s' "$s"; }
merge_compile_commands() {
  local exec_root="$1" workspace="$2" out="$3"; shift 3
  local dir_json ws_json frag line key first=1
  dir_json="$(json_escape "$exec_root")"
  ws_json="$(json_escape "$workspace")"
  declare -A seen=()
  {
    printf '[\\n'
    for frag in "$@"; do
      while IFS= read -r line; do
        [[ -n "$line" ]] || continue
        key="${line#*\\"file\\":\\"}"; key="${key%%\\"*}"
        [[ -z "${seen[$key]:-}" ]] || continue
        seen[$key]=1
        line="${line//\\"__EXEC_ROOT__\\"/"\\"$dir_json\\""}"
        line="${line//\\"__WORKSPACE__\\//"\\"$ws_json/"}"
        [[ $first -eq 1 ]] || printf ',\\n'
        first=0
        printf '  %s' "$line"
      done < "$frag"
    done
    printf '\\n]\\n'
  } > "$out.tmp"
  mv -f "$out.tmp" "$out"
}
"""

def _compile_commands_of(ctx):
    return depset(transitive = [
        d[CppFormatEditsInfo].compile_commands
        for d in ctx.attr.deps
        if CppFormatEditsInfo in d
    ]).to_list()

def _compile_commands_impl(ctx):
    frags = _compile_commands_of(ctx)
    frag_lines = "".join([
        'frags+=("$(rlocation "' + _rlocation_path(f) + '")")\n'
        for f in frags
    ])
    script = ctx.actions.declare_file(ctx.label.name + ".sh")
    ctx.actions.write(
        output = script,
        is_executable = True,
        content = (
            _RUNFILES_PREAMBLE + "frags=()\n" + frag_lines +
            _MERGE_COMPILE_COMMANDS_SNIPPET +
            # Under `bazel run` the runfiles tree sits inside bazel-bin, which
            # is inside the execution root -- the directory every relative
            # flag in the entries resolves against.
            'ws="${BUILD_WORKSPACE_DIRECTORY:?run this with \'bazel run\'}"\n' +
            'rf="${RUNFILES_DIR:-$PWD}"\n' +
            'exec_root="${rf%/bazel-out/*}"\n' +
            'out="${1:-' + ctx.attr.out + '}"\n' +
            '[[ "$out" = /* ]] || out="${BUILD_WORKING_DIRECTORY:-$ws}/$out"\n' +
            'merge_compile_commands "$exec_root" "$ws" "$out" "${frags[@]}"\n' +
            'echo "cpp_format: wrote $out (${#frags[@]} targets)"\n'
        ),
    )

    # The transitive headers ride along as default outputs (not runfiles, which
    # would symlink every one of them): `bazel run` builds them, so the
    # generated headers the entries name exist once the file is written.
    headers = [d[CcInfo].compilation_context.headers for d in ctx.attr.deps if CcInfo in d]
    runfiles = ctx.runfiles(files = frags)
    runfiles = runfiles.merge(ctx.attr._bash_runfiles[DefaultInfo].default_runfiles)
    return [DefaultInfo(
        executable = script,
        files = depset(direct = [script], transitive = headers),
        runfiles = runfiles,
    )]

cpp_format_compile_commands = rule(
    doc = "bazel run this to write a compile_commands.json for `deps` (transitively) " +
          "into $BUILD_WORKSPACE_DIRECTORY (default compile_commands.json; override " +
          "with a positional arg).  Nothing is compiled and cpp_format is not run: the " +
          "entries are the compile commands the cpp_format aspect derives for each target.",
    implementation = _compile_commands_impl,
    executable = True,
    attrs = {
        "deps": attr.label_list(
            aspects = [cpp_format_aspect],
            providers = [CcInfo],
            doc = "cc_* targets to cover (transitively).",
        ),
        "out": attr.string(
            default = "compile_commands.json",
            doc = "Default workspace-relative output path.",
        ),
        "_bash_runfiles": attr.label(default = Label("@bazel_tools//tools/bash/runfiles")),
    },
)

def cpp_format_targets(name, deps, **kwargs):
    """Defines <name>.check (test), <name>.diff, <name>.fix and
    <name>.compile_commands (bazel run)."""
    _cpp_format_test(name = name + ".check", deps = deps, mode_flags = "--check", **kwargs)
    _cpp_format_run(name = name + ".diff", deps = deps, mode_flags = "", **kwargs)
    _cpp_format_run(name = name + ".fix", deps = deps, mode_flags = "--apply", **kwargs)
    cpp_format_compile_commands(name = name + ".compile_commands", deps = deps, **kwargs)

# ---------------------------------------------------------------------------
# `bazel run //…:install` — drop cpp_format.sh into the consumer's workspace
# ---------------------------------------------------------------------------

# The canonical `<repo>//bazel/integration:cpp_format.bzl%cpp_format_aspect`
# spec, resolved in the consumer's module graph (so it is valid from their
# command line whether the kit was imported by URL or vendored).
_ASPECT_SPEC = str(Label(":cpp_format.bzl")) + "%cpp_format_aspect"

# The default ASPECT literal in cpp_format.sh that install bakes over.
_ASPECT_PLACEHOLDER = "//third_party/cpp_format:cpp_format.bzl%cpp_format_aspect"

def _install_impl(ctx):
    # Bake the imported aspect label into the placed script, so it runs with no
    # CPP_FORMAT_ASPECT needed.  Env still overrides (the `:-` default remains).
    placed = ctx.actions.declare_file(ctx.label.name + ".cpp_format.sh")
    ctx.actions.expand_template(
        template = ctx.file._script,
        output = placed,
        substitutions = {_ASPECT_PLACEHOLDER: _ASPECT_SPEC},
    )
    launcher = ctx.actions.declare_file(ctx.label.name + ".launch.sh")
    ctx.actions.write(
        output = launcher,
        is_executable = True,
        content = (
            _RUNFILES_PREAMBLE +
            'dest="${1:-' + ctx.attr.dest + '}"\n' +
            'ws="${BUILD_WORKSPACE_DIRECTORY:?run this with \'bazel run\'}"\n' +
            'src="$(rlocation "' + _rlocation_path(placed) + '")"\n' +
            'mkdir -p "$(dirname "$ws/$dest")"\n' +
            'cp -f "$src" "$ws/$dest"\n' +
            'chmod 755 "$ws/$dest"\n' +
            'echo "cpp_format: installed wrapper -> $dest"\n' +
            'echo "run it with: $dest check | diff | fix [pattern]"\n'
        ),
    )
    runfiles = ctx.runfiles(files = [placed])
    runfiles = runfiles.merge(ctx.attr._bash_runfiles[DefaultInfo].default_runfiles)
    return [DefaultInfo(executable = launcher, runfiles = runfiles)]

cpp_format_install = rule(
    doc = "bazel run this to copy cpp_format.sh into $BUILD_WORKSPACE_DIRECTORY " +
          "(default tools/cpp_format.sh; override with a positional arg).",
    implementation = _install_impl,
    executable = True,
    attrs = {
        "dest": attr.string(
            default = "tools/cpp_format.sh",
            doc = "Default workspace-relative install path.",
        ),
        "_script": attr.label(
            default = Label(":cpp_format.sh"),
            allow_single_file = True,
        ),
        "_bash_runfiles": attr.label(default = Label("@bazel_tools//tools/bash/runfiles")),
    },
)
