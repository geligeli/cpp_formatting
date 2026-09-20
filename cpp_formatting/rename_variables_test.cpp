#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cpp_formatting/naming_convention.h"
#include "cpp_formatting/rename_variables_lib.h"
#include "llvm/ADT/StringRef.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Callback that appends a suffix to every variable it encounters.
static VariableRenameCallback addSuffix(std::string suffix) {
  return [suffix](std::string_view name, std::string& newName) {
    newName = std::string(name) + suffix;
    return true;
  };
}

// Callback that renames exactly one name.
static VariableRenameCallback renameOne(std::string from, std::string to) {
  return [from, to](std::string_view name, std::string& newName) -> bool {
    if (name == from) {
      newName = to;
      return true;
    }
    return false;
  };
}

static auto rewriteMember(const char* code, VariableRenameCallback cb)
    -> std::string {
  return rewriteVariableNames(code, std::move(cb), VariableScope::Member,
                              {"-std=c++20", "-xc++"});
}

static auto rewriteLocal(const char* code, VariableRenameCallback cb)
    -> std::string {
  return rewriteVariableNames(code, std::move(cb), VariableScope::Local,
                              {"-std=c++20", "-xc++"});
}

static auto rewriteGlobal(const char* code, VariableRenameCallback cb)
    -> std::string {
  return rewriteVariableNames(code, std::move(cb), VariableScope::Global,
                              {"-std=c++20", "-xc++"});
}

static auto rewriteStaticMember(const char* code, VariableRenameCallback cb)
    -> std::string {
  return rewriteVariableNames(code, std::move(cb), VariableScope::StaticMember,
                              {"-std=c++20", "-xc++"});
}

static auto rewriteConstMember(const char* code, VariableRenameCallback cb)
    -> std::string {
  return rewriteVariableNames(code, std::move(cb), VariableScope::ConstMember,
                              {"-std=c++20", "-xc++"});
}

static auto rewriteStaticGlobal(const char* code, VariableRenameCallback cb)
    -> std::string {
  return rewriteVariableNames(code, std::move(cb), VariableScope::StaticGlobal,
                              {"-std=c++20", "-xc++"});
}

static auto rewriteConstGlobal(const char* code, VariableRenameCallback cb)
    -> std::string {
  return rewriteVariableNames(code, std::move(cb), VariableScope::ConstGlobal,
                              {"-std=c++20", "-xc++"});
}

// ---------------------------------------------------------------------------
// Member variables
// ---------------------------------------------------------------------------

TEST(RenameMemberVariables, DeclarationAndImplicitThis) {
  EXPECT_EQ(rewriteMember(
                "struct S { int value_; int get() const { return value_; } };",
                renameOne("value_", "count_")),
            "struct S { int count_; int get() const { return count_; } };");
}

TEST(RenameMemberVariables, DotAccess) {
  EXPECT_EQ(rewriteMember("struct S { int x_; }; int f(S s) { return s.x_; }",
                          renameOne("x_", "y_")),
            "struct S { int y_; }; int f(S s) { return s.y_; }");
}

TEST(RenameMemberVariables, ArrowAccess) {
  EXPECT_EQ(rewriteMember("struct S { int x_; }; int f(S* s) { return s->x_; }",
                          renameOne("x_", "y_")),
            "struct S { int y_; }; int f(S* s) { return s->y_; }");
}

TEST(RenameMemberVariables, MultipleMembers) {
  EXPECT_EQ(rewriteMember("struct S { int a_; int b_; };", addSuffix("x")),
            "struct S { int a_x; int b_x; };");
}

TEST(RenameMemberVariables, SelectiveRename) {
  // Only "a_" is renamed; "b_" is unchanged.
  EXPECT_EQ(
      rewriteMember("struct S { int a_; int b_; };", renameOne("a_", "z_")),
      "struct S { int z_; int b_; };");
}

TEST(RenameMemberVariables, CallbackReturnsFalse) {
  const char* code = "struct S { int x_; };";
  EXPECT_EQ(
      rewriteMember(code, [](std::string_view, std::string&) { return false; }),
      code);
}

TEST(RenameMemberVariables, AssignmentInMethod) {
  EXPECT_EQ(rewriteMember("struct S { int x_; void set(int v) { x_ = v; } };",
                          renameOne("x_", "val_")),
            "struct S { int val_; void set(int v) { val_ = v; } };");
}

TEST(RenameMemberVariables, StaticDataMember) {
  EXPECT_EQ(rewriteMember("struct S { static int count_; }; int S::count_ = 0;",
                          renameOne("count_", "total_")),
            "struct S { static int total_; }; int S::total_ = 0;");
}

// ---------------------------------------------------------------------------
// Local variables
// ---------------------------------------------------------------------------

TEST(RenameLocalVariables, BasicLocal) {
  EXPECT_EQ(
      rewriteLocal("int f() { int x = 1; return x; }", renameOne("x", "y")),
      "int f() { int y = 1; return y; }");
}

TEST(RenameLocalVariables, MultipleLocals) {
  EXPECT_EQ(rewriteLocal("int f() { int a = 1; int b = 2; return a + b; }",
                         addSuffix("_")),
            "int f() { int a_ = 1; int b_ = 2; return a_ + b_; }");
}

TEST(RenameLocalVariables, FunctionParameter) {
  EXPECT_EQ(
      rewriteLocal("int identity(int x) { return x; }", renameOne("x", "val")),
      "int identity(int val) { return val; }");
}

TEST(RenameLocalVariables, DoesNotRenameGlobal) {
  const char* code = "int g = 0; int f() { return g; }";
  EXPECT_EQ(rewriteLocal(code, addSuffix("_")), code);
}

TEST(RenameLocalVariables, DoesNotRenameMember) {
  const char* code = "struct S { int x_; int get() { return x_; } };";
  EXPECT_EQ(rewriteLocal(code, addSuffix("_")), code);
}

TEST(RenameLocalVariables, LocalShadowsGlobal) {
  // Only the local "x" is renamed; the global "x" is untouched.
  EXPECT_EQ(rewriteLocal("int x = 0; int f() { int x = 1; return x; }",
                         renameOne("x", "y")),
            "int x = 0; int f() { int y = 1; return y; }");
}

// ---------------------------------------------------------------------------
// Global variables
// ---------------------------------------------------------------------------

TEST(RenameGlobalVariables, BasicGlobal) {
  EXPECT_EQ(rewriteGlobal("int counter = 0; int get() { return counter; }",
                          renameOne("counter", "total")),
            "int total = 0; int get() { return total; }");
}

TEST(RenameGlobalVariables, MultipleGlobals) {
  EXPECT_EQ(rewriteGlobal("int a = 1; int b = 2;", addSuffix("_g")),
            "int a_g = 1; int b_g = 2;");
}

TEST(RenameGlobalVariables, DoesNotRenameLocal) {
  const char* code = "int f() { int x = 1; return x; }";
  EXPECT_EQ(rewriteGlobal(code, addSuffix("_")), code);
}

TEST(RenameGlobalVariables, DoesNotRenameMember) {
  const char* code = "struct S { int x_; };";
  EXPECT_EQ(rewriteGlobal(code, addSuffix("_")), code);
}

TEST(RenameGlobalVariables, ExternDeclarationAndDefinition) {
  // Both the extern declaration and the definition carry the new name.
  EXPECT_EQ(rewriteGlobal("extern int g; int g = 0;", renameOne("g", "global")),
            "extern int global; int global = 0;");
}

TEST(RenameGlobalVariables, NamespaceScope) {
  EXPECT_EQ(
      rewriteGlobal("namespace ns { int val = 0; } int f() { return ns::val; }",
                    renameOne("val", "value")),
      "namespace ns { int value = 0; } int f() { return ns::value; }");
}

// ---------------------------------------------------------------------------
// Static data members (StaticMember scope)
// ---------------------------------------------------------------------------

TEST(RenameStaticMemberVariables, RenamesStaticDataMember) {
  EXPECT_EQ(
      rewriteStaticMember("struct S { static int count_; }; int S::count_ = 0;",
                          renameOne("count_", "total_")),
      "struct S { static int total_; }; int S::total_ = 0;");
}

TEST(RenameStaticMemberVariables, DoesNotRenameNonStaticField) {
  const char* code = "struct S { int x_; static int y_; };";
  EXPECT_EQ(rewriteStaticMember(code, addSuffix("x")),
            "struct S { int x_; static int y_x; };");
}

TEST(RenameStaticMemberVariables, RenamesStaticConstexprMember) {
  EXPECT_EQ(rewriteStaticMember("struct S { static constexpr int kMax = 42; };",
                                renameOne("kMax", "kLimit")),
            "struct S { static constexpr int kLimit = 42; };");
}

// ---------------------------------------------------------------------------
// Const/constexpr static members (ConstMember scope)
// ---------------------------------------------------------------------------

TEST(RenameConstMemberVariables, RenamesConstexprMember) {
  EXPECT_EQ(
      rewriteConstMember(
          "struct S { static constexpr int kMax = 42; static int count_; };",
          addSuffix("_r")),
      "struct S { static constexpr int kMax_r = 42; static int count_; };");
}

TEST(RenameConstMemberVariables, RenamesConstMember) {
  EXPECT_EQ(
      rewriteConstMember(
          "struct S { static const int kSize; }; const int S::kSize = 10;",
          renameOne("kSize", "kCapacity")),
      "struct S { static const int kCapacity; }; const int S::kCapacity = 10;");
}

TEST(RenameConstMemberVariables, DoesNotRenameNonConstStaticMember) {
  const char* code =
      "struct S { static int count_; static constexpr int kMax = 0; };";
  // Only the constexpr member is renamed.
  EXPECT_EQ(
      rewriteConstMember(code, addSuffix("_r")),
      "struct S { static int count_; static constexpr int kMax_r = 0; };");
}

TEST(RenameConstMemberVariables, DoesNotRenameNonStaticField) {
  const char* code = "struct S { int x_; static constexpr int kMax = 0; };";
  EXPECT_EQ(rewriteConstMember(code, addSuffix("_r")),
            "struct S { int x_; static constexpr int kMax_r = 0; };");
}

// ---------------------------------------------------------------------------
// Static-keyword globals (StaticGlobal scope)
// ---------------------------------------------------------------------------

TEST(RenameStaticGlobalVariables, RenamesStaticGlobal) {
  EXPECT_EQ(rewriteStaticGlobal(
                "static int sCounter = 0; int get() { return sCounter; }",
                renameOne("sCounter", "counter")),
            "static int counter = 0; int get() { return counter; }");
}

TEST(RenameStaticGlobalVariables, DoesNotRenameNonStaticGlobal) {
  const char* code = "int plain = 0; static int hidden = 1;";
  EXPECT_EQ(rewriteStaticGlobal(code, addSuffix("_s")),
            "int plain = 0; static int hidden_s = 1;");
}

TEST(RenameStaticGlobalVariables, DoesNotRenameStaticDataMember) {
  const char* code = "struct S { static int count_; }; int S::count_ = 0;";
  EXPECT_EQ(rewriteStaticGlobal(code, addSuffix("_s")), code);
}

TEST(RenameStaticGlobalVariables, DoesNotRenameLocal) {
  const char* code = "static int g = 0; int f() { int x = 1; return x + g; }";
  EXPECT_EQ(rewriteStaticGlobal(code, addSuffix("_s")),
            "static int g_s = 0; int f() { int x = 1; return x + g_s; }");
}

// ---------------------------------------------------------------------------
// Const/constexpr globals (ConstGlobal scope)
// ---------------------------------------------------------------------------

TEST(RenameConstGlobalVariables, RenamesConstexprGlobal) {
  EXPECT_EQ(
      rewriteConstGlobal("constexpr int kMax = 100; int f() { return kMax; }",
                         renameOne("kMax", "kLimit")),
      "constexpr int kLimit = 100; int f() { return kLimit; }");
}

TEST(RenameConstGlobalVariables, RenamesConstGlobal) {
  EXPECT_EQ(
      rewriteConstGlobal("const int kSize = 10; int f() { return kSize; }",
                         renameOne("kSize", "kCapacity")),
      "const int kCapacity = 10; int f() { return kCapacity; }");
}

TEST(RenameConstGlobalVariables, DoesNotRenameNonConstGlobal) {
  const char* code = "int mutable_ = 0; constexpr int kConst = 1;";
  EXPECT_EQ(rewriteConstGlobal(code, addSuffix("_r")),
            "int mutable_ = 0; constexpr int kConst_r = 1;");
}

TEST(RenameConstGlobalVariables, DoesNotRenameLocal) {
  const char* code = "constexpr int kMax = 5; int f() { int x = 1; return x; }";
  EXPECT_EQ(rewriteConstGlobal(code, addSuffix("_r")),
            "constexpr int kMax_r = 5; int f() { int x = 1; return x; }");
}

TEST(RenameConstGlobalVariables, DoesNotRenameConstMember) {
  const char* code =
      "struct S { static constexpr int kMax = 0; }; constexpr int kGlobal = 1;";
  EXPECT_EQ(rewriteConstGlobal(code, addSuffix("_r")),
            "struct S { static constexpr int kMax = 0; }; constexpr int "
            "kGlobal_r = 1;");
}

