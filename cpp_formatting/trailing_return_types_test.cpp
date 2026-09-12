#include <gtest/gtest.h>

#include "cpp_formatting/trailing_return_types_lib.h"

// ---------------------------------------------------------------------------
// Helper
// ---------------------------------------------------------------------------

// Thin wrapper so tests read as: rewrite("...") == "..."
static auto rewrite(const char* code) -> std::string {
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
  // clang-format off
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
  // clang-format on
}

TEST(TrailingReturnTypes, NestedReturnTypeOutOfLineDefinition) {
  // A member returning a type nested in its own class.  In the in-class
  // declaration the trailing `-> V` needs no qualification; in the out-of-line
  // definition the written type is already `S::V` and is moved across as-is.
  EXPECT_EQ(rewrite("struct S {\n"
                    "  struct V {};\n"
                    "  V get();\n"
                    "};\n"
                    "S::V S::get() { return {}; }\n"),
            "struct S {\n"
            "  struct V {};\n"
            "  auto get() -> V;\n"
            "};\n"
            "auto S::get() -> S::V { return {}; }\n");
}

// ---------------------------------------------------------------------------
// C++23 explicit object parameters ("deducing this", P0847)
//
// A member function with an explicit object parameter (`this Self self`) is
// rewritten exactly like an ordinary member function: only the return type is
// hoisted; the `this`-parameter is left untouched.  The `-std=c++23` helper is
// required because the syntax is not valid before C++23.
// ---------------------------------------------------------------------------

static auto rewrite23(const char* code) -> std::string {
  return rewriteToTrailingReturnTypes(code, {"-std=c++23", "-xc++"});
}

TEST(TrailingReturnTypesDeducingThis, ByConstReference) {
  EXPECT_EQ(
      rewrite23("struct S { int get(this const S& self) { return 42; } };"),
      "struct S { auto get(this const S& self) -> int { return 42; } };");
}

TEST(TrailingReturnTypesDeducingThis, ByValue) {
  EXPECT_EQ(
      rewrite23("struct S { int v; int get(this S self) { return self.v; } };"),
      "struct S { int v; auto get(this S self) -> int { return self.v; } };");
}

TEST(TrailingReturnTypesDeducingThis, ReferenceReturn) {
  EXPECT_EQ(rewrite23("struct S { int v; int& ref(this S& self) { return "
                      "self.v; } };"),
            "struct S { int v; auto ref(this S& self) -> int& { return self.v; "
            "} };");
}

TEST(TrailingReturnTypesDeducingThis, TemplatedSelf) {
  // `this auto&& self` is an abbreviated function template; the explicit `int`
  // return type must still be hoisted.
  EXPECT_EQ(
      rewrite23(
          "struct S { int v; int get(this auto&& self) { return self.v; } };"),
      "struct S { int v; auto get(this auto&& self) -> int { return self.v; } "
      "};");
}

TEST(TrailingReturnTypesDeducingThis, VoidNotRewritten) {
  const char* code =
      "struct S { void noop(this const S& self) { (void)self; } };";
  EXPECT_EQ(rewrite23(code), code);
}

TEST(TrailingReturnTypesDeducingThis, AlreadyTrailingNotRewritten) {
  const char* code =
      "struct S { auto get(this const S& self) -> int { return 42; } };";
  EXPECT_EQ(rewrite23(code), code);
}

TEST(TrailingReturnTypesDeducingThis, AutoDeducedNotRewritten) {
  const char* code =
      "struct S { int v; auto get(this const S& self) { return self.v; } };";
  EXPECT_EQ(rewrite23(code), code);
}

TEST(TrailingReturnTypes, TemplateInstantiationRewrittenOnce) {
  // An implicit instantiation shares the pattern's source locations. If it is
  // matched too, the same declaration is rewritten twice -- the second time
  // reading back the `auto` the first one wrote.
  EXPECT_EQ(rewrite("class Message {\n"
                    " public:\n"
                    "  template <typename T>\n"
                    "  Message& append(const T& v) {\n"
                    "    (void)v;\n"
                    "    return *this;\n"
                    "  }\n"
                    "};\n"
                    "inline void use() {\n"
                    "  Message m;\n"
                    "  m.append(1);\n"
                    "}\n"),
            "class Message {\n"
            " public:\n"
            "  template <typename T>\n"
            "  auto append(const T& v) -> Message& {\n"
            "    (void)v;\n"
            "    return *this;\n"
            "  }\n"
            "};\n"
            "inline void use() {\n"
            "  Message m;\n"
            "  m.append(1);\n"
            "}\n");
}

