#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cpp_formatting/const_placement_lib.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static auto east(const char* Code) -> std::string {
  return rewriteConstPlacement(Code, ConstStyle::East, {"-std=c++20", "-xc++"});
}

static auto west(const char* Code) -> std::string {
  return rewriteConstPlacement(Code, ConstStyle::West, {"-std=c++20", "-xc++"});
}

// ---------------------------------------------------------------------------
// The basic move, both ways
// ---------------------------------------------------------------------------

TEST(EastConst, Variable) {
  EXPECT_EQ(east("const int a = 0;"), "int const a = 0;");
}

TEST(EastConst, PointerToConst) {
  EXPECT_EQ(east("const int* p = nullptr;"), "int const* p = nullptr;");
}

TEST(EastConst, ReferenceToConst) {
  EXPECT_EQ(east("int v = 0; const int& r = v;"),
            "int v = 0; int const& r = v;");
}

TEST(EastConst, Parameter) {
  EXPECT_EQ(east("void f(const int x, const double y);"),
            "void f(int const x, double const y);");
}

TEST(EastConst, ReturnType) {
  EXPECT_EQ(east("const int f();"), "int const f();");
}

TEST(EastConst, QualifiedName) {
  EXPECT_EQ(east("namespace n { struct S {}; } const n::S s{};"),
            "namespace n { struct S {}; } n::S const s{};");
}

TEST(EastConst, ElaboratedTypeSpecifier) {
  EXPECT_EQ(east("struct S {}; const struct S s{};"),
            "struct S {}; struct S const s{};");
}

TEST(EastConst, MultiTokenBuiltin) {
  EXPECT_EQ(east("const unsigned long long a = 0;"),
            "unsigned long long const a = 0;");
}

TEST(EastConst, TemplateArgument) {
  EXPECT_EQ(east("template <class T> struct V {}; V<const int> v;"),
            "template <class T> struct V {}; V<int const> v;");
}

TEST(EastConst, AliasDeclaration) {
  EXPECT_EQ(east("using A = const int;"), "using A = int const;");
}

TEST(EastConst, Typedef) {
  EXPECT_EQ(east("typedef const int A;"), "typedef int const A;");
}

TEST(EastConst, Auto) {
  EXPECT_EQ(east("const auto a = 1;"), "auto const a = 1;");
}

TEST(EastConst, Decltype) {
  EXPECT_EQ(east("int v = 0; const decltype(v) a = 1;"),
            "int v = 0; decltype(v) const a = 1;");
}

TEST(EastConst, Cast) {
  EXPECT_EQ(east("int v = 0; const int& r = static_cast<const int&>(v);"),
            "int v = 0; int const& r = static_cast<int const&>(v);");
}

TEST(EastConst, ArrayElement) {
  EXPECT_EQ(east("const int a[3] = {};"), "int const a[3] = {};");
}

TEST(EastConst, StorageClassStaysPut) {
  EXPECT_EQ(east("static const int a = 0;"), "static int const a = 0;");
}

TEST(EastConst, TemplateParameterType) {
  EXPECT_EQ(east("template <class T> void f(const T& t);"),
            "template <class T> void f(T const& t);");
}

TEST(WestConst, Variable) {
  EXPECT_EQ(west("int const a = 0;"), "const int a = 0;");
}

TEST(WestConst, PointerToConst) {
  EXPECT_EQ(west("int const* p = nullptr;"), "const int* p = nullptr;");
}

TEST(WestConst, Parameter) {
  EXPECT_EQ(west("void f(int const x);"), "void f(const int x);");
}

TEST(WestConst, TemplateArgument) {
  EXPECT_EQ(west("template <class T> struct V {}; V<int const> v;"),
            "template <class T> struct V {}; V<const int> v;");
}

TEST(WestConst, AbstractParameter) {
  EXPECT_EQ(west("void f(int const);"), "void f(const int);");
}

// ---------------------------------------------------------------------------
// The load-bearing distinction: a qualifier on a *declarator component* is
// already east-only and must never move.
// ---------------------------------------------------------------------------

TEST(ConstPlacement, ConstPointerIsNotMovedWest) {
  // `int* const p` is a const pointer; `const int* p` is a different type.
  EXPECT_EQ(west("int* const p = nullptr;"), "int* const p = nullptr;");
}

TEST(ConstPlacement, ConstPointerIsUntouchedByEast) {
  EXPECT_EQ(east("int* const p = nullptr;"), "int* const p = nullptr;");
}

TEST(ConstPlacement, OnlyThePointeeQualifierMoves) {
  EXPECT_EQ(east("const int* const p = nullptr;"),
            "int const* const p = nullptr;");
  EXPECT_EQ(west("int const* const p = nullptr;"),
            "const int* const p = nullptr;");
}

