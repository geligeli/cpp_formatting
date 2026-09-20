"""Runs a binary built for another platform as a test of this one.

`bazel test //...` builds for the default platform, where glibc is linked
dynamically; the static link only happens under
`--platforms=//tools/platforms:linux_<cpu>_glibc_static`.  The transition makes
one target build that way regardless, so the static glibc of the toolchain fork
(see MODULE.bazel) has a test in the ordinary suite.
"""

# A sanitizer runtime cannot be linked into a static executable (ASan and TSan
# refuse outright), so under `--config=asan` and friends the binary is built
# without: the transition turns the toolchain's switches off for it, the way the
# toolchain does for its own startup files.  What is under test here is the
# link, not the program.
_SANITIZER_FLAGS = [
    "@llvm//config:asan",
    "@llvm//config:tsan",
    "@llvm//config:ubsan",
]

def _platform_transition_impl(_settings, attr):
    settings = {flag: False for flag in _SANITIZER_FLAGS}
    settings["//command_line_option:platforms"] = str(attr.platform)
    return settings

_platform_transition = transition(
    implementation = _platform_transition_impl,
    inputs = [],
    outputs = ["//command_line_option:platforms"] + _SANITIZER_FLAGS,
)

def _for_platform_test_impl(ctx):
    binary = ctx.attr.binary[0][DefaultInfo].files_to_run.executable
    out = ctx.actions.declare_file(ctx.label.name)
    ctx.actions.symlink(output = out, target_file = binary, is_executable = True)
    return [DefaultInfo(executable = out)]

for_platform_test = rule(
    implementation = _for_platform_test_impl,
    attrs = {
        "binary": attr.label(
            cfg = _platform_transition,
            executable = True,
            mandatory = True,
        ),
        "platform": attr.label(mandatory = True),
    },
    test = True,
)
