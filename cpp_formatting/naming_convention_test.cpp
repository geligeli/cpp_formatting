#include "cpp_formatting/naming_convention.h"

#include <gtest/gtest.h>

// ---------------------------------------------------------------------------
// splitIntoWords — all nine input patterns
// ---------------------------------------------------------------------------

TEST(SplitIntoWords, PlainVariable) {
  EXPECT_EQ(splitIntoWords("variable"), (std::vector<std::string>{"variable"}));
}

TEST(SplitIntoWords, LeadingUnderscore) {
  EXPECT_EQ(splitIntoWords("_variable"),
            (std::vector<std::string>{"variable"}));
}

TEST(SplitIntoWords, TrailingUnderscore) {
  EXPECT_EQ(splitIntoWords("variable_"),
            (std::vector<std::string>{"variable"}));
}

TEST(SplitIntoWords, MemberPrefix) {
  EXPECT_EQ(splitIntoWords("m_variable"),
            (std::vector<std::string>{"variable"}));
}

TEST(SplitIntoWords, SnakeCase) {
  EXPECT_EQ(splitIntoWords("snake_case"),
            (std::vector<std::string>{"snake", "case"}));
}

TEST(SplitIntoWords, CamelCase) {
  EXPECT_EQ(splitIntoWords("camelCase"),
            (std::vector<std::string>{"camel", "case"}));
}

TEST(SplitIntoWords, UpperCamelCase) {
  EXPECT_EQ(splitIntoWords("UpperCamelCase"),
            (std::vector<std::string>{"upper", "camel", "case"}));
}

TEST(SplitIntoWords, UpperSnakeCase) {
  EXPECT_EQ(splitIntoWords("UPPER_SNAKE_CASE"),
            (std::vector<std::string>{"upper", "snake", "case"}));
}

TEST(SplitIntoWords, KConstant) {
  EXPECT_EQ(splitIntoWords("kSomeConstant"),
            (std::vector<std::string>{"some", "constant"}));
}

// Multi-word affixed forms.
TEST(SplitIntoWords, MemberPrefixMultiWord) {
  EXPECT_EQ(splitIntoWords("m_snake_case"),
            (std::vector<std::string>{"snake", "case"}));
}

TEST(SplitIntoWords, BothUnderscores) {
  EXPECT_EQ(splitIntoWords("_snake_case_"),
            (std::vector<std::string>{"snake", "case"}));
}

// Acronyms: uppercase run followed by a lowercase letter causes a split
// before the last uppercase letter of the run.
TEST(SplitIntoWords, AcronymInMiddle) {
  EXPECT_EQ(splitIntoWords("URLParser"),
            (std::vector<std::string>{"url", "parser"}));
}

TEST(SplitIntoWords, AcronymAtStart) {
  EXPECT_EQ(splitIntoWords("parseURL"),
            (std::vector<std::string>{"parse", "url"}));
}

// Edge cases.
TEST(SplitIntoWords, SingleChar) {
  EXPECT_EQ(splitIntoWords("x"), (std::vector<std::string>{"x"}));
}

TEST(SplitIntoWords, KNotFollowedByUpper) {
  // "k" without an uppercase following is not a constant prefix.
  EXPECT_EQ(splitIntoWords("key"), (std::vector<std::string>{"key"}));
}

TEST(SplitIntoWords, WithDigits) {
  EXPECT_EQ(splitIntoWords("var1Name"),
            (std::vector<std::string>{"var1", "name"}));
}

// ---------------------------------------------------------------------------
// formatName — single word
// ---------------------------------------------------------------------------

TEST(FormatName, SingleWord_SnakeCase) {
  EXPECT_EQ(formatName({"variable"}, NamingStyle::SnakeCase), "variable");
}

TEST(FormatName, SingleWord_LeadingUnderscore) {
  EXPECT_EQ(formatName({"variable"}, NamingStyle::LeadingUnderscore),
            "_variable");
}

TEST(FormatName, SingleWord_TrailingUnderscore) {
  EXPECT_EQ(formatName({"variable"}, NamingStyle::TrailingUnderscore),
            "variable_");
}

TEST(FormatName, SingleWord_MemberPrefix) {
  EXPECT_EQ(formatName({"variable"}, NamingStyle::MemberPrefix), "m_variable");
}

TEST(FormatName, SingleWord_CamelCase) {
  EXPECT_EQ(formatName({"variable"}, NamingStyle::CamelCase), "variable");
}

TEST(FormatName, SingleWord_UpperCamelCase) {
  EXPECT_EQ(formatName({"variable"}, NamingStyle::UpperCamelCase), "Variable");
}

TEST(FormatName, SingleWord_UpperSnakeCase) {
  EXPECT_EQ(formatName({"variable"}, NamingStyle::UpperSnakeCase), "VARIABLE");
}

TEST(FormatName, SingleWord_KConstant) {
  EXPECT_EQ(formatName({"variable"}, NamingStyle::KConstant), "kVariable");
}

// ---------------------------------------------------------------------------
// formatName — multi-word
// ---------------------------------------------------------------------------

static const std::vector<std::string> kWords = {"some", "value"};

TEST(FormatName, MultiWord_SnakeCase) {
  EXPECT_EQ(formatName(kWords, NamingStyle::SnakeCase), "some_value");
}