TEST(ConstPlacement, ConstPointerToConstPointer) {
  EXPECT_EQ(east("const int* const* pp = nullptr;"),
            "int const* const* pp = nullptr;");
}

TEST(ConstPlacement, ReferenceQualifierIsNotInvented) {
  // Nothing in `int& r` is qualified, so neither direction touches it.
  EXPECT_EQ(east("int v = 0; int& r = v;"), "int v = 0; int& r = v;");
}

TEST(ConstPlacement, ConstMemberFunctionIsUntouched) {
  // The trailing `const` of a member function qualifies the implicit object
  // parameter.  It is not a QualifiedTypeLoc, so the pass never sees it.
  EXPECT_EQ(east("struct S { int f() const; };"),
            "struct S { int f() const; };");
  EXPECT_EQ(west("struct S { int f() const; };"),
            "struct S { int f() const; };");
}

TEST(ConstPlacement, ConstMemberFunctionWithConstReturn) {
  EXPECT_EQ(east("struct S { const int* f() const; };"),
            "struct S { int const* f() const; };");
  EXPECT_EQ(west("struct S { int const* f() const; };"),
            "struct S { const int* f() const; };");
}

TEST(ConstPlacement, ConstArrayOfConstPointers) {
  EXPECT_EQ(east("const char* const names[2] = {};"),
            "char const* const names[2] = {};");
}

// ---------------------------------------------------------------------------
// Nested qualified types.  The outer type specifier *spans* the inner ones, so
// the moves have to be applied innermost-first or an outer rewrite lands on
// text the inner ones then edit in the middle of.
// ---------------------------------------------------------------------------

TEST(ConstPlacement, NestedTemplateArgumentsMoveWithTheirOuterType) {
  EXPECT_EQ(east("template <class A, class B> struct P {};\n"
                 "struct W {};\n"
                 "void f(const P<const W, const W*>& p);\n"),
            "template <class A, class B> struct P {};\n"
            "struct W {};\n"
            "void f(P<W const, W const*> const& p);\n");
}

TEST(ConstPlacement, NestedTemplateArgumentsMoveBackWest) {
  EXPECT_EQ(west("template <class A, class B> struct P {};\n"
                 "struct W {};\n"
                 "void f(P<W const, W const*> const& p);\n"),
            "template <class A, class B> struct P {};\n"
            "struct W {};\n"
            "void f(const P<const W, const W*>& p);\n");
}

TEST(ConstPlacement, QualifierInsideAFunctionTypeTemplateArgument) {
  EXPECT_EQ(east("template <class F> struct Fn {};\n"
                 "void g(const Fn<int(const int&)>& f);\n"),
            "template <class F> struct Fn {};\n"
            "void g(Fn<int(int const&)> const& f);\n");
}

TEST(ConstPlacement, ThreeLevelsOfNesting) {
  EXPECT_EQ(east("template <class T> struct V {};\n"
                 "struct W {};\n"
                 "const V<const V<const W>>* p = nullptr;\n"),
            "template <class T> struct V {};\n"
            "struct W {};\n"
            "V<V<W const> const> const* p = nullptr;\n");
}

// ---------------------------------------------------------------------------
// Macros: a qualifier the preprocessor produced has no byte range to move.
// ---------------------------------------------------------------------------

TEST(ConstPlacement, QualifierFromMacroBodyIsDeclined) {
  EXPECT_EQ(east("#define CONST_INT const int\nCONST_INT a = 0;\n"),
            "#define CONST_INT const int\nCONST_INT a = 0;\n");
}

TEST(ConstPlacement, QualifierSpelledAsAMacroIsDeclined) {
  EXPECT_EQ(east("#define CONST const\nCONST int a = 0;\n"),
            "#define CONST const\nCONST int a = 0;\n");
}

TEST(ConstPlacement, MacroTypeEndingInAngleBracketIsDeclined) {
  // The `>` of a split `>>` is reached through getFileLoc, so this checks that
  // path is not a hole for a macro whose expansion happens to end in `>`.
  EXPECT_EQ(east("template <class T> struct V {}; struct W {};\n"
                 "#define VEC V<W>\n"
                 "const VEC* q = nullptr;\n"),
            "template <class T> struct V {}; struct W {};\n"
            "#define VEC V<W>\n"
            "const VEC* q = nullptr;\n");
}

TEST(ConstPlacement, TypeSpelledAsAMacroIsDeclined) {
  EXPECT_EQ(east("#define INT int\nconst INT a = 0;\n"),
            "#define INT int\nconst INT a = 0;\n");
}

// ---------------------------------------------------------------------------
// Several declarators over one decl-specifier-seq
// ---------------------------------------------------------------------------

