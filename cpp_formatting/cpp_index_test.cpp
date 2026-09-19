#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cpp_formatting/cpp_index_lib.h"
#include "cpp_formatting/cpp_index_merge.h"

using cpp_index::IndexUnit;
using cpp_index::Occurrence;
using cpp_index::Symbol;

namespace {

/// The index of the symbol named \p Name (unqualified); -1 when absent or
/// ambiguous.
auto symbolNamed(const IndexUnit& U, llvm::StringRef Name) -> int32_t {
  int32_t Found = -1;
  for (int32_t I = 0; I < U.symbols_size(); ++I) {
    if (U.symbols(I).name() != Name) continue;
    if (Found >= 0) return -1;
    Found = I;
  }
  return Found;
}

auto symbolQualified(const IndexUnit& U, llvm::StringRef Qualified) -> int32_t {
  for (int32_t I = 0; I < U.symbols_size(); ++I)
    if (U.symbols(I).qualified_name() == Qualified) return I;
  return -1;
}

auto occurrencesOf(const IndexUnit& U, int32_t Sym)
    -> std::vector<const Occurrence*> {
  std::vector<const Occurrence*> Out;
  for (const Occurrence& O : U.occurrences())
    if (O.symbol() == Sym) Out.push_back(&O);
  return Out;
}

auto fileOf(const IndexUnit& U, const Occurrence& O) -> std::string {
  return U.files(O.file()).path();
}

/// The byte offset of the \p Nth (0-based) occurrence of \p Needle in \p Code.
auto offsetOf(llvm::StringRef Code, llvm::StringRef Needle, unsigned Nth = 0)
    -> uint32_t {
  size_t Pos = Code.find(Needle);
  for (unsigned I = 0; I < Nth && Pos != llvm::StringRef::npos; ++I)
    Pos = Code.find(Needle, Pos + 1);
  EXPECT_NE(Pos, llvm::StringRef::npos) << "needle not found: " << Needle.str();
  return static_cast<uint32_t>(Pos);
}

/// The occurrence of \p Sym whose range starts at \p Begin, or nullptr.
auto occurrenceAt(const IndexUnit& U, int32_t Sym, uint32_t Begin)
    -> const Occurrence* {
  for (const Occurrence* O : occurrencesOf(U, Sym))
    if (O->begin() == Begin) return O;
  return nullptr;
}

auto hasRole(const Occurrence* O, cpp_index::Role R) -> bool {
  return O && (O->roles() & static_cast<uint32_t>(R));
}

auto hasRelation(
    const google::protobuf::RepeatedPtrField<cpp_index::Relation>& Rels,
    cpp_index::RelationKind K, int32_t Target) -> bool {
  for (const cpp_index::Relation& R : Rels)
    if (R.kind() == K && R.symbol() == Target) return true;
  return false;
}

}  // namespace

