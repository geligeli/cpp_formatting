#include <gtest/gtest.h>

#include "cpp_formatting/rename_variables_lib.h"

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
  EXPECT_EQ(rewriteMethod("struct S { int get(); };\n"
                          "int f(S& s, S* p) { return s.get() + p->get(); }",
                          renameOne("get", "value")),
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
  EXPECT_EQ(rewriteMethod("struct S { int get(); };\n"
                          "auto p = &S::get;",
                          renameOne("get", "value")),
            "struct S { int value(); };\n"
            "auto p = &S::value;");
}

TEST(RenameMemberFunctions, OverloadedMethods) {
  EXPECT_EQ(rewriteMethod("struct S { int get(); int get(int); };\n"
                          "int f(S& s) { return s.get() + s.get(1); }",
                          renameOne("get", "value")),
            "struct S { int value(); int value(int); };\n"
            "int f(S& s) { return s.value() + s.value(1); }");
}

TEST(RenameMemberFunctions, DoesNotRenameCtorDtorOrOperators) {
  EXPECT_EQ(rewriteMethod("struct S { S(); ~S(); operator int() const; bool "
                          "operator==(const S&) const; int get(); };",
                          addSuffix("_x")),
            "struct S { S(); ~S(); operator int() const; bool "
            "operator==(const S&) const; int get_x(); };");
}

TEST(RenameMemberFunctions, DoesNotRenameFreeFunctions) {
  EXPECT_EQ(rewriteMethod("int getValue() { return 1; }",
                          renameOne("getValue", "value")),
            "int getValue() { return 1; }");
}

TEST(RenameMemberFunctions, VirtualOverrideHierarchyRenamedTogether) {
  // The base declaration, every override, and every call site — through base
  // or derived — all carry the new name.
  EXPECT_EQ(rewriteMethod("struct B { virtual int get() const; };\n"
                          "struct D : B { int get() const override; };\n"
                          "int f(B& b, D& d) { return b.get() + d.get(); }",
                          renameOne("get", "value")),
            "struct B { virtual int value() const; };\n"
            "struct D : B { int value() const override; };\n"
            "int f(B& b, D& d) { return b.value() + d.value(); }");
}

TEST(RenameMemberFunctions, TemplateMethodRenamedInInstantiation) {
  // The MemberExpr in non-template code references the instantiated
  // CXXMethodDecl; the tool must walk up the instantiation chain.
  EXPECT_EQ(rewriteMethod("template<typename T> struct Box { T get(); };\n"
                          "int f() { Box<int> b; return b.get(); }",
                          renameOne("get", "value")),
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
