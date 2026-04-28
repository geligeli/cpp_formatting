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
