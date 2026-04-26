#include "cpp_formatting/trailing_return_types_lib.h"

#include <gtest/gtest.h>

// ---------------------------------------------------------------------------
// Helper
// ---------------------------------------------------------------------------

// Thin wrapper so tests read as: rewrite("...") == "..."
static std::string rewrite(const char* code) {
  return rewriteToTrailingReturnTypes(code, {"-std=c++20", "-xc++"});
}

// ---------------------------------------------------------------------------
// Basic scalar return types
// ---------------------------------------------------------------------------

TEST(TrailingReturnTypes, IntReturn) {
  EXPECT_EQ(rewrite("int foo() { return 42; }"),
            "auto foo() -> int { return 42; }");
}

TEST(TrailingReturnTypes, DoubleReturn) {
  EXPECT_EQ(rewrite("double compute() { return 3.14; }"),
            "auto compute() -> double { return 3.14; }");
}

TEST(TrailingReturnTypes, BoolReturn) {
  EXPECT_EQ(rewrite("bool isReady() { return true; }"),
            "auto isReady() -> bool { return true; }");
}

TEST(TrailingReturnTypes, LongLongReturn) {
  EXPECT_EQ(rewrite("long long getBig() { return 1LL << 40; }"),
            "auto getBig() -> long long { return 1LL << 40; }");
}

TEST(TrailingReturnTypes, FloatReturn) {
  EXPECT_EQ(rewrite("float getF() { return 1.0f; }"),
            "auto getF() -> float { return 1.0f; }");
}

// ---------------------------------------------------------------------------
// Pointer and reference return types
// ---------------------------------------------------------------------------

TEST(TrailingReturnTypes, RawPointerReturn) {
  EXPECT_EQ(rewrite("int* getPtr() { static int x = 0; return &x; }"),
            "auto getPtr() -> int* { static int x = 0; return &x; }");
}

TEST(TrailingReturnTypes, ConstIntReturn) {
  EXPECT_EQ(rewrite("const int getValue() { return 42; }"),
            "auto getValue() -> const int { return 42; }");
}

TEST(TrailingReturnTypes, ConstPointerReturn) {
  EXPECT_EQ(
      rewrite("const int* getConstPtr() { static int x = 0; return &x; }"),
      "auto getConstPtr() -> const int* { static int x = 0; return &x; }");
}

TEST(TrailingReturnTypes, ReferenceReturn) {
  EXPECT_EQ(rewrite("int g_val = 0; int& getRef() { return g_val; }"),
            "int g_val = 0; auto getRef() -> int& { return g_val; }");
}

TEST(TrailingReturnTypes, ConstReferenceReturn) {
  EXPECT_EQ(
      rewrite("int g_val = 0; const int& getConstRef() { return g_val; }"),
      "int g_val = 0; auto getConstRef() -> const int& { return g_val; }");
}

// ---------------------------------------------------------------------------
// Functions with parameters
// ---------------------------------------------------------------------------

TEST(TrailingReturnTypes, SingleParam) {
  EXPECT_EQ(rewrite("int identity(int x) { return x; }"),
            "auto identity(int x) -> int { return x; }");
}

TEST(TrailingReturnTypes, MultipleParams) {
  EXPECT_EQ(rewrite("int add(int a, int b) { return a + b; }"),
            "auto add(int a, int b) -> int { return a + b; }");
}

TEST(TrailingReturnTypes, PointerParam) {
  EXPECT_EQ(rewrite("int deref(int* p) { return *p; }"),
            "auto deref(int* p) -> int { return *p; }");
}

// ---------------------------------------------------------------------------
// Cases that should NOT be rewritten
// ---------------------------------------------------------------------------

TEST(TrailingReturnTypes, AlreadyTrailingReturn) {
  const char* code = "auto foo() -> int { return 42; }";
  EXPECT_EQ(rewrite(code), code);
}

TEST(TrailingReturnTypes, VoidReturnNotRewritten) {
  const char* code = "void doNothing() {}";
  EXPECT_EQ(rewrite(code), code);
}

TEST(TrailingReturnTypes, VoidWithParamsNotRewritten) {
  const char* code = "void process(int x, int y) { (void)x; (void)y; }";
  EXPECT_EQ(rewrite(code), code);
}

TEST(TrailingReturnTypes, ForwardDeclarationOnlyRewritten) {
  // A declaration with no definition anywhere in the TU should be rewritten.
  EXPECT_EQ(rewrite("int foo();"), "auto foo() -> int;");
}

TEST(TrailingReturnTypes, ForwardDeclWithDefinitionBothRewritten) {
  // When both a forward-declaration and a definition exist in the same TU,
  // both are rewritten independently.
  EXPECT_EQ(rewrite("int foo();\nint foo() { return 42; }"),
            "auto foo() -> int;\nauto foo() -> int { return 42; }");
}

// ---------------------------------------------------------------------------
// Multiple functions in one translation unit
// ---------------------------------------------------------------------------

