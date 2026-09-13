#ifndef CPP_FORMATTING_NAMING_CONVENTION_H_
#define CPP_FORMATTING_NAMING_CONVENTION_H_

#include <string>
#include <string_view>
#include <vector>

// ---------------------------------------------------------------------------
// NamingStyle
// ---------------------------------------------------------------------------

enum class NamingStyle {
  SnakeCase,           // variable, snake_case
  LeadingUnderscore,   // _variable, _snake_case
  TrailingUnderscore,  // variable_, snake_case_
  MemberPrefix,        // m_variable, m_snake_case
  CamelCase,           // camelCase
  UpperCamelCase,      // UpperCamelCase
  UpperSnakeCase,      // UPPER_SNAKE_CASE
  KConstant,           // kSomeConstant
};

// ---------------------------------------------------------------------------
// Core API
// ---------------------------------------------------------------------------

// Split a variable name (in any supported style) into lowercase word tokens.
// Strips leading/trailing underscores, "m_" prefix, and "k" constant prefix
// before splitting on underscore and camelCase boundaries.
std::vector<std::string> splitIntoWords(std::string_view name);

// Join lowercase word tokens into the requested naming style.
std::string formatName(const std::vector<std::string>& words,
                       NamingStyle style);

// Convenience: split then reformat.
std::string renameToStyle(std::string_view name, NamingStyle style);

// ---------------------------------------------------------------------------
// Style name helpers
// ---------------------------------------------------------------------------

// Parse a style keyword (e.g. "snake_case", "camelCase") into NamingStyle.
// Returns false if unrecognised.
bool parseNamingStyle(std::string_view keyword, NamingStyle& out);

// Return the canonical keyword string for a style (inverse of
// parseNamingStyle).
std::string_view namingStyleKeyword(NamingStyle style);

// ---------------------------------------------------------------------------
// Style compatibility
// ---------------------------------------------------------------------------

// True when two styles can produce the same name from different inputs, so two
// rename rules using them may pick one name for two different declarations.
//
// Disjointness has to be proved, not assumed: this returns true unless some
// structural property holds for *every* name one style produces and for none
// of the other's.  `trailing_` and `snake_case` are disjoint because only the
// first ever ends in `_`; `m_prefix` and `snake_case` are not, because
// snake_case turns a method named MType into m_type.
//
// The proof assumes an identifier whose first word begins with a letter, which
// is what `_1st` would violate -- both `UpperCamelCase` and `snake_case` leave
// such a word alone and could then agree. Guarding a combination that is
// otherwise sound against a name of that shape is not worth refusing the
// combination outright.
bool namingStylesCanCollide(NamingStyle a, NamingStyle b);

#endif  // CPP_FORMATTING_NAMING_CONVENTION_H_