TEST(TrailingReturnTypes, MemberTemplateOfInstantiatedClassRewrittenOnce) {
  // Instantiating a class template re-creates its member function *templates*
  // as patterns: those keep TSK_Undeclared while still pointing back at the
  // original source locations, so only the "inside an instantiation" arm of
  // isInstantiated() keeps them from being rewritten a second time.
  EXPECT_EQ(rewrite("template <typename U>\n"
                    "class Box {\n"
                    " public:\n"
                    "  template <typename P>\n"
                    "  static const int* get(const Box& b) {\n"
                    "    (void)b;\n"
                    "    return nullptr;\n"
                    "  }\n"
                    "};\n"
                    "inline void use() {\n"
                    "  Box<int> b;\n"
                    "  Box<int>::get<char>(b);\n"
                    "}\n"),
            "template <typename U>\n"
            "class Box {\n"
            " public:\n"
            "  template <typename P>\n"
            "  static auto get(const Box& b) -> const int* {\n"
            "    (void)b;\n"
            "    return nullptr;\n"
            "  }\n"
            "};\n"
            "inline void use() {\n"
            "  Box<int> b;\n"
            "  Box<int>::get<char>(b);\n"
            "}\n");
}

TEST(TrailingReturnTypes, FunctionReturningFunctionPointerNotRewritten) {
  // The return type's source range wraps the name and parameters here, so
  // there is no prefix to hoist -- moving it would duplicate the declarator.
  const char* code = "int (*make(int))(bool);";
  EXPECT_EQ(rewrite(code), code);
}

TEST(TrailingReturnTypes, TypedefedFunctionPointerReturnIsRewritten) {
  // Spelled through an alias the return type *is* a plain prefix, so it is
  // rewritten as usual -- the guard above must not over-reach.
  EXPECT_EQ(rewrite("using fn_ptr = int (*)(bool);\nfn_ptr make(int);\n"),
            "using fn_ptr = int (*)(bool);\nauto make(int) -> fn_ptr;\n");
}

// ---------------------------------------------------------------------------
// Leading return types -- the reverse direction
// ---------------------------------------------------------------------------

// Thin wrapper so tests read as: unwind("...") == "..."
static auto unwind(const char* code) -> std::string {
  return rewriteToLeadingReturnTypes(code, {"-std=c++20", "-xc++"});
}

TEST(LeadingReturnTypes, IntReturn) {
  EXPECT_EQ(unwind("auto foo() -> int { return 42; }"),
            "int foo() { return 42; }");
}

TEST(LeadingReturnTypes, VoidReturn) {
  // Nothing excludes void on this side: `-> void` moves back like any type.
  EXPECT_EQ(unwind("auto foo() -> void {}"), "void foo() {}");
}

TEST(LeadingReturnTypes, DeclarationOnly) {
  EXPECT_EQ(unwind("auto foo() -> double;"), "double foo();");
}

TEST(LeadingReturnTypes, PointerReturn) {
  EXPECT_EQ(unwind("auto foo() -> int* { return nullptr; }"),
            "int* foo() { return nullptr; }");
}

TEST(LeadingReturnTypes, ConstQualifiedPointerReturn) {
  // The cv-qualifier is not in the TypeLoc's range; the backwards scan has to
  // pick it up here exactly as it does in the forward direction.
  EXPECT_EQ(unwind("auto foo() -> const int* { return nullptr; }"),
            "const int* foo() { return nullptr; }");
}

TEST(LeadingReturnTypes, ReferenceReturn) {
  EXPECT_EQ(unwind("int g;\nauto foo() -> int& { return g; }\n"),
            "int g;\nint& foo() { return g; }\n");
}

TEST(LeadingReturnTypes, MultiWordBuiltin) {
  EXPECT_EQ(unwind("auto foo() -> unsigned long long { return 0; }"),
            "unsigned long long foo() { return 0; }");
}

TEST(LeadingReturnTypes, SpecifiersKeepTheirPlace) {
  // The placeholder is replaced in place, so everything written in front of it
  // stays in front of it.
  EXPECT_EQ(
      unwind("struct S { static constexpr auto f() -> int { return 1; } };"),
      "struct S { static constexpr int f() { return 1; } };");
}

TEST(LeadingReturnTypes, TrailingQualifiersStay) {
  EXPECT_EQ(
      unwind("struct S { auto f() const noexcept -> int { return 1; } };"),
      "struct S { int f() const noexcept { return 1; } };");
}

TEST(LeadingReturnTypes, ArrowOnItsOwnLine) {
  EXPECT_EQ(unwind("auto foo()\n    -> int { return 1; }"),
            "int foo() { return 1; }");
}

TEST(LeadingReturnTypes, TemplatedReturnType) {
  EXPECT_EQ(unwind("template <class T> struct V {};\n"
                   "auto foo() -> V<int> { return {}; }\n"),
            "template <class T> struct V {};\n"
            "V<int> foo() { return {}; }\n");
}