TEST(CppIndex, StructFieldAndMethod) {
  const std::string Code = R"(
    namespace demo {
    struct Widget {
      int count;
      int get() const;
    };
    int Widget::get() const { return count; }
    }  // namespace demo
  )";
  const IndexUnit U = indexCode(Code);
  ASSERT_EQ(U.files_size(), 1);
  EXPECT_EQ(U.files(0).path(), "input.cc");
  EXPECT_EQ(U.files(0).kind(), cpp_index::SOURCE);
  ASSERT_EQ(U.translation_units_size(), 1);
  EXPECT_EQ(U.translation_units(0), "input.cc");
  EXPECT_EQ(U.producer(), "cpp_format");
  EXPECT_EQ(U.schema_version(), kIndexSchemaVersion);

  const int32_t Demo = symbolNamed(U, "demo");
  const int32_t Widget = symbolNamed(U, "Widget");
  const int32_t Count = symbolNamed(U, "count");
  const int32_t Get = symbolNamed(U, "get");
  ASSERT_GE(Demo, 0);
  ASSERT_GE(Widget, 0);
  ASSERT_GE(Count, 0);
  ASSERT_GE(Get, 0);

  EXPECT_EQ(U.symbols(Demo).kind(), cpp_index::NAMESPACE);
  EXPECT_EQ(U.symbols(Widget).kind(), cpp_index::STRUCT);
  EXPECT_EQ(U.symbols(Widget).qualified_name(), "demo::Widget");
  EXPECT_EQ(U.symbols(Widget).usr(), "c:@N@demo@S@Widget");
  EXPECT_EQ(U.symbols(Widget).language(), cpp_index::CXX);
  EXPECT_TRUE(
      hasRelation(U.symbols(Widget).relations(), cpp_index::CHILD_OF, Demo));
  EXPECT_EQ(U.symbols(Count).kind(), cpp_index::FIELD);
  EXPECT_EQ(U.symbols(Count).type(), "int");
  EXPECT_EQ(U.symbols(Count).qualified_name(), "demo::Widget::count");
  EXPECT_TRUE(
      hasRelation(U.symbols(Count).relations(), cpp_index::CHILD_OF, Widget));
  EXPECT_EQ(U.symbols(Get).kind(), cpp_index::INSTANCE_METHOD);
  EXPECT_EQ(U.symbols(Get).type(), "int () const");

  // The field: one definition, one read reference inside get().
  const Occurrence* CountDef = occurrenceAt(U, Count, offsetOf(Code, "count;"));
  ASSERT_NE(CountDef, nullptr);
  EXPECT_TRUE(hasRole(CountDef, cpp_index::DEFINITION));
  EXPECT_EQ(CountDef->end(), CountDef->begin() + 5);
  EXPECT_EQ(CountDef->macro(), cpp_index::NOT_IN_MACRO);
  const Occurrence* CountRef =
      occurrenceAt(U, Count, offsetOf(Code, "count; }"));
  ASSERT_NE(CountRef, nullptr);
  EXPECT_TRUE(hasRole(CountRef, cpp_index::REFERENCE));
  EXPECT_TRUE(hasRole(CountRef, cpp_index::READ));
  EXPECT_TRUE(hasRelation(CountRef->relations(), cpp_index::CONTAINED_BY, Get));
  EXPECT_EQ(occurrencesOf(U, Count).size(), 2u);

  // The method: an in-class declaration and an out-of-line definition, both
  // children of Widget, and the definition's qualifier references Widget.
  const Occurrence* GetDecl =
      occurrenceAt(U, Get, offsetOf(Code, "get() const;"));
  const Occurrence* GetDef =
      occurrenceAt(U, Get, offsetOf(Code, "get() const {"));
  ASSERT_NE(GetDecl, nullptr);
  ASSERT_NE(GetDef, nullptr);
  EXPECT_TRUE(hasRole(GetDecl, cpp_index::DECLARATION));
  EXPECT_FALSE(hasRole(GetDecl, cpp_index::DEFINITION));
  EXPECT_TRUE(hasRole(GetDef, cpp_index::DEFINITION));
  EXPECT_TRUE(
      hasRelation(U.symbols(Get).relations(), cpp_index::CHILD_OF, Widget));
  const Occurrence* WidgetQual =
      occurrenceAt(U, Widget, offsetOf(Code, "Widget::get"));
  ASSERT_NE(WidgetQual, nullptr);
  EXPECT_TRUE(hasRole(WidgetQual, cpp_index::REFERENCE));

  // The symbol's canonical location is its first declaration.
  ASSERT_TRUE(U.symbols(Widget).has_canonical());
  EXPECT_EQ(U.symbols(Widget).canonical().begin(), offsetOf(Code, "Widget {"));
  EXPECT_EQ(U.symbols(Widget).canonical().end(),
            offsetOf(Code, "Widget {") + 6);
}