// ---------------------------------------------------------------------------
// Template member variables
// ---------------------------------------------------------------------------

TEST(RenameMemberVariables, TemplateMemberRenamedInInstantiation) {
  // The MemberExpr in non-template code references the instantiated FieldDecl;
  // the tool must walk up the instantiation chain to find the rename entry.
  EXPECT_EQ(rewriteMember("template<typename T> struct Box { T val_; };\n"
                          "int f() { Box<int> b; return b.val_; }",
                          renameOne("val_", "value_")),
            "template<typename T> struct Box { T value_; };\n"
            "int f() { Box<int> b; return b.value_; }");
}

// A member accessed through a template parameter (`x.val`) is a dependent
// expression with no resolved member in the template pattern.  When the same TU
// instantiates the template, the tool resolves the token via the instantiation
// and rewrites it consistently with the concrete member's rename.
TEST(RenameMemberVariables, DependentMemberThroughTemplateParam) {
  EXPECT_EQ(rewriteMember("template <class T> void set_val(T& x) "
                          "{ x.val = 12; }\n"
                          "struct A { int val; };\n"
                          "void use() { A a; set_val(a); }\n",
                          renameOne("val", "val_")),
            "template <class T> void set_val(T& x) { x.val_ = 12; }\n"
            "struct A { int val_; };\n"
            "void use() { A a; set_val(a); }\n");
}

// Same feature via the C++20 abbreviated-function-template spelling.
TEST(RenameMemberVariables, DependentMemberThroughAbbreviatedTemplate) {
  EXPECT_EQ(rewriteMember("auto set_val(auto& x) { x.val = 12; }\n"
                          "struct A { int val; };\n"
                          "void use() { A a; set_val(a); }\n",
                          renameOne("val", "val_")),
            "auto set_val(auto& x) { x.val_ = 12; }\n"
            "struct A { int val_; };\n"
            "void use() { A a; set_val(a); }\n");
}

// Two instantiations that agree on the new name still rewrite the shared token
// exactly once.
TEST(RenameMemberVariables, DependentMemberTwoInstantiationsAgree) {
  EXPECT_EQ(rewriteMember("template <class T> void set_val(T& x) "
                          "{ x.val = 12; }\n"
                          "struct A { int val; };\n"
                          "struct B { int val; };\n"
                          "void use() { A a; set_val(a); B b; set_val(b); }\n",
                          renameOne("val", "val_")),
            "template <class T> void set_val(T& x) { x.val_ = 12; }\n"
            "struct A { int val_; };\n"
            "struct B { int val_; };\n"
            "void use() { A a; set_val(a); B b; set_val(b); }\n");
}

// A template that is never instantiated gives no information about which member
// its dependent token binds to, so the token is left untouched (conservative).
TEST(RenameMemberVariables, DependentMemberUninstantiatedLeftAlone) {
  EXPECT_EQ(rewriteMember("template <class T> void set_val(T& x) "
                          "{ x.val = 12; }\n"
                          "struct A { int val; };\n",
                          renameOne("val", "val_")),
            "template <class T> void set_val(T& x) { x.val = 12; }\n"
            "struct A { int val_; };\n");
}

TEST(RenameMemberVariables, RenamesMemberInConstructorInitializer) {
  EXPECT_EQ(
      rewriteMember(
          "struct S { S() : val1_(0), val2_(2) {} int val1_; int val2_; };\n",
          renameOne("val1_", "renamed_")),
      "struct S { S() : renamed_(0), val2_(2) {} int renamed_; int val2_; "
      "};\n");
}

TEST(RenameMemberVariables, RenamesMemberInDesignatedInitializer) {
  EXPECT_EQ(rewriteMember("struct S { int value_; int other_; };\n"
                          "S s{.value_ = 1, .other_ = 2};\n",
                          renameOne("value_", "renamed_")),
            "struct S { int renamed_; int other_; };\n"
            "S s{.renamed_ = 1, .other_ = 2};\n");
}

// ---------------------------------------------------------------------------
// C++23 explicit object parameters ("deducing this", P0847)
//
// Access to a data member through the explicit object parameter (`self.value_`)
// is an ordinary MemberExpr, so member renames must rewrite it.  The explicit
// object parameter itself is a ParmVarDecl and participates in local renames.
// These helpers pass `-std=c++23` because the syntax is C++23-only.
// ---------------------------------------------------------------------------

static auto rewriteMember23(const char* code, VariableRenameCallback cb)
    -> std::string {
  return rewriteVariableNames(code, std::move(cb), VariableScope::Member,
                              {"-std=c++23", "-xc++"});
}

static auto rewriteLocal23(const char* code, VariableRenameCallback cb)
    -> std::string {
  return rewriteVariableNames(code, std::move(cb), VariableScope::Local,
                              {"-std=c++23", "-xc++"});
}

TEST(RenameMemberVariables, MemberAccessThroughExplicitObjectParam) {
  EXPECT_EQ(rewriteMember23("struct S { int value_; int get(this const S& "
                            "self) { return self.value_; } };",
                            renameOne("value_", "count_")),
            "struct S { int count_; int get(this const S& self) { return "
            "self.count_; } };");
}

TEST(RenameMemberVariables, MemberAccessThroughByValueSelf) {
  EXPECT_EQ(rewriteMember23("struct S { int x_; void set(this S self, int v) { "
                            "self.x_ = v; } };",
                            renameOne("x_", "y_")),
            "struct S { int y_; void set(this S self, int v) { self.y_ = v; } "
            "};");
}

TEST(RenameLocalVariables, RenamesExplicitObjectParameter) {
  // The explicit object parameter `self` is a ParmVarDecl (a local), so both
  // its declaration and its use as the base of `self.v` are renamed; the
  // member `v` is left untouched.
  EXPECT_EQ(rewriteLocal23("struct S { int v; int get(this const S& self) { "
                           "return self.v; } };",
                           renameOne("self", "me")),
            "struct S { int v; int get(this const S& me) { return me.v; } };");
}

// ---------------------------------------------------------------------------
// Member functions (Method scope)
// ---------------------------------------------------------------------------

static auto rewriteMethod(const char* code, VariableRenameCallback cb)
    -> std::string {
  return rewriteVariableNames(code, std::move(cb), VariableScope::Method,
                              {"-std=c++20", "-xc++"});
}

TEST(RenameMemberFunctions, DeclarationAndImplicitThisCall) {
  EXPECT_EQ(
      rewriteMethod("struct S { int getValue() const { return 1; } int use() { "
                    "return getValue(); } };",
                    renameOne("getValue", "value")),
      "struct S { int value() const { return 1; } int use() { return "
      "value(); } };");
}

TEST(RenameMemberFunctions, DotAndArrowCall) {
  EXPECT_EQ(
      rewriteMethod("struct S { int fetch(); };\n"
                    "int f(S& s, S* p) { return s.fetch() + p->fetch(); }",
                    renameOne("fetch", "value")),
      "struct S { int value(); };\n"
      "int f(S& s, S* p) { return s.value() + p->value(); }");
}

TEST(RenameMemberFunctions, OutOfLineDefinition) {
  EXPECT_EQ(rewriteMethod(
                "struct S { int getValue(); }; int S::getValue() { return 1; }",
                renameOne("getValue", "value")),
            "struct S { int value(); }; int S::value() { return 1; }");
}

TEST(RenameMemberFunctions, StaticMethod) {
  EXPECT_EQ(rewriteMethod("struct S { static int count(); };\n"
                          "int S::count() { return 0; }\n"
                          "int f() { return S::count(); }",
                          renameOne("count", "total")),
            "struct S { static int total(); };\n"
            "int S::total() { return 0; }\n"
            "int f() { return S::total(); }");
}

TEST(RenameMemberFunctions, PointerToMemberFunction) {
  EXPECT_EQ(rewriteMethod("struct S { int fetch(); };\n"
                          "auto p = &S::fetch;",
                          renameOne("fetch", "value")),
            "struct S { int value(); };\n"
            "auto p = &S::value;");
}

TEST(RenameMemberFunctions, OverloadedMethods) {
  EXPECT_EQ(rewriteMethod("struct S { int fetch(); int fetch(int); };\n"
                          "int f(S& s) { return s.fetch() + s.fetch(1); }",
                          renameOne("fetch", "value")),
            "struct S { int value(); int value(int); };\n"
            "int f(S& s) { return s.value() + s.value(1); }");
}

TEST(RenameMemberFunctions, DoesNotRenameCtorDtorOrOperators) {
  EXPECT_EQ(rewriteMethod("struct S { S(); ~S(); operator int() const; bool "
                          "operator==(const S&) const; int fetch(); };",
                          addSuffix("_x")),
            "struct S { S(); ~S(); operator int() const; bool "
            "operator==(const S&) const; int fetch_x(); };");
}

TEST(RenameMemberFunctions, DoesNotRenameFreeFunctions) {
  EXPECT_EQ(rewriteMethod("int getValue() { return 1; }",
                          renameOne("getValue", "value")),
            "int getValue() { return 1; }");
}

TEST(RenameMemberFunctions, VirtualOverrideHierarchyRenamedTogether) {
  // The base declaration, every override, and every call site — through base
  // or derived — all carry the new name.
  EXPECT_EQ(rewriteMethod("struct B { virtual int fetch() const; };\n"
                          "struct D : B { int fetch() const override; };\n"
                          "int f(B& b, D& d) { return b.fetch() + d.fetch(); }",
                          renameOne("fetch", "value")),
            "struct B { virtual int value() const; };\n"
            "struct D : B { int value() const override; };\n"
            "int f(B& b, D& d) { return b.value() + d.value(); }");
}

TEST(RenameMemberFunctions, TemplateMethodRenamedInInstantiation) {
  // The MemberExpr in non-template code references the instantiated
  // CXXMethodDecl; the tool must walk up the instantiation chain.
  EXPECT_EQ(rewriteMethod("template<typename T> struct Box { T fetch(); };\n"
                          "int f() { Box<int> b; return b.fetch(); }",
                          renameOne("fetch", "value")),
            "template<typename T> struct Box { T value(); };\n"
            "int f() { Box<int> b; return b.value(); }");
}

TEST(RenameMemberFunctions, CoroutineMethod) {
  // A coroutine member function (its body contains a co_return) must be
  // renamed at the declaration, the out-of-line definition, and call sites.
  const char* code =
      "#include <coroutine>\n"
      "struct Task {\n"
      "  struct promise_type {\n"
      "    Task get_return_object() { return {}; }\n"
      "    std::suspend_never initial_suspend() const noexcept { return {}; }\n"
      "    std::suspend_never final_suspend() const noexcept { return {}; }\n"
      "    void return_void() {}\n"
      "    void unhandled_exception() {}\n"
      "  };\n"
      "};\n"
      "struct Worker { Task runTask(); };\n"
      "Task Worker::runTask() { co_return; }\n"
      "Task f(Worker& w) { return w.runTask(); }";
  const char* expected =
      "#include <coroutine>\n"
      "struct Task {\n"
      "  struct promise_type {\n"
      "    Task get_return_object() { return {}; }\n"
      "    std::suspend_never initial_suspend() const noexcept { return {}; }\n"
      "    std::suspend_never final_suspend() const noexcept { return {}; }\n"
      "    void return_void() {}\n"
      "    void unhandled_exception() {}\n"
      "  };\n"
      "};\n"
      "struct Worker { Task execute(); };\n"
      "Task Worker::execute() { co_return; }\n"
      "Task f(Worker& w) { return w.execute(); }";
  EXPECT_EQ(rewriteMethod(code, renameOne("runTask", "execute")), expected);
}

TEST(RenameMemberFunctions, CoroutineLambdaCallSite) {
  // A call to a coroutine member function from inside a coroutine lambda.
  // The lambda's operator() desugaring references methods without a simple
  // identifier (e.g. conversion operators); the rename visitors must not
  // trip getName()'s isIdentifier() assertion on them.
  const char* code =
      "#include <coroutine>\n"
      "struct Task {\n"
      "  struct promise_type {\n"
      "    Task get_return_object() { return {}; }\n"
      "    std::suspend_never initial_suspend() const noexcept { return {}; }\n"
      "    std::suspend_never final_suspend() const noexcept { return {}; }\n"
      "    void return_void() {}\n"
      "    void unhandled_exception() {}\n"
      "  };\n"
      "  bool await_ready() const noexcept { return true; }\n"
      "  void await_suspend(std::coroutine_handle<>) const noexcept {}\n"
      "  void await_resume() const noexcept {}\n"
      "};\n"
      "struct W {\n"
      "  Task runTask();\n"
      "  Task runAll() {\n"
      "    auto lam = [this]() -> Task { co_await runTask(); };\n"
      "    co_await lam();\n"
      "  }\n"
      "};\n"
      "Task W::runTask() { co_return; }";
  const char* expected =
      "#include <coroutine>\n"
      "struct Task {\n"
      "  struct promise_type {\n"
      "    Task get_return_object() { return {}; }\n"
      "    std::suspend_never initial_suspend() const noexcept { return {}; }\n"
      "    std::suspend_never final_suspend() const noexcept { return {}; }\n"
      "    void return_void() {}\n"
      "    void unhandled_exception() {}\n"
      "  };\n"
      "  bool await_ready() const noexcept { return true; }\n"
      "  void await_suspend(std::coroutine_handle<>) const noexcept {}\n"
      "  void await_resume() const noexcept {}\n"
      "};\n"
      "struct W {\n"
      "  Task execute();\n"
      "  Task runAll() {\n"
      "    auto lam = [this]() -> Task { co_await execute(); };\n"
      "    co_await lam();\n"
      "  }\n"
      "};\n"
      "Task W::execute() { co_return; }";
  EXPECT_EQ(rewriteMethod(code, renameOne("runTask", "execute")), expected);
}

