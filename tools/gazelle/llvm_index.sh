#!/usr/bin/env bash
# llvm_index.sh -- regenerate tools/gazelle/llvm.ccindex, the header -> label
# index gazelle_cc resolves `#include "clang/..."` / `"llvm/..."` with.
#
# gazelle_cc knows the headers of Bazel Central Registry modules from an index
# it ships, but @llvm-project is not one: it comes from the module extension in
# //third_party/llvm (the BCR stops at LLVM 17).  So the mapping is derived here
# from the LLVM Bazel overlay itself -- every cc_library under //llvm and
# //clang, every header it lists under `include/` -- and checked in, because
# Gazelle must not need a Bazel query (and an LLVM fetch) to run.
#
# Run it after bumping LLVM_VERSION; the diff shows the headers that moved
# between libraries.
#
# A header listed by several libraries (`clang/Index/*.h` is in :index, :sema
# and both libclangs) goes to the one with the fewest headers -- the most
# specific -- and then to the first by name, so the file is deterministic and
# Gazelle never has to guess.
set -euo pipefail

BAZEL="${BAZEL:-bazel}"
cd "$("$BAZEL" info workspace)"
out=tools/gazelle/llvm.ccindex

"$BAZEL" query --output=streamed_jsonproto \
  'kind("cc_library rule", @llvm-project//llvm:all + @llvm-project//clang:all)' |
  python3 -c '
import collections, json, sys

owners = collections.defaultdict(set)
size = collections.Counter()
for line in sys.stdin:
    rule = json.loads(line)["rule"]
    attrs = {a["name"]: a.get("stringListValue", []) for a in rule["attribute"]}
    for header in attrs.get("hdrs", []) + attrs.get("textual_hdrs", []):
        path = header.partition(":")[2]
        # The overlay exports <package>/include on the include path.
        if path.startswith("include/") and path.endswith((".h", ".def", ".inc")):
            owners[path[len("include/"):]].add(rule["name"])
            size[rule["name"]] += 1

index = {
    header: [min(labels, key=lambda label: (size[label], label))]
    for header, labels in sorted(owners.items())
}
# One header per line: an LLVM bump then diffs as the headers that moved.
lines = ["  %s: %s" % (json.dumps(h), json.dumps(l)) for h, l in index.items()]
sys.stdout.write("{\n" + ",\n".join(lines) + "\n}\n")
' > "$out.tmp"
mv "$out.tmp" "$out"
echo "wrote $out ($(grep -c '"@' "$out") headers)" >&2