TEST(CppIndex, FreeFunctionDeclarationDefinitionAndCall) {
  const std::string Code = R"(
    int f(int a);
    int f(int a) { return a; }
    void g() { f(1); }
  )";
  const IndexUnit U = indexCode(Code);
  const int32_t F = symbolNamed(U, "f");
  const int32_t G = symbolNamed(U, "g");
  ASSERT_GE(F, 0);
  ASSERT_GE(G, 0);
  EXPECT_EQ(U.symbols(F).kind(), cpp_index::FUNCTION);
  EXPECT_EQ(U.symbols(F).type(), "int (int)");

  EXPECT_TRUE(hasRole(occurrenceAt(U, F, offsetOf(Code, "f(int a);")),
                      cpp_index::DECLARATION));
  EXPECT_TRUE(hasRole(occurrenceAt(U, F, offsetOf(Code, "f(int a) {")),
                      cpp_index::DEFINITION));
  const Occurrence* Call = occurrenceAt(U, F, offsetOf(Code, "f(1)"));
  ASSERT_NE(Call, nullptr);
  EXPECT_TRUE(hasRole(Call, cpp_index::REFERENCE));
  EXPECT_TRUE(hasRole(Call, cpp_index::CALL));
  EXPECT_TRUE(hasRelation(Call->relations(), cpp_index::CALLED_BY, G));

  // Parameters are indexed in declarations as well as definitions: `a` has
  // two declaration-ish occurrences plus the read in the body.  The two `a`s
  // are distinct parameter symbols (one per declaration), so look them up by
  // offset.
  int32_t ParamSymbols = 0;
  for (const Symbol& S : U.symbols())
    if (S.name() == "a") {
      EXPECT_EQ(S.kind(), cpp_index::PARAMETER);
      ++ParamSymbols;
    }
  EXPECT_GE(ParamSymbols, 1);
  bool SawDeclParam = false;
  for (const Occurrence& O : U.occurrences())
    if (O.begin() == offsetOf(Code, "a);")) SawDeclParam = true;
  EXPECT_TRUE(SawDeclParam);
}

TEST(CppIndex, EnumAndEnumerator) {
  const std::string Code = R"(
    enum class Color { Red, Green };
    Color c = Color::Red;
  )";
  const IndexUnit U = indexCode(Code);
  const int32_t Color = symbolNamed(U, "Color");
  const int32_t Red = symbolNamed(U, "Red");
  ASSERT_GE(Color, 0);
  ASSERT_GE(Red, 0);
  EXPECT_EQ(U.symbols(Color).kind(), cpp_index::ENUM);
  EXPECT_EQ(U.symbols(Red).kind(), cpp_index::ENUM_CONSTANT);
  EXPECT_EQ(U.symbols(Red).qualified_name(), "Color::Red");
  EXPECT_TRUE(hasRole(occurrenceAt(U, Red, offsetOf(Code, "Red,")),
                      cpp_index::DEFINITION));
  EXPECT_TRUE(hasRole(occurrenceAt(U, Red, offsetOf(Code, "Red;")),
                      cpp_index::REFERENCE));
  // `Color` is referenced twice on the last line (the type and the qualifier)
  // and defined once.
  EXPECT_EQ(occurrencesOf(U, Color).size(), 3u);
}

TEST(CppIndex, TypeAliasVariableAndLocals) {
  const std::string Code = R"(
    using Int = int;
    Int v = 0;
    void f() {
      int x = 1;
      x = 2;
    }
  )";
  const IndexUnit U = indexCode(Code);
  const int32_t Int = symbolNamed(U, "Int");
  const int32_t V = symbolNamed(U, "v");
  const int32_t X = symbolNamed(U, "x");
  ASSERT_GE(Int, 0);
  ASSERT_GE(V, 0);
  ASSERT_GE(X, 0);
  EXPECT_EQ(U.symbols(Int).kind(), cpp_index::TYPE_ALIAS);
  EXPECT_EQ(U.symbols(V).kind(), cpp_index::VARIABLE);
  EXPECT_EQ(U.symbols(V).type(), "Int");
  EXPECT_TRUE(hasRole(occurrenceAt(U, Int, offsetOf(Code, "Int v")),
                      cpp_index::REFERENCE));
  // A function-local variable carries the LOCAL property; its assignment is a
  // write.
  EXPECT_TRUE(U.symbols(X).properties() & cpp_index::LOCAL);
  EXPECT_TRUE(hasRole(occurrenceAt(U, X, offsetOf(Code, "x = 1")),
                      cpp_index::DEFINITION));
  const Occurrence* Write = occurrenceAt(U, X, offsetOf(Code, "x = 2"));
  ASSERT_NE(Write, nullptr);
  EXPECT_TRUE(hasRole(Write, cpp_index::WRITE));
}