// Awaitable task type shared by the coroutine tests below.
static const char kAwaitableTask[] =
    "#include <coroutine>\n"
    "struct Task {\n"
    "  struct promise_type {\n"
    "    Task get_return_object() { return {}; }\n"
    "    std::suspend_never initial_suspend() const noexcept { return {}; }\n"
    "    std::suspend_never final_suspend() const noexcept { return {}; }\n"
    "    void return_void() {}\n"
    "    void unhandled_exception() {}\n"
    "  };\n"
    "  bool await_ready() const noexcept { return true; }\n"
    "  void await_suspend(std::coroutine_handle<>) const noexcept {}\n"
    "  int await_resume() const noexcept { return 0; }\n"
    "};\n";

TEST(RenameMemberFunctions, CoroutineMethodImplicitThisCoAwait) {
  // Call sites through implicit and explicit `this` inside another coroutine
  // member of the same class.
  std::string code = std::string(kAwaitableTask) +
                     "struct Worker {\n"
                     "  Task runTask();\n"
                     "  Task runAll() {\n"
                     "    co_await runTask();\n"
                     "    co_await this->runTask();\n"
                     "  }\n"
                     "};\n"
                     "Task Worker::runTask() { co_return; }";
  std::string expected = std::string(kAwaitableTask) +
                         "struct Worker {\n"
                         "  Task execute();\n"
                         "  Task runAll() {\n"
                         "    co_await execute();\n"
                         "    co_await this->execute();\n"
                         "  }\n"
                         "};\n"
                         "Task Worker::execute() { co_return; }";
  EXPECT_EQ(rewriteMethod(code.c_str(), renameOne("runTask", "execute")),
            expected);
}

TEST(RenameMemberFunctions, CoroutineMethodCoAwaitWithArguments) {
  // co_await on a member call with arguments, from a free coroutine.
  std::string code = std::string(kAwaitableTask) +
                     "struct Worker { Task runTask(int count); };\n"
                     "Task Worker::runTask(int count) { co_return; }\n"
                     "Task g(Worker& w) {\n"
                     "  int total = co_await w.runTask(1);\n"
                     "  total += co_await w.runTask(2);\n"
                     "  co_return;\n"
                     "}";
  std::string expected = std::string(kAwaitableTask) +
                         "struct Worker { Task execute(int count); };\n"
                         "Task Worker::execute(int count) { co_return; }\n"
                         "Task g(Worker& w) {\n"
                         "  int total = co_await w.execute(1);\n"
                         "  total += co_await w.execute(2);\n"
                         "  co_return;\n"
                         "}";
  EXPECT_EQ(rewriteMethod(code.c_str(), renameOne("runTask", "execute")),
            expected);
}

TEST(RenameMemberFunctions, CoroutineMethodVirtualOverrideHierarchy) {
  // Virtual coroutine member functions: the whole override hierarchy and all
  // call sites (co_await or plain) are renamed together.
  std::string code = std::string(kAwaitableTask) +
                     "struct Base { virtual Task runTask(); };\n"
                     "struct Derived : Base { Task runTask() override; };\n"
                     "Task Base::runTask() { co_return; }\n"
                     "Task Derived::runTask() { co_return; }\n"
                     "Task f(Base& b) { co_await b.runTask(); }\n"
                     "Task g(Base& b) { return b.runTask(); }";
  std::string expected = std::string(kAwaitableTask) +
                         "struct Base { virtual Task execute(); };\n"
                         "struct Derived : Base { Task execute() override; };\n"
                         "Task Base::execute() { co_return; }\n"
                         "Task Derived::execute() { co_return; }\n"
                         "Task f(Base& b) { co_await b.execute(); }\n"
                         "Task g(Base& b) { return b.execute(); }";
  EXPECT_EQ(rewriteMethod(code.c_str(), renameOne("runTask", "execute")),
            expected);
}

TEST(RenameMemberFunctions, CoroutineMethodInClassTemplate) {
  // The MemberExpr at the call site references the instantiated
  // CXXMethodDecl; the tool must walk back to the primary template.
  std::string code =
      std::string(kAwaitableTask) +
      "template <typename T> struct Box { Task getTask(); };\n"
      "template <typename T> Task Box<T>::getTask() { co_return; "
      "}\n"
      "Task f() { Box<int> b; return b.getTask(); }";
  std::string expected =
      std::string(kAwaitableTask) +
      "template <typename T> struct Box { Task fetch(); };\n"
      "template <typename T> Task Box<T>::fetch() { co_return; }\n"
      "Task f() { Box<int> b; return b.fetch(); }";
  EXPECT_EQ(rewriteMethod(code.c_str(), renameOne("getTask", "fetch")),
            expected);
}

TEST(RenameMemberFunctions, StaticCoroutineMethod) {
  std::string code = std::string(kAwaitableTask) +
                     "struct S { static Task makeTask(); };\n"
                     "Task S::makeTask() { co_return; }\n"
                     "Task g() { return S::makeTask(); }";
  std::string expected = std::string(kAwaitableTask) +
                         "struct S { static Task create(); };\n"
                         "Task S::create() { co_return; }\n"
                         "Task g() { return S::create(); }";
  EXPECT_EQ(rewriteMethod(code.c_str(), renameOne("makeTask", "create")),
            expected);
}

// ---------------------------------------------------------------------------
// Collision handling: a rename whose new name is already taken in the same
// scope is skipped entirely -- declaration and uses -- rather than applied.
// ---------------------------------------------------------------------------

TEST(RenameCollisions, MemberClashingWithMethodIsSkipped) {
  // googletest's RE has both `pattern_` and `pattern()`; renaming the member to
  // `pattern` would redeclare the method's name in the same class.
  const char* code =
      "struct RE {\n"
      "  const char* pattern() const { return pattern_; }\n"
      "  const char* pattern_;\n"
      "};\n";
  EXPECT_EQ(rewriteMember(code, renameOne("pattern_", "pattern")), code);
}

TEST(RenameCollisions, MemberClashingWithMemberIsSkipped) {
  const char* code =
      "struct S {\n"
      "  int count;\n"
      "  int countCache;\n"
      "};\n";
  EXPECT_EQ(rewriteMember(code, renameOne("countCache", "count")), code);
}

TEST(RenameCollisions, TwoMembersRenamingToTheSameNameKeepsOne) {
  // First one through claims the name; the second is left alone, so the result
  // still compiles.
  const char* code =
      "struct S {\n"
      "  int fooBar;\n"
      "  int foo_bar_;\n"
      "};\n";
  auto cb = [](llvm::StringRef name, std::string& newName) {
    if (name == "fooBar" || name == "foo_bar_") {
      newName = "foo_bar";
      return true;
    }
    return false;
  };
  EXPECT_EQ(rewriteMember(code, cb),
            "struct S {\n"
            "  int foo_bar;\n"
            "  int foo_bar_;\n"
            "};\n");
}

TEST(RenameCollisions, ShadowingAnInheritedNameIsAllowed) {
  // Not a collision: a derived member may legally shadow a base member, so the
  // guard must not over-reach and skip it.
  EXPECT_EQ(rewriteMember("struct B {\n"
                          "  int value;\n"
                          "};\n"
                          "struct D : B {\n"
                          "  int valueCache;\n"
                          "};\n",
                          renameOne("valueCache", "value")),
            "struct B {\n"
            "  int value;\n"
            "};\n"
            "struct D : B {\n"
            "  int value;\n"
            "};\n");
}

TEST(RenameCollisions, UnrelatedScopesDoNotCollide) {
  // Same name in a different class is not a clash.
  EXPECT_EQ(rewriteMember("struct A {\n"
                          "  int value;\n"
                          "};\n"
                          "struct B {\n"
                          "  int valueCache;\n"
                          "};\n",
                          renameOne("valueCache", "value")),
            "struct A {\n"
            "  int value;\n"
            "};\n"
            "struct B {\n"
            "  int value;\n"
            "};\n");
}

// ---------------------------------------------------------------------------
// Macros.  A name written as a macro *argument* is spelled at the call site and
// renames like any other; one spelled in a macro *body*, or formed by token
// pasting, has no byte range of its own and vetoes the whole rename.
// ---------------------------------------------------------------------------

TEST(RenameMacros, MacroArgumentIsRenamed) {
  EXPECT_EQ(rewriteMember("#define FWD(x) ((x) + 0)\n"
                          "struct S { int itemCount; };\n"
                          "int use(S& s) { return FWD(s.itemCount); }\n",
                          renameOne("itemCount", "item_count")),
            "#define FWD(x) ((x) + 0)\n"
            "struct S { int item_count; };\n"
            "int use(S& s) { return FWD(s.item_count); }\n");
}

TEST(RenameMacros, MacroArgumentForwardedThroughMacrosIsRenamed) {
  EXPECT_EQ(rewriteMember("#define FWD(x) ((x) + 0)\n"
                          "#define OUTER(y) FWD(y)\n"
                          "struct S { int itemCount; };\n"
                          "int use(S& s) { return OUTER(s.itemCount); }\n",
                          renameOne("itemCount", "item_count")),
            "#define FWD(x) ((x) + 0)\n"
            "#define OUTER(y) FWD(y)\n"
            "struct S { int item_count; };\n"
            "int use(S& s) { return OUTER(s.item_count); }\n");
}

TEST(RenameMacros, ArgumentExpandedTwiceIsRewrittenOnce) {
  // `s.itemCount` reaches the visitor as two MemberExprs sharing one spelling
  // location; rewriting it twice would corrupt the token.
  EXPECT_EQ(rewriteMember("#define TWICE(x) ((x) + (x))\n"
                          "struct S { int itemCount; };\n"
                          "int use(S& s) { return TWICE(s.itemCount); }\n",
                          renameOne("itemCount", "item_count")),
            "#define TWICE(x) ((x) + (x))\n"
            "struct S { int item_count; };\n"
            "int use(S& s) { return TWICE(s.item_count); }\n");
}

TEST(RenameMacros, DeclarationWrittenAsMacroArgumentIsRenamed) {
  EXPECT_EQ(rewriteMember("#define FIELD(n) int n;\n"
                          "struct S { FIELD(itemCount) };\n"
                          "int use(S& s) { return s.itemCount; }\n",
                          renameOne("itemCount", "item_count")),
            "#define FIELD(n) int n;\n"
            "struct S { FIELD(item_count) };\n"
            "int use(S& s) { return s.item_count; }\n");
}

TEST(RenameMacros, ReferenceInMacroBodyVetoesTheRename) {
  // The body token is one location shared by every expansion of BUMP, so it
  // cannot be rewritten -- and renaming the declaration alone would not
  // compile.  The whole rename is skipped.
  const char* code =
      "struct S { int itemCount; };\n"
      "#define BUMP(s) ((s).itemCount += 1)\n"
      "int use(S& s) { BUMP(s); return s.itemCount; }\n";
  EXPECT_EQ(rewriteMember(code, renameOne("itemCount", "item_count")), code);
}

TEST(RenameMacros, VetoLeavesOtherMembersAlone) {
  EXPECT_EQ(rewriteMember("struct S { int itemCount; int otherCount; };\n"
                          "#define BUMP(s) ((s).itemCount += 1)\n"
                          "int use(S& s) { BUMP(s); return s.otherCount; }\n",
                          addSuffix("_")),
            "struct S { int itemCount; int otherCount_; };\n"
            "#define BUMP(s) ((s).itemCount += 1)\n"
            "int use(S& s) { BUMP(s); return s.otherCount_; }\n");
}

TEST(RenameMacros, TokenPastedReferenceVetoesTheRename) {
  // `s.PASTE(item)` builds the name in Clang's scratch buffer; no file holds
  // it, so there is nothing to rewrite and the rename is skipped.
  const char* code =
      "struct S { int itemCount; };\n"
      "#define PASTE(p) p##Count\n"
      "int use(S& s) { return s.PASTE(item); }\n";
  EXPECT_EQ(rewriteMember(code, renameOne("itemCount", "item_count")), code);
}

TEST(RenameMacros, MethodReferencedFromMacroBodyVetoesWholeOverrideFamily) {
  // The macro names the override, but renaming the base alone would break
  // `override` checking -- the veto keys on the base-most declaration so the
  // whole hierarchy is skipped together.
  const char* code =
      "struct B { virtual int getVal() const { return 0; } };\n"
      "struct D : B { int getVal() const override { return 1; } };\n"
      "#define CALL(d) ((d).getVal())\n"
      "int use(D& d) { return CALL(d); }\n";
  EXPECT_EQ(rewriteMethod(code, renameOne("getVal", "get_val")), code);
}

