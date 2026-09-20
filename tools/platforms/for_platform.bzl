"""Runs a binary built for another platform as a test of this one.

`bazel test //...` builds for the default platform, where glibc is linked
dynamically; the static link only happens under
`--platforms=//tools/platforms:linux_<cpu>_glibc_static`.  The transition makes
one target build that way regardless, so the static glibc of the toolchain fork
(see MODULE.bazel) has a test in the ordinary suite.
"""

# Nothing here may name `@llvm`: this file is loaded whenever the package is,
# including by `bazel query @cpp_formatting//...` in a repository that merely
# depends on this one -- where `@llvm`, a dev dependency, does not exist.  A
# label in a BUILD file is resolved lazily and is harmless there; a transition's
# `outputs` are resolved when the .bzl is loaded and are not.  (That is why the
# sanitizer switches are not turned off here; the BUILD file makes the test
# incompatible with a sanitizer configuration instead.)
def _platform_transition_impl(_settings, attr):
    return {"//command_line_option:platforms": str(attr.platform)}

_platform_transition = transition(
    implementation = _platform_transition_impl,
    inputs = [],
    outputs = ["//command_line_option:platforms"],
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
