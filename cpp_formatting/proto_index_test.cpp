#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "cpp_formatting/cpp_index_merge.h"
#include "cpp_formatting/index.pb.h"
#include "cpp_formatting/proto_index_lib.h"
#include "google/protobuf/compiler/importer.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/io/zero_copy_stream_impl_lite.h"
#include "gtest/gtest.h"

namespace {

namespace gpb = google::protobuf;

using cpp_index::IndexUnit;
using cpp_index::Occurrence;
using cpp_index::Symbol;

/// Files by import name, in memory.
class MemoryTree : public gpb::compiler::SourceTree {
 public:
  void add(const std::string& Name, std::string Text) {
    Files[Name] = std::move(Text);
  }
  auto Open(absl::string_view Name) -> gpb::io::ZeroCopyInputStream* override {
    auto It = Files.find(std::string(Name));
    if (It == Files.end()) return nullptr;
    return new gpb::io::ArrayInputStream(It->second.data(),
                                         static_cast<int>(It->second.size()));
  }
  auto text(const std::string& Name) const -> const std::string& {
    return Files.at(Name);
  }

 private:
  std::map<std::string, std::string> Files;
};

/// `[begin, end)` of the \p Nth (from 0) \p Needle in \p Text.
auto rangeOf(const std::string& Text, const std::string& Needle, int Nth = 0)
    -> std::pair<uint32_t, uint32_t> {
  size_t At = Text.find(Needle);
  for (int I = 0; I < Nth && At != std::string::npos; ++I)
    At = Text.find(Needle, At + 1);
  EXPECT_NE(At, std::string::npos) << Needle << " #" << Nth;
  return {static_cast<uint32_t>(At), static_cast<uint32_t>(At + Needle.size())};
}

auto symbolIndex(const IndexUnit& U, const std::string& Usr) -> int32_t {
  for (int32_t I = 0; I < U.symbols_size(); ++I)
    if (U.symbols(I).usr() == Usr) return I;
  return -1;
}

auto symbol(const IndexUnit& U, const std::string& Usr) -> const Symbol* {
  const int32_t I = symbolIndex(U, Usr);
  return I < 0 ? nullptr : &U.symbols(I);
}

/// The roles of \p Usr's occurrence on exactly \p R in \p Path; 0 when none.
auto rolesAt(const IndexUnit& U, const std::string& Path,
             std::pair<uint32_t, uint32_t> R, const std::string& Usr)
    -> uint32_t {
  for (const Occurrence& O : U.occurrences())
    if (U.files(O.file()).path() == Path && O.begin() == R.first &&
        O.end() == R.second && U.symbols(O.symbol()).usr() == Usr)
      return O.roles();
  return 0;
}

auto isChildOf(const IndexUnit& U, const std::string& Usr,
               const std::string& Parent) -> bool {
  const Symbol* S = symbol(U, Usr);
  if (!S) return false;
  for (const cpp_index::Relation& R : S->relations())
    if (R.kind() == cpp_index::CHILD_OF &&
        U.symbols(R.symbol()).usr() == Parent)
      return true;
  return false;
}

auto index(MemoryTree& Tree, const std::string& Name,
           const ProtoIndexOptions& Options = {}) -> IndexUnit {
  IndexUnit U;
  std::string Error;
  EXPECT_TRUE(indexProtoFile(Tree, Name, Options, U, Error)) << Error;
  return U;
}

auto bytes(const IndexUnit& U) -> std::string {
  std::string Out;
  U.SerializeToString(&Out);
  return Out;
}

constexpr char kShop[] = R"(syntax = "proto3";

package shop.v1;

enum Color {
  COLOR_UNSPECIFIED = 0;
  RED = 1;
}

message Item {
  string name = 1;
  Color color = 2;
  repeated Part parts = 3;
  message Part {
    int32 weight = 1;
  }
  oneof price {
    int64 cents = 4;
    Part barter = 5;
  }
}

service Shop {
  rpc Find(Item) returns (stream Item.Part);
}
)";