TEST(RenameMacros, DependentMemberInMacroArgumentIsRenamed) {
  // googletest's TYPED_TEST shape: `this->member_` in a class template is a
  // dependent access, and it is written inside a macro argument.  The token is
  // spelled at the call site, so the cross-TU resolution keys on that and the
  // rewrite lands there.
  EXPECT_EQ(
      rewriteMember("#define CHECK(x) ((void)(x))\n"
                    "template <class T> struct Fixture { T itemCount; };\n"
                    "template <class T> struct Derived : Fixture<T> {\n"
                    "  void f() { CHECK(this->itemCount); }\n"
                    "};\n"
                    "template struct Derived<int>;\n",
                    renameOne("itemCount", "item_count")),
      "#define CHECK(x) ((void)(x))\n"
      "template <class T> struct Fixture { T item_count; };\n"
      "template <class T> struct Derived : Fixture<T> {\n"
      "  void f() { CHECK(this->item_count); }\n"
      "};\n"
      "template struct Derived<int>;\n");
}

TEST(RenameMacros, DependentMemberInMacroBodyIsLeftAlone) {
  // The other half: a dependent token spelled in a macro body has no location
  // of its own to rewrite, so it is not a resolution candidate at all.
  const char* code =
      "#define BUMP(p) ((p)->itemCount += 1)\n"
      "template <class T> struct Fixture { T itemCount; };\n"
      "template <class T> struct Derived : Fixture<T> {\n"
      "  void f() { BUMP(this); }\n"
      "};\n"
      "template struct Derived<int>;\n";
  EXPECT_EQ(rewriteMember(code, renameOne("itemCount", "item_count")), code);
}

// ---------------------------------------------------------------------------
// Members of a class nested inside a class template.  `Outer<int>::Inner` is an
// ordinary CXXRecordDecl instantiated from a member class, not a
// ClassTemplateSpecializationDecl, so mapping a field back to the pattern needs
// getInstantiatedFromMemberClass() as well.
// ---------------------------------------------------------------------------

TEST(RenameNestedTemplateClass, FieldUseInInstantiationIsRenamed) {
  EXPECT_EQ(
      rewriteMember("template <class T> struct Outer { struct Inner { int "
                    "itemCount; }; };\n"
                    "int use(Outer<int>::Inner& i) { return i.itemCount; }\n",
                    renameOne("itemCount", "item_count")),
      "template <class T> struct Outer { struct Inner { int "
      "item_count; }; };\n"
      "int use(Outer<int>::Inner& i) { return i.item_count; }\n");
}

TEST(RenameNestedTemplateClass, DependentUseThroughNestedClassIsRenamed) {
  // gtest-param-util.h's ValuesInIteratorRangeGenerator<T>::Iterator shape:
  // `Iterator` is dependent, so the access is a CXXDependentScopeMemberExpr and
  // the instantiation has to resolve it to the pattern's field.  While the
  // field failed to map back, every such resolution was vetoed instead.
  EXPECT_EQ(
      rewriteMember(
          "template <class U> const U* down(const void* p) { return "
          "static_cast<const U*>(p); }\n"
          "template <class T> struct Gen {\n"
          "  struct Iterator {\n"
          "    int itemCount;\n"
          "    bool eq(const void* o) const { return itemCount == down<const "
          "Iterator>(o)->itemCount; }\n"
          "  };\n"
          "};\n"
          "int use() { Gen<int>::Iterator i{0}; return i.eq(&i); }\n",
          renameOne("itemCount", "item_count")),
      "template <class U> const U* down(const void* p) { return "
      "static_cast<const U*>(p); }\n"
      "template <class T> struct Gen {\n"
      "  struct Iterator {\n"
      "    int item_count;\n"
      "    bool eq(const void* o) const { return item_count == down<const "
      "Iterator>(o)->item_count; }\n"
      "  };\n"
      "};\n"
      "int use() { Gen<int>::Iterator i{0}; return i.eq(&i); }\n");
}

TEST(RenameNestedTemplateClass, MethodAndStaticMemberAlreadyMapBack) {
  // These have a back-pointer of their own (getInstantiatedFromMemberFunction /
  // getInstantiatedFromStaticDataMember), so they never needed the positional
  // walk that fields do -- pinned so the asymmetry stays deliberate.
  const char* code =
      "template <class T> struct Outer {\n"
      "  struct Inner { static int sharedCount; int getVal() const { return 0; "
      "} };\n"
      "};\n"
      "template <class T> int Outer<T>::Inner::sharedCount = 0;\n"
      "int useM(Outer<int>::Inner& i) { return i.getVal(); }\n"
      "int useS() { return Outer<int>::Inner::sharedCount; }\n";
  EXPECT_NE(
      rewriteMethod(code, renameOne("getVal", "get_val")).find("i.get_val()"),
      std::string::npos);
  EXPECT_NE(rewriteMember(code, renameOne("sharedCount", "shared_count"))
                .find("Outer<int>::Inner::shared_count"),
            std::string::npos);
}

TEST(RenameNestedTemplateClass, PartialSpecializationFieldsAreNotCrossMatched) {
  // A partial specialization is a different class with its own member list.
  // Matching its fields by index against the *primary* template rebinds uses to
  // an unrelated member -- `a.otherValue` became `a.primary_field` -- which is
  // a miscompile, not a missed rename.
  EXPECT_EQ(rewriteMember("template <class T> struct Action { int "
                          "primaryField; };\n"
                          "template <class T> struct Action<T*> {\n"
                          "  int otherValue;\n"
                          "  explicit Action(int v) : otherValue(v) {}\n"
                          "};\n"
                          "int use() { Action<int*> a(1); return a.otherValue; "
                          "}\n",
                          addSuffix("X")),
            "template <class T> struct Action { int primaryFieldX; };\n"
            "template <class T> struct Action<T*> {\n"
            "  int otherValueX;\n"
            "  explicit Action(int v) : otherValueX(v) {}\n"
            "};\n"
            "int use() { Action<int*> a(1); return a.otherValueX; }\n");
}

TEST(RenameNestedTemplateClass,
     ExplicitSpecializationFieldsAreTheirOwnPattern) {
  // An explicit specialization is not an instantiation at all: its members are
  // written out, so they must not be mapped onto the primary template's.
  EXPECT_EQ(rewriteMember("template <class T> struct Box { int primaryField; "
                          "};\n"
                          "template <> struct Box<int> { int otherValue; };\n"
                          "int use() { Box<int> b{1}; return b.otherValue; }\n",
                          addSuffix("X")),
            "template <class T> struct Box { int primaryFieldX; };\n"
            "template <> struct Box<int> { int otherValueX; };\n"
            "int use() { Box<int> b{1}; return b.otherValueX; }\n");
}

// ---------------------------------------------------------------------------
// Qualified dependent names.  `Helper<T>::member` is a
// DependentScopeDeclRefExpr
// -- a dependent *name*, not a member access on an object -- so it needs the
// same cross-TU resolution `x.member` gets.
// ---------------------------------------------------------------------------

TEST(RenameQualifiedDependentName, StaticDataMemberIsRenamed) {
  // gtest-internal.h's `&(TypeIdHelper<T>::dummy_)` shape.
  EXPECT_EQ(rewriteMember(
                "template <class T> struct Helper { static int dummyValue; };\n"
                "template <class T> int Helper<T>::dummyValue = 0;\n"
                "template <class T> int* get() { return "
                "&Helper<T>::dummyValue; }\n"
                "int* use() { return get<int>(); }\n",
                renameOne("dummyValue", "dummy_value")),
            "template <class T> struct Helper { static int dummy_value; };\n"
            "template <class T> int Helper<T>::dummy_value = 0;\n"
            "template <class T> int* get() { return "
            "&Helper<T>::dummy_value; }\n"
            "int* use() { return get<int>(); }\n");
}

TEST(RenameQualifiedDependentName, StaticMemberFunctionIsRenamed) {
  EXPECT_EQ(rewriteMethod("template <class T> struct Helper { static int "
                          "computeIt() { return 0; } };\n"
                          "template <class T> int get() { return "
                          "Helper<T>::computeIt(); }\n"
                          "int use() { return get<int>(); }\n",
                          renameOne("computeIt", "compute_it")),
            "template <class T> struct Helper { static int compute_it() { "
            "return 0; } };\n"
            "template <class T> int get() { return "
            "Helper<T>::compute_it(); }\n"
            "int use() { return get<int>(); }\n");
}

TEST(RenameQualifiedDependentName, TwoInstantiationsAgree) {
  EXPECT_EQ(rewriteMember(
                "template <class T> struct Helper { static int dummyValue; };\n"
                "template <class T> int Helper<T>::dummyValue = 0;\n"
                "template <class T> int get() { return Helper<T>::dummyValue; "
                "}\n"
                "int use() { return get<int>() + get<char>(); }\n",
                renameOne("dummyValue", "dummy_value")),
            "template <class T> struct Helper { static int dummy_value; };\n"
            "template <class T> int Helper<T>::dummy_value = 0;\n"
            "template <class T> int get() { return Helper<T>::dummy_value; }\n"
            "int use() { return get<int>() + get<char>(); }\n");
}

TEST(RenameQualifiedDependentName, UninstantiatedLeftAlone) {
  // Nothing instantiates get(), so no instantiation resolves the token and it
  // is left alone -- the same bound the member-access path has.
  EXPECT_EQ(rewriteMember(
                "template <class T> struct Helper { static int dummyValue; };\n"
                "template <class H> int get() { return H::dummyValue; }\n",
                renameOne("dummyValue", "dummy_value")),
            "template <class T> struct Helper { static int dummy_value; };\n"
            "template <class H> int get() { return H::dummyValue; }\n");
}

// ---------------------------------------------------------------------------
// Unsafe new names.  collides() asks whether the new name is already *taken* in
// the declaration's own scope; these are three ways the new name ends up
// meaning something other than the member without being taken there.
// ---------------------------------------------------------------------------

TEST(RenameUnsafeNewName, KeywordIsRefused) {
  // googletest has `char char_;`, and snake_case would make that `char char;`.
  const char* code = "struct S { int char_; };\n";
  EXPECT_EQ(rewriteMember(code, renameOne("char_", "char")), code);
}

TEST(RenameUnsafeNewName, MacroNameIsRefused) {
  // The new name expands rather than naming the member.  googletest renames
  // `errno_` to `errno`, which glibc defines.
  const char* code =
      "#define kThing 1\n"
      "struct S { int thing_; };\n"
      "int use(S& s) { return s.thing_; }\n";
  EXPECT_EQ(rewriteMember(code, renameOne("thing_", "kThing")), code);
}

TEST(RenameUnsafeNewName, ShadowedAtTheUseSiteIsRefused) {
  // The member is not taken in the class, but at the use site a parameter of
  // the new name is in scope, so the rewrite would rebind the use to it:
  // `action_ = action` would become the self-assignment `action = action`.
  const char* code =
      "struct S {\n"
      "  int action_;\n"
      "  void WillByDefault(const int& action) { action_ = action; }\n"
      "};\n";
  EXPECT_EQ(rewriteMember(code, renameOne("action_", "action")), code);
}

TEST(RenameUnsafeNewName, ShadowInAnUnrelatedFunctionDoesNotBlock) {
  // The local only shadows where it is declared; a member used in a different
  // function is still renamed.
  EXPECT_EQ(
      rewriteMember("struct S {\n"
                    "  int action_;\n"
                    "  void set(int v) { action_ = v; }\n"
                    "};\n"
                    "void elsewhere() { int action = 1; (void)action; }\n",
                    renameOne("action_", "action")),
      "struct S {\n"
      "  int action;\n"
      "  void set(int v) { action = v; }\n"
      "};\n"
      "void elsewhere() { int action = 1; (void)action; }\n");
}

TEST(RenameUnsafeNewName, QualifiedUseIsNotCapturedByALocal) {
  // Only unqualified lookup can be captured.  `this->action_` names the member
  // whatever else is in scope, so the parameter does not block the rename --
  // the distinction that keeps this check from refusing most of googletest.
  EXPECT_EQ(rewriteMember("struct S {\n"
                          "  int action_;\n"
                          "  void f(const int& action) { this->action_ = "
                          "action; }\n"
                          "};\n",
                          renameOne("action_", "action")),
            "struct S {\n"
            "  int action;\n"
            "  void f(const int& action) { this->action = action; }\n"
            "};\n");
}

TEST(RenameUnsafeNewName, MemberOnAnotherObjectIsNotCapturedByALocal) {
  EXPECT_EQ(rewriteMember("struct S { int action_; };\n"
                          "void f(S& s, const int& action) { s.action_ = "
                          "action; }\n",
                          renameOne("action_", "action")),
            "struct S { int action; };\n"
            "void f(S& s, const int& action) { s.action = action; }\n");
}

// ---------------------------------------------------------------------------
// Scope relations — the guard that keeps unsound rule combinations out
// ---------------------------------------------------------------------------