TEST(CppIndex, MacroDefinitionReferenceAndUndef) {
  const std::string Code = R"(
#define N 3
    int a[N];
#undef N
  )";
  const IndexUnit U = indexCode(Code);
  const int32_t N = symbolNamed(U, "N");
  ASSERT_GE(N, 0);
  EXPECT_EQ(U.symbols(N).kind(), cpp_index::MACRO);
  EXPECT_TRUE(llvm::StringRef(U.symbols(N).usr()).starts_with("c:input.cc@"));
  const Occurrence* Def = occurrenceAt(U, N, offsetOf(Code, "N 3"));
  const Occurrence* Ref = occurrenceAt(U, N, offsetOf(Code, "N]"));
  const Occurrence* Undef = occurrenceAt(U, N, offsetOf(Code, "N\n"));
  ASSERT_NE(Def, nullptr);
  ASSERT_NE(Ref, nullptr);
  ASSERT_NE(Undef, nullptr);
  EXPECT_TRUE(hasRole(Def, cpp_index::DEFINITION));
  EXPECT_TRUE(hasRole(Ref, cpp_index::REFERENCE));
  // The macro's own name token is ordinary source, not an expansion.
  EXPECT_EQ(Ref->macro(), cpp_index::NOT_IN_MACRO);
  EXPECT_TRUE(hasRole(Undef, cpp_index::UNDEFINITION));
  ASSERT_TRUE(U.symbols(N).has_canonical());
  EXPECT_EQ(U.symbols(N).canonical().begin(), offsetOf(Code, "N 3"));
}

TEST(CppIndex, MacroArgumentVersusMacroBody) {
  const std::string Code = R"(
    struct S {
      int m;
    };
#define FWD(x) (x)
#define GETM(s) ((s).m)
    int f(S s) { return FWD(s.m) + GETM(s); }
  )";
  const IndexUnit U = indexCode(Code);
  const int32_t M = symbolNamed(U, "m");
  ASSERT_GE(M, 0);
  // Spelled as a macro argument: the occurrence sits on the argument's own
  // spelling at the call site.
  const Occurrence* Arg = occurrenceAt(U, M, offsetOf(Code, "m) + GETM"));
  ASSERT_NE(Arg, nullptr);
  EXPECT_EQ(Arg->macro(), cpp_index::MACRO_ARGUMENT);
  EXPECT_EQ(Arg->end(), Arg->begin() + 1);
  EXPECT_TRUE(hasRole(Arg, cpp_index::REFERENCE));
  // Spelled in the macro body: the occurrence lands on the invocation's name
  // token, `GETM`, which the reference to `s` (an argument) does not.
  const Occurrence* Body = occurrenceAt(U, M, offsetOf(Code, "GETM(s); }"));
  ASSERT_NE(Body, nullptr);
  EXPECT_EQ(Body->macro(), cpp_index::MACRO_BODY);
  EXPECT_EQ(Body->end(), Body->begin() + 4);
  EXPECT_EQ(occurrencesOf(U, M).size(), 3u);  // definition + the two
}

TEST(CppIndex, OverrideAndBaseRelations) {
  const std::string Code = R"(
    struct B {
      virtual void f();
    };
    struct D : B {
      void f() override;
    };
  )";
  const IndexUnit U = indexCode(Code);
  const int32_t B = symbolNamed(U, "B");
  const int32_t D = symbolNamed(U, "D");
  const int32_t BF = symbolQualified(U, "B::f");
  const int32_t DF = symbolQualified(U, "D::f");
  ASSERT_GE(B, 0);
  ASSERT_GE(D, 0);
  ASSERT_GE(BF, 0);
  ASSERT_GE(DF, 0);
  EXPECT_TRUE(
      hasRelation(U.symbols(DF).relations(), cpp_index::OVERRIDE_OF, BF));
  EXPECT_TRUE(hasRelation(U.symbols(B).relations(), cpp_index::BASE_OF, D));
  // The base-specifier (the second `B {`) is a reference to B.
  EXPECT_TRUE(hasRole(occurrenceAt(U, B, offsetOf(Code, "B {", 1)),
                      cpp_index::REFERENCE));
}

