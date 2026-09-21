// The C++ side of the cross-language index fixture: every use below is of
// code protoc generated from proto/inventory.proto and proto/unit.proto, and
// the index links each one back to the message, field or enumerator it came
// from (see proto_index_test.sh).
#include "bazel/testdata/proto/inventory.pb.h"
#include "proto/unit.pb.h"

namespace demo {

int total_pieces(const inventory::Inventory& inventory) {
  int total = 0;
  for (const inventory::Inventory::Slot& slot : inventory.slots()) {
    if (slot.unit() == inventory::PIECE) total += slot.count();
  }
  return total;
}

void restock(inventory::Inventory& inventory) {
  inventory::Inventory::Slot* slot = inventory.add_slots();
  slot->set_label("bolts");
  slot->set_count(3);
  slot->set_unit(inventory::PIECE);
  inventory.set_team("tools");
}

}  // namespace demo
