// The suffix array of a byte string: the start offsets of every suffix of the
// text, in lexicographic (unsigned byte) order.  libsais (SA-IS, linear time,
// no workspace beyond the result for a byte alphabet) behind a small seam, so
// that the text index does not see its C interface.
#ifndef CODE_BROWSER_SUFFIX_ARRAY_H_
#define CODE_BROWSER_SUFFIX_ARRAY_H_

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace code_browser {

// The longest text BuildSuffixArray accepts: offsets are int32_t.
inline constexpr size_t kMaxSuffixArrayText = 0x7fffffff;

// Empty when `text.size()` exceeds kMaxSuffixArrayText (or libsais fails).
auto BuildSuffixArray(std::string_view text) -> std::vector<int32_t>;

}  // namespace code_browser

#endif  // CODE_BROWSER_SUFFIX_ARRAY_H_