TEST(ScopeRelations, FineGrainedScopesOverlapTheBroadOnes) {
  // A static data member is matched by all three, so two rules naming any two
  // of them would rename it twice.
  EXPECT_TRUE(scopesCanMatchSameDecl(VariableScope::Member,
                                     VariableScope::StaticMember));
  EXPECT_TRUE(scopesCanMatchSameDecl(VariableScope::Member,
                                     VariableScope::ConstMember));
  EXPECT_TRUE(scopesCanMatchSameDecl(VariableScope::StaticMember,
                                     VariableScope::ConstMember));
  // A `static const int x;` at namespace scope is matched by all three.
  EXPECT_TRUE(scopesCanMatchSameDecl(VariableScope::Global,
                                     VariableScope::StaticGlobal));
  EXPECT_TRUE(scopesCanMatchSameDecl(VariableScope::Global,
                                     VariableScope::ConstGlobal));
  EXPECT_TRUE(scopesCanMatchSameDecl(VariableScope::StaticGlobal,
                                     VariableScope::ConstGlobal));
}

TEST(ScopeRelations, DistinctFamiliesNeverMatchTheSameDecl) {
  // matchesScope() excludes members from Global and locals from everything
  // else, and Method matches no VarDecl at all.
  EXPECT_FALSE(
      scopesCanMatchSameDecl(VariableScope::Member, VariableScope::Method));
  EXPECT_FALSE(
      scopesCanMatchSameDecl(VariableScope::Member, VariableScope::Global));
  EXPECT_FALSE(
      scopesCanMatchSameDecl(VariableScope::Local, VariableScope::Global));
  EXPECT_FALSE(
      scopesCanMatchSameDecl(VariableScope::Method, VariableScope::Global));
}

TEST(ScopeRelations, MembersAndMethodsShareAClass) {
  // The pair that needs disjoint styles: a field and a member function of one
  // class cannot have the same name.
  EXPECT_TRUE(
      scopesShareADeclContext(VariableScope::Member, VariableScope::Method));
  EXPECT_TRUE(scopesShareADeclContext(VariableScope::StaticMember,
                                      VariableScope::Method));
  EXPECT_TRUE(scopesShareADeclContext(VariableScope::Global,
                                      VariableScope::StaticGlobal));
  // A local never shares a DeclContext with a member or a global, so a local
  // rule can use any style alongside them.
  EXPECT_FALSE(
      scopesShareADeclContext(VariableScope::Local, VariableScope::Member));
  EXPECT_FALSE(
      scopesShareADeclContext(VariableScope::Local, VariableScope::Global));
  EXPECT_FALSE(
      scopesShareADeclContext(VariableScope::Member, VariableScope::Global));
}

// ---------------------------------------------------------------------------
// References no visitor used to see, and names that must never be touched.
// Together these are what "declined or correct, never broken" rests on.
// ---------------------------------------------------------------------------

TEST(RenameOffsetOf, MemberNamedInOffsetofIsRenamed) {
  // offsetof's member designator is an OffsetOfExpr component, not a
  // MemberExpr.  Through the macro the name is an argument spelled at the call
  // site, so it renames like any other reference.
  const char* code =
      "#define OFFSETOF(T, m) __builtin_offsetof(T, m)\n"
      "struct Spec { int pad_; int conv_; };\n"
      "constexpr unsigned long kOff = OFFSETOF(Spec, conv_);\n"
      "constexpr unsigned long kRaw = __builtin_offsetof(Spec, conv_);\n";
  EXPECT_EQ(rewriteMember(code, renameOne("conv_", "conv")),
            "#define OFFSETOF(T, m) __builtin_offsetof(T, m)\n"
            "struct Spec { int pad_; int conv; };\n"
            "constexpr unsigned long kOff = OFFSETOF(Spec, conv);\n"
            "constexpr unsigned long kRaw = __builtin_offsetof(Spec, conv);\n");
}

TEST(RenameOffsetOf, OffsetofOnDependentTypeDeclinesTheName) {
  // Which member `conv_` names is known only per instantiation, and the
  // rewrite pass never walks those.  The name is declined, not half-renamed.
  const char* code =
      "struct Spec { int conv_; };\n"
      "template <class T> constexpr unsigned long off() {\n"
      "  return __builtin_offsetof(T, conv_);\n"
      "}\n"
      "constexpr unsigned long kOff = off<Spec>();\n";
  EXPECT_EQ(rewriteMember(code, renameOne("conv_", "conv")), code);
}

TEST(RenameAnonymousUnion, MemberOfDependentBaseInAnonymousUnionIsRenamed) {
  // abseil's StatusOr: the member lives in an anonymous union of a dependent
  // base.  The instantiation reaches it through an implicit MemberExpr on the
  // union's unnamed field at the same location, which used to read as "a
  // member not being renamed" and veto the token while the declaration was
  // renamed anyway.
  const char* code =
      "struct Status { bool ok() const { return true; } };\n"
      "template <class T> struct Data { union { Status status_; }; };\n"
      "template <class T> struct StatusOr : Data<T> {\n"
      "  bool ok() const { return this->status_.ok(); }\n"
      "};\n"
      "bool use(StatusOr<int>& s) { return s.ok(); }\n";
  EXPECT_EQ(rewriteMember(code, renameOne("status_", "status")),
            "struct Status { bool ok() const { return true; } };\n"
            "template <class T> struct Data { union { Status status; }; };\n"
            "template <class T> struct StatusOr : Data<T> {\n"
            "  bool ok() const { return this->status.ok(); }\n"
            "};\n"
            "bool use(StatusOr<int>& s) { return s.ok(); }\n");
}

TEST(RenameAnonymousUnion, NonDependentUseThroughInstantiationIsRenamed) {
  const char* code =
      "struct Status { bool ok() const { return true; } };\n"
      "template <class T> struct Data { union { Status status_; }; };\n"
      "struct Derived : Data<int> { bool ok() const { return status_.ok(); } "
      "};\n";
  EXPECT_EQ(rewriteMember(code, renameOne("status_", "status")),
            "struct Status { bool ok() const { return true; } };\n"
            "template <class T> struct Data { union { Status status; }; };\n"
            "struct Derived : Data<int> { bool ok() const { return "
            "status.ok(); } };\n");
}

TEST(RenameMemberFunctions, ProtocolNamesAreDeclined) {
  // A range-for calls begin()/end() without spelling them, so renaming them is
  // a complete rename that still does not compile.  No reference audit can
  // see it; the names are declined by fiat.  A sibling renames normally.
  const char* code =
      "struct Bag { int* begin() const; int* end() const; int* first() const; "
      "};";
  EXPECT_EQ(rewriteMethod(code, addSuffix("_x")),
            "struct Bag { int* begin() const; int* end() const; int* first_x() "
            "const; };");
}

TEST(RenameMemberFunctions, MembersOfForeignSpecializationAreDeclined) {
  // std::numeric_limits<Fix>::is_specialized is named by the primary template
  // in <limits>; a specialization of one of *our* templates is unaffected.
  const char* code =
      "#include <limits>\n"
      "struct Fix { int raw_; };\n"
      "namespace std {\n"
      "template <> struct numeric_limits<Fix> {\n"
      "  static constexpr bool is_specialized = true;\n"
      "  static int lowest() { return 0; }\n"
      "};\n"
      "}\n"
      "template <class T> struct Ours { int get_raw() const; };\n"
      "template <> struct Ours<int> { int get_raw() const; };\n";
  EXPECT_EQ(rewriteMethod(code, addSuffix("_x")),
            "#include <limits>\n"
            "struct Fix { int raw_; };\n"
            "namespace std {\n"
            "template <> struct numeric_limits<Fix> {\n"
            "  static constexpr bool is_specialized = true;\n"
            "  static int lowest() { return 0; }\n"
            "};\n"
            "}\n"
            "template <class T> struct Ours { int get_raw_x() const; };\n"
            "template <> struct Ours<int> { int get_raw_x() const; };\n");
  EXPECT_EQ(rewriteStaticMember(code, addSuffix("_x")), code);
}

TEST(RenameStaticMemberVariables, TraitValueIsDeclined) {
  // std::conjunction and friends read B::value on user traits.
  EXPECT_EQ(rewriteStaticMember("struct IsFoo { static constexpr bool value = "
                                "true; static constexpr bool other = false; };",
                                addSuffix("_")),
            "struct IsFoo { static constexpr bool value = true; static "
            "constexpr bool other_ = false; };");
}

// ---------------------------------------------------------------------------
// A rename must still bind from every use site, and every spelling of the old
// name must be one the tool understands.
// ---------------------------------------------------------------------------

TEST(RenameHiding, DerivedClassDeclaringTheNewNameDeclines) {
  // Lookup of `status_` from inside Derived starts at Derived; once the base
  // field is called `status`, Derived's method status() is found first.
  // collides() only looks at the declaring class, so this is checked per use.
  const char* code =
      "struct Base { int status_; };\n"
      "struct Derived : Base {\n"
      "  int status() const;\n"
      "  int f() const { return status_; }\n"
      "};\n";
  EXPECT_EQ(rewriteMember(code, renameOne("status_", "status")), code);
}

TEST(RenameHiding, QualifiedAndBaseTypedAccessesAreNotHidden) {
  // `Base::status_` starts lookup at Base, and `b.status_` on a Base never
  // passes through Derived, so neither access can be captured.
  const char* code =
      "struct Base { int status_; };\n"
      "struct Derived : Base {\n"
      "  int status() const;\n"
      "  int f() const { return Base::status_; }\n"
      "};\n"
      "int g(Base& b) { return b.status_; }\n";
  EXPECT_EQ(rewriteMember(code, renameOne("status_", "status")),
            "struct Base { int status; };\n"
            "struct Derived : Base {\n"
            "  int status() const;\n"
            "  int f() const { return Base::status; }\n"
            "};\n"
            "int g(Base& b) { return b.status; }\n");
}

TEST(RenameHiding, DependentBaseHidingIsSeenInTheInstantiation) {
  // abseil's StatusOr<T> : StatusOrData<T>, with a method status() next to
  // the base's field status_.  The pattern's this->status_ is dependent, so
  // the check runs on the instantiation's resolved access.
  const char* code =
      "struct Status { bool ok() const { return true; } };\n"
      "template <class T> struct Data { Status status_; };\n"
      "template <class T> struct StatusOr : Data<T> {\n"
      "  const Status& status() const { return this->status_; }\n"
      "  bool ok() const { return this->status_.ok(); }\n"
      "};\n"
      "bool use(StatusOr<int>& s) { return s.ok(); }\n";
  EXPECT_EQ(rewriteMember(code, renameOne("status_", "status")), code);
}

TEST(RenameSpellingAudit, UsingDeclarationDeclines) {
  // `using Base::val_;` spells the member and no visitor rewrites it.
  const char* code =
      "struct Base { int val_; };\n"
      "struct D : Base { using Base::val_; int f() const { return val_; } };\n";
  EXPECT_EQ(rewriteMember(code, renameOne("val_", "value")), code);
}

TEST(RenameSpellingAudit, UnresolvedCallInsideATemplateDeclines) {
  // Init(p) with a dependent argument is an UnresolvedMemberExpr -- an overload
  // set, not a member -- so the call would be left spelling the old name.
  const char* code =
      "struct B {\n"
      "  template <class U> B(U* p) { Init(p); }\n"
      "  template <class M> void Init(M&& m) {}\n"
      "};\n"
      "int use(int* p) { B b(p); return 0; }\n";
  EXPECT_EQ(rewriteMethod(code, renameOne("Init", "init")), code);
}

TEST(RenameSpellingAudit, PastedMacroArgumentDeclines) {
  // The argument DoThis is spelled at the call site but only ever consumed by
  // `##`: the pasted gmock_DoThis is a different identifier that nonetheless
  // changes when DoThis is renamed.  No AST node accounts for that token.
  const char* code =
      "struct Spec { int a; };\n"
      "#define MOCK(name) Spec gmock_##name(int) { return {}; } "
      "int name(int x) { return x; }\n"
      "struct MockFoo { MOCK(DoThis) };\n"
      "#define CALL(obj, call) (obj).gmock_##call\n"
      "int use(MockFoo& m) { return CALL(m, DoThis)(1).a + m.DoThis(2); }\n";
  EXPECT_EQ(rewriteMethod(code, renameOne("DoThis", "do_this")), code);
}

TEST(RenameSpellingAudit, CommentsAndStringsDoNotCount) {
  const char* code =
      "struct S { int val_; };  // val_ is the payload\n"
      "const char* kName = \"val_\";\n"
      "int f(S& x) { return x.val_; }\n";
  EXPECT_EQ(rewriteMember(code, renameOne("val_", "value")),
            "struct S { int value; };  // val_ is the payload\n"
            "const char* kName = \"val_\";\n"
            "int f(S& x) { return x.value; }\n");
}