TEST(ProtoIndex, DefinitionsKindsAndTypes) {
  MemoryTree Tree;
  Tree.add("shop/shop.proto", kShop);
  const IndexUnit U = index(Tree, "shop/shop.proto");
  const std::string& T = Tree.text("shop/shop.proto");

  ASSERT_EQ(U.files_size(), 1);
  EXPECT_EQ(U.files(0).path(), "shop/shop.proto");
  EXPECT_EQ(U.files(0).kind(), cpp_index::SOURCE);
  ASSERT_EQ(U.translation_units_size(), 1);
  EXPECT_EQ(U.translation_units(0), "shop/shop.proto");
  EXPECT_EQ(U.producer(), "cpp_format");

  const Symbol* Item = symbol(U, "proto:shop.v1.Item");
  ASSERT_NE(Item, nullptr);
  EXPECT_EQ(Item->name(), "Item");
  EXPECT_EQ(Item->qualified_name(), "shop.v1.Item");
  EXPECT_EQ(Item->kind(), cpp_index::MESSAGE);
  EXPECT_EQ(Item->language(), cpp_index::PROTO);
  ASSERT_TRUE(Item->has_canonical());
  EXPECT_EQ(Item->canonical().begin(), rangeOf(T, "Item").first);
  EXPECT_EQ(Item->canonical().end(), rangeOf(T, "Item").second);

  EXPECT_EQ(
      rolesAt(U, "shop/shop.proto", rangeOf(T, "Item"), "proto:shop.v1.Item"),
      cpp_index::DEFINITION);
  EXPECT_EQ(rolesAt(U, "shop/shop.proto", rangeOf(T, "name"),
                    "proto:shop.v1.Item.name"),
            cpp_index::DEFINITION);
  EXPECT_EQ(symbol(U, "proto:shop.v1.Item.name")->kind(), cpp_index::FIELD);
  EXPECT_EQ(symbol(U, "proto:shop.v1.Item.name")->type(), "string = 1");
  EXPECT_EQ(symbol(U, "proto:shop.v1.Item.color")->type(), "shop.v1.Color = 2");
  EXPECT_EQ(symbol(U, "proto:shop.v1.Item.parts")->type(),
            "repeated shop.v1.Item.Part = 3");

  EXPECT_EQ(symbol(U, "proto:shop.v1.Color")->kind(), cpp_index::ENUM);
  // An enumerator's full name is a sibling of its enum, as in C++.
  const Symbol* Red = symbol(U, "proto:shop.v1.RED");
  ASSERT_NE(Red, nullptr);
  EXPECT_EQ(Red->kind(), cpp_index::ENUM_CONSTANT);
  EXPECT_EQ(Red->type(), "= 1");
  EXPECT_EQ(
      rolesAt(U, "shop/shop.proto", rangeOf(T, "RED"), "proto:shop.v1.RED"),
      cpp_index::DEFINITION);

  EXPECT_EQ(symbol(U, "proto:shop.v1.Item.Part")->kind(), cpp_index::MESSAGE);
  EXPECT_EQ(symbol(U, "proto:shop.v1.Item.price")->kind(), cpp_index::ONEOF);
  EXPECT_EQ(symbol(U, "proto:shop.v1.Shop")->kind(), cpp_index::SERVICE);
  const Symbol* Find = symbol(U, "proto:shop.v1.Shop.Find");
  ASSERT_NE(Find, nullptr);
  EXPECT_EQ(Find->kind(), cpp_index::RPC);
  EXPECT_EQ(Find->type(), "(shop.v1.Item) returns (stream shop.v1.Item.Part)");

  // The package is declared, not defined: every file that says it does.
  const Symbol* Package = symbol(U, "proto:shop.v1");
  ASSERT_NE(Package, nullptr);
  EXPECT_EQ(Package->kind(), cpp_index::PACKAGE);
  EXPECT_FALSE(Package->has_canonical());
  EXPECT_EQ(
      rolesAt(U, "shop/shop.proto", rangeOf(T, "shop.v1"), "proto:shop.v1"),
      cpp_index::DECLARATION);
}

