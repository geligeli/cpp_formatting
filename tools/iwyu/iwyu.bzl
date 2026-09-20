"""include-what-you-use over the build graph.

Developer tooling for *this* repository (not part of the cpp_format
integration in //bazel): an aspect that runs IWYU (https://include-what-you-use.org/)
on every first-party C++ file with the compile command Bazel itself derives
for it, and writes IWYU's report next to the target.  `tools/iwyu/iwyu.sh`
drives it -- it builds the reports, then prints them (`check`) or feeds them to
IWYU's `fix_includes.py` (`fix`).

The shape follows `cpp_format_aspect`, for the same reasons:

  * One action per file, with `compilation_context.headers` and the toolchain
    as inputs, exactly like `CppCompile` -- so the reports cache per file and a
    generated header (`index.pb.h`) is there when IWYU parses its includer.
  * IWYU is built from source against the repo's own Clang (`@iwyu`, pinned
    next to LLVM in //third_party/llvm:extensions.bzl) in the exec
    configuration, and is handed `-resource-dir` for the staged builtin
    headers: nothing on the host is used but the C++ standard library the
    toolchain already points at.
  * A per-target manifest names the target's current reports, because Bazel
    never deletes the report of a file that has left a target.

What is analysed: every translation unit a target lists, and every header of
the target that no translation unit of the same stem accompanies.  IWYU reports
on a main file *and its associated header* (`foo.cpp` -> `foo.h`, and
`foo_test.cpp` -> `foo.h` too), so `foo.h` needs no action of its own; a
header-only library's header has no such partner and is parsed as a main file.

A report is never a build failure -- IWYU's suggestions need review, and the
gate is `iwyu.sh check`'s exit code.  A file IWYU cannot *parse* does fail its
action, with the diagnostics.

Tag a target `no-iwyu` to skip it.
"""

load("@rules_cc//cc:action_names.bzl", "CPP_COMPILE_ACTION_NAME")
load("@rules_cc//cc:find_cc_toolchain.bzl", "find_cc_toolchain", "use_cc_toolchain")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

# Forward declarations are not suggested in place of includes: the code base is
# Google style, which includes the header.  The line length only affects the
# `// for X` comments in the report, which fix_includes is told to drop.
_IWYU_OPTIONS = [
    "--no_fwd_decls",
    "--cxx17ns",
    "--max_line_length=200",
]

_HDR_EXTS = ["h", "hh", "hpp", "hxx", "h++"]
_TU_EXTS = ["cc", "cpp", "cxx", "c++"]

def _own_files(ctx):
    out = []
    seen = {}
    for attr in ("srcs", "hdrs"):
        for t in getattr(ctx.rule.attr, attr, []):
            for f in t.files.to_list():
                # Source files of this repository only: a generated source
                # (the embedded byte arrays) is not ours to tidy.
                if (f.is_source and f.path not in seen and
                    not f.short_path.startswith("../") and
                    f.extension in _TU_EXTS + _HDR_EXTS):
                    seen[f.path] = True
                    out.append(f)
    return out

def _stem(f):
    return f.basename[:-(len(f.extension) + 1)]

def _main_files(ctx):
    files = _own_files(ctx)
    tus = [f for f in files if f.extension in _TU_EXTS]
    stems = {_stem(f): True for f in tus}
    orphans = [f for f in files if f.extension in _HDR_EXTS and _stem(f) not in stems]
    return tus + orphans

# `implementation_deps` are compiled against but deliberately not part of the
# target's own CcInfo (that is the point of them), so their include paths and
# headers have to be merged back in to reproduce the target's compile command.
def _compilation_context(target, ctx):
    contexts = [target[CcInfo].compilation_context]
    for dep in getattr(ctx.rule.attr, "implementation_deps", []):
        if CcInfo in dep:
            contexts.append(dep[CcInfo].compilation_context)
    if len(contexts) == 1:
        return contexts[0]
    return cc_common.merge_compilation_contexts(compilation_contexts = contexts)

# External repositories whose headers the code base includes with quotes.
_QUOTED_INCLUDE_REPOS = ["llvm-project", "protobuf"]

# The hermetic toolchain (the `llvm` module) has repositories of its own, and
# one of them is *also* called llvm-project: it holds the libc++ and libc++abi
# every target compiles against, at external/llvm++llvm+llvm-project/libcxx/...
# That one must stay behind -isystem.  Demoted to -I, the standard library is
# spelled "string" instead of <string>, and IWYU no longer recognises it as the
# C++ library at all -- its libc++ mappings key on the <> spelling -- so it
# recommends the implementation headers ("__fwd/string.h", "__utility/move.h";
# 106 files flagged).  A canonical repository name starts with its module's.
_TOOLCHAIN_MODULE_PREFIX = "llvm+"

def _is_quoted_include_dir(path):
    components = path.split("/")
    for c in components:
        if c.startswith(_TOOLCHAIN_MODULE_PREFIX):
            return False
    for repo in _QUOTED_INCLUDE_REPOS:
        if ("external/" + repo) in path or ("+" + repo + "/") in path:
            return True
    return False