TEST(RenameSpellingAudit, UnrelatedDeclarationsWithTheSameNameDoNotCount) {
  // A free function and an enumerator that happen to be called Init are
  // declarations the visitor sees; only spellings nobody accounts for decline.
  const char* code =
      "struct S { void Init(); };\n"
      "void Init();\n"
      "struct T { int Init; };\n"
      "int g(S& s, T& t) { s.Init(); Init(); return t.Init; }\n";
  EXPECT_EQ(rewriteMethod(code, renameOne("Init", "init")),
            "struct S { void init(); };\n"
            "void Init();\n"
            "struct T { int Init; };\n"
            "int g(S& s, T& t) { s.init(); Init(); return t.Init; }\n");
}

TEST(RenameMacros, DeclarationThroughAPastingMacroIsDeclined) {
  // gmock's ACTION_P shape: the argument declares a member *and* is pasted
  // into a typedef the body may spell.  Renaming the member would change the
  // typedef.  A declaration through a macro that does not paste still renames.
  const char* code =
      "#define PARAM(name) int name; typedef int name##_type;\n"
      "#define FIELD(name) int name;\n"
      "struct S { PARAM(foo) FIELD(other) foo_type f() const { return foo; } "
      "};\n";
  EXPECT_EQ(rewriteMember(code, addSuffix("_")),
            "#define PARAM(name) int name; typedef int name##_type;\n"
            "#define FIELD(name) int name;\n"
            "struct S { PARAM(foo) FIELD(other_) foo_type f() const { return "
            "foo; } };\n");
}

TEST(RenameAnonymousUnion, CollidesWithAnAccessorOnTheEnclosingClass) {
  // re2's Regexp shape: the variant fields live in anonymous structs inside an
  // anonymous union, and the class has an accessor of the same name for each.
  // The name is looked up in the class, not in the anonymous struct, so
  // `runes_ -> runes` would declare a field and a method of one name.
  const char* code =
      "struct Regexp {\n"
      "  int nrunes() { return nrunes_; }\n"
      "  int* runes() { return runes_; }\n"
      "  union {\n"
      "    struct { int nrunes_; int* runes_; };\n"
      "    struct { int cap_; };\n"
      "  };\n"
      "};\n";
  EXPECT_EQ(rewriteMember(code, renameOne("runes_", "runes")), code);
  EXPECT_EQ(rewriteMember(code, renameOne("nrunes_", "nrunes")), code);
  // cap_ has no accessor of that name, so it renames -- the check is not a
  // blanket refusal to touch anonymous-union members.
  EXPECT_EQ(rewriteMember(code, renameOne("cap_", "cap")),
            "struct Regexp {\n"
            "  int nrunes() { return nrunes_; }\n"
            "  int* runes() { return runes_; }\n"
            "  union {\n"
            "    struct { int nrunes_; int* runes_; };\n"
            "    struct { int cap; };\n"
            "  };\n"
            "};\n");
}

TEST(RenameAnonymousUnion, TwoAnonymousStructsCannotClaimOneName) {
  // Members of different anonymous structs in one union are still all members
  // of the enclosing class, so two of them cannot rename to the same name.
  const char* code =
      "struct S {\n"
      "  union {\n"
      "    struct { int aVal; };\n"
      "    struct { int a_val; };\n"
      "  };\n"
      "};\n";
  EXPECT_EQ(rewriteMember(code, renameOne("aVal", "a_val")), code);
}

// ---------------------------------------------------------------------------
// Class-scope capture: the new name is already used, unqualified, inside the
// scope the rename introduces it into, to mean something from further out.
// ---------------------------------------------------------------------------

TEST(RenameCapture, MethodRenamedToTheNameOfItsReturnTypeDeclines) {
  // game_arena: Engine::capabilities() -> Capabilities() is the name of the
  // struct it returns, from the enclosing namespace.  Inside the class (and
  // the override in Impl) the name would then find the member function.
  const char* code =
      "namespace ns {\n"
      "struct Capabilities { bool isolates = false; };\n"
      "class Engine {\n"
      " public:\n"
      "  virtual ~Engine() = default;\n"
      "  virtual Capabilities capabilities() const = 0;\n"
      "};\n"
      "class Impl : public Engine {\n"
      " public:\n"
      "  Capabilities capabilities() const override { return {}; }\n"
      "};\n"
      "}  // namespace ns\n";
  EXPECT_EQ(rewriteMethod(code, renameOne("capabilities", "Capabilities")),
            code);
}

TEST(RenameCapture, MemberRenamedToAFunctionCalledUnqualifiedDeclines) {
  // highway: AlignedDeleter::free_ -> free, next to a static member function
  // that calls ::free.  After the rename that call names the data member.
  const char* code =
      "void free(void* p);\n"
      "struct Deleter {\n"
      "  static void Delete(void* p) { free(p); }\n"
      "  void (*free_)(void*) = nullptr;\n"
      "};\n";
  EXPECT_EQ(rewriteMember(code, renameOne("free_", "free")), code);
}

TEST(RenameCapture, QualifiedUsesAndUnrelatedScopesAreNotCaptured) {
  // `ns::Capabilities` starts lookup at the qualifier, `struct Capabilities`
  // finds only class names, and Other is not on any lookup path from Engine.
  const char* code =
      "namespace ns {\n"
      "struct Capabilities {};\n"
      "struct Other { Capabilities c; };\n"
      "class Engine {\n"
      " public:\n"
      "  ns::Capabilities capabilities() const;\n"
      "  struct Capabilities also() const;\n"
      "};\n"
      "}  // namespace ns\n"
      "int use(ns::Engine& e) { e.capabilities(); return 0; }\n";
  EXPECT_EQ(rewriteMethod(code, renameOne("capabilities", "Capabilities")),
            "namespace ns {\n"
            "struct Capabilities {};\n"
            "struct Other { Capabilities c; };\n"
            "class Engine {\n"
            " public:\n"
            "  ns::Capabilities Capabilities() const;\n"
            "  struct Capabilities also() const;\n"
            "};\n"
            "}  // namespace ns\n"
            "int use(ns::Engine& e) { e.Capabilities(); return 0; }\n");
}

TEST(RenameCapture, BaseMemberUsedInDerivedIsHiddenByTheRenamedMember) {
  // `n` inside D means Base::n today; a D::n would be found first.
  const char* code =
      "struct Base { int n = 1; };\n"
      "struct D : Base {\n"
      "  int count_ = 2;\n"
      "  int f() const { return n + count_; }\n"
      "};\n";
  EXPECT_EQ(rewriteMember(code, renameOne("count_", "n")), code);
}

TEST(RenameCapture, AccessThroughADerivedObjectIsCaptured) {
  // `d.n` looks n up starting at D; today it reaches Base::n.
  const char* code =
      "struct Base { int n = 1; };\n"
      "struct D : Base { int count_ = 2; };\n"
      "int g(D& d) { return d.n; }\n";
  EXPECT_EQ(rewriteMember(code, renameOne("count_", "n")), code);
}

TEST(RenameCapture, DerivedClassOwnMemberStillPrecedes) {
  // D declares its own n, which hides whatever Base gains: not a capture.
  const char* code =
      "struct Base { int count_ = 1; };\n"
      "struct D : Base { int n = 2; int f() const { return n; } };\n"
      "int g(D& d) { return d.n; }\n";
  EXPECT_EQ(rewriteMember(code, renameOne("count_", "n")),
            "struct Base { int n = 1; };\n"
            "struct D : Base { int n = 2; int f() const { return n; } };\n"
            "int g(D& d) { return d.n; }\n");
}

TEST(RenameCapture, LocalRenamedToAMemberUsedInTheSameFunctionDeclines) {
  // The parameter `count` renamed to `n` would hide the member n in f.
  const char* code =
      "struct S {\n"
      "  int n = 0;\n"
      "  int f(int count) { return count + n; }\n"
      "};\n";
  EXPECT_EQ(rewriteLocal(code, renameOne("count", "n")), code);
}

TEST(RenameCapture, ParametersOfTheNewNameArePreceded) {
  // A parameter is found before class scope, so a member renamed onto a
  // parameter's name is not a capture of the *parameter* (the reverse -- the
  // member use captured by the parameter -- is scan() case (b), and it does
  // not apply here because count_ is never used unqualified in that ctor).
  const char* code =
      "struct S {\n"
      "  explicit S(int count) : count_(count) {}\n"
      "  int count_;\n"
      "};\n";
  EXPECT_EQ(rewriteMember(code, renameOne("count_", "count")),
            "struct S {\n"
            "  explicit S(int count) : count(count) {}\n"
            "  int count;\n"
            "};\n");
}

TEST(RenameCapture, DependentBaseCaptureIsSeenInTheInstantiation) {
  // Base<T>::n is reached through a dependent base; the check runs on the
  // instantiation, where the base is resolved.
  const char* code =
      "template <class T> struct Base { T n = 1; };\n"
      "template <class T> struct D : Base<T> {\n"
      "  T count_ = 2;\n"
      "  T f() const { return this->n + count_; }\n"
      "};\n"
      "int use() { D<int> d; return d.f(); }\n";
  EXPECT_EQ(rewriteMember(code, renameOne("count_", "n")), code);
}

TEST(RenameAnonymousUnion, LocalOfTheNewNameCapturesAUnionMember) {
  // protobuf's LazyString: `auto init_value = init_value_;` with init_value_
  // in an anonymous union.  The access is an implicit this-> behind an
  // implicit member access on the unnamed field; case (b) must see through
  // it, or the rewrite is the self-initialization `auto init_value =
  // init_value;`.
  const char* code =
      "struct L {\n"
      "  union {\n"
      "    int init_value_;\n"
      "    char buf_[4];\n"
      "  };\n"
      "  int f() const { auto init_value = init_value_; return init_value; }\n"
      "};\n";
  EXPECT_EQ(rewriteMember(code, renameOne("init_value_", "init_value")), code);
}

// ---------------------------------------------------------------------------
// Spelling audit: macro arguments the preprocessor consumed
// ---------------------------------------------------------------------------

TEST(RenameSpellingAudit, ArgumentOnlyPastedOrStringizedDoesNotCount) {
  // absl's ABSL_FLAG(std::string, docker_image, ...) next to a struct member
  // docker_image: the flag's name is only ever pasted and stringized, so no
  // token of the expansion is spelled at the argument -- nothing there can
  // refer to the member, and the member renames.
  const char* code =
      "#define FLAG(T, name, help) \\\n"
      "  T FLAGS_##name;            \\\n"
      "  const char* HelpFor##name() { return #name \" \" help; }\n"
      "struct Config { int docker_image = 0; };\n"
      "FLAG(int, docker_image, \"image\")\n"
      "int use(Config& c) { return c.docker_image + FLAGS_docker_image; }\n";
  EXPECT_EQ(
      rewriteMember(code, renameOne("docker_image", "docker_image_")),
      "#define FLAG(T, name, help) \\\n"
      "  T FLAGS_##name;            \\\n"
      "  const char* HelpFor##name() { return #name \" \" help; }\n"
      "struct Config { int docker_image_ = 0; };\n"
      "FLAG(int, docker_image, \"image\")\n"
      "int use(Config& c) { return c.docker_image_ + FLAGS_docker_image; }\n");
}

TEST(RenameSpellingAudit, ArgumentForwardedIntoAPasteDoesNotCount) {
  // The paste happens one macro down; the argument reaches it through an
  // ordinary use in the outer body, and is still consumed.
  const char* code =
      "#define INNER(name) int FLAGS_##name;\n"
      "#define OUTER(name) INNER(name)\n"
      "struct Config { int docker_image = 0; };\n"
      "OUTER(docker_image)\n"
      "int use(Config& c) { return c.docker_image + FLAGS_docker_image; }\n";
  EXPECT_EQ(
      rewriteMember(code, renameOne("docker_image", "docker_image_")),
      "#define INNER(name) int FLAGS_##name;\n"
      "#define OUTER(name) INNER(name)\n"
      "struct Config { int docker_image_ = 0; };\n"
      "OUTER(docker_image)\n"
      "int use(Config& c) { return c.docker_image_ + FLAGS_docker_image; }\n");
}

TEST(RenameSpellingAudit, ArgumentAlsoUsedPlainlyStillCounts) {
  // A parameter used plainly anywhere in the body is emitted, and here the
  // emission is a using-declaration -- a reference the tool does not rewrite.
  const char* code =
      "struct Base { int val_; };\n"
      "#define BRING(n) using Base::n; int gmock_##n;\n"
      "struct D : Base { BRING(val_) int f() const { return val_; } };\n";
  EXPECT_EQ(rewriteMember(code, renameOne("val_", "value")), code);
}

// ---------------------------------------------------------------------------
// Dependent tokens: a binding this rule does not rename declines the others
// ---------------------------------------------------------------------------

TEST(RenameDependentTokens, BindingToADeclinedMemberDeclinesTheRenamedOne) {
  // protobuf's MicroString::kInlineCapacity (declined: a parameter of the new
  // name captures it in a default argument) next to the derived
  // MicroStringExtraImpl::kInlineCapacity (renamed).  T::kInlineCapacity in
  // the test binds to both, so the token cannot be rewritten and the derived
  // one must keep its name too.
  const char* code =
      "struct B {\n"
      "  static constexpr int kCap = 1;\n"
      "  int f(int cap = kCap) const { return cap; }\n"
      "};\n"
      "struct D : private B {\n"
      "  static constexpr int kCap = 3;\n"
      "};\n"
      "template <class T> int g() { return T::kCap; }\n"
      "int use() { return g<B>() + g<D>(); }\n";
  EXPECT_EQ(rewriteStaticMember(code, renameOne("kCap", "cap")), code);
}

