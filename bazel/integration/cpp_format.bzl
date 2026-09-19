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

A second aspect, `cpp_index_aspect`, builds the symbol index the same way: one
`cpp_format --emit-index` action per translation unit writes its
`cpp_index.IndexUnit` (see cpp_formatting/index.proto in the cpp_formatting
repository) -- the TU's own file plus every owned header it includes -- and `cpp_index_targets(name, deps)` defines `<name>.index`, whose
action merges the transitive units into one `Index` with
`cpp_format --merge-index`.  Unlike `.fix`, merging mutates nothing, so it is
an ordinary cached build action and `bazel build` produces the index file.
`cpp_format.sh index` does the same for a target pattern and writes `index.pb`
into the workspace.
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
_TU_EXTS = ["cc", "cpp", "cxx", "c++"]
_SRC_EXTS = _TU_EXTS + _HDR_EXTS

def _own_files(ctx, exts, attrs = ("srcs", "hdrs")):
    out = []
    seen = {}
    for attr in attrs:
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

# The files that are actually translation units.  A header is not one: C++ has
# no way to compile a header, only to include it, and parsing one standalone
# assumes it is self-contained in *this* target's compilation context -- which
# a build does not guarantee and Bazel does not check without layering_check.
# protobuf is where that assumption breaks: arena_cleanup.h ends with
# `#include "google/protobuf/port_def.inc"` while its target depends only on
# abseil, because every TU that includes it has already pulled :port in; and
# protobuf_headers globs every header with no deps at all.  So headers are
# parsed where they are included, by the actions of the targets that include
# them, and are reachable for rewriting through --owned-files.
def _own_translation_units(ctx):
    return _own_files(ctx, _TU_EXTS)

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

# The attributes a cc_* target's C++ dependencies arrive through.  Both are
# followed: `implementation_deps` are compiled against exactly like `deps` --
# their headers are included by this target's sources, so their declarations
# are renamed at use sites here and their records belong to the same change --
# they are just not re-exported to the target's own dependents.
_DEP_ATTRS = ["deps", "implementation_deps"]

def _cc_deps(ctx):
    return [d for attr in _DEP_ATTRS for d in getattr(ctx.rule.attr, attr, [])]

# The compilation context the target's own sources are compiled with.  A
# target's CcInfo is what it exports, which by design leaves out its
# `implementation_deps`; their include paths, defines and headers have to be
# merged back in, or a source that includes one of their headers does not parse
# ('sqlite3.h' file not found) and the target's compile_commands entry is wrong
# in the same way.
def _compilation_context(target, ctx):
    contexts = [target[CcInfo].compilation_context] + [
        d[CcInfo].compilation_context
        for d in getattr(ctx.rule.attr, "implementation_deps", [])
        if CcInfo in d
    ]
    if len(contexts) == 1:
        return contexts[0]
    return cc_common.merge_compilation_contexts(compilation_contexts = contexts)

# The target's own `copts`, so the tool parses each file under the same
# preprocessor conditions the compiler does.  abseil's randen_hwaes.cc is the
# case: its body sits behind `#if ABSL_HAVE_ACCELERATED_AES`, which only
# `-maes -msse4.1` from the target's copts turns on, so without them the tool
# never saw the references it holds and the build broke where the tool had
# looked at nothing.  A flag that still contains a make variable or a
# `$(location)` is dropped rather than mis-expanded; `cc_ctx.defines` and
# `local_defines` already arrive through the compilation context.
def _rule_copts(ctx):
    return [c for c in getattr(ctx.rule.attr, "copts", []) if "$(" not in c and "$" + "{" not in c]

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
        user_compile_flags = ctx.fragments.cpp.copts + ctx.fragments.cpp.cxxopts + _rule_copts(ctx),
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
        for d in _cc_deps(ctx)
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
    cc_ctx = _compilation_context(target, ctx)
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

    # A target with no translation unit of its own -- a header-only library --
    # emits no records.  Its headers are still propagated as owned, which is
    # what makes their declarations renameable, and their uses rewritten, in
    # every TU that includes them.  The one thing lost is a header that *no*
    # TU in the formatted set includes: nothing parses it, so nothing renames
    # it -- and nothing renames its members anywhere either, so the result is
    # less reach, never a half-applied rename.
    tus = _own_translation_units(ctx)
    if not tus:
        # The manifest is still written, empty.  Bazel never deletes the output
        # of an action that is no longer registered, so a target that used to
        # have records -- every header-only library did, before a header stopped
        # being a translation unit -- would keep its stale manifest in bazel-bin,
        # and cpp_format.sh would read it and fail on a record file nothing
        # produces any more.  Writing an empty one overwrites it.
        manifest = ctx.actions.declare_file(ctx.label.name + ".cpp_format.manifest")
        ctx.actions.write(manifest, "")
        return [
            CppFormatEditsInfo(
                records = depset(transitive = transitive),
                headers = mine_headers,
                compile_commands = mine_cc,
            ),
            OutputGroupInfo(
                cpp_format_edits = depset(direct = [manifest], transitive = transitive),
                cpp_format_compile_commands = cc_group,
            ),
        ]

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
    for src in tus:
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
    attr_aspects = _DEP_ATTRS,
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
            ' --root="${BUILD_WORKSPACE_DIRECTORY:-$PWD}" --records-from="$list" "$@" || rc=$?\n' +
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
# Symbol index
# ---------------------------------------------------------------------------

