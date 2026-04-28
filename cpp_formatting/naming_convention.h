#ifndef CPP_FORMATTING_NAMING_CONVENTION_H_
#define CPP_FORMATTING_NAMING_CONVENTION_H_

#include <string>
#include <string_view>
#include <vector>

// ---------------------------------------------------------------------------
// NamingStyle
// ---------------------------------------------------------------------------

enum class NamingStyle {
  SnakeCase,          // variable, snake_case
  LeadingUnderscore,  // _variable, _snake_case
  TrailingUnderscore, // variable_, snake_case_
  MemberPrefix,       // m_variable, m_snake_case
  CamelCase,          // camelCase
  UpperCamelCase,     // UpperCamelCase
  UpperSnakeCase,     // UPPER_SNAKE_CASE
  KConstant,          // kSomeConstant
};

// ---------------------------------------------------------------------------
// Core API
// ---------------------------------------------------------------------------

// Split a variable name (in any supported style) into lowercase word tokens.
// Strips leading/trailing underscores, "m_" prefix, and "k" constant prefix
// before splitting on underscore and camelCase boundaries.
std::vector<std::string> splitIntoWords(std::string_view name);

// Join lowercase word tokens into the requested naming style.
std::string formatName(const std::vector<std::string>& words, NamingStyle style);

// Convenience: split then reformat.
std::string renameToStyle(std::string_view name, NamingStyle style);

// ---------------------------------------------------------------------------
// Style name helpers
// ---------------------------------------------------------------------------

// Parse a style keyword (e.g. "snake_case", "camelCase") into NamingStyle.
// Returns false if unrecognised.
bool parseNamingStyle(std::string_view keyword, NamingStyle& out);

// Return the canonical keyword string for a style (inverse of parseNamingStyle).
std::string_view namingStyleKeyword(NamingStyle style);

#endif  // CPP_FORMATTING_NAMING_CONVENTION_H_