TEST(RenameDependentTokens, BindingToAMemberOutsideTheFileSetDeclines) {
  // protobuf's TcParser::GetTable spells T::_table_ for every message class,
  // the checked-in ones (renamed) and the build-generated ones (not ours)
  // alike.  A binding to a member outside the files being formatted keeps the
  // token, so every renamed target of that token must keep its name.
  const char* code =
      "#include \"theirs.h\"\n"
      "struct Ours { static const int _table_ = 1; };\n"
      "template <class T> int table() { return T::_table_; }\n"
      "int use() { return table<Ours>() + table<Theirs>(); }\n";
  EXPECT_EQ(
      rewriteVariableNames(
          code, renameOne("_table_", "table_"), VariableScope::Member,
          {"-std=c++20", "-xc++"},
          {{"theirs.h", "struct Theirs { static const int _table_ = 2; };\n"}}),
      code);
}

TEST(RenameSpellingAudit, DeclarationPastedByANestedMacroDeclines) {
  // gmock's real shape: MOCK_METHOD spells the method plainly one macro down
  // and forwards the same argument to GMOCK_MOCKER_, which pastes it.  The
  // declaration's own token never passes through a macro whose body contains
  // ##, so only the preprocessor's record of the paste can catch it -- and
  // must, because ON_CALL(m, DoThis) forms gmock_DoThis from the old spelling.
  const char* code =
      "#define MOCKER(m) gmock_##m\n"
      "#define IMPL(m) \\\n"
      "  int m(int x) { return MOCKER(m)(x); } \\\n"
      "  int MOCKER(m)(int x) { return x; }\n"
      "#define MOCK(m) IMPL(m)\n"
      "struct MockFoo { MOCK(DoThis) };\n"
      "#define ON_CALL(obj, call) (obj).MOCKER(call)\n"
      "int use(MockFoo& m) { return ON_CALL(m, DoThis)(1) + m.DoThis(2); }\n";
  EXPECT_EQ(rewriteMethod(code, renameOne("DoThis", "do_this")), code);
}

// ---------------------------------------------------------------------------
// Collision resolution across rules
// ---------------------------------------------------------------------------

static auto memberTrailingMethodSnake() {
  return std::vector<std::pair<VariableScope, VariableRenameCallback>>{
      {VariableScope::Member,
       [](std::string_view n, std::string& out) {
         out = renameToStyle(n, NamingStyle::TrailingUnderscore);
         return out != n;
       }},
      {VariableScope::Method, [](std::string_view n, std::string& out) {
         out = renameToStyle(n, NamingStyle::SnakeCase);
         return out != n;
       }}};
}

TEST(RenameResolution, NameVacatedByAnotherRuleIsFreeInTheSamePass) {
  // googletest's Flags: the method AlsoRun() wants `also_run`, which the
  // field also_run holds -- until the member rule moves it to also_run_.
  // Deciding per rule against the AST's names declined the method on the
  // first run and accepted it on the second; now both go in one pass.
  const char* code =
      "struct Flags {\n"
      "  int also_run = 0;\n"
      "  static Flags AlsoRun(int also_run) {\n"
      "    Flags f;\n"
      "    f.also_run = also_run;\n"
      "    return f;\n"
      "  }\n"
      "};\n";
  EXPECT_EQ(rewriteWithRules(code, memberTrailingMethodSnake()),
            "struct Flags {\n"
            "  int also_run_ = 0;\n"
            "  static Flags also_run(int also_run) {\n"
            "    Flags f;\n"
            "    f.also_run_ = also_run;\n"
            "    return f;\n"
            "  }\n"
            "};\n");
}

TEST(RenameResolution, TwoRulesForOneNameGoToTheFirstRule) {
  // The getter/field pair under member/snake_case + method/snake_case: both
  // want `value`; the member rule is first, so the method keeps its name.
  std::vector<std::pair<VariableScope, VariableRenameCallback>> rules{
      {VariableScope::Member, renameOne("value_", "value")},
      {VariableScope::Method, renameOne("Value", "value")}};
  const char* code =
      "class Widget {\n"
      " public:\n"
      "  int Value() const { return value_; }\n"
      " private:\n"
      "  int value_;\n"
      "};\n";
  EXPECT_EQ(rewriteWithRules(code, rules),
            "class Widget {\n"
            " public:\n"
            "  int Value() const { return value; }\n"
            " private:\n"
            "  int value;\n"
            "};\n");
}

TEST(RenameResolution, RenamesThatWaitOnEachOtherAreBothDeclined) {
  // a -> b and b -> a: each name is vacated only by the other, so neither
  // can go first.
  std::vector<std::pair<VariableScope, VariableRenameCallback>> rules{
      {VariableScope::Member, [](std::string_view n, std::string& out) {
         if (n == "a")
           out = "b";
         else if (n == "b")
           out = "a";
         else
           return false;
         return true;
       }}};
  const char* code = "struct S { int a; int b; };\n";
  EXPECT_EQ(rewriteWithRules(code, rules), code);
}

TEST(RenameResolution, DependentFallsWithAVetoedMover) {
  // The method's `count` is free only because the field count moves to
  // count_; a macro body references the field, which vetoes that move -- and
  // the method's rename with it, or the class would declare both a field and
  // a method named count.
  const char* code =
      "#define BUMP(s) ((s).count += 1)\n"
      "struct S {\n"
      "  int count = 0;\n"
      "  int Count() const { return count; }\n"
      "};\n"
      "int use(S& s) { return BUMP(s); }\n";
  EXPECT_EQ(rewriteWithRules(code, memberTrailingMethodSnake()), code);
}

// ---------------------------------------------------------------------------
// Types and namespaces
// ---------------------------------------------------------------------------

static auto rewriteType(const char* code, VariableRenameCallback cb)
    -> std::string {
  return rewriteVariableNames(code, std::move(cb), VariableScope::Type,
                              {"-std=c++20", "-xc++"});
}
static auto rewriteNamespace(const char* code, VariableRenameCallback cb)
    -> std::string {
  return rewriteVariableNames(code, std::move(cb), VariableScope::Namespace,
                              {"-std=c++20", "-xc++"});
}
static VariableRenameCallback toStyle(NamingStyle style) {
  return [style](std::string_view n, std::string& out) {
    out = renameToStyle(n, style);
    return out != n;
  };
}

TEST(RenameTypes, ClassWithEverySpellingOfItsName) {
  const char* code =
      "struct widget;\n"
      "struct widget {\n"
      "  widget();\n"
      "  ~widget();\n"
      "  widget(const widget&) = default;\n"
      "  int f() const;\n"
      "  static widget make();\n"
      "};\n"
      "widget::widget() {}\n"
      "widget::~widget() {}\n"
      "int widget::f() const { return 0; }\n"
      "widget widget::make() { return widget(); }\n"
      "int use(widget* p) {\n"
      "  widget w = widget::make();\n"
      "  p->~widget();\n"
      "  return sizeof(widget) + w.f() + static_cast<widget*>(p)->f();\n"
      "}\n";
  EXPECT_EQ(rewriteType(code, renameOne("widget", "Widget")),
            "struct Widget;\n"
            "struct Widget {\n"
            "  Widget();\n"
            "  ~Widget();\n"
            "  Widget(const Widget&) = default;\n"
            "  int f() const;\n"
            "  static Widget make();\n"
            "};\n"
            "Widget::Widget() {}\n"
            "Widget::~Widget() {}\n"
            "int Widget::f() const { return 0; }\n"
            "Widget Widget::make() { return Widget(); }\n"
            "int use(Widget* p) {\n"
            "  Widget w = Widget::make();\n"
            "  p->~Widget();\n"
            "  return sizeof(Widget) + w.f() + static_cast<Widget*>(p)->f();\n"
            "}\n");
}

TEST(RenameTypes, NestedTypesQualifiersFriendsAndUsingDeclarations) {
  const char* code =
      "struct outer {\n"
      "  struct inner { int v; };\n"
      "  enum mode { kOn };\n"
      "  friend struct helper;\n"
      "};\n"
      "struct helper {};\n"
      "struct derived : outer { using outer::inner; using outer::mode; };\n"
      "outer::inner make() { return outer::inner{outer::kOn}; }\n"
      "struct outer::inner* p = nullptr;\n";
  EXPECT_EQ(
      rewriteType(code, toStyle(NamingStyle::UpperCamelCase)),
      "struct Outer {\n"
      "  struct Inner { int v; };\n"
      "  enum Mode { kOn };\n"
      "  friend struct Helper;\n"
      "};\n"
      "struct Helper {};\n"
      "struct Derived : Outer { using Outer::Inner; using Outer::Mode; };\n"
      "Outer::Inner make() { return Outer::Inner{Outer::kOn}; }\n"
      "struct Outer::Inner* p = nullptr;\n");
}

TEST(RenameTypes, TemplatesSpecializationsAndTemplateArguments) {
  const char* code =
      "template <class T> struct box { T v; };\n"
      "template <> struct box<int> { int v; };\n"
      "template <class T> struct box<T*> { T* v; };\n"
      "template struct box<char>;\n"
      "template <template <class> class W> struct wrap { W<double> w; };\n"
      "box<double> b;\n"
      "wrap<box> ww;\n"
      "template <class T> using boxed = box<T>;\n"
      "boxed<long> bl;\n";
  EXPECT_EQ(
      rewriteType(code, toStyle(NamingStyle::UpperCamelCase)),
      "template <class T> struct Box { T v; };\n"
      "template <> struct Box<int> { int v; };\n"
      "template <class T> struct Box<T*> { T* v; };\n"
      "template struct Box<char>;\n"
      "template <template <class> class W> struct Wrap { W<double> w; };\n"
      "Box<double> b;\n"
      "Wrap<Box> ww;\n"
      "template <class T> using Boxed = Box<T>;\n"
      "Boxed<long> bl;\n");
}

TEST(RenameTypes, EnumsTypedefsAndAliases) {
  const char* code =
      "enum color { red };\n"
      "enum class shade : int { dark };\n"
      "typedef color colour_t;\n"
      "using colour_alias = color;\n"
      "color c = red;\n"
      "colour_t d = c;\n"
      "colour_alias e = c;\n"
      "shade s = shade::dark;\n";
  EXPECT_EQ(rewriteType(code, toStyle(NamingStyle::UpperCamelCase)),
            "enum Color { red };\n"
            "enum class Shade : int { dark };\n"
            "typedef Color ColourT;\n"
            "using ColourAlias = Color;\n"
            "Color c = red;\n"
            "ColourT d = c;\n"
            "ColourAlias e = c;\n"
            "Shade s = Shade::dark;\n");
}

TEST(RenameTypes, ProtocolTypeNamesAreDeclined) {
  // value_type and iterator are what the standard library reads; sibling
  // names rename.
  const char* code =
      "struct vec {\n"
      "  typedef int value_type;\n"
      "  struct iterator {};\n"
      "  struct cursor {};\n"
      "};\n";
  EXPECT_EQ(rewriteType(code, toStyle(NamingStyle::UpperCamelCase)),
            "struct Vec {\n"
            "  typedef int value_type;\n"
            "  struct iterator {};\n"
            "  struct Cursor {};\n"
            "};\n");
}

TEST(RenameTypes, DependentTypeNameResolvedThroughTheInstantiation) {
  const char* code =
      "struct holder { struct inner { int v; }; };\n"
      "template <class T> int get(typename T::inner x) { return x.v; }\n"
      "template <class T> struct user { typename T::inner i; };\n"
      "int use() { holder::inner i{1}; user<holder> u; return get<holder>(i); "
      "}\n";
  EXPECT_EQ(rewriteType(code, renameOne("inner", "Inner")),
            "struct holder { struct Inner { int v; }; };\n"
            "template <class T> int get(typename T::Inner x) { return x.v; }\n"
            "template <class T> struct user { typename T::Inner i; };\n"
            "int use() { holder::Inner i{1}; user<holder> u; return "
            "get<holder>(i); }\n");
}

TEST(RenameTypes, SpecializationOfATemplateThatIsNotOursIsLeftAlone) {
  const char* code =
      "#include \"theirs.h\"\n"
      "template <> struct their_hash<int> { int operator()(int) const; };\n"
      "struct mine {};\n";
  EXPECT_EQ(
      rewriteVariableNames(
          code, toStyle(NamingStyle::UpperCamelCase), VariableScope::Type,
          {"-std=c++20", "-xc++"},
          {{"theirs.h", "template <class T> struct their_hash;\n"}}),
      "#include \"theirs.h\"\n"
      "template <> struct their_hash<int> { int operator()(int) const; };\n"
      "struct Mine {};\n");
}