CppIndexInfo = provider(
    doc = "Transitive symbol-index state: per-translation-unit index units, and " +
          "the first-party headers the dep closure owns (indexed by the TUs that " +
          "include them).",
    fields = {
        "units": "depset of per-translation-unit cpp_index.IndexUnit files (binary protobuf)",
        "headers": "depset of first-party header source files in the dep closure",
    },
)

def _index_unit_path(ctx, src):
    return ctx.label.name + ".cpp_index/" + src.short_path + ".pb"

# The target's own files whose occurrences its index actions record: srcs,
# hdrs *and* textual_hdrs -- plus, added by the caller, the dep closure's
# headers, exactly as for the formatter.  A header is not a translation unit
# (see _own_translation_units), so it is indexed by every TU that includes it,
# its own target's and its dependents'; the merge dedups what they agree on.
# Textual headers are the one addition over the formatter's set: nothing is
# rewritten, and an .inc is only ever parsed through its includer -- this is
# the one place the tool can see what is in it.
def _index_owned(ctx):
    out = _own_sources(ctx)
    seen = {f.path: True for f in out}
    for t in getattr(ctx.rule.attr, "textual_hdrs", []):
        for f in t.files.to_list():
            if f.is_source and f.path not in seen and not f.short_path.startswith("../"):
                seen[f.path] = True
                out.append(f)
    return out