TEST(ProtoIndex, ReferencesAndMembership) {
  MemoryTree Tree;
  Tree.add("shop/shop.proto", kShop);
  const IndexUnit U = index(Tree, "shop/shop.proto");
  const std::string& T = Tree.text("shop/shop.proto");
  const std::string P = "shop/shop.proto";

  // `Color color = 2;`: the type, not the enum's own definition above it.
  EXPECT_EQ(rolesAt(U, P, rangeOf(T, "Color", 1), "proto:shop.v1.Color"),
            cpp_index::REFERENCE);
  EXPECT_EQ(rolesAt(U, P, rangeOf(T, "Part", 0), "proto:shop.v1.Item.Part"),
            cpp_index::REFERENCE);  // repeated Part parts
  EXPECT_EQ(rolesAt(U, P, rangeOf(T, "Part", 1), "proto:shop.v1.Item.Part"),
            cpp_index::DEFINITION);  // message Part
  EXPECT_EQ(rolesAt(U, P, rangeOf(T, "Part", 2), "proto:shop.v1.Item.Part"),
            cpp_index::REFERENCE);  // Part barter
  // The rpc's types: `stream` is not part of the name, a dotted name is.
  EXPECT_EQ(rolesAt(U, P, rangeOf(T, "Item", 1), "proto:shop.v1.Item"),
            cpp_index::REFERENCE);
  EXPECT_EQ(rolesAt(U, P, rangeOf(T, "Item.Part"), "proto:shop.v1.Item.Part"),
            cpp_index::REFERENCE);

  EXPECT_TRUE(isChildOf(U, "proto:shop.v1.Item", "proto:shop.v1"));
  EXPECT_TRUE(isChildOf(U, "proto:shop.v1.Item.name", "proto:shop.v1.Item"));
  EXPECT_TRUE(isChildOf(U, "proto:shop.v1.Item.Part", "proto:shop.v1.Item"));
  EXPECT_TRUE(isChildOf(U, "proto:shop.v1.Item.Part.weight",
                        "proto:shop.v1.Item.Part"));
  EXPECT_TRUE(isChildOf(U, "proto:shop.v1.RED", "proto:shop.v1.Color"));
  EXPECT_TRUE(isChildOf(U, "proto:shop.v1.Item.price", "proto:shop.v1.Item"));
  // A oneof's field is the message's and the oneof's.
  EXPECT_TRUE(isChildOf(U, "proto:shop.v1.Item.cents", "proto:shop.v1.Item"));
  EXPECT_TRUE(
      isChildOf(U, "proto:shop.v1.Item.cents", "proto:shop.v1.Item.price"));
  EXPECT_TRUE(isChildOf(U, "proto:shop.v1.Shop.Find", "proto:shop.v1.Shop"));
}

// The tokenizer counts a tab as "up to the next multiple of eight" columns and
// every other byte -- a UTF-8 continuation byte, a BOM's three -- as one.
TEST(ProtoIndex, TabsAndMultibyteTextStillGiveByteOffsets) {
  MemoryTree Tree;
  Tree.add("t.proto",
           "\xEF\xBB\xBFsyntax = \"proto3\";\n"
           "message M {\n"
           "\t// gr\xC3\xBC\xC3\x9F"
           "e\n"
           "\tM\t\tnext = 1;  /* \xC3\xA9 */ int32\tafter = 2;\n"
           "}\n");
  const IndexUnit U = index(Tree, "t.proto");
  const std::string& T = Tree.text("t.proto");
  EXPECT_EQ(rolesAt(U, "t.proto", rangeOf(T, "M", 0), "proto:M"),
            cpp_index::DEFINITION);
  EXPECT_EQ(rolesAt(U, "t.proto", rangeOf(T, "M", 1), "proto:M"),
            cpp_index::REFERENCE);
  EXPECT_EQ(rolesAt(U, "t.proto", rangeOf(T, "next"), "proto:M.next"),
            cpp_index::DEFINITION);
  EXPECT_EQ(rolesAt(U, "t.proto", rangeOf(T, "after"), "proto:M.after"),
            cpp_index::DEFINITION);
}

TEST(ProtoIndex, MapValueIsAReferenceAndTheEntryMessageIsNotASymbol) {
  MemoryTree Tree;
  Tree.add("m.proto", R"(syntax = "proto3";
message Value {}
enum Kind { KIND_UNSPECIFIED = 0; }
message Holder {
  map<string, Value> by_name = 1;
  map<int32,Kind>by_id = 2;
  map<string, int32> counts = 3;
  optional int32 maybe = 4;
}
)");
  const IndexUnit U = index(Tree, "m.proto");
  const std::string& T = Tree.text("m.proto");
  EXPECT_EQ(rolesAt(U, "m.proto", rangeOf(T, "Value", 1), "proto:Value"),
            cpp_index::REFERENCE);
  EXPECT_EQ(rolesAt(U, "m.proto", rangeOf(T, "Kind", 1), "proto:Kind"),
            cpp_index::REFERENCE);
  EXPECT_EQ(symbol(U, "proto:Holder.by_name")->type(),
            "map<string, Value> = 1");
  EXPECT_EQ(symbol(U, "proto:Holder.ByNameEntry"), nullptr);
  EXPECT_EQ(symbol(U, "proto:Holder.ByNameEntry.value"), nullptr);
  // A proto3 `optional` is a oneof nobody wrote.
  EXPECT_EQ(symbol(U, "proto:Holder.maybe")->type(), "optional int32 = 4");
  EXPECT_EQ(symbol(U, "proto:Holder._maybe"), nullptr);
  for (const Symbol& S : U.symbols()) EXPECT_NE(S.kind(), cpp_index::ONEOF);
}

