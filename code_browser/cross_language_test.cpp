// The API over an index of two languages: a .proto, the C++ generated from it
// and a C++ file that uses it.  The index is put together the way the pipeline
// does it -- a unit per producer, merged, then linked by the merge's one
// cross-language rule (linkGenerated) -- so nothing here hands the browser a
// GENERATED_FROM relation it would not get from `cpp_format --merge-index`.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "code_browser/api.h"
#include "code_browser/api.pb.h"
#include "code_browser/file_cache.h"
#include "code_browser/index_db.h"
#include "code_browser/index_schema.h"
#include "code_browser/repo.h"
#include "cpp_formatting/cpp_index_merge.h"
#include "cpp_formatting/index.pb.h"
#include "google/protobuf/json/json.h"

namespace code_browser {
namespace {

namespace fs = std::filesystem;
namespace api = code_browser::api;

// protos/shop.proto, with `name` at [71, 75) on line 5; user.cpp, with
// `set_name` at [42, 50) and `name` at [58, 62), both on line 2.
constexpr const char kShopProto[] =
    "syntax = \"proto3\";\n"
    "import \"lib/dep.proto\";\n"
    "package shop;\n"
    "message Item {\n"
    "  string name = 1;\n"
    "}\n";
constexpr const char kUserCpp[] =
    "#include \"shop.pb.h\"\n"
    "void f(shop::Item& i) { i.set_name(i.name()); }\n";
constexpr const char kHeader[] = "bazel-out/bin/protos/shop.pb.h";

constexpr const char kField[] = "proto:shop.Item.name";
constexpr const char kGetter[] = "c:@N@shop@S@Item@F@name#1";
constexpr const char kSetter[] = "c:@N@shop@S@Item@F@set_name#";

auto Offset(const char* text, const char* needle, int nth = 0) -> uint32_t {
  std::string_view t(text);
  size_t at = t.find(needle);
  for (int i = 0; i < nth; ++i) at = t.find(needle, at + 1);
  EXPECT_NE(at, std::string_view::npos) << needle;
  return static_cast<uint32_t>(at);
}

class CrossLanguageTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() /
           ("code_browser_cross_language_test_" +
            std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
            "_" + std::to_string(counter_++));
    fs::remove_all(dir_);
    fs::create_directories(dir_ / "protos/lib");
    Write("protos/shop.proto", kShopProto);
    Write("protos/lib/dep.proto", "syntax = \"proto3\";\n");
    Write("user.cpp", kUserCpp);

    cpp_index::IndexUnit merged = mergeUnits({ProtoUnit(), CxxUnit()});
    EXPECT_EQ(linkGenerated(merged), 2u);
    ImportOptions opts;
    opts.source_path = "test.pb";
    std::string error;
    db_ = IndexDb::FromIndex(buildIndex(merged), opts, &error);
    ASSERT_TRUE(db_) << error;
    RepoOptions ropts;
    ropts.root = dir_;
    std::optional<Repo> repo = Repo::Open(ropts, &error);
    ASSERT_TRUE(repo) << error;
    repo_ = std::make_unique<Repo>(*repo);
    files_ = std::make_unique<FileCache>(1 << 20);
    handler_ = std::make_unique<ApiHandler>(*db_, *repo_, *files_);
  }
  void TearDown() override { fs::remove_all(dir_); }

  // What `--emit-proto-index` writes: the field, defined in the .proto and
  // anchored on the header's getter and setter -- and on a second header that
  // no C++ in the index includes.
  static auto ProtoUnit() -> cpp_index::IndexUnit {
    cpp_index::IndexUnit u;
    const int32_t proto = AddFile(u, "protos/shop.proto");
    AddFile(u, "protos/lib/dep.proto");
    const int32_t header = AddFile(u, kHeader);
    const int32_t unused = AddFile(u, "bazel-out/bin/protos/unused.pb.h");
    const uint32_t name = Offset(kShopProto, "name");
    const int32_t field =
        AddSymbol(u, kField, "name", "shop.Item.name", cpp_index::FIELD,
                  cpp_index::PROTO, proto, name, name + 4);
    u.mutable_symbols(field)->set_type("string = 1");
    AddOcc(u, proto, name, name + 4, field, cpp_index::DEFINITION);
    AddOcc(u, header, 100, 104, field, cpp_index::GENERATES);
    AddOcc(u, header, 200, 208, field, cpp_index::GENERATES | cpp_index::WRITE);
    AddOcc(u, unused, 10, 14, field, cpp_index::GENERATES);
    return u;
  }