TEST(LeadingReturnTypes, MemberFunctionInClass) {
  // Declared in-class: the trailing type already resolves in class scope and
  // so does the leading one, so a member type is fine here.
  EXPECT_EQ(unwind("struct S {\n"
                   "  struct Inner {};\n"
                   "  auto f() -> Inner { return {}; }\n"
                   "};\n"),
            "struct S {\n"
            "  struct Inner {};\n"
            "  Inner f() { return {}; }\n"
            "};\n");
}

TEST(LeadingReturnTypes, OperatorWithoutSpace) {
  EXPECT_EQ(
      unwind("struct S { auto operator=(const S&) -> S& { return *this; } };"),
      "struct S { S& operator=(const S&) { return *this; } };");
}

// --- guards ----------------------------------------------------------------

TEST(LeadingReturnTypes, LambdaNotRewritten) {
  // A lambda has no declarator-id to put a leading return type on.
  const char* code = "auto x = []() -> int { return 1; };";
  EXPECT_EQ(unwind(code), code);
}

TEST(LeadingReturnTypes, FunctionPointerReturnNotRewritten) {
  // `int (*)(bool)` before the name is `int (*make(int))(bool)` -- a different
  // declarator, not the same text moved.
  const char* code = "auto make(int) -> int (*)(bool);";
  EXPECT_EQ(unwind(code), code);
}

TEST(LeadingReturnTypes, ArrayReferenceReturnNotRewritten) {
  const char* code = "int a[4];\nauto get() -> int (&)[4] { return a; }\n";
  EXPECT_EQ(unwind(code), code);
}

TEST(LeadingReturnTypes, TypedefedFunctionPointerReturnIsRewritten) {
  // Through an alias it is a plain prefix again -- the shape guard must not
  // over-reach.
  EXPECT_EQ(
      unwind("using fn_ptr = int (*)(bool);\nauto make(int) -> fn_ptr;\n"),
      "using fn_ptr = int (*)(bool);\nfn_ptr make(int);\n");
}

TEST(LeadingReturnTypes, DeducedPlaceholderNotRewritten) {
  const char* code = "auto foo() -> auto { return 1; }";
  EXPECT_EQ(unwind(code), code);
}

TEST(LeadingReturnTypes, DecltypeAutoPlaceholderNotRewritten) {
  const char* code = "auto foo() -> decltype(auto) { return 1; }";
  EXPECT_EQ(unwind(code), code);
}

TEST(LeadingReturnTypes, ParameterReferenceNotRewritten) {
  // `a` is not in scope before the declarator-id.
  const char* code =
      "struct B { int size() const { return 0; } };\n"
      "auto f(const B& a) -> decltype(a.size()) { return a.size(); }\n";
  EXPECT_EQ(unwind(code), code);
}

TEST(LeadingReturnTypes, DependentParameterReferenceNotRewritten) {
  // The dependent spelling carries no resolved ParmVarDecl, which is why the
  // guard works on token text.
  const char* code =
      "template <class T>\n"
      "auto f(T& a) -> decltype(a.get()) { return a.get(); }\n";
  EXPECT_EQ(unwind(code), code);
}

TEST(LeadingReturnTypes, OutOfLineMemberTypeNotRewritten) {
  // `Inner` is found in class scope after the declarator-id, but not before it.
  const char* code =
      "struct S {\n"
      "  struct Inner {};\n"
      "  auto f() -> Inner;\n"
      "};\n"
      "auto S::f() -> Inner { return {}; }\n";
  // The in-class declaration still moves; the out-of-line definition must not.
  EXPECT_EQ(unwind(code),
            "struct S {\n"
            "  struct Inner {};\n"
            "  Inner f();\n"
            "};\n"
            "auto S::f() -> Inner { return {}; }\n");
}

TEST(LeadingReturnTypes, OutOfLineMemberTypeInTemplateArgNotRewritten) {
  // `std::vector` is written qualified and would be fine, but `Inner` inside
  // its argument list is not -- template arguments have to be checked too.
  const char* code =
      "namespace ns { template <class T> struct Vec {}; }\n"
      "struct S {\n"
      "  struct Inner {};\n"
      "  auto f() -> ns::Vec<Inner>;\n"
      "};\n"
      "auto S::f() -> ns::Vec<Inner> { return {}; }\n";
  EXPECT_EQ(unwind(code),
            "namespace ns { template <class T> struct Vec {}; }\n"
            "struct S {\n"
            "  struct Inner {};\n"
            "  ns::Vec<Inner> f();\n"
            "};\n"
            "auto S::f() -> ns::Vec<Inner> { return {}; }\n");
}