TEST(ProtoIndex, GroupsAndExtensions) {
  MemoryTree Tree;
  Tree.add("g.proto", R"(syntax = "proto2";
package g;
message Base {
  extensions 100 to 199;
  optional group Result = 1 {
    optional string url = 2;
  }
}
extend Base {
  optional int32 extra = 100;
}
message Scope {
  extend Base {
    optional Scope nested = 101;
  }
}
)");
  const IndexUnit U = index(Tree, "g.proto");
  const std::string& T = Tree.text("g.proto");
  // One token names the group's message and its (lowercased) field.
  EXPECT_EQ(rolesAt(U, "g.proto", rangeOf(T, "Result"), "proto:g.Base.Result"),
            cpp_index::DEFINITION);
  EXPECT_EQ(rolesAt(U, "g.proto", rangeOf(T, "Result"), "proto:g.Base.result"),
            cpp_index::DEFINITION);
  EXPECT_EQ(symbol(U, "proto:g.Base.result")->type(), "optional group = 1");

  // An extension is named in the scope that declares it, and references the
  // message it extends.
  const Symbol* Extra = symbol(U, "proto:g.extra");
  ASSERT_NE(Extra, nullptr);
  EXPECT_EQ(Extra->type(), "optional int32 (extends g.Base) = 100");
  EXPECT_EQ(rolesAt(U, "g.proto", rangeOf(T, "Base", 1), "proto:g.Base"),
            cpp_index::REFERENCE);
  EXPECT_TRUE(isChildOf(U, "proto:g.Scope.nested", "proto:g.Scope"));
  EXPECT_EQ(rolesAt(U, "g.proto", rangeOf(T, "Base", 2), "proto:g.Base"),
            cpp_index::REFERENCE);
  EXPECT_EQ(rolesAt(U, "g.proto", rangeOf(T, "Scope", 1), "proto:g.Scope"),
            cpp_index::REFERENCE);
}

constexpr char kDep[] = R"(syntax = "proto3";
package lib;
message Shared {
  int32 id = 1;
}
)";

constexpr char kUser[] = R"(syntax = "proto3";
package app;
import "lib/dep.proto";
message User {
  lib.Shared shared = 1;
}
)";

// Under Bazel an import name resolves to a `_virtual_imports` symlink; the
// index names the source, for the file being indexed and for what it imports.
TEST(ProtoIndex, ImportedSymbolsCarryTheirLocationUnderTheMappedPath) {
  MemoryTree Tree;
  Tree.add("lib/dep.proto", kDep);
  Tree.add("app/user.proto", kUser);
  ProtoIndexOptions Options;
  Options.PathMap = {{"lib/dep.proto", "third_party/lib/src/lib/dep.proto"},
                     {"app/user.proto", "external/app+/app/user.proto"}};
  const IndexUnit U = index(Tree, "app/user.proto", Options);

  ASSERT_EQ(U.files_size(), 2);
  EXPECT_EQ(U.files(0).path(), "external/app+/app/user.proto");
  EXPECT_EQ(U.files(0).kind(), cpp_index::EXTERNAL);
  EXPECT_EQ(U.files(1).path(), "third_party/lib/src/lib/dep.proto");

  const Symbol* Shared = symbol(U, "proto:lib.Shared");
  ASSERT_NE(Shared, nullptr);
  EXPECT_EQ(Shared->kind(), cpp_index::MESSAGE);
  ASSERT_TRUE(Shared->has_canonical());
  EXPECT_EQ(U.files(Shared->canonical().file()).path(),
            "third_party/lib/src/lib/dep.proto");
  EXPECT_EQ(Shared->canonical().begin(), rangeOf(kDep, "Shared").first);
  // Referenced here, defined there: no occurrence in the imported file, and
  // nothing of it that this file does not name.
  EXPECT_EQ(rolesAt(U, "external/app+/app/user.proto",
                    rangeOf(kUser, "lib.Shared"), "proto:lib.Shared"),
            cpp_index::REFERENCE);
  for (const Occurrence& O : U.occurrences()) EXPECT_EQ(O.file(), 0);
  EXPECT_EQ(symbol(U, "proto:lib.Shared.id"), nullptr);

  // The two units agree on everything they both say, so their merge does not
  // depend on the order.
  const IndexUnit Dep = index(Tree, "lib/dep.proto", Options);
  EXPECT_EQ(bytes(mergeUnits({U, Dep})), bytes(mergeUnits({Dep, U})));
  const IndexUnit Both = mergeUnits({U, Dep});
  EXPECT_TRUE(isChildOf(Both, "proto:lib.Shared", "proto:lib"));
}

