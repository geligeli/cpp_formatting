"""`data` and `env` for a test that parses C++ fixtures which include the standard library.

    cc_test(
        ...
        data = HERMETIC_STD_DATA,
        env = HERMETIC_STD_ENV,
    )

See the BUILD file for why.  Linux only, like the hermetic toolchain: elsewhere
both are empty and the fixtures resolve against the machine's own headers, as
they always did.  The paths are relative to the test's working directory (the
runfiles root of the main repository), which is where an in-memory tool run and
a script that does not `cd` resolve them.
"""

_DIRS = ["//tools/hermetic_std:dir%d" % i for i in range(5)]

HERMETIC_STD_DATA = select({
    "@platforms//os:linux": _DIRS,
    "//conditions:default": [],
})

HERMETIC_STD_ENV = select({
    "@platforms//os:linux": {
        # The libc++ in these directories is newer than the Clang the tools
        # link; it supports the two latest Clang releases and says so with a
        # #warning on an older one, which -w (the tools append it) silences.
        "CPLUS_INCLUDE_PATH": ":".join(["$(rootpath %s)" % d for d in _DIRS]),
    },
    "//conditions:default": {},
})