  // What `--emit-index` writes for user.cpp: the accessors, declared in a
  // header it does not own, and its two uses of them.
  static auto CxxUnit() -> cpp_index::IndexUnit {
    cpp_index::IndexUnit u;
    const int32_t user = AddFile(u, "user.cpp");
    const int32_t header = AddFile(u, kHeader);
    const int32_t getter =
        AddSymbol(u, kGetter, "name", "shop::Item::name",
                  cpp_index::INSTANCE_METHOD, cpp_index::CXX, header, 100, 104);
    const int32_t setter =
        AddSymbol(u, kSetter, "set_name", "shop::Item::set_name",
                  cpp_index::INSTANCE_METHOD, cpp_index::CXX, header, 200, 208);
    const uint32_t set = Offset(kUserCpp, "set_name");
    const uint32_t get = Offset(kUserCpp, "name()");
    AddOcc(u, user, set, set + 8, setter,
           cpp_index::REFERENCE | cpp_index::CALL);
    AddOcc(u, user, get, get + 4, getter,
           cpp_index::REFERENCE | cpp_index::CALL);
    return u;
  }

  static auto AddFile(cpp_index::IndexUnit& u, const std::string& path)
      -> int32_t {
    cpp_index::File* f = u.add_files();
    f->set_path(path);
    f->set_kind(fileKindForPath(path));
    return u.files_size() - 1;
  }
  static auto AddSymbol(cpp_index::IndexUnit& u, const char* usr,
                        const char* name, const char* qualified,
                        cpp_index::SymbolKind kind,
                        cpp_index::Language language, int32_t file,
                        uint32_t begin, uint32_t end) -> int32_t {
    cpp_index::Symbol* s = u.add_symbols();
    s->set_usr(usr);
    s->set_name(name);
    s->set_qualified_name(qualified);
    s->set_kind(kind);
    s->set_language(language);
    s->mutable_canonical()->set_file(file);
    s->mutable_canonical()->set_begin(begin);
    s->mutable_canonical()->set_end(end);
    return u.symbols_size() - 1;
  }
  static void AddOcc(cpp_index::IndexUnit& u, int32_t file, uint32_t begin,
                     uint32_t end, int32_t symbol, uint32_t roles) {
    cpp_index::Occurrence* o = u.add_occurrences();
    o->set_file(file);
    o->set_begin(begin);
    o->set_end(end);
    o->set_symbol(symbol);
    o->set_roles(roles);
  }

  void Write(const std::string& rel, const std::string& text) {
    std::ofstream(dir_ / rel, std::ios::binary) << text;
  }
  auto Get(const std::string& path, const std::string& query = "")
      -> ApiResponse {
    return handler_->Handle({"GET", path, query, ""});
  }
  template <class M>
  auto Parse(const ApiResponse& r) -> M {
    M m;
    EXPECT_EQ(r.status, 200) << r.body;
    EXPECT_TRUE(google::protobuf::json::JsonStringToMessage(r.body, &m).ok())
        << r.body;
    return m;
  }
  auto Id(const char* usr) -> std::string {
    return std::to_string(db_->SymbolByUsr(usr)->id);
  }

  fs::path dir_;
  std::unique_ptr<IndexDb> db_;
  std::unique_ptr<Repo> repo_;
  std::unique_ptr<FileCache> files_;
  std::unique_ptr<ApiHandler> handler_;
  static inline int counter_ = 0;
};