TEST(RenameNamespaces, EverySpellingIncludingTheClosingComment) {
  const char* code =
      "namespace MyLib {\n"
      "int f();\n"
      "}  // namespace MyLib\n"
      "namespace MyLib {\n"
      "int h();\n"
      "}  // namespace MyLib\n"
      "namespace ML = MyLib;\n"
      "using namespace MyLib;\n"
      "int g() { return MyLib::f() + ML::h(); }\n";
  EXPECT_EQ(rewriteNamespace(code, toStyle(NamingStyle::SnakeCase)),
            "namespace my_lib {\n"
            "int f();\n"
            "}  // namespace my_lib\n"
            "namespace my_lib {\n"
            "int h();\n"
            "}  // namespace my_lib\n"
            "namespace ml = my_lib;\n"
            "using namespace my_lib;\n"
            "int g() { return my_lib::f() + ml::h(); }\n");
}

TEST(RenameNamespaces, NestedDefinitionAndTheOnesLeftAlone) {
  // A namespace first declared in a file we do not own (TheirLib, reopened
  // here the way `namespace std` is reopened for a specialization) keeps its
  // name; the anonymous namespace has none; an inline namespace renames.
  const char* code =
      "#include \"theirs.h\"\n"
      "namespace TheirLib { int g(); }\n"
      "namespace a::BadName {\n"
      "int f();\n"
      "}  // namespace a::BadName\n"
      "namespace { int g(); }\n"
      "inline namespace V1 { int k(); }\n"
      "int use() { return a::BadName::f() + V1::k() + TheirLib::f(); }\n";
  EXPECT_EQ(
      rewriteVariableNames(code, toStyle(NamingStyle::SnakeCase),
                           VariableScope::Namespace, {"-std=c++20", "-xc++"},
                           {{"theirs.h", "namespace TheirLib { int f(); }\n"}}),
      "#include \"theirs.h\"\n"
      "namespace TheirLib { int g(); }\n"
      "namespace a::bad_name {\n"
      "int f();\n"
      "}  // namespace a::bad_name\n"
      "namespace { int g(); }\n"
      "inline namespace v1 { int k(); }\n"
      "int use() { return a::bad_name::f() + v1::k() + TheirLib::f(); }\n");
}

// ---------------------------------------------------------------------------
// Fine-grained local and access scopes, and which rule claims a declaration
// ---------------------------------------------------------------------------

static auto rewriteScope(const char* code, VariableScope scope,
                         NamingStyle style) -> std::string {
  return rewriteVariableNames(code, toStyle(style), scope);
}

using Rules = std::vector<std::pair<VariableScope, VariableRenameCallback>>;

TEST(RenameFineGrainedLocals, ConstLocalIsAValueFixedForTheProgram) {
  // `static const` and `constexpr` locals are constants; a plain `const` local
  // is initialized afresh on every call, a parameter is not a local variable
  // at all, and a mutable static is static but not const.
  const char* code =
      "int Scale(const int input_value) {\n"
      "  static const int max_scale = 8;\n"
      "  constexpr int base_offset = 2;\n"
      "  static int call_count = 0;\n"
      "  const int this_call = input_value + base_offset;\n"
      "  ++call_count;\n"
      "  return this_call < max_scale ? this_call : max_scale;\n"
      "}\n";
  EXPECT_EQ(
      rewriteScope(code, VariableScope::ConstLocal, NamingStyle::KConstant),
      "int Scale(const int input_value) {\n"
      "  static const int kMaxScale = 8;\n"
      "  constexpr int kBaseOffset = 2;\n"
      "  static int call_count = 0;\n"
      "  const int this_call = input_value + kBaseOffset;\n"
      "  ++call_count;\n"
      "  return this_call < kMaxScale ? this_call : kMaxScale;\n"
      "}\n");
}

TEST(RenameFineGrainedLocals, StaticLocalIsStorageNotConstness) {
  const char* code =
      "int Next() {\n"
      "  static int callCount = 0;\n"
      "  static const int stepSize = 2;\n"
      "  thread_local int perThread = 0;\n"
      "  int plainLocal = stepSize;\n"
      "  perThread += plainLocal;\n"
      "  return callCount += perThread;\n"
      "}\n";
  EXPECT_EQ(
      rewriteScope(code, VariableScope::StaticLocal, NamingStyle::SnakeCase),
      "int Next() {\n"
      "  static int call_count = 0;\n"
      "  static const int step_size = 2;\n"
      "  thread_local int per_thread = 0;\n"
      "  int plainLocal = step_size;\n"
      "  per_thread += plainLocal;\n"
      "  return call_count += per_thread;\n"
      "}\n");
}

TEST(RenameFineGrainedLocals, TheMostSpecificRuleClaimsTheDeclaration) {
  // The case the scopes exist for: locals are snake_case, except constants.
  // `kArenaOverride` under `local` alone lost its k: `arena_override`.
  const char* code =
      "int Strip(int TextLength) {\n"
      "  static const int kArenaOverride = 3;\n"
      "  int Remaining = TextLength - kArenaOverride;\n"
      "  return Remaining;\n"
      "}\n";
  const char* expected =
      "int Strip(int text_length) {\n"
      "  static const int kArenaOverride = 3;\n"
      "  int remaining = text_length - kArenaOverride;\n"
      "  return remaining;\n"
      "}\n";
  EXPECT_EQ(
      rewriteWithRules(
          code,
          Rules{{VariableScope::Local, toStyle(NamingStyle::SnakeCase)},
                {VariableScope::ConstLocal, toStyle(NamingStyle::KConstant)}}),
      expected);
  // The order of the rules does not decide it.
  EXPECT_EQ(
      rewriteWithRules(
          code,
          Rules{{VariableScope::ConstLocal, toStyle(NamingStyle::KConstant)},
                {VariableScope::Local, toStyle(NamingStyle::SnakeCase)}}),
      expected);
  // Without the specific rule the broad one takes everything, as before.
  EXPECT_EQ(rewriteWithRules(code, Rules{{VariableScope::Local,
                                          toStyle(NamingStyle::SnakeCase)}}),
            "int Strip(int text_length) {\n"
            "  static const int arena_override = 3;\n"
            "  int remaining = text_length - arena_override;\n"
            "  return remaining;\n"
            "}\n");
}

TEST(RenameFineGrainedLocals, ConstBeatsStaticForAStaticConst) {
  // A `static const` local is in both fine-grained scopes; constness wins, and
  // the mutable static is left to static_local.
  const char* code =
      "int f() {\n"
      "  static const int limitValue = 4;\n"
      "  static int hitCount = 0;\n"
      "  return ++hitCount < limitValue;\n"
      "}\n";
  EXPECT_EQ(
      rewriteWithRules(
          code,
          Rules{{VariableScope::StaticLocal, toStyle(NamingStyle::SnakeCase)},
                {VariableScope::ConstLocal, toStyle(NamingStyle::KConstant)}}),
      "int f() {\n"
      "  static const int kLimitValue = 4;\n"
      "  static int hit_count = 0;\n"
      "  return ++hit_count < kLimitValue;\n"
      "}\n");
}

TEST(RenameMemberAccess, EachAccessScopeTakesItsOwnMembers) {
  const char* code =
      "class Widget {\n"
      " public:\n"
      "  int publicField;\n"
      "  static int publicStatic;\n"
      " protected:\n"
      "  int protectedField;\n"
      " private:\n"
      "  int privateField;\n"
      "  int Sum() { return publicField + protectedField + privateField; }\n"
      "};\n";
  EXPECT_EQ(
      rewriteScope(code, VariableScope::PrivateMember,
                   NamingStyle::TrailingUnderscore),
      "class Widget {\n"
      " public:\n"
      "  int publicField;\n"
      "  static int publicStatic;\n"
      " protected:\n"
      "  int protectedField;\n"
      " private:\n"
      "  int private_field_;\n"
      "  int Sum() { return publicField + protectedField + private_field_; }\n"
      "};\n");
  EXPECT_EQ(
      rewriteScope(code, VariableScope::PublicMember, NamingStyle::SnakeCase),
      "class Widget {\n"
      " public:\n"
      "  int public_field;\n"
      "  static int public_static;\n"
      " protected:\n"
      "  int protectedField;\n"
      " private:\n"
      "  int privateField;\n"
      "  int Sum() { return public_field + protectedField + privateField; }\n"
      "};\n");
  EXPECT_EQ(
      rewriteScope(code, VariableScope::ProtectedMember,
                   NamingStyle::SnakeCase),
      "class Widget {\n"
      " public:\n"
      "  int publicField;\n"
      "  static int publicStatic;\n"
      " protected:\n"
      "  int protected_field;\n"
      " private:\n"
      "  int privateField;\n"
      "  int Sum() { return publicField + protected_field + privateField; }\n"
      "};\n");
}

TEST(RenameMemberAccess, StructVersusClassIsTheGoogleRule) {
  // "Data members of classes (but not structs) have trailing underscores":
  // a struct's members are public, a class's private, and constants are
  // kConstant whatever their access -- constness outranks access.
  const char* code =
      "struct Point {\n"
      "  int xPos;\n"
      "  int yPos;\n"
      "};\n"
      "class Path {\n"
      " public:\n"
      "  static constexpr int max_points = 8;\n"
      "  int Length() const { return pointCount < max_points ? pointCount : "
      "max_points; }\n"
      " private:\n"
      "  static constexpr int chunk_size = 4;\n"
      "  int pointCount = chunk_size;\n"
      "};\n"
      "int Use(Point p) { return p.xPos + p.yPos; }\n";
  EXPECT_EQ(
      rewriteWithRules(
          code,
          Rules{
              {VariableScope::Member, toStyle(NamingStyle::TrailingUnderscore)},
              {VariableScope::PublicMember, toStyle(NamingStyle::SnakeCase)},
              {VariableScope::ConstMember, toStyle(NamingStyle::KConstant)}}),
      "struct Point {\n"
      "  int x_pos;\n"
      "  int y_pos;\n"
      "};\n"
      "class Path {\n"
      " public:\n"
      "  static constexpr int kMaxPoints = 8;\n"
      "  int Length() const { return point_count_ < kMaxPoints ? point_count_ "
      ": kMaxPoints; }\n"
      " private:\n"
      "  static constexpr int kChunkSize = 4;\n"
      "  int point_count_ = kChunkSize;\n"
      "};\n"
      "int Use(Point p) { return p.x_pos + p.y_pos; }\n");
}

TEST(RenameMemberAccess, AnAnonymousMemberLendsItsAccess) {
  // A field of an anonymous union is `public` inside the union; what a user of
  // the class sees is the access of the anonymous member itself.
  const char* code =
      "class Value {\n"
      " public:\n"
      "  int Get() const { return intValue; }\n"
      " private:\n"
      "  union {\n"
      "    int intValue;\n"
      "    float floatValue;\n"
      "  };\n"
      "};\n";
  EXPECT_EQ(
      rewriteScope(code, VariableScope::PublicMember, NamingStyle::SnakeCase),
      code);
  EXPECT_EQ(rewriteScope(code, VariableScope::PrivateMember,
                         NamingStyle::TrailingUnderscore),
            "class Value {\n"
            " public:\n"
            "  int Get() const { return int_value_; }\n"
            " private:\n"
            "  union {\n"
            "    int int_value_;\n"
            "    float float_value_;\n"
            "  };\n"
            "};\n");
}

TEST(ScopeRelations, SpecificityOrdersEveryOverlap) {
  // Whenever two different scopes can match one declaration, one of them has
  // to be strictly more specific, or neither rule could claim it.
  const VariableScope all[] = {
      VariableScope::Member,          VariableScope::StaticMember,
      VariableScope::ConstMember,     VariableScope::PublicMember,
      VariableScope::ProtectedMember, VariableScope::PrivateMember,
      VariableScope::Local,           VariableScope::StaticLocal,
      VariableScope::ConstLocal,      VariableScope::Global,
      VariableScope::StaticGlobal,    VariableScope::ConstGlobal,
      VariableScope::Method,          VariableScope::Type,
      VariableScope::Namespace};
  for (VariableScope a : all)
    for (VariableScope b : all)
      if (a != b && scopesCanMatchSameDecl(a, b))
        EXPECT_NE(scopeSpecificity(a), scopeSpecificity(b))
            << static_cast<int>(a) << " vs " << static_cast<int>(b);
  // A member has exactly one access.
  EXPECT_FALSE(scopesCanMatchSameDecl(VariableScope::PublicMember,
                                      VariableScope::PrivateMember));
  EXPECT_TRUE(
      scopesCanMatchSameDecl(VariableScope::Local, VariableScope::ConstLocal));
  EXPECT_FALSE(scopesCanMatchSameDecl(VariableScope::ConstLocal,
                                      VariableScope::ConstGlobal));
}

TEST(ScopeRelations, EveryScopeNameRoundTrips) {
  EXPECT_EQ(parseVariableScope("const_local"), VariableScope::ConstLocal);
  EXPECT_EQ(parseVariableScope("private_member"), VariableScope::PrivateMember);
  EXPECT_EQ(parseVariableScope("member"), VariableScope::Member);
  EXPECT_EQ(parseVariableScope("locals"), std::nullopt);
  EXPECT_NE(variableScopeNames().find("static_local"), std::string::npos);
}