TEST(CppIndex, TemplatesSpecializationsAndInstantiations) {
  const std::string Code = R"(
    template <class T>
    struct Box {
      T v;
    };
    template <>
    struct Box<int> {
      int v;
    };
    Box<int> bi;
    Box<char> bc;
    int use() { return bc.v; }
  )";
  const IndexUnit U = indexCode(Code);
  int32_t Primary = -1;
  int32_t Spec = -1;
  for (int32_t I = 0; I < U.symbols_size(); ++I) {
    const Symbol& S = U.symbols(I);
    if (S.name() != "Box") continue;
    if (S.properties() & cpp_index::TEMPLATE_SPECIALIZATION)
      Spec = I;
    else
      Primary = I;
  }
  ASSERT_GE(Primary, 0);
  ASSERT_GE(Spec, 0);
  EXPECT_TRUE(U.symbols(Primary).properties() & cpp_index::GENERIC);
  EXPECT_TRUE(hasRelation(U.symbols(Spec).relations(),
                          cpp_index::SPECIALIZATION_OF, Primary));
  // `Box<char>` instantiates the primary; the instantiation is not walked, so
  // the pattern's field is defined once and `bc.v` refers to that pattern.
  int32_t PatternV = -1;
  for (int32_t I = 0; I < U.symbols_size(); ++I)
    if (U.symbols(I).name() == "v" &&
        hasRelation(U.symbols(I).relations(), cpp_index::CHILD_OF, Primary))
      PatternV = I;
  ASSERT_GE(PatternV, 0);
  const std::vector<const Occurrence*> VOccs = occurrencesOf(U, PatternV);
  EXPECT_EQ(VOccs.size(), 2u);
  EXPECT_NE(occurrenceAt(U, PatternV, offsetOf(Code, "v;")), nullptr);  // T v;
  EXPECT_NE(occurrenceAt(U, PatternV, offsetOf(Code, "v; }")),
            nullptr);  // bc.v
  // The template parameter is a symbol of its own.
  const int32_t T = symbolNamed(U, "T");
  ASSERT_GE(T, 0);
  EXPECT_EQ(U.symbols(T).kind(), cpp_index::TEMPLATE_TYPE_PARM);
}

TEST(CppIndex, OwnedFilesDecideWhichOccurrencesAreRecorded) {
  const std::string Header = "struct W { int n; };\n";
  const std::string Code = R"(#include "w.h"
                                 int f(W w) { return w.n; }
  )";
  const clang::tooling::FileContentMappings Files = {{"w.h", Header}};

  // Not owned: the header's symbols are recorded (with their canonical
  // location in it) but none of its occurrences are.
  {
    const IndexUnit U = indexCode(Code, {"-std=c++17"}, Files);
    const int32_t W = symbolNamed(U, "W");
    const int32_t N = symbolNamed(U, "n");
    ASSERT_GE(W, 0);
    ASSERT_GE(N, 0);
    ASSERT_EQ(U.files_size(), 2);
    EXPECT_EQ(U.files(0).path(), "input.cc");
    EXPECT_EQ(U.files(1).path(), "w.h");
    ASSERT_TRUE(U.symbols(W).has_canonical());
    EXPECT_EQ(U.symbols(W).canonical().file(), 1);
    EXPECT_EQ(U.symbols(W).canonical().begin(), 7u);
    for (const Occurrence& O : U.occurrences())
      EXPECT_EQ(fileOf(U, O), "input.cc");
    EXPECT_EQ(occurrencesOf(U, N).size(), 1u);
    EXPECT_TRUE(hasRole(occurrencesOf(U, N)[0], cpp_index::REFERENCE));
  }
  // Owned: the header's definitions are recorded too.
  {
    const IndexUnit U = indexCode(Code, {"-std=c++17"}, Files, {"w.h"});
    const int32_t N = symbolNamed(U, "n");
    ASSERT_GE(N, 0);
    ASSERT_EQ(occurrencesOf(U, N).size(), 2u);
    const Occurrence* Def = occurrencesOf(U, N)[1];
    EXPECT_EQ(fileOf(U, *Def), "w.h");
    EXPECT_TRUE(hasRole(Def, cpp_index::DEFINITION));
    EXPECT_EQ(Def->begin(), static_cast<uint32_t>(Header.find("n;")));
  }
}

TEST(CppIndex, SystemHeadersAreNotFilteredByClang) {
  // Bazel's `includes = [...]` becomes -isystem, so a first-party header is a
  // system header in every dependent's TU; an owned one must still be indexed.
  const std::string Header =
      "namespace lib { struct Vec { void push(int); }; }\n";
  const std::string Code = R"(#include <vec.h>
                                 void f(lib::Vec& v) { v.push(1); }
  )";
  const clang::tooling::FileContentMappings Files = {{"sys/vec.h", Header}};
  const std::vector<std::string> Args = {"-std=c++17", "-isystem", "sys"};

  const IndexUnit Unowned = indexCode(Code, Args, Files);
  const int32_t Push = symbolNamed(Unowned, "push");
  ASSERT_GE(Push, 0);
  EXPECT_EQ(occurrencesOf(Unowned, Push).size(), 1u);  // the call only
  EXPECT_TRUE(hasRole(occurrencesOf(Unowned, Push)[0], cpp_index::CALL));
  ASSERT_TRUE(Unowned.symbols(Push).has_canonical());
  EXPECT_EQ(Unowned.files(Unowned.symbols(Push).canonical().file()).path(),
            "sys/vec.h");

  const IndexUnit Owned = indexCode(Code, Args, Files, {"sys/vec.h"});
  const int32_t Push2 = symbolNamed(Owned, "push");
  ASSERT_GE(Push2, 0);
  EXPECT_EQ(occurrencesOf(Owned, Push2).size(), 2u);
}