def _compile_flags(ctx, cc_toolchain, cc_ctx):
    feature_config = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
        requested_features = ctx.features,
        unsupported_features = ctx.disabled_features,
    )

    # The target's own copts matter here: //code_browser is -std=c++20.
    copts = [c for c in getattr(ctx.rule.attr, "copts", []) if "$(" not in c]
    variables = cc_common.create_compile_variables(
        feature_configuration = feature_config,
        cc_toolchain = cc_toolchain,
        user_compile_flags = ctx.fragments.cpp.copts + ctx.fragments.cpp.cxxopts + copts,
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

    out = []
    for i, f in enumerate(flags):
        # gcc-only; Clang (and so IWYU) rejects it.
        if f == "-fno-canonical-system-headers":
            continue

        # IWYU spells a suggested include by how the compile command reaches
        # the header: <> behind -isystem, "" behind -I.  Bazel puts every
        # `includes = [...]` of an external repository behind -isystem, but the
        # code base writes LLVM, Clang and protobuf headers with quotes, as
        # those projects do.  This is not cosmetic: IWYU decides whether the
        # associated header already provides an include by comparing
        # *spellings*, so with the wrong one every .cpp is told to repeat what
        # its own .h includes.
        if f == "-isystem" and i + 1 < len(flags) and _is_quoted_include_dir(flags[i + 1]):
            f = "-I"
        out.append(f)
    return out

def _resource_dir(builtin_headers):
    for f in builtin_headers:
        idx = f.path.find("/staging/")
        if idx != -1:
            return f.path[:idx] + "/staging"
    return None

# IWYU writes its report to stderr and exits 0 whether or not it has
# suggestions; anything else is a parse failure.  $1 = iwyu, $2 = report.
_RUN = """
iwyu="$1"; report="$2"; shift 2
"$iwyu" "$@" 2> "$report" && exit 0
rc=$?
cat "$report" >&2
exit $rc
"""

def _iwyu_aspect_impl(target, ctx):
    if ctx.label.workspace_name != "" or CcInfo not in target:
        return []
    if "no-iwyu" in getattr(ctx.rule.attr, "tags", []):
        return []
    mains = _main_files(ctx)
    if not mains:
        return []

    cc_toolchain = find_cc_toolchain(ctx)
    cc_ctx = _compilation_context(target, ctx)
    flags = _compile_flags(ctx, cc_toolchain, cc_ctx)
    builtin = ctx.files._builtin_headers
    res_dir = _resource_dir(builtin)

    common = ctx.actions.args()
    common.add_all(["-x", "c++"])
    common.add_all(flags)

    # The project's warnings are not IWYU's business, and with -Werror one of
    # them would fail the parse.
    common.add("-w")
    if res_dir:
        common.add("-resource-dir=" + res_dir)
    for m in ctx.files._mappings:
        common.add_all(["-Xiwyu", "--mapping_file=" + m.path])
    for opt in _IWYU_OPTIONS:
        common.add_all(["-Xiwyu", opt])

    reports = []
    for src in mains:
        report = ctx.actions.declare_file(ctx.label.name + ".iwyu/" + src.short_path + ".txt")
        args = ctx.actions.args()
        args.add(ctx.executable._iwyu)
        args.add(report)
        ctx.actions.run_shell(
            command = _RUN,
            arguments = [args, common, src.path],
            tools = [ctx.executable._iwyu],
            inputs = depset(
                direct = [src] + builtin + ctx.files._mappings,
                transitive = [cc_ctx.headers, cc_toolchain.all_files],
            ),
            outputs = [report],
            mnemonic = "Iwyu",
            progress_message = "include-what-you-use: " + src.short_path,
        )
        reports.append(report)

    manifest = ctx.actions.declare_file(ctx.label.name + ".iwyu.manifest")
    ctx.actions.write(manifest, "".join([r.path + "\n" for r in reports]))
    return [OutputGroupInfo(iwyu = depset(direct = [manifest] + reports))]

iwyu_aspect = aspect(
    implementation = _iwyu_aspect_impl,
    # Not propagated: iwyu.sh queries the targets it wants and names each one.
    attr_aspects = [],
    attrs = {
        "_iwyu": attr.label(
            default = Label("@iwyu//:include-what-you-use"),
            executable = True,
            cfg = "exec",
        ),
        "_builtin_headers": attr.label(
            default = Label("@llvm-project//clang:builtin_headers_gen"),
        ),
        "_mappings": attr.label_list(
            default = [
                Label("//tools/iwyu:mappings.imp"),
                # libc++'s private headers, generated from its module map.
                Label("//tools/iwyu:libcxx.imp"),
            ],
            allow_files = [".imp"],
        ),
    },
    fragments = ["cpp"],
    toolchains = use_cc_toolchain(),
)