TEST(ConstPlacement, SharedDeclSpecifierMovesOnce) {
  EXPECT_EQ(east("const int a = 0, b = 0;"), "int const a = 0, b = 0;");
  EXPECT_EQ(west("int const a = 0, b = 0;"), "const int a = 0, b = 0;");
}

TEST(ConstPlacement, SharedDeclSpecifierWithMixedDeclarators) {
  EXPECT_EQ(east("const int a = 0, *p = nullptr;"),
            "int const a = 0, *p = nullptr;");
}

// ---------------------------------------------------------------------------
// volatile, and runs of qualifiers
// ---------------------------------------------------------------------------

TEST(ConstPlacement, VolatileMovesToo) {
  EXPECT_EQ(east("volatile int a = 0;"), "int volatile a = 0;");
  EXPECT_EQ(west("int volatile a = 0;"), "volatile int a = 0;");
}

TEST(ConstPlacement, ConstVolatileRunMovesAsAUnit) {
  EXPECT_EQ(east("const volatile int a = 0;"), "int const volatile a = 0;");
  EXPECT_EQ(west("int const volatile a = 0;"), "const volatile int a = 0;");
}

TEST(ConstPlacement, RunOrderIsPreserved) {
  EXPECT_EQ(east("volatile const int a = 0;"), "int volatile const a = 0;");
}

TEST(ConstPlacement, QualifiersSplitAcrossTheTypeAreGathered) {
  EXPECT_EQ(east("const int volatile a = 0;"), "int const volatile a = 0;");
  EXPECT_EQ(west("const int volatile a = 0;"), "const volatile int a = 0;");
}

// ---------------------------------------------------------------------------
// Templates: a pattern is rewritten once, an instantiation never.
// ---------------------------------------------------------------------------

TEST(ConstPlacement, TemplatePatternIsRewrittenOnce) {
  EXPECT_EQ(east("template <class T> struct W { const T v; };\n"
                 "W<int> w{0};\nW<char> x{0};\n"),
            "template <class T> struct W { T const v; };\n"
            "W<int> w{0};\nW<char> x{0};\n");
}

TEST(ConstPlacement, ExplicitSpecializationIsRewritten) {
  EXPECT_EQ(east("template <class T> struct W { T v; };\n"
                 "template <> struct W<char> { const char v; };\n"),
            "template <class T> struct W { T v; };\n"
            "template <> struct W<char> { char const v; };\n");
}

// ---------------------------------------------------------------------------
// Fixpoint and round trip
// ---------------------------------------------------------------------------

TEST(ConstPlacement, EastIsAFixpoint) {
  const char* Code =
      "int const a = 0;\nint const* const p = nullptr;\n"
      "void f(int const& r);\nstruct S { int const* g() const; };\n";
  EXPECT_EQ(east(Code), Code);
}

TEST(ConstPlacement, WestIsAFixpoint) {
  const char* Code =
      "const int a = 0;\nconst int* const p = nullptr;\n"
      "void f(const int& r);\nstruct S { const int* g() const; };\n";
  EXPECT_EQ(west(Code), Code);
}

TEST(ConstPlacement, NestedAngleBracketsRoundTrip) {
  const char* Code =
      "template <class T> struct V {};\nstruct W {};\n"
      "const V<const V<const W>>* p = nullptr;\n";
  const std::string Moved = east(Code);
  EXPECT_EQ(Moved,
            "template <class T> struct V {};\nstruct W {};\n"
            "V<V<W const> const> const* p = nullptr;\n");
  EXPECT_EQ(west(Moved.c_str()), Code);
}

TEST(ConstPlacement, WestUndoesEast) {
  const char* Code =
      "const int a = 0;\nconst int* const p = nullptr;\n"
      "void f(const int& r, const unsigned x);\n"
      "struct S { const int* g() const; };\n"
      "template <class T> struct V {};\nV<const int> v;\n";
  const std::string Moved = east(Code);
  EXPECT_NE(Moved, Code);
  EXPECT_EQ(west(Moved.c_str()), Code);
}

// ---------------------------------------------------------------------------
// parseConstStyle
// ---------------------------------------------------------------------------

TEST(ParseConstStyle, KnownNames) {
  ConstStyle S{};
  EXPECT_TRUE(parseConstStyle("east", S));
  EXPECT_EQ(S, ConstStyle::East);
  EXPECT_TRUE(parseConstStyle("west", S));
  EXPECT_EQ(S, ConstStyle::West);
}

TEST(ParseConstStyle, UnknownName) {
  ConstStyle S{};
  EXPECT_FALSE(parseConstStyle("", S));
  EXPECT_FALSE(parseConstStyle("East", S));
  EXPECT_FALSE(parseConstStyle("right", S));
}