TEST(TrailingReturnTypes, MultipleFunctions) {
  EXPECT_EQ(
      rewrite("int foo() { return 1; }\ndouble bar() { return 2.0; }"),
      "auto foo() -> int { return 1; }\nauto bar() -> double { return 2.0; }");
}

TEST(TrailingReturnTypes, MixedVoidAndNonVoid) {
  EXPECT_EQ(rewrite("void init() {}\nint getValue() { return 7; }"),
            "void init() {}\nauto getValue() -> int { return 7; }");
}

TEST(TrailingReturnTypes, MixedAlreadyTrailingAndPlain) {
  EXPECT_EQ(
      rewrite(
          "auto existing() -> int { return 1; }\nint plain() { return 2; }"),
      "auto existing() -> int { return 1; }\nauto plain() -> int { return 2; "
      "}");
}

// ---------------------------------------------------------------------------
// Class member functions
// ---------------------------------------------------------------------------

TEST(TrailingReturnTypes, MemberFunction) {
  EXPECT_EQ(rewrite("struct S { int get() { return 42; } };"),
            "struct S { auto get() -> int { return 42; } };");
}

TEST(TrailingReturnTypes, ConstMemberFunction) {
  // cv-qualifier on a member function must appear BEFORE the trailing return
  // in the final output: `auto get() const -> int`.
  EXPECT_EQ(rewrite("struct S { int get() const { return 42; } };"),
            "struct S { auto get() const -> int { return 42; } };");
}

TEST(TrailingReturnTypes, StaticMemberFunction) {
  EXPECT_EQ(rewrite("struct S { static int create() { return 0; } };"),
            "struct S { static auto create() -> int { return 0; } };");
}

// ---------------------------------------------------------------------------
// Namespaces
// ---------------------------------------------------------------------------

TEST(TrailingReturnTypes, FunctionInNamespace) {
  EXPECT_EQ(rewrite("namespace ns { int getValue() { return 5; } }"),
            "namespace ns { auto getValue() -> int { return 5; } }");
}

// ---------------------------------------------------------------------------
// Deduced auto return type — must NOT be rewritten
// ---------------------------------------------------------------------------

// `auto foo() { return 123; }` already uses auto return-type deduction.
// Rewriting it would produce `auto foo() -> auto { ... }` which is at best
// redundant and at worst changes semantics for complex deduced types.
TEST(TrailingReturnTypes, AutoDeducedReturnNotRewritten) {
  const char* code = "auto foo() { return 123; }";
  EXPECT_EQ(rewrite(code), code);
}

TEST(TrailingReturnTypes, DecltypeAutoReturnNotRewritten) {
  const char* code = "decltype(auto) bar() { return 123; }";
  EXPECT_EQ(rewrite(code), code);
}

// ---------------------------------------------------------------------------
// Operator overloads — including no-space-between-type-and-name
// ---------------------------------------------------------------------------

// Canonical form: space between `Foo&` and `operator=`.
TEST(TrailingReturnTypes, OperatorAssignment) {
  EXPECT_EQ(
      rewrite("struct Foo { Foo& operator=(Foo& other) { return *this; } };"),
      "struct Foo { auto operator=(Foo& other) -> Foo& { return *this; } };");
}

// Stress-test for the AutoReplacement padding logic: when there is no
// whitespace between the return type and the function name the rewriter must
// insert `auto ` (with a trailing space) so the tokens don't merge.
TEST(TrailingReturnTypes, OperatorAssignmentNoSpaceBeforeName) {
  EXPECT_EQ(
      rewrite("struct Foo { Foo&operator=(Foo& other) { return *this; } };"),
      "struct Foo { auto operator=(Foo& other) -> Foo& { return *this; } };");
}

TEST(TrailingReturnTypes, OperatorPlusPlus) {
  EXPECT_EQ(rewrite("struct Foo { Foo& operator++() { return *this; } };"),
            "struct Foo { auto operator++() -> Foo& { return *this; } };");
}

// Conversion operators must NOT be rewritten: `operator auto() -> bool` is
// not a valid conversion operator; the return type is part of the name.
TEST(TrailingReturnTypes, ConversionOperatorNotRewritten) {
  const char* code =
      "struct Foo { explicit operator bool() const { return true; } };";
  EXPECT_EQ(rewrite(code), code);
}

// ---------------------------------------------------------------------------
// Namespace-qualified return types (regression: LValueReferenceTypeLoc only
// reports the `&` sigil as its local begin, not the start of the base type)
// ---------------------------------------------------------------------------

TEST(TrailingReturnTypes, NamespacedReferenceForwardDeclRewritten) {
  // A declaration-only function with a complex namespaced return type should
  // be rewritten just like a definition.
  EXPECT_EQ(rewrite("namespace std { struct ostream {}; }\n"
                    "struct S {};\n"
                    "std::ostream &operator<<(std::ostream &os, const S &s);"),
            "namespace std { struct ostream {}; }\n"
            "struct S {};\n"
            "auto operator<<(std::ostream &os, const S &s) -> std::ostream &;");
}

