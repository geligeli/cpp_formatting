#!/usr/bin/env bash
# Reads //bazel/testdata:inventory_index.pb -- the C++ aspect's units for
# inventory_user.cpp merged with the proto aspect's units for the two .proto
# files -- and checks the link between the languages, in both directions.
#
# Nothing in the pipeline guesses a name: protoc reports which bytes of the
# generated header came from which field, the proto producer records them as
# anchors, and the merge links a C++ symbol to a proto symbol when the range
# Clang declares it on is exactly such an anchor.  So this test is what pins
# that the two tools agree on those ranges, with the real protoc and the real
# cc_proto_library.
#
# Arguments (Bazel $(location ...) expansions):
#   $1  the merged index (inventory_index.pb)
#   $2  cpp_format binary
#   $3  proto/inventory.proto
#   $4  proto/unit.proto
#   $5  inventory_user.cpp

set -euo pipefail

index="$1"
cpp_format="$2"
inventory_proto="$3"
unit_proto="$4"
user_cpp="$5"
out="${TEST_TMPDIR:-/tmp}"
fail() { echo "FAIL: $*" >&2; exit 1; }

# The byte offset of the first match of $2 in file $1.
offset_of() { grep -bo -m1 -- "$2" "$1" | cut -d: -f1; }
lookup() { "$cpp_format" --dump-index --lookup="$1:$2" "$index"; }

"$cpp_format" --dump-index --format=text "$index" > "$out/index.txt"

# Both .proto files are files of the index, under their real paths: unit.proto
# is imported as "proto/unit.proto" and reaches protoc as a _virtual_imports
# symlink, and the index names neither.
for f in inventory.proto unit.proto; do
  grep -q "path: \"bazel/testdata/proto/$f\"" "$out/index.txt" \
    || fail "$f not in the index under its real path"
done
grep -q 'path: "proto/unit.proto"' "$out/index.txt" && fail "an import name in the index"
grep -q '_virtual_imports/[^"]*\.proto"' "$out/index.txt" && fail "a virtual import path in the index"
grep -q 'path: "/' "$out/index.txt" && fail "absolute path in the index"
grep -q 'language: PROTO' "$out/index.txt" || fail "no PROTO symbols"
echo "PASS: the .proto files are in the index under their real paths"

# Within the proto language: the field's type is a reference to an enum that
# another file defines, under that file's real path.
off_type="$(offset_of "$inventory_proto" 'Unit unit = 3')"
off_enum="$(offset_of "$unit_proto" 'Unit {')"
lookup bazel/testdata/proto/inventory.proto "$off_type" > "$out/type.txt"
grep -q '^proto:demo.inventory.Unit$' "$out/type.txt" \
  || fail "the field's type does not resolve to the enum"
grep -q "^  bazel/testdata/proto/unit.proto:$off_enum-$((off_enum + 4)) DEFINITION$" "$out/type.txt" \
  || fail "the enum's definition in unit.proto not listed"
grep -q "^  bazel/testdata/proto/inventory.proto:$off_type-$((off_type + 4)) REFERENCE$" "$out/type.txt" \
  || fail "the reference in inventory.proto not listed"
echo "PASS: a reference across .proto files"

# proto -> C++: the field `count` lists what was generated from it.
off_count="$(offset_of "$inventory_proto" 'count = 2')"
lookup bazel/testdata/proto/inventory.proto "$off_count" > "$out/count.txt"
cat "$out/count.txt"
grep -q '^proto:demo.inventory.Inventory.Slot.count$' "$out/count.txt" \
  || fail "lookup at the field did not find it"
grep -q '^  generates c:@N@demo@N@inventory@S@Inventory_Slot@F@count#1$' "$out/count.txt" \
  || fail "the getter is not linked to the field"
grep -q '^  generates c:@N@demo@N@inventory@S@Inventory_Slot@F@set_count#' "$out/count.txt" \
  || fail "the setter is not linked to the field"
grep -q 'inventory.pb.h:[0-9]*-[0-9]* WRITE|GENERATES$' "$out/count.txt" \
  || fail "no setter anchor in the generated header"
echo "PASS: a proto field lists the C++ generated from it"

# C++ -> proto, for each kind of thing the fixture uses.
expect_origin() {  # <needle in inventory_user.cpp> <skip> <proto usr>
  local off
  off="$(offset_of "$user_cpp" "$1")"
  lookup bazel/testdata/inventory_user.cpp "$((off + $2))" > "$out/origin.txt"
  grep -q "^  generated from $3\$" "$out/origin.txt" \
    || { cat "$out/origin.txt"; fail "'$1' does not lead to $3"; }
}
expect_origin 'set_count(3)' 0 proto:demo.inventory.Inventory.Slot.count
expect_origin 'slot.count()' 5 proto:demo.inventory.Inventory.Slot.count
expect_origin 'add_slots()' 0 proto:demo.inventory.Inventory.slots
expect_origin 'set_team(' 0 proto:demo.inventory.Inventory.team
# The nested message is the class Inventory_Slot, used through the alias
# Inventory::Slot; the alias is what the code names.
expect_origin 'Slot\* slot' 0 proto:demo.inventory.Inventory.Slot
expect_origin 'Inventory& inventory) {' 0 proto:demo.inventory.Inventory
# An enumerator of the file with the stripped prefix: its header is the
# sibling of the _virtual_imports symlink, and the anchors name it there.
expect_origin 'PIECE) total' 0 proto:demo.inventory.PIECE
echo "PASS: generated C++ leads back to the .proto"

echo "ALL PROTO INDEX TESTS PASSED"
