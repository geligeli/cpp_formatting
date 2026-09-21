#include "shop/shop.pb.h"

int main() {
  shop::Item item;
  item.set_name("bolt");
  item.set_quantity(item.quantity() + 3);
  return item.quantity() == 3 ? 0 : 1;
}