TEST(CppIndex, OutputIsDeterministic) {
  const std::string Code = R"(
#define TWICE(x) x x
    struct S {
      int a;
      int b;
      void f();
    };
    void S::f() { TWICE(a = b;) }
    template <class T>
    T id(T t) {
      return t;
    }
    int z = id(1) + id(2);
  )";
  std::string A;
  std::string B;
  indexCode(Code).SerializeToString(&A);
  indexCode(Code).SerializeToString(&B);
  EXPECT_FALSE(A.empty());
  EXPECT_EQ(A, B);
  // And it is already in canonical form.
  IndexUnit U = indexCode(Code);
  normalizeUnit(U);
  std::string C;
  U.SerializeToString(&C);
  EXPECT_EQ(A, C);
}

TEST(CppIndex, DependentTokensResolvedFromInstantiations) {
  const std::string Code = R"(
    struct A { int m; };
    struct B { int m; };
    template <class T> int get(T& t) { return t.m; }
    int use(A& a, B& b) { return get(a) + get(b); }
  )";
  const IndexUnit U = indexCode(Code);
  const int32_t AM = symbolQualified(U, "A::m");
  const int32_t BM = symbolQualified(U, "B::m");
  ASSERT_GE(AM, 0);
  ASSERT_GE(BM, 0);
  // `t.m` binds to both members, one DEPENDENT occurrence each, on the token
  // as written in the pattern.
  const uint32_t Tok = offsetOf(Code, "t.m") + 2;
  const Occurrence* ToA = occurrenceAt(U, AM, Tok);
  const Occurrence* ToB = occurrenceAt(U, BM, Tok);
  ASSERT_NE(ToA, nullptr);
  ASSERT_NE(ToB, nullptr);
  EXPECT_EQ(ToA->roles(),
            static_cast<uint32_t>(cpp_index::REFERENCE | cpp_index::DEPENDENT));
  EXPECT_EQ(ToB->roles(),
            static_cast<uint32_t>(cpp_index::REFERENCE | cpp_index::DEPENDENT));
  EXPECT_EQ(ToA->end(), Tok + 1);
  EXPECT_EQ(ToA->macro(), cpp_index::NOT_IN_MACRO);
  EXPECT_EQ(occurrencesOf(U, AM).size(), 2u);  // definition + the use
  // Resolved in this TU, so nothing is left pending.
  EXPECT_EQ(U.pending_size(), 0);
}

TEST(CppIndex, DependentTokenWithoutInstantiationIsPending) {
  const std::string Code = R"(
    struct A { int m; };
    template <class T> int get(T& t) { return t.m; }
  )";
  const IndexUnit U = indexCode(Code);
  ASSERT_EQ(U.pending_size(), 1);
  const cpp_index::DependentToken& P = U.pending(0);
  EXPECT_EQ(U.files(P.file()).path(), "input.cc");
  EXPECT_EQ(P.name(), "m");
  EXPECT_EQ(P.begin(), offsetOf(Code, "t.m") + 2);
  EXPECT_EQ(P.end(), P.begin() + 1);
  for (const Occurrence& O : U.occurrences())
    EXPECT_FALSE(O.roles() & cpp_index::DEPENDENT);
}

