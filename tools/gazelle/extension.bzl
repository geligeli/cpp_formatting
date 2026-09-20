"""The repository the Gazelle targets are defined in.

`gazelle` and `gazelle_cc` are dev dependencies, so a BUILD file of this module
that `load()`s from them does not load when the module is somebody's dependency
-- and then `bazel query @cpp_formatting//...` in the consumer's repository
fails on `//tools/gazelle`, however deliberately that package was kept apart.
A *label* is another matter: it is resolved only when the target is analysed.
So the rules are instantiated in a generated repository, which only this
extension's (dev-only) use makes exist, and `//tools/gazelle` holds aliases to
them: `bazel run //tools/gazelle` is unchanged, and the package loads anywhere.

A repository an extension generates sees what the module hosting the extension
sees, which as the root module includes the dev dependencies `@gazelle` and
`@gazelle_cc`.  As a dependency this extension is never used (its
`use_extension` is `dev_dependency = True`), so the repository is never
fetched and the load below never evaluated.
"""

_BUILD = '''\
load("@gazelle//:def.bzl", "gazelle", "gazelle_binary")

package(default_visibility = ["//visibility:public"])

gazelle_binary(
    name = "gazelle_cc",
    languages = ["@gazelle_cc//language/cc"],
)

gazelle(
    name = "gazelle",
    gazelle = ":gazelle_cc",
)
'''

def _gazelle_runner_repo_impl(rctx):
    rctx.file("BUILD.bazel", _BUILD)

_gazelle_runner_repo = repository_rule(implementation = _gazelle_runner_repo_impl)

def _gazelle_runner_impl(_mctx):
    _gazelle_runner_repo(name = "gazelle_runner")

gazelle_runner = module_extension(implementation = _gazelle_runner_impl)
