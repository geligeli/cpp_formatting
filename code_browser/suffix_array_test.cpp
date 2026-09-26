#include "code_browser/suffix_array.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <compare>
#include <cstdint>
#include <initializer_list>
#include <numeric>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace code_browser {
namespace {

auto Naive(std::string_view text) -> std::vector<int32_t> {
  std::vector<int32_t> sa(text.size());
  std::iota(sa.begin(), sa.end(), 0);
  // string_view compares as unsigned char through char_traits<char>.
  std::sort(sa.begin(), sa.end(), [&](int32_t a, int32_t b) {
    return text.substr(a) < text.substr(b);
  });
  return sa;
}

void ExpectMatchesNaive(const std::string& text) {
  EXPECT_EQ(BuildSuffixArray(text), Naive(text))
      << "text of " << text.size() << " bytes";
}

TEST(SuffixArray, Small) {
  ExpectMatchesNaive("");
  ExpectMatchesNaive("a");
  ExpectMatchesNaive("ba");
  ExpectMatchesNaive("banana");
  ExpectMatchesNaive("mississippi");
  ExpectMatchesNaive("abracadabra abracadabra");
  EXPECT_EQ(BuildSuffixArray("banana"),
            (std::vector<int32_t>{5, 3, 1, 0, 4, 2}));
}

TEST(SuffixArray, BytesAreUnsigned) {
  ExpectMatchesNaive(
      std::string("a\xff"
                  "b\x80"
                  "a\x01\xff\x00"
                  "zz\xfe",
                  11));
  std::string all;
  for (int c = 255; c >= 0; --c) all += static_cast<char>(c);
  ExpectMatchesNaive(all + all);
}

TEST(SuffixArray, Degenerate) {
  for (const size_t n : {9u, 10u, 11u, 100u, 1000u, 4097u}) {
    ExpectMatchesNaive(std::string(n, 'a'));
    std::string abab, aab, ab_n;
    for (size_t i = 0; i < n; ++i) {
      abab += "ab"[i % 2];
      aab += "aab"[i % 3];
      ab_n += i % 97 == 96 ? 'b' : 'a';
    }
    ExpectMatchesNaive(abab);
    ExpectMatchesNaive(aab);
    ExpectMatchesNaive(ab_n);
  }
  // The Fibonacci word: the classic worst case for LMS recursion depth.
  std::string a = "a", b = "ab";
  while (b.size() < 5000) {
    std::string next = b + a;
    a = std::move(b);
    b = std::move(next);
  }
  ExpectMatchesNaive(b);
}

TEST(SuffixArray, RandomOverSeveralAlphabets) {
  std::mt19937 rng(12345);
  for (const int alphabet : {1, 2, 3, 4, 26, 256}) {
    for (int round = 0; round < 40; ++round) {
      const size_t n = std::uniform_int_distribution<size_t>(0, 3000)(rng);
      std::uniform_int_distribution<int> pick(0, alphabet - 1);
      std::string text(n, '\0');
      for (char& c : text)
        c = static_cast<char>(alphabet == 256 ? pick(rng) : 'a' + pick(rng));
      ExpectMatchesNaive(text);
    }
  }
}

TEST(SuffixArray, SourceLikeText) {
  std::string text;
  for (int i = 0; i < 300; ++i)
    text += "int f" + std::to_string(i % 17) + "(int x) { return x * " +
            std::to_string(i) + "; }\n";
  text += '\0';
  text += "// second file\n";
  text += '\0';
  ExpectMatchesNaive(text);
}

}  // namespace
}  // namespace code_browser
