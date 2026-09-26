#include "code_browser/suffix_array.h"

#include <libsais.h>

namespace code_browser {

auto BuildSuffixArray(std::string_view text) -> std::vector<int32_t> {
  if (text.size() > kMaxSuffixArrayText) return {};
  std::vector<int32_t> sa(text.size());
  if (text.empty()) return sa;
  if (libsais(reinterpret_cast<const uint8_t*>(text.data()), sa.data(),
              static_cast<int32_t>(text.size()), /*fs=*/0,
              /*freq=*/nullptr) != 0)
    return {};
  return sa;
}

}  // namespace code_browser