// From the code to the .proto: every summary of a generated symbol carries
// what it was generated from, located in the source a person reads.
TEST_F(CrossLanguageTest, AGeneratedSymbolCarriesItsOrigin) {
  const auto info = Parse<api::SymbolInfo>(Get("/api/symbol/" + Id(kSetter)));
  EXPECT_EQ(info.symbol().language(), cpp_index::CXX);
  ASSERT_TRUE(info.symbol().has_origin());
  const api::SymbolSummary& origin = info.symbol().origin();
  EXPECT_EQ(origin.usr(), kField);
  EXPECT_EQ(origin.language(), cpp_index::PROTO);
  EXPECT_EQ(origin.kind(), cpp_index::FIELD);
  EXPECT_EQ(origin.qualified_name(), "shop.Item.name");
  EXPECT_EQ(origin.definition().path(), "protos/shop.proto");
  EXPECT_EQ(origin.definition().line(), 5u);
  EXPECT_FALSE(origin.has_origin());
  // protoc marked the setter's anchor SET: using it writes the field.
  EXPECT_TRUE(info.symbol().modifies_origin());
  EXPECT_FALSE(Parse<api::SymbolInfo>(Get("/api/symbol/" + Id(kGetter)))
                   .symbol()
                   .modifies_origin());
  // Its own declaration is still the generated header's.
  EXPECT_EQ(info.symbol().definition().path(), kHeader);
  ASSERT_EQ(info.related_size(), 1);
  EXPECT_EQ(info.related(0).kind(), cpp_index::GENERATED_FROM);
  EXPECT_FALSE(info.related(0).reverse());

  // The same summary is what a file's annotations hand the page, so a click
  // on `set_name` needs no second request to know where it came from.
  const auto ann =
      Parse<api::Annotations>(Get("/api/annotations", "path=user.cpp"));
  ASSERT_EQ(ann.symbols_size(), 2);
  for (const api::SymbolSummary& s : ann.symbols())
    EXPECT_EQ(s.origin().usr(), kField) << s.usr();
}

// From the .proto to the code.
TEST_F(CrossLanguageTest, AProtoFieldListsWhatWasGeneratedFromIt) {
  const auto info = Parse<api::SymbolInfo>(Get("/api/symbol/" + Id(kField)));
  EXPECT_EQ(info.symbol().language(), cpp_index::PROTO);
  EXPECT_FALSE(info.symbol().has_origin());
  ASSERT_EQ(info.related_size(), 2);
  for (const api::Related& r : info.related()) {
    EXPECT_EQ(r.kind(), cpp_index::GENERATED_FROM);
    EXPECT_TRUE(r.reverse());
    EXPECT_EQ(r.symbol().language(), cpp_index::CXX);
  }
  // An anchor is not an occurrence of the field: one definition, in one
  // file; the two uses in C++ are the generated symbols'.
  EXPECT_EQ(info.counts().total(), 1u);
  EXPECT_EQ(info.counts().definitions(), 1u);
  EXPECT_EQ(info.counts().files(), 1u);
  EXPECT_EQ(info.counts().generated(), 2u);
  // A C++ symbol has nothing generated from it.
  EXPECT_EQ(Parse<api::SymbolInfo>(Get("/api/symbol/" + Id(kSetter)))
                .counts()
                .generated(),
            0u);
}

TEST_F(CrossLanguageTest, ReferencesExpandOverGeneratedSymbols) {
  // On its own, the field occurs where it is defined -- not on its anchors.
  const auto plain = Parse<api::References>(Get("/api/refs/" + Id(kField)));
  EXPECT_EQ(plain.total(), 1u);
  ASSERT_EQ(plain.files_size(), 1);
  EXPECT_EQ(plain.files(0).path(), "protos/shop.proto");
  EXPECT_EQ(plain.symbols_size(), 0);

  // Expanded: the uses of the accessors too, each saying which accessor.
  const auto all = Parse<api::References>(
      Get("/api/refs/" + Id(kField), "expand=generated"));
  EXPECT_EQ(all.total(), 3u);
  EXPECT_FALSE(all.truncated());
  ASSERT_EQ(all.symbols_size(), 2);
  for (const api::SymbolSummary& generated : all.symbols())
    EXPECT_EQ(generated.modifies_origin(), generated.usr() == kSetter);
  ASSERT_EQ(all.files_size(), 2);
  EXPECT_EQ(all.files(0).path(), "protos/shop.proto");
  EXPECT_EQ(all.files(0).refs(0).symbol(), 0);  // the field itself: unset
  EXPECT_EQ(all.files(1).path(), "user.cpp");
  ASSERT_EQ(all.files(1).refs_size(), 2);
  EXPECT_EQ(std::to_string(all.files(1).refs(0).symbol()), Id(kSetter));
  EXPECT_EQ(std::to_string(all.files(1).refs(1).symbol()), Id(kGetter));
  EXPECT_EQ(all.files(1).refs(0).line_text(),
            "void f(shop::Item& i) { i.set_name(i.name()); }");

  // The filters and the paging run over the union.
  const auto calls = Parse<api::References>(
      Get("/api/refs/" + Id(kField), "expand=generated&role=CALL"));
  EXPECT_EQ(calls.total(), 2u);
  const auto page = Parse<api::References>(
      Get("/api/refs/" + Id(kField), "expand=generated&limit=1&offset=1"));
  EXPECT_EQ(page.total(), 3u);
  EXPECT_TRUE(page.truncated());
  ASSERT_EQ(page.files_size(), 1);
  EXPECT_EQ(page.files(0).path(), "user.cpp");

  // The anchors are there for whoever asks for them by role.
  const auto anchors =
      Parse<api::References>(Get("/api/refs/" + Id(kField), "role=GENERATES"));
  EXPECT_EQ(anchors.total(), 2u);
  EXPECT_EQ(anchors.files(0).path(), kHeader);
  EXPECT_EQ(anchors.files(0).refs(1).role_names(), "WRITE|GENERATES");

  // Expanding a symbol nothing was generated from is the plain listing.
  const auto setter = Parse<api::References>(
      Get("/api/refs/" + Id(kSetter), "expand=generated"));
  EXPECT_EQ(setter.total(), 1u);
  EXPECT_EQ(setter.symbols_size(), 0);
  EXPECT_EQ(Get("/api/refs/" + Id(kField), "expand=everything").status, 400);
}