def _index_aspect_impl(target, ctx):
    dep_infos = [
        d[CppIndexInfo]
        for d in _cc_deps(ctx)
        if CppIndexInfo in d
    ]
    transitive = depset(transitive = [i.units for i in dep_infos])
    dep_headers = depset(transitive = [i.headers for i in dep_infos])

    # First-party cc_* targets only, as for the formatter.  A `no-cpp-index`
    # target is skipped and contributes no headers; `no-cpp-format` does not
    # apply -- an index wants the whole repository, formatted or not.
    if ctx.label.workspace_name != "" or CcInfo not in target:
        return [CppIndexInfo(units = transitive, headers = dep_headers)]
    if "no-cpp-index" in getattr(ctx.rule.attr, "tags", []):
        return [CppIndexInfo(units = transitive, headers = dep_headers)]
    mine_headers = depset(direct = _owned_headers(ctx), transitive = [dep_headers])

    # A header-only target has no translation unit and emits nothing; its
    # headers are indexed by the TUs that include them (see
    # _own_translation_units), which is what the propagated set is for.
    tus = _own_translation_units(ctx)
    if not tus:
        return [CppIndexInfo(units = transitive, headers = mine_headers)]

    cc_toolchain = find_cc_toolchain(ctx)
    cc_ctx = _compilation_context(target, ctx)
    _compiler, flags = _compile_flags(ctx, cc_toolchain, cc_ctx)
    binary = ctx.file._cpp_format

    owned_list = ctx.actions.declare_file(ctx.label.name + ".cpp_index/owned-files.txt")
    owned = ctx.actions.args()
    owned.add_all(depset(direct = _index_owned(ctx), transitive = [dep_headers]))
    owned.set_param_file_format("multiline")
    ctx.actions.write(owned_list, owned)

    # As for the emit-edits actions: C++ forced, the derived flags carried, and
    # the binary supplying its own self-extracted -resource-dir.
    compile_args = ctx.actions.args()
    compile_args.add("--")
    compile_args.add("-x")
    compile_args.add("c++")
    compile_args.add_all(flags)

    # One action per translation unit, exactly like the emit-edits actions:
    # parsed once, cached per file, and every occurrence in this file and in
    # every owned header it includes recorded against paths relative to the
    # exec root (so the unit is usable from a remote cache on another machine).
    units = []
    for src in tus:
        unit = ctx.actions.declare_file(_index_unit_path(ctx, src))
        args = ctx.actions.args()
        args.add("--emit-index", unit)
        args.add("--owned-files", owned_list)
        args.add(src)
        ctx.actions.run(
            executable = binary,
            tools = [binary],
            arguments = [args, compile_args],
            inputs = depset(
                direct = [src, owned_list],
                transitive = [cc_ctx.headers, cc_toolchain.all_files],
            ),
            outputs = [unit],
            mnemonic = "CppIndexEmit",
            progress_message = "cpp_format: indexing " + src.short_path,
        )
        units.append(unit)

    # Read by cpp_format.sh instead of globbing the units directory, for the
    # same reason as the edit-record manifest: a removed source's unit is never
    # deleted by Bazel and would keep its stale occurrences in the index.
    manifest = ctx.actions.declare_file(ctx.label.name + ".cpp_index.manifest")
    ctx.actions.write(manifest, "".join([u.path + "\n" for u in units]))

    mine = depset(direct = units, transitive = [transitive])
    return [
        CppIndexInfo(units = mine, headers = mine_headers),
        OutputGroupInfo(cpp_index = depset(direct = [manifest], transitive = [mine])),
    ]

cpp_index_aspect = aspect(
    implementation = _index_aspect_impl,
    attr_aspects = _DEP_ATTRS,
    fragments = ["cpp"],
    toolchains = use_cc_toolchain(),
    attrs = {
        "_cpp_format": attr.label(
            default = Label("@cpp_format_bin//:cpp_format"),
            allow_single_file = True,
        ),
    },
)

def _index_impl(ctx):
    units = depset(transitive = [
        d[CppIndexInfo].units
        for d in ctx.attr.deps
        if CppIndexInfo in d
    ])
    binary = ctx.file._cpp_format
    out = ctx.actions.declare_file(ctx.label.name + ".pb")

    # The fixed flags and the unit list are separate Args objects: the list
    # goes through a param file (a repository's worth of units does not fit on
    # a command line), and `use_param_file` would sweep `--merge-index` into
    # that file too, where the binary's sub-command dispatch cannot see it.
    fixed = ctx.actions.args()
    fixed.add("--merge-index")
    fixed.add("--output", out)
    listed = ctx.actions.args()
    listed.add_all(units)
    listed.use_param_file("--records-from=%s", use_always = True)
    listed.set_param_file_format("multiline")
    ctx.actions.run(
        executable = binary,
        tools = [binary],
        arguments = [fixed, listed],
        inputs = units,
        outputs = [out],
        mnemonic = "CppIndexMerge",
        progress_message = "cpp_format: merging index " + ctx.label.name,
    )
    return [DefaultInfo(files = depset([out]))]

_cpp_index = rule(
    doc = "Builds <name>.pb, the merged cpp_index.Index of `deps` (transitively). " +
          "An ordinary build action: `bazel build` it, and read it with " +
          "`cpp_format --dump-index`.",
    implementation = _index_impl,
    attrs = {
        "deps": attr.label_list(
            aspects = [cpp_index_aspect],
            providers = [CcInfo],
            doc = "cc_* targets to index (transitively).",
        ),
        "_cpp_format": attr.label(
            default = Label("@cpp_format_bin//:cpp_format"),
            allow_single_file = True,
        ),
    },
)

def cpp_index_targets(name, deps, **kwargs):
    """Defines <name>.index, a build target whose output is the merged index
    (<name>.index.pb) of `deps` and everything they depend on."""
    _cpp_index(name = name + ".index", deps = deps, **kwargs)

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
