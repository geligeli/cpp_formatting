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