TEST(FormatName, MultiWord_LeadingUnderscore) {
  EXPECT_EQ(formatName(kWords, NamingStyle::LeadingUnderscore), "_some_value");
}

TEST(FormatName, MultiWord_TrailingUnderscore) {
  EXPECT_EQ(formatName(kWords, NamingStyle::TrailingUnderscore), "some_value_");
}

TEST(FormatName, MultiWord_MemberPrefix) {
  EXPECT_EQ(formatName(kWords, NamingStyle::MemberPrefix), "m_some_value");
}

TEST(FormatName, MultiWord_CamelCase) {
  EXPECT_EQ(formatName(kWords, NamingStyle::CamelCase), "someValue");
}

TEST(FormatName, MultiWord_UpperCamelCase) {
  EXPECT_EQ(formatName(kWords, NamingStyle::UpperCamelCase), "SomeValue");
}

TEST(FormatName, MultiWord_UpperSnakeCase) {
  EXPECT_EQ(formatName(kWords, NamingStyle::UpperSnakeCase), "SOME_VALUE");
}

TEST(FormatName, MultiWord_KConstant) {
  EXPECT_EQ(formatName(kWords, NamingStyle::KConstant), "kSomeValue");
}

// ---------------------------------------------------------------------------
// renameToStyle — round-trips
// ---------------------------------------------------------------------------

// For each source style, verify that renaming to every target style gives the
// correct string.  We use "someValue" (two words) as the canonical test name.

struct RoundTripCase {
  const char* source;
  NamingStyle target;
  const char* expected;
};

static const RoundTripCase kRoundTrips[] = {
    // From snake_case
    {"some_value", NamingStyle::SnakeCase, "some_value"},
    {"some_value", NamingStyle::LeadingUnderscore, "_some_value"},
    {"some_value", NamingStyle::TrailingUnderscore, "some_value_"},
    {"some_value", NamingStyle::MemberPrefix, "m_some_value"},
    {"some_value", NamingStyle::CamelCase, "someValue"},
    {"some_value", NamingStyle::UpperCamelCase, "SomeValue"},
    {"some_value", NamingStyle::UpperSnakeCase, "SOME_VALUE"},
    {"some_value", NamingStyle::KConstant, "kSomeValue"},
    // From camelCase
    {"someValue", NamingStyle::SnakeCase, "some_value"},
    {"someValue", NamingStyle::LeadingUnderscore, "_some_value"},
    {"someValue", NamingStyle::MemberPrefix, "m_some_value"},
    {"someValue", NamingStyle::UpperCamelCase, "SomeValue"},
    {"someValue", NamingStyle::UpperSnakeCase, "SOME_VALUE"},
    {"someValue", NamingStyle::KConstant, "kSomeValue"},
    // From UpperCamelCase
    {"SomeValue", NamingStyle::SnakeCase, "some_value"},
    {"SomeValue", NamingStyle::CamelCase, "someValue"},
    {"SomeValue", NamingStyle::KConstant, "kSomeValue"},
    // From UPPER_SNAKE_CASE
    {"SOME_VALUE", NamingStyle::SnakeCase, "some_value"},
    {"SOME_VALUE", NamingStyle::CamelCase, "someValue"},
    {"SOME_VALUE", NamingStyle::UpperCamelCase, "SomeValue"},
    {"SOME_VALUE", NamingStyle::KConstant, "kSomeValue"},
    // From kConstant
    {"kSomeValue", NamingStyle::SnakeCase, "some_value"},
    {"kSomeValue", NamingStyle::CamelCase, "someValue"},
    {"kSomeValue", NamingStyle::UpperSnakeCase, "SOME_VALUE"},
    // From _leading
    {"_some_value", NamingStyle::SnakeCase, "some_value"},
    {"_some_value", NamingStyle::CamelCase, "someValue"},
    // From trailing_
    {"some_value_", NamingStyle::SnakeCase, "some_value"},
    {"some_value_", NamingStyle::UpperCamelCase, "SomeValue"},
    // From m_prefix
    {"m_some_value", NamingStyle::SnakeCase, "some_value"},
    {"m_some_value", NamingStyle::KConstant, "kSomeValue"},
};

TEST(RenameToStyle, RoundTrips) {
  for (const auto& [src, style, expected] : kRoundTrips)
    EXPECT_EQ(renameToStyle(src, style), expected)
        << "source=" << src << " style=" << namingStyleKeyword(style);
}

// ---------------------------------------------------------------------------
// parseNamingStyle / namingStyleKeyword
// ---------------------------------------------------------------------------

TEST(NamingStyleKeyword, ParseRoundTrip) {
  constexpr NamingStyle kAllStyles[] = {
      NamingStyle::SnakeCase,          NamingStyle::LeadingUnderscore,
      NamingStyle::TrailingUnderscore, NamingStyle::MemberPrefix,
      NamingStyle::CamelCase,          NamingStyle::UpperCamelCase,
      NamingStyle::UpperSnakeCase,     NamingStyle::KConstant,
  };
  for (NamingStyle s : kAllStyles) {
    NamingStyle parsed{};
    EXPECT_TRUE(parseNamingStyle(namingStyleKeyword(s), parsed));
    EXPECT_EQ(parsed, s);
  }
}

TEST(NamingStyleKeyword, UnknownKeyword) {
  NamingStyle out{};
  EXPECT_FALSE(parseNamingStyle("unknown", out));
}