// A generated header that no indexed C++ includes is named by anchors alone:
// nothing is declared in it and nothing in it is a use.  It is not a file of
// the tree.
TEST_F(CrossLanguageTest, AHeaderOnlyAnchorsNameIsNotInTheTree) {
  const auto list =
      Parse<api::FileList>(Get("/api/files", "prefix=bazel-out/bin/protos"));
  ASSERT_EQ(list.entries_size(), 1);
  EXPECT_EQ(list.entries(0).path(), kHeader);
  EXPECT_EQ(
      Get("/api/annotations", "path=bazel-out/bin/protos/unused.pb.h").status,
      404);
  // shop.proto, dep.proto, user.cpp and the one header.
  EXPECT_EQ(Parse<api::RepoInfo>(Get("/api/repo")).stats().files(), 4u);
  // The header that is there shows its anchors: a way back from generated
  // text to the .proto.
  const auto ann = Parse<api::Annotations>(
      Get("/api/annotations", std::string("path=") + kHeader));
  ASSERT_EQ(ann.spans_size(), 2);
  EXPECT_EQ(ann.spans(0).roles(), uint32_t(cpp_index::GENERATES));
  ASSERT_EQ(ann.symbols_size(), 1);
  EXPECT_EQ(ann.symbols(0).usr(), kField);
}

// A .proto's `import` is a path from an import root, never from the file's
// own directory.
TEST_F(CrossLanguageTest, ProtoImportsAreLinks) {
  const auto inc =
      Parse<api::Includes>(Get("/api/includes", "path=protos/shop.proto"));
  ASSERT_EQ(inc.includes_size(), 1);
  EXPECT_EQ(inc.includes(0).spelling(), "lib/dep.proto");
  EXPECT_EQ(inc.includes(0).begin(), Offset(kShopProto, "lib/dep.proto"));
  EXPECT_EQ(inc.includes(0).end(), inc.includes(0).begin() + 13);
  ASSERT_EQ(inc.includes(0).targets_size(), 1);
  EXPECT_EQ(inc.includes(0).targets(0).path(), "protos/lib/dep.proto");
  EXPECT_TRUE(inc.includes(0).targets(0).available());
  // `#include` means nothing in a .proto, and `import` nothing in C++.
  EXPECT_EQ(Parse<api::Includes>(Get("/api/includes", "path=user.cpp"))
                .includes(0)
                .spelling(),
            "shop.pb.h");
}

TEST_F(CrossLanguageTest, SearchKnowsBothSpellingsOfAQualifiedName) {
  const auto dotted =
      Parse<api::SearchResults>(Get("/api/search", "q=shop.Item.name"));
  ASSERT_GE(dotted.hits_size(), 1);
  EXPECT_EQ(dotted.hits(0).symbol().usr(), kField);
  const auto colons =
      Parse<api::SearchResults>(Get("/api/search", "q=Item::set_name"));
  ASSERT_EQ(colons.hits_size(), 1);
  EXPECT_EQ(colons.hits(0).symbol().usr(), kSetter);
  EXPECT_EQ(colons.hits(0).symbol().origin().usr(), kField);
}

}  // namespace
}  // namespace code_browser
