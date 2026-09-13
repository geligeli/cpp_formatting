#include "cpp_formatting/naming_convention.h"

#include <cctype>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static std::string toLower(std::string_view s) {
  std::string r(s);
  for (char& c : r)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return r;
}

static std::string toUpper(std::string_view s) {
  std::string r(s);
  for (char& c : r)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return r;
}

static std::string capitalize(std::string_view s) {
  if (s.empty()) return {};
  std::string r(s);
  r[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(r[0])));
  return r;
}

static std::string joinWith(const std::vector<std::string>& words,
                            std::string_view sep) {
  std::string r;
  for (size_t i = 0; i < words.size(); i++) {
    if (i) r += sep;
    r += words[i];
  }
  return r;
}

// ---------------------------------------------------------------------------
// splitIntoWords
// ---------------------------------------------------------------------------

std::vector<std::string> splitIntoWords(std::string_view name) {
  // Strip leading underscores.
  while (!name.empty() && name.front() == '_') name.remove_prefix(1);
  // Strip trailing underscores.
  while (!name.empty() && name.back() == '_') name.remove_suffix(1);
  // Strip "m_" member prefix.
  if (name.size() >= 2 && name[0] == 'm' && name[1] == '_')
    name.remove_prefix(2);
  // Strip "k" constant prefix only when the next character is uppercase.
  if (name.size() >= 2 && name[0] == 'k' &&
      std::isupper(static_cast<unsigned char>(name[1])))
    name.remove_prefix(1);

  std::vector<std::string> words;
  std::string cur;

  auto flush = [&] {
    if (!cur.empty()) {
      words.push_back(toLower(cur));
      cur.clear();
    }
  };

  for (size_t i = 0; i < name.size(); ++i) {
    const char c = name[i];
    if (c == '_') {
      flush();
      continue;
    }
    const bool isUp = std::isupper(static_cast<unsigned char>(c)) != 0;
    if (isUp && !cur.empty()) {
      // Boundary before an uppercase letter when the previous char was
      // lowercase (camelCase split) or when the next char is lowercase and
      // the current run is all-uppercase (acronym end: "URLParser" → URL|P).
      const bool prevLower =
          std::islower(static_cast<unsigned char>(cur.back())) != 0;
      const bool nextLower =
          (i + 1 < name.size()) &&
          std::islower(static_cast<unsigned char>(name[i + 1])) != 0;
      if (prevLower || nextLower) flush();
    }
    cur += c;
  }
  flush();
  return words;
}

// ---------------------------------------------------------------------------
// formatName
// ---------------------------------------------------------------------------

std::string formatName(const std::vector<std::string>& words,
                       NamingStyle style) {
  if (words.empty()) return {};
  switch (style) {
    case NamingStyle::SnakeCase:
      return joinWith(words, "_");

    case NamingStyle::LeadingUnderscore:
      return "_" + joinWith(words, "_");

    case NamingStyle::TrailingUnderscore:
      return joinWith(words, "_") + "_";

    case NamingStyle::MemberPrefix:
      return "m_" + joinWith(words, "_");

    case NamingStyle::CamelCase: {
      std::string r = words[0];
      for (size_t i = 1; i < words.size(); ++i) r += capitalize(words[i]);
      return r;
    }

    case NamingStyle::UpperCamelCase: {
      std::string r;
      for (const auto& w : words) r += capitalize(w);
      return r;
    }

    case NamingStyle::UpperSnakeCase: {
      std::vector<std::string> up;
      up.reserve(words.size());
      for (const auto& w : words) up.push_back(toUpper(w));
      return joinWith(up, "_");
    }

    case NamingStyle::KConstant: {
      std::string r = "k";
      for (const auto& w : words) r += capitalize(w);
      return r;
    }
  }
  return {};
}

// ---------------------------------------------------------------------------
// renameToStyle
// ---------------------------------------------------------------------------

std::string renameToStyle(std::string_view name, NamingStyle style) {
  return formatName(splitIntoWords(name), style);
}

// ---------------------------------------------------------------------------
// parseNamingStyle / namingStyleKeyword
// ---------------------------------------------------------------------------

static constexpr struct {
  const char* keyword;
  NamingStyle style;
} kStyleTable[] = {
    {"snake_case", NamingStyle::SnakeCase},
    {"_leading", NamingStyle::LeadingUnderscore},
    {"trailing_", NamingStyle::TrailingUnderscore},
    {"m_prefix", NamingStyle::MemberPrefix},
    {"camelCase", NamingStyle::CamelCase},
    {"UpperCamelCase", NamingStyle::UpperCamelCase},
    {"UPPER_SNAKE_CASE", NamingStyle::UpperSnakeCase},
    {"kConstant", NamingStyle::KConstant},
};

bool parseNamingStyle(std::string_view keyword, NamingStyle& out) {
  for (const auto& [kw, style] : kStyleTable) {
    if (keyword == kw) {
      out = style;
      return true;
    }
  }
  return false;
}

std::string_view namingStyleKeyword(NamingStyle style) {
  for (const auto& [kw, s] : kStyleTable)
    if (s == style) return kw;
  return "snake_case";
}

// ---------------------------------------------------------------------------
// namingStylesCanCollide
// ---------------------------------------------------------------------------

namespace {

// Does the property hold for every name the style produces, for none of them,
// or does it depend on the input?  Only Yes-vs-No is proof of disjointness.
enum class Holds { No, Yes, Sometimes };

struct StyleShape {
  Holds EndsUnderscore;
  Holds StartsUnderscore;
  Holds FirstCharUpper;
  Holds ContainsUnderscore;
  Holds ContainsUpper;
};

// Read straight off formatName(), given that splitIntoWords() never yields an
// empty word (so a joined name never starts or ends with the separator) and
// that formatName() returns "" for an empty word list (so every produced name
// has at least one word in it).
StyleShape shapeOf(NamingStyle style) {
  constexpr Holds N = Holds::No, Y = Holds::Yes, S = Holds::Sometimes;
  switch (style) {
    //                    ends_  _starts  Upper  has_  hasUpper
    case NamingStyle::SnakeCase:
      return {N, N, N, S, N};
    case NamingStyle::LeadingUnderscore:
      return {N, Y, N, Y, N};
    case NamingStyle::TrailingUnderscore:
      return {Y, N, N, Y, N};
    case NamingStyle::MemberPrefix:
      return {N, N, N, Y, N};
    case NamingStyle::CamelCase:
      return {N, N, N, N, S};  // one word is all lowercase
    case NamingStyle::UpperCamelCase:
      return {N, N, Y, N, Y};
    case NamingStyle::UpperSnakeCase:
      return {N, N, Y, S, Y};
    case NamingStyle::KConstant:
      return {N, N, N, N, Y};  // leading 'k', then a capitalized word
  }
  return {S, S, S, S, S};
}

bool provesDisjoint(Holds a, Holds b) {
  return a != Holds::Sometimes && b != Holds::Sometimes && a != b;
}

}  // namespace

bool namingStylesCanCollide(NamingStyle a, NamingStyle b) {
  if (a == b) return true;
  const StyleShape A = shapeOf(a);
  const StyleShape B = shapeOf(b);
  return !(provesDisjoint(A.EndsUnderscore, B.EndsUnderscore) ||
           provesDisjoint(A.StartsUnderscore, B.StartsUnderscore) ||
           provesDisjoint(A.FirstCharUpper, B.FirstCharUpper) ||
           provesDisjoint(A.ContainsUnderscore, B.ContainsUnderscore) ||
           provesDisjoint(A.ContainsUpper, B.ContainsUpper));
}