TEST(ProtoIndex, ResolvePathNamesWhatTheMapDoesNot) {
  MemoryTree Tree;
  Tree.add("lib/dep.proto", kDep);
  ProtoIndexOptions Options;
  Options.ResolvePath = [](const std::string& Import) {
    return "protos/" + Import;
  };
  const IndexUnit U = index(Tree, "lib/dep.proto", Options);
  ASSERT_EQ(U.files_size(), 1);
  EXPECT_EQ(U.files(0).path(), "protos/lib/dep.proto");
}

auto annotate(gpb::GeneratedCodeInfo& Info, const std::string& Source,
              std::vector<int> Path, int Begin, int End,
              gpb::GeneratedCodeInfo::Annotation::Semantic Semantic =
                  gpb::GeneratedCodeInfo::Annotation::NONE) {
  gpb::GeneratedCodeInfo::Annotation* A = Info.add_annotation();
  A->set_source_file(Source);
  for (const int P : Path) A->add_path(P);
  A->set_begin(Begin);
  A->set_end(End);
  if (Semantic != gpb::GeneratedCodeInfo::Annotation::NONE)
    A->set_semantic(Semantic);
}

// What protoc says about a generated header becomes occurrences of the proto
// symbols *in that header*: bytes of a file, never a C++ name.
TEST(ProtoIndex, AnchorsAreOccurrencesInTheGeneratedFile) {
  MemoryTree Tree;
  Tree.add("shop/shop.proto", kShop);
  using Annotation = gpb::GeneratedCodeInfo::Annotation;
  gpb::GeneratedCodeInfo Info;
  annotate(Info, "shop/shop.proto", {4, 0}, 100, 104);        // Item
  annotate(Info, "shop/shop.proto", {4, 0, 2, 0}, 200, 204);  // name()
  annotate(Info, "shop/shop.proto", {4, 0, 2, 0}, 210, 218,   // set_name
           Annotation::SET);
  annotate(Info, "shop/shop.proto", {4, 0, 2, 0}, 220, 232,  // mutable_name
           Annotation::ALIAS);
  annotate(Info, "shop/shop.proto", {4, 0, 3, 0}, 300, 309);        // Item_Part
  annotate(Info, "shop/shop.proto", {4, 0, 3, 0, 2, 0}, 310, 316);  // weight
  annotate(Info, "shop/shop.proto", {4, 0, 8, 0}, 400, 410);  // price_case
  annotate(Info, "shop/shop.proto", {5, 0}, 500, 505);        // Color
  annotate(Info, "shop/shop.proto", {5, 0, 2, 1}, 510, 513);  // RED
  annotate(Info, "shop/shop.proto", {6, 0, 2, 0}, 600, 604);  // Find
  // Not ours to describe: out of range, a sub-field, another file, no range.
  annotate(Info, "shop/shop.proto", {4, 9}, 700, 701);
  annotate(Info, "shop/shop.proto", {4, 0, 2, 0, 1}, 702, 703);
  annotate(Info, "other.proto", {4, 0}, 704, 705);
  annotate(Info, "shop/shop.proto", {4, 0}, 706, 706);

  ProtoIndexOptions Options;
  ProtoAnchors Anchors;
  Info.SerializeToString(&Anchors.Metadata);
  Anchors.GeneratedPath = "bazel-out/k8/bin/shop/shop.pb.h";
  Options.Anchors.push_back(Anchors);
  const IndexUnit U = index(Tree, "shop/shop.proto", Options);

  const std::string H = "bazel-out/k8/bin/shop/shop.pb.h";
  ASSERT_EQ(U.files_size(), 2);
  EXPECT_EQ(U.files(0).path(), H);
  EXPECT_EQ(U.files(0).kind(), cpp_index::GENERATED);

  const uint32_t G = cpp_index::GENERATES;
  EXPECT_EQ(rolesAt(U, H, {100, 104}, "proto:shop.v1.Item"), G);
  EXPECT_EQ(rolesAt(U, H, {200, 204}, "proto:shop.v1.Item.name"), G);
  EXPECT_EQ(rolesAt(U, H, {210, 218}, "proto:shop.v1.Item.name"),
            G | cpp_index::WRITE);
  EXPECT_EQ(rolesAt(U, H, {220, 232}, "proto:shop.v1.Item.name"),
            G | cpp_index::WRITE);
  EXPECT_EQ(rolesAt(U, H, {300, 309}, "proto:shop.v1.Item.Part"), G);
  EXPECT_EQ(rolesAt(U, H, {310, 316}, "proto:shop.v1.Item.Part.weight"), G);
  EXPECT_EQ(rolesAt(U, H, {400, 410}, "proto:shop.v1.Item.price"), G);
  EXPECT_EQ(rolesAt(U, H, {500, 505}, "proto:shop.v1.Color"), G);
  EXPECT_EQ(rolesAt(U, H, {510, 513}, "proto:shop.v1.RED"), G);
  EXPECT_EQ(rolesAt(U, H, {600, 604}, "proto:shop.v1.Shop.Find"), G);
  int InHeader = 0;
  for (const Occurrence& O : U.occurrences())
    if (U.files(O.file()).path() == H) ++InHeader;
  EXPECT_EQ(InHeader, 10);

  // And that is all the merge needs: a C++ symbol declared on an anchored
  // range is generated from the anchor's symbol.
  IndexUnit Cxx;
  cpp_index::File* F = Cxx.add_files();
  F->set_path(H);
  F->set_kind(cpp_index::GENERATED);
  Symbol* Setter = Cxx.add_symbols();
  Setter->set_usr("c:@N@shop@N@v1@S@Item@F@set_name#");
  Setter->set_language(cpp_index::CXX);
  Setter->mutable_canonical()->set_file(0);
  Setter->mutable_canonical()->set_begin(210);
  Setter->mutable_canonical()->set_end(218);
  IndexUnit Merged = mergeUnits({U, Cxx});
  EXPECT_EQ(linkGenerated(Merged), 1u);
  const Symbol* Linked = symbol(Merged, "c:@N@shop@N@v1@S@Item@F@set_name#");
  ASSERT_EQ(Linked->relations_size(), 1);
  EXPECT_EQ(Linked->relations(0).kind(), cpp_index::GENERATED_FROM);
  EXPECT_EQ(Merged.symbols(Linked->relations(0).symbol()).usr(),
            "proto:shop.v1.Item.name");
}

TEST(ProtoIndex, OutputIsDeterministicAndCanonical) {
  MemoryTree Tree;
  Tree.add("shop/shop.proto", kShop);
  const IndexUnit A = index(Tree, "shop/shop.proto");
  const IndexUnit B = index(Tree, "shop/shop.proto");
  EXPECT_EQ(bytes(A), bytes(B));
  IndexUnit Again = A;
  normalizeUnit(Again);
  EXPECT_EQ(bytes(A), bytes(Again));
}

TEST(ProtoIndex, AFileThatDoesNotCompileIsAnErrorWithItsDiagnostics) {
  MemoryTree Tree;
  Tree.add("bad.proto",
           "syntax = \"proto3\";\nmessage M {\n  Missing m = 1;\n}\n");
  IndexUnit U;
  std::string Error;
  EXPECT_FALSE(indexProtoFile(Tree, "bad.proto", {}, U, Error));
  EXPECT_NE(Error.find("bad.proto:3:3"), std::string::npos) << Error;
  EXPECT_NE(Error.find("Missing"), std::string::npos) << Error;
  EXPECT_FALSE(indexProtoFile(Tree, "absent.proto", {}, U, Error));
}

}  // namespace
