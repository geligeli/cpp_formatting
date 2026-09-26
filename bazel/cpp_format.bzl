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
repository change, resolving dependent tokens across files and targets.

Nothing here applies the aspect for you, because what to apply it to is a
target *pattern* (`//...`), and a pattern is a command-line notion: a rule's
`deps` can only name labels.  `bazel/integration/cpp_format.sh` is the driver --
it queries the cc_* targets under a pattern, builds the aspect's output group
over them and merges the result outside Bazel, which is also the only place a
fix can happen (an action cannot mutate workspace sources): `check`, `diff`,
`fix`, `compile_commands`, `index`, `browse` and `coverage`.  `--config=lint` and
`--config=index` in .bazelrc build the per-file outputs alone.

The compile command the aspect derives for a target is also what an editor
wants, so the aspect writes it out as a `<name>.compile_commands.jsonl`
fragment per target (a plain `ctx.actions.write`, no tool run, nothing
compiled), and `cpp_format.sh compile_commands` merges the fragments into one
file -- no second Bazel dependency needed for a compilation database.

A second aspect, `cpp_index_aspect`, builds the symbol index the same way: one
`cpp_format --emit-index` action per translation unit writes its
`cpp_index.IndexUnit` (see cpp_formatting/index.proto) -- the TU's own file
plus every owned header it includes.  `cpp_format.sh index` merges the units of
a target pattern into one `Index` with `cpp_format --merge-index` and writes
`index.pb` into the workspace; `cpp_format.sh browse` then serves the workspace
in //code_browser over it, and `cpp_format.sh coverage` with the tests' line
coverage overlaid.

The providers are public so that a rule of your own can consume the aspects'
outputs; bazel/testdata/aspect_outputs.bzl does, for this repository's tests.
"""

load("@rules_cc//cc:action_names.bzl", "CPP_COMPILE_ACTION_NAME")
load("@rules_cc//cc:find_cc_toolchain.bzl", "find_cc_toolchain", "use_cc_toolchain")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

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

def _resource_dir(builtin_headers):
    # The staged Clang builtin headers live under ".../staging/include"; the
    # resource dir is the "staging" parent.  Derived from the actual file paths
    # so it survives a module-extension rename (unlike a hardcoded path).
    for f in builtin_headers:
        idx = f.path.find("/staging/")
        if idx != -1:
            return f.path[:idx] + "/staging"
    return None

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
# written by `ctx.actions.write` (no tool runs, nothing is compiled).
# `cpp_format.sh compile_commands` merges the fragments into a
# `compile_commands.json`.  Two things are only known at run
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
    # deps (e.g. @llvm-project) but produces nothing there.  A `no-cpp-format`
    # target contributes no owned headers either: its declarations are never
    # renamed, so a dependent must not rename their uses.
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
    for src in tus:
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

# Test code as Bazel knows it: a *_test rule, or anything only tests may depend
# on (`testonly`, which every test rule has implicitly).  Each file such a
# target owns gets a `testonly` attribute in the index, valued with the rule's
# kind -- from one unit in text form (the merge reads `.txtpb`), written here
# with no tool: the attribute joins the file's entry from whichever units
# indexed it, so a header-only testonly library is covered like any other.
def _testonly_units(ctx):
    if not getattr(ctx.rule.attr, "testonly", False):
        return []
    files = _index_owned(ctx)
    if not files:
        return []
    unit = ctx.actions.declare_file(ctx.label.name + ".cpp_index/testonly.txtpb")
    kind = _text_string(ctx.rule.kind)
    ctx.actions.write(unit, "".join([
        "files { path: %s kind: SOURCE attributes { key: \"testonly\" value: %s } }\n" %
        (_text_string(f.path), kind)
        for f in files
    ]))
    return [unit]

# A string literal of the protobuf text format.
def _text_string(s):
    return "\"" + s.replace("\\", "\\\\").replace("\"", "\\\"") + "\""

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

    # A header-only target has no translation unit and emits nothing (unless it
    # is testonly); its headers are indexed by the TUs that include them (see
    # _own_translation_units), which is what the propagated set is for.
    tus = _own_translation_units(ctx)
    testonly = _testonly_units(ctx)
    if not tus and not testonly:
        return [CppIndexInfo(units = transitive, headers = mine_headers)]
    if not tus:
        return _index_outputs(ctx, testonly, transitive, mine_headers)

    cc_toolchain = find_cc_toolchain(ctx)
    cc_ctx = _compilation_context(target, ctx)
    _compiler, flags = _compile_flags(ctx, cc_toolchain, cc_ctx)
    builtin = ctx.files._builtin_headers
    res_dir = _resource_dir(builtin)

    owned_list = ctx.actions.declare_file(ctx.label.name + ".cpp_index/owned-files.txt")
    owned = ctx.actions.args()
    owned.add_all(depset(direct = _index_owned(ctx), transitive = [dep_headers]))
    owned.set_param_file_format("multiline")
    ctx.actions.write(owned_list, owned)

    compile_args = ctx.actions.args()
    compile_args.add("--")
    compile_args.add("-x")
    compile_args.add("c++")
    compile_args.add_all(flags)
    if res_dir:
        compile_args.add("-resource-dir=" + res_dir)

    # One action per translation unit, exactly like the emit-edits actions:
    # parsed once, cached per file, and every occurrence in this file and in
    # every owned header it includes recorded against paths relative to the
    # exec root (so the unit is usable from a remote cache on another machine).
    units = list(testonly)
    for src in tus:
        unit = ctx.actions.declare_file(_index_unit_path(ctx, src))
        args = ctx.actions.args()
        args.add("--emit-index", unit)
        args.add("--owned-files", owned_list)
        args.add(src)
        ctx.actions.run(
            executable = ctx.executable._cpp_format,
            arguments = [args, compile_args],
            inputs = depset(
                direct = [src, owned_list] + builtin,
                transitive = [cc_ctx.headers, cc_toolchain.all_files],
            ),
            outputs = [unit],
            mnemonic = "CppIndexEmit",
            progress_message = "cpp_format: indexing " + src.short_path,
        )
        units.append(unit)
    return _index_outputs(ctx, units, transitive, mine_headers)

def _index_outputs(ctx, units, transitive, mine_headers):
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
            default = Label("//cpp_formatting:cpp_format"),
            executable = True,
            cfg = "exec",
        ),
        "_builtin_headers": attr.label(
            default = Label("@llvm-project//clang:builtin_headers_gen"),
        ),
    },
)