TEST(LeadingReturnTypes, OutOfLineQualifiedTypeIsRewritten) {
  // A name written qualified resolves the same from either position, so the
  // out-of-line guard must not reject the common case.
  EXPECT_EQ(unwind("namespace ns { struct T {}; }\n"
                   "struct S { auto f() -> ns::T; };\n"
                   "auto S::f() -> ns::T { return {}; }\n"),
            "namespace ns { struct T {}; }\n"
            "struct S { ns::T f(); };\n"
            "ns::T S::f() { return {}; }\n");
}

TEST(LeadingReturnTypes, OutOfLineBuiltinIsRewritten) {
  EXPECT_EQ(unwind("struct S { auto f() -> int; };\n"
                   "auto S::f() -> int { return 1; }\n"),
            "struct S { int f(); };\n"
            "int S::f() { return 1; }\n");
}

TEST(LeadingReturnTypes, OutOfLineTemplateParameterIsRewritten) {
  // A template parameter is introduced by the parameter list, which precedes
  // the declarator either way.
  EXPECT_EQ(unwind("template <class T> struct S { auto f() -> T; };\n"
                   "template <class T> auto S<T>::f() -> T { return {}; }\n"),
            "template <class T> struct S { T f(); };\n"
            "template <class T> T S<T>::f() { return {}; }\n");
}

TEST(LeadingReturnTypes, TemplateInstantiationRewrittenOnce) {
  // The pattern is rewritten; the instantiation points back at the same
  // locations and must not be rewritten a second time.
  EXPECT_EQ(unwind("template <class T> auto id(T v) -> T { return v; }\n"
                   "inline void use() { id<int>(1); }\n"),
            "template <class T> T id(T v) { return v; }\n"
            "inline void use() { id<int>(1); }\n");
}

TEST(LeadingReturnTypes, MacroReturnTypeNotRewritten) {
  const char* code = "#define INT_T int\nauto foo() -> INT_T { return 1; }\n";
  EXPECT_EQ(unwind(code), code);
}

// --- round trip ------------------------------------------------------------

TEST(LeadingReturnTypes, RoundTripsWithTrailingDirection) {
  const char* original =
      "struct S {\n"
      "  static const int* get(const S& s);\n"
      "  int value() const noexcept;\n"
      "};\n";
  std::string forward =
      rewriteToTrailingReturnTypes(original, {"-std=c++20", "-xc++"});
  EXPECT_EQ(forward,
            "struct S {\n"
            "  static auto get(const S& s) -> const int*;\n"
            "  auto value() const noexcept -> int;\n"
            "};\n");
  EXPECT_EQ(rewriteToLeadingReturnTypes(forward, {"-std=c++20", "-xc++"}),
            original);
}

TEST(LeadingReturnTypes, IsAFixpoint) {
  const std::string once = unwind("auto foo() -> int { return 1; }");
  EXPECT_EQ(unwind(once.c_str()), once);
}

TEST(TrailingReturnTypes, MacroInReturnTypeNotRewritten) {
  // abseil's `const ElfW(Phdr)* GetPhdr(int) const;`. The type's spelling comes
  // partly from a macro, so its source range does not cover the written text:
  // hoisting it produced `const ElfW(Phdr)auto GetPhdr(int) const -> *;`.
  const char* code =
      "#define ELFW(x) Elf64_##x\n"
      "struct Elf64_Phdr {};\n"
      "struct S {\n"
      "  const ELFW(Phdr) * GetPhdr(int index) const;\n"
      "};\n";
  EXPECT_EQ(rewrite(code), code);
}

TEST(TrailingReturnTypes, MacroElsewhereStillRewritten) {
  // The guard must be about the *return type* only -- a macro in the body or
  // the parameter list is no reason to skip.
  EXPECT_EQ(rewrite("#define ZERO 0\n"
                    "int value(int n) { return n + ZERO; }\n"),
            "#define ZERO 0\n"
            "auto value(int n) -> int { return n + ZERO; }\n");
}

TEST(TrailingReturnTypes, TrailingAttributeNotRewritten) {
  // abseil's `pointer data() noexcept ABSL_ATTRIBUTE_LIFETIME_BOUND;`. The
  // attribute is part of parameters-and-qualifiers, so `-> pointer` would have
  // to follow it; inserting at the qualifier end puts it on the wrong side.
  const char* code =
      "#define LB [[clang::lifetimebound]]\n"
      "struct S {\n"
      "  int* data() noexcept LB;\n"
      "};\n";
  EXPECT_EQ(rewrite(code), code);
}

TEST(TrailingReturnTypes, LeadingAttributeStillRewritten) {
  // An attribute *before* the declaration is unaffected by where the arrow
  // goes, so it must still be rewritten.
  EXPECT_EQ(rewrite("[[nodiscard]] int value();\n"),
            "[[nodiscard]] auto value() -> int;\n");
}
