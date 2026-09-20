"""Collects header files into one directory artifact, so a test's `env` can name it."""

def _header_directory_impl(ctx):
    out = ctx.actions.declare_directory(ctx.label.name)
    marker = ctx.attr.strip_through
    lines = []
    for f in ctx.files.srcs:
        at = f.path.find(marker)
        if at == -1:
            fail("%s is not under a '%s' directory" % (f.path, marker))
        lines.append(f.path + "\t" + f.path[at + len(marker):])
    manifest = ctx.actions.declare_file(ctx.label.name + ".manifest")
    ctx.actions.write(manifest, "\n".join(lines) + "\n")
    ctx.actions.run_shell(
        inputs = ctx.files.srcs + [manifest],
        outputs = [out],
        arguments = [out.path, manifest.path],
        command = """
set -eu
tab="$(printf '\\t')"
while IFS="$tab" read -r src rel; do
  mkdir -p "$1/$(dirname "$rel")"
  cp "$src" "$1/$rel"
done < "$2"
""",
        mnemonic = "HeaderDirectory",
    )
    return [DefaultInfo(files = depset([out]), runfiles = ctx.runfiles([out]))]

header_directory = rule(
    implementation = _header_directory_impl,
    attrs = {
        "srcs": attr.label_list(allow_files = True, mandatory = True),
        "strip_through": attr.string(
            mandatory = True,
            doc = "Path component(s) up to and including which each file's path is dropped, e.g. '/staging/include/'.",
        ),
    },
)