TEST(TrailingReturnTypes, NamespacedReferenceReturn) {
  EXPECT_EQ(
      rewrite(
          "namespace std { struct ostream {}; }\n"
          "std::ostream &getStream() { static std::ostream os; return os; }"),
      "namespace std { struct ostream {}; }\n"
      "auto getStream() -> std::ostream & { static std::ostream os; return os; "
      "}");
}

TEST(TrailingReturnTypes, FriendNamespacedReferenceReturn) {
  EXPECT_EQ(rewrite("namespace std { struct ostream {}; }\n"
                    "struct S {\n"
                    "  friend std::ostream &operator<<(std::ostream &os, const "
                    "S &s) { return os; }\n"
                    "};"),
            "namespace std { struct ostream {}; }\n"
            "struct S {\n"
            "  friend auto operator<<(std::ostream &os, const S &s) -> "
            "std::ostream & { return os; }\n"
            "};");
}

// ---------------------------------------------------------------------------
// Non-trivial template return types
// ---------------------------------------------------------------------------

// Single template parameter.
TEST(TrailingReturnTypes, SingleTemplateParamReturn) {
  EXPECT_EQ(rewrite("template<typename T> struct Box { T val; };\n"
                    "Box<int> makeBox() { return {42}; }"),
            "template<typename T> struct Box { T val; };\n"
            "auto makeBox() -> Box<int> { return {42}; }");
}

// Two template parameters.
TEST(TrailingReturnTypes, TwoTemplateParamsReturn) {
  EXPECT_EQ(
      rewrite("template<typename A, typename B> struct Pair { A a; B b; };\n"
              "Pair<int, double> getPair() { return {1, 2.0}; }"),
      "template<typename A, typename B> struct Pair { A a; B b; };\n"
      "auto getPair() -> Pair<int, double> { return {1, 2.0}; }");
}

// Deeply nested template arguments.
TEST(TrailingReturnTypes, NestedTemplateReturn) {
  EXPECT_EQ(
      rewrite("template<typename T> struct Vec { T v; };\n"
              "template<typename K, typename V> struct Map { K k; V v; };\n"
              "Map<int, Vec<double>> getData() { return {}; }"),
      "template<typename T> struct Vec { T v; };\n"
      "template<typename K, typename V> struct Map { K k; V v; };\n"
      "auto getData() -> Map<int, Vec<double>> { return {}; }");
}

// Template type as a pointer return.
TEST(TrailingReturnTypes, TemplatePointerReturn) {
  EXPECT_EQ(rewrite("template<typename T> struct Node { T val; Node* next; };\n"
                    "Node<int>* getNode() { static Node<int> n; return &n; }"),
            "template<typename T> struct Node { T val; Node* next; };\n"
            "auto getNode() -> Node<int>* { static Node<int> n; return &n; }");
}

// Template parameter itself is the return type (dependent type).
TEST(TrailingReturnTypes, TemplateParamAsReturnType) {
  EXPECT_EQ(rewrite("template<typename T> T identity(T x) { return x; }"),
            "template<typename T> auto identity(T x) -> T { return x; }");
}

// WiredOperatorIssue: originally filed with `#include <ostream>` /
// `#include <chrono>`, but the test helper (runToolOnCodeWithArgs) cannot
// resolve system headers in the sandbox.  Inline stubs reproduce the same
// AST structure that triggered the bug (LValueReferenceTypeLoc range).
static const char* kOstreamStub = "namespace std { struct ostream {}; }\n";
static const char* kChronoStub =
    "namespace std { struct ostream {}; "
    "namespace chrono { struct nanoseconds {}; } }\n";

TEST(WiredOperatorIssue, NoChrono) {
  EXPECT_EQ(rewrite((std::string(kOstreamStub) +
                     "struct F { friend std::ostream &operator<<(std::ostream "
                     "&os, const F &) { return os; } };")
                        .c_str()),
            std::string(kOstreamStub) +
                "struct F { friend auto operator<<(std::ostream &os, const F "
                "&) -> std::ostream & { return os; } };");
}

TEST(WiredOperatorIssue, WithChrono) {
  EXPECT_EQ(rewrite((std::string(kChronoStub) +
                     "struct F { friend std::ostream &operator<<(std::ostream "
                     "&os, const F &) { return os; } };")
                        .c_str()),
            std::string(kChronoStub) +
                "struct F { friend auto operator<<(std::ostream &os, const F "
                "&) -> std::ostream & { return os; } };");
}

TEST(TrailingReturnTypes, DefaultedComparisonOperator) {
  EXPECT_EQ(rewrite(
                R"cpp(
#include <compare>
                  struct F {
                    auto operator<=>(const F&) const = default;
                  };
                )cpp"),
            R"cpp(
#include <compare>
              struct F {
                auto operator<=>(const F&) const = default;
              };
            )cpp");
}