TEST(CppIndex, DependentQualifiedNameAndOverloadedCallBind) {
  const std::string Code = R"(
    template <class T> struct Tr { static const int k = 1; };
    template <class T> int g() { return Tr<T>::k; }
    int z = g<int>();
    struct S {
      void f(int);
      void f(double);
      template <class T> void call(T t) { f(t); }
    };
    void u(S& s) { s.call(1); s.call(2.0); }
  )";
  const IndexUnit U = indexCode(Code);
  // `Tr<T>::k` is a dependent *name*.  Clang's indexer already resolves it
  // through the primary template's definition (a plain REFERENCE); the
  // instantiation binds it to the pattern's static member -- not the member
  // of Tr<int> -- and the two reports merge into one occurrence carrying
  // both roles.
  const int32_t K = symbolNamed(U, "k");
  const int32_t Tr = symbolNamed(U, "Tr");
  ASSERT_GE(K, 0);
  ASSERT_GE(Tr, 0);
  EXPECT_TRUE(hasRelation(U.symbols(K).relations(), cpp_index::CHILD_OF, Tr));
  const Occurrence* KUse = occurrenceAt(U, K, offsetOf(Code, "::k") + 2);
  ASSERT_NE(KUse, nullptr);
  EXPECT_TRUE(hasRole(KUse, cpp_index::DEPENDENT));
  // `f(t)` with a dependent argument is an unresolved overload set in the
  // pattern; the two instantiations bind it to the two overloads.
  std::vector<int32_t> Fs;
  for (int32_t I = 0; I < U.symbols_size(); ++I)
    if (U.symbols(I).qualified_name() == "S::f") Fs.push_back(I);
  ASSERT_EQ(Fs.size(), 2u);
  for (const int32_t F : Fs) {
    const Occurrence* Use = occurrenceAt(U, F, offsetOf(Code, "f(t)"));
    ASSERT_NE(Use, nullptr) << U.symbols(F).type();
    EXPECT_TRUE(hasRole(Use, cpp_index::DEPENDENT));
  }
  EXPECT_EQ(U.pending_size(), 0);
}

TEST(CppIndex, DependentTokenAsMacroArgument) {
  const std::string Code = R"(
    #define CHECK(x) (void)(x)
    struct A { int m; };
    template <class T> void h(T& t) { CHECK(t.m); }
    void use(A& a) { h(a); }
  )";
  const IndexUnit U = indexCode(Code);
  const int32_t AM = symbolQualified(U, "A::m");
  ASSERT_GE(AM, 0);
  const Occurrence* Use = occurrenceAt(U, AM, offsetOf(Code, "t.m") + 2);
  ASSERT_NE(Use, nullptr);
  EXPECT_TRUE(hasRole(Use, cpp_index::DEPENDENT));
  EXPECT_EQ(Use->macro(), cpp_index::MACRO_ARGUMENT);
}

TEST(CppIndex, PastedMacroArgumentIsAnOccurrenceOfTheFormedName) {
  // ABSL_FLAG's shape: the flag's name is only ever pasted, so the variable
  // FLAGS_docker_image is spelled nowhere.  Its definition lands on the
  // invocation token; the argument that spelled its tail gets a PASTED
  // occurrence of the same symbol -- at every invocation that pastes it.
  const std::string Code = R"(
#define FLAG(name) int FLAGS_##name = 0;
#define GET(name) FLAGS_##name
    FLAG(docker_image)
    int use() { return GET(docker_image); }
  )";
  const IndexUnit U = indexCode(Code);
  const int32_t F = symbolNamed(U, "FLAGS_docker_image");
  ASSERT_GE(F, 0);
  const Occurrence* Def =
      occurrenceAt(U, F, offsetOf(Code, "FLAG(docker_image)"));
  ASSERT_NE(Def, nullptr);
  EXPECT_EQ(Def->macro(), cpp_index::MACRO_BODY);
  EXPECT_TRUE(hasRole(Def, cpp_index::DEFINITION));
  const Occurrence* Arg = occurrenceAt(U, F, offsetOf(Code, "docker_image)\n"));
  ASSERT_NE(Arg, nullptr);
  EXPECT_EQ(Arg->macro(), cpp_index::MACRO_ARGUMENT);
  EXPECT_EQ(Arg->roles(), static_cast<uint32_t>(cpp_index::PASTED));
  EXPECT_EQ(Arg->end(), Arg->begin() + 12);
  const Occurrence* Use =
      occurrenceAt(U, F, offsetOf(Code, "docker_image); }"));
  ASSERT_NE(Use, nullptr);
  EXPECT_EQ(Use->roles(), static_cast<uint32_t>(cpp_index::PASTED));
  // definition + reference on the two invocations, PASTED on both arguments
  EXPECT_EQ(occurrencesOf(U, F).size(), 4u);
}
