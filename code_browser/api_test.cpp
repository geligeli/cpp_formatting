#include "code_browser/api.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "code_browser/api.pb.h"
#include "code_browser/file_cache.h"
#include "code_browser/index_db.h"
#include "code_browser/index_schema.h"
#include "code_browser/repo.h"
#include "code_browser/text_index.h"
#include "cpp_formatting/cpp_index_merge.h"
#include "cpp_formatting/index.pb.h"
#include "google/protobuf/json/json.h"

namespace code_browser {
namespace {

namespace fs = std::filesystem;
namespace api = code_browser::api;

// A checkout of three files and an index whose offsets match them exactly.
//
//   widget.h    struct Widget {\n  int count;\n  void Set(int v);\n};\n
//   widget.cpp  #include "widget.h"\nvoid Widget::Set(int v) { count = v; }\n
//   sub/util.h  struct Gadget : Widget {};\n
constexpr const char kWidgetH[] =
    "struct Widget {\n  int count;\n  void Set(int v);\n};\n";
constexpr const char kWidgetCpp[] =
    "#include \"widget.h\"\nvoid Widget::Set(int v) { count = v; }\n";
constexpr const char kUtilH[] = "struct Gadget : Widget {};\n";

class ApiTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() /
           ("code_browser_api_test_" +
            std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
            "_" + std::to_string(counter_++));
    fs::remove_all(dir_);
    fs::create_directories(dir_ / "sub");
    Write("widget.h", kWidgetH);
    Write("widget.cpp", kWidgetCpp);
    Write("sub/util.h", kUtilH);

    cpp_index::IndexUnit u;
    u.set_producer("test");
    const auto file = [&](const char* path, cpp_index::FileKind kind) {
      cpp_index::File* f = u.add_files();
      f->set_path(path);
      f->set_kind(kind);
      return u.files_size() - 1;
    };
    const int32_t widget_h = file("widget.h", cpp_index::SOURCE);
    const int32_t widget_cpp = file("widget.cpp", cpp_index::SOURCE);
    const int32_t util_h = file("sub/util.h", cpp_index::SOURCE);
    const int32_t gen_h = file("bazel-out/gen/x.pb.h", cpp_index::GENERATED);
    (void)gen_h;
    // Two more files named x.pb.h, for the ranking of an ambiguous include.
    file("bazel-out/other/deep/x.pb.h", cpp_index::GENERATED);
    file("sub/a/b/x.pb.h", cpp_index::SOURCE);
    const auto symbol = [&](const char* usr, const char* name,
                            const char* qualified, cpp_index::SymbolKind kind,
                            int32_t cfile, uint32_t cb, uint32_t ce,
                            const char* type = "") {
      cpp_index::Symbol* s = u.add_symbols();
      s->set_usr(usr);
      s->set_name(name);
      s->set_qualified_name(qualified);
      s->set_kind(kind);
      s->set_type(type);
      s->mutable_canonical()->set_file(cfile);
      s->mutable_canonical()->set_begin(cb);
      s->mutable_canonical()->set_end(ce);
      return u.symbols_size() - 1;
    };
    const int32_t widget = symbol("c:@S@Widget", "Widget", "Widget",
                                  cpp_index::STRUCT, widget_h, 7, 13);
    const int32_t count =
        symbol("c:@S@Widget@FI@count", "count", "Widget::count",
               cpp_index::FIELD, widget_h, 22, 27, "int");
    const int32_t set =
        symbol("c:@S@Widget@F@Set#I#", "Set", "Widget::Set",
               cpp_index::INSTANCE_METHOD, widget_h, 36, 39, "void (int)");
    const int32_t gadget = symbol("c:@S@Gadget", "Gadget", "Gadget",
                                  cpp_index::STRUCT, util_h, 7, 13);
    const auto relate = [&](int32_t s, cpp_index::RelationKind k, int32_t t) {
      cpp_index::Relation* r = u.mutable_symbols(s)->add_relations();
      r->set_kind(k);
      r->set_symbol(t);
    };
    relate(count, cpp_index::CHILD_OF, widget);
    relate(set, cpp_index::CHILD_OF, widget);
    relate(widget, cpp_index::BASE_OF, gadget);
    const auto occ = [&](int32_t f, uint32_t b, uint32_t e, int32_t s,
                         uint32_t roles) {
      cpp_index::Occurrence* o = u.add_occurrences();
      o->set_file(f);
      o->set_begin(b);
      o->set_end(e);
      o->set_symbol(s);
      o->set_roles(roles);
    };
    constexpr uint32_t kDef = cpp_index::DEFINITION | cpp_index::DECLARATION;
    occ(widget_h, 7, 13, widget, kDef);
    occ(widget_h, 22, 27, count, kDef);
    occ(widget_h, 36, 39, set, cpp_index::DECLARATION);
    occ(widget_cpp, 25, 31, widget, cpp_index::REFERENCE);
    occ(widget_cpp, 33, 36, set, kDef);
    occ(widget_cpp, 46, 51, count, cpp_index::REFERENCE | cpp_index::WRITE);
    occ(util_h, 7, 13, gadget, kDef);
    occ(util_h, 16, 22, widget, cpp_index::REFERENCE);
    normalizeUnit(u);
    ImportOptions opts;
    opts.source_path = "test.pb";
    std::string error;
    db_ = IndexDb::FromIndex(buildIndex(u), opts, &error);
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

  void Write(const std::string& rel, const std::string& text) {
    std::ofstream(dir_ / rel, std::ios::binary) << text;
  }
  auto Get(const std::string& path, const std::string& query = "",
           const std::string& if_none_match = "") -> ApiResponse {
    return handler_->Handle({"GET", path, query, if_none_match});
  }
  template <class M>
  auto Parse(const ApiResponse& r) -> M {
    M m;
    EXPECT_EQ(r.status, 200) << r.body;
    EXPECT_TRUE(google::protobuf::json::JsonStringToMessage(r.body, &m).ok())
        << r.body;
    return m;
  }
  auto Header(const ApiResponse& r, const std::string& name) -> std::string {
    for (const auto& [k, v] : r.headers)
      if (k == name) return v;
    return "";
  }
  auto SymbolId(const char* usr) -> int32_t {
    return db_->SymbolByUsr(usr)->id;
  }

  fs::path dir_;
  std::unique_ptr<IndexDb> db_;
  std::unique_ptr<Repo> repo_;
  std::unique_ptr<FileCache> files_;
  std::unique_ptr<ApiHandler> handler_;
  static inline int counter_ = 0;
};

TEST_F(ApiTest, Helpers) {
  EXPECT_EQ(PercentDecode("a%20b+c"), "a b c");
  EXPECT_EQ(PercentDecode("%2e%2E"), "..");
  EXPECT_FALSE(PercentDecode("%2"));
  EXPECT_FALSE(PercentDecode("%zz"));
  EXPECT_FALSE(PercentDecode("%00"));
  const auto q = ParseQuery("a=1&b=x%20y&c&=d&bad=%zz&e=");
  ASSERT_EQ(q.size(), 5u);
  EXPECT_EQ(q[0], std::make_pair(std::string("a"), std::string("1")));
  EXPECT_EQ(q[1], std::make_pair(std::string("b"), std::string("x y")));
  EXPECT_EQ(q[2], std::make_pair(std::string("c"), std::string("")));
  EXPECT_EQ(q[3], std::make_pair(std::string(""), std::string("d")));
  EXPECT_EQ(q[4], std::make_pair(std::string("e"), std::string("")));
  EXPECT_EQ(ParseRoles("DEFINITION|CALL"),
            uint32_t(cpp_index::DEFINITION | cpp_index::CALL));
  EXPECT_EQ(ParseRoles("36"), 36u);
  EXPECT_EQ(ParseRoles(""), 0u);
  EXPECT_FALSE(ParseRoles("NOPE"));
  EXPECT_FALSE(ParseRoles("ROLE_NONE"));
}

TEST_F(ApiTest, MethodsAndUnknownRoutes) {
  ApiResponse r = handler_->Handle({"POST", "/api/repo", "", ""});
  EXPECT_EQ(r.status, 405);
  EXPECT_EQ(Header(r, "Allow"), "GET, HEAD");
  EXPECT_EQ(Get("/api/nope").status, 404);
  EXPECT_EQ(Get("/index.html").status, 404);
  EXPECT_EQ(handler_->Handle({"HEAD", "/api/repo", "", ""}).status, 200);
  const api::Error e = [&] {
    api::Error m;
    google::protobuf::json::JsonStringToMessage(Get("/api/nope").body, &m);
    return m;
  }();
  EXPECT_EQ(e.status(), 404);
  EXPECT_FALSE(e.message().empty());
}

TEST_F(ApiTest, RepoInfo) {
  const api::RepoInfo info = Parse<api::RepoInfo>(Get("/api/repo"));
  EXPECT_EQ(info.root(), fs::canonical(dir_).string());
  EXPECT_EQ(info.head_commit(), "");
  EXPECT_EQ(info.index_etag(), db_->etag());
  EXPECT_EQ(info.stats().files(), 6u);
  EXPECT_EQ(info.stats().symbols(), 4u);
  EXPECT_EQ(info.stats().occurrences(), 8u);
  EXPECT_EQ(info.stats().source_path(), "test.pb");
}

TEST_F(ApiTest, FilesListsOneLevel) {
  api::FileList root = Parse<api::FileList>(Get("/api/files"));
  ASSERT_EQ(root.entries_size(), 4);  // bazel-out/, sub/, widget.cpp, widget.h
  EXPECT_EQ(root.entries(0).name(), "bazel-out");
  EXPECT_TRUE(root.entries(0).is_dir());
  EXPECT_EQ(root.entries(2).name(), "widget.cpp");
  EXPECT_TRUE(root.entries(2).available());
  EXPECT_EQ(root.entries(2).kind(), cpp_index::SOURCE);
  const api::FileList gen =
      Parse<api::FileList>(Get("/api/files", "prefix=bazel-out/gen"));
  ASSERT_EQ(gen.entries_size(), 1);
  EXPECT_FALSE(gen.entries(0).available());  // no exec root here
  EXPECT_EQ(gen.entries(0).kind(), cpp_index::GENERATED);
  EXPECT_EQ(Get("/api/files", "prefix=nope").status, 404);
  EXPECT_EQ(Get("/api/files", "prefix=..").status, 400);
}

TEST_F(ApiTest, FileBytesWithHeadersAndEtag) {
  ApiResponse r = Get("/api/file", "path=widget.cpp");
  ASSERT_EQ(r.status, 200) << r.body;
  EXPECT_EQ(r.body, kWidgetCpp);
  EXPECT_EQ(r.content_type, "text/plain; charset=utf-8");
  EXPECT_EQ(Header(r, "X-File-Id"),
            std::to_string(*db_->FileIdOf("widget.cpp")));
  EXPECT_EQ(Header(r, "X-File-Kind"), "SOURCE");
  EXPECT_FALSE(Header(r, "Last-Modified").empty());
  ASSERT_FALSE(r.etag.empty());
  EXPECT_EQ(Get("/api/file", "path=widget.cpp", r.etag).status, 304);
  EXPECT_EQ(Get("/api/file", "path=widget.cpp", "W/" + r.etag).status, 304);
  EXPECT_EQ(Get("/api/file", "path=widget.cpp", "\"other\"").status, 200);
  // A file that is not indexed but is in the checkout is still served.
  Write("notes.txt", "hi\n");
  EXPECT_EQ(Get("/api/file", "path=notes.txt").body, "hi\n");
  EXPECT_EQ(Get("/api/file", "path=nope.h").status, 404);
  EXPECT_EQ(Get("/api/file", "path=../etc/passwd").status, 400);
  EXPECT_EQ(Get("/api/file", "path=%2e%2e/etc/passwd").status, 400);
  EXPECT_EQ(Get("/api/file", "").status, 400);
  EXPECT_EQ(Get("/api/file", "path=bazel-out/gen/x.pb.h").status, 404);
}

TEST_F(ApiTest, AnnotationsCarrySpansAndSymbols) {
  ApiResponse r = Get("/api/annotations", "path=widget.cpp");
  const api::Annotations a = Parse<api::Annotations>(r);
  EXPECT_EQ(a.path(), "widget.cpp");
  ASSERT_EQ(a.spans_size(), 3);
  EXPECT_EQ(a.spans(0).begin(), 25u);
  EXPECT_EQ(a.spans(2).begin(), 46u);
  EXPECT_EQ(a.spans(2).roles(),
            uint32_t(cpp_index::REFERENCE | cpp_index::WRITE));
  ASSERT_EQ(a.symbols_size(), 3);
  bool saw_count = false;
  for (const api::SymbolSummary& s : a.symbols()) {
    if (s.name() != "count") continue;
    saw_count = true;
    EXPECT_EQ(s.kind(), cpp_index::FIELD);
    EXPECT_EQ(s.type(), "int");
    EXPECT_EQ(s.definition().path(), "widget.h");
    EXPECT_EQ(s.definition().line(), 2u);
    EXPECT_EQ(s.definition().column(), 7u);
  }
  EXPECT_TRUE(saw_count);
  EXPECT_EQ(r.etag, "\"" + db_->etag() + "\"");
  EXPECT_EQ(Get("/api/annotations", "path=widget.cpp", r.etag).status, 304);
  EXPECT_EQ(Get("/api/annotations", "path=notes.txt").status, 404);
}

TEST_F(ApiTest, IncludesResolveAgainstTheIndexedFiles) {
  ApiResponse r = Get("/api/includes", "path=widget.cpp");
  api::Includes inc = Parse<api::Includes>(r);
  ASSERT_EQ(inc.includes_size(), 1);
  EXPECT_EQ(inc.includes(0).begin(), 10u);
  EXPECT_EQ(inc.includes(0).end(), 18u);
  EXPECT_EQ(inc.includes(0).spelling(), "widget.h");
  EXPECT_FALSE(inc.includes(0).angled());
  ASSERT_EQ(inc.includes(0).targets_size(), 1);
  EXPECT_EQ(inc.includes(0).targets(0).path(), "widget.h");
  EXPECT_EQ(inc.includes(0).targets(0).kind(), cpp_index::SOURCE);
  EXPECT_TRUE(inc.includes(0).targets(0).available());
  ASSERT_FALSE(r.etag.empty());
  EXPECT_EQ(Get("/api/includes", "path=widget.cpp", r.etag).status, 304);

  // A file the index does not know still has its includes resolved: next to
  // the includer, by path suffix, up a directory, and not at all.
  Write("sub/user.cpp",
        "// c\n"
        "#include \"util.h\"\n"
        "  #  include <gen/x.pb.h>\n"
        "#include \"../widget.h\"\n"
        "#include \"missing.h\"\n"
        "#include WIDGET_HEADER\n"
        "#define X \"widget.h\"\n");
  inc = Parse<api::Includes>(Get("/api/includes", "path=sub/user.cpp"));
  ASSERT_EQ(inc.includes_size(), 4);
  ASSERT_EQ(inc.includes(0).targets_size(), 1);
  EXPECT_EQ(inc.includes(0).targets(0).path(), "sub/util.h");
  EXPECT_TRUE(inc.includes(1).angled());
  EXPECT_EQ(inc.includes(1).spelling(), "gen/x.pb.h");
  ASSERT_EQ(inc.includes(1).targets_size(), 1);
  EXPECT_EQ(inc.includes(1).targets(0).path(), "bazel-out/gen/x.pb.h");
  EXPECT_EQ(inc.includes(1).targets(0).kind(), cpp_index::GENERATED);
  EXPECT_FALSE(inc.includes(1).targets(0).available());
  ASSERT_EQ(inc.includes(2).targets_size(), 1);
  EXPECT_EQ(inc.includes(2).targets(0).path(), "widget.h");
  EXPECT_EQ(inc.includes(3).targets_size(), 0);

  // Two candidates: the includer's sibling first, then the other match.
  Write("sub/widget.cpp", "#include \"widget.h\"\n#include <widget.h>\n");
  inc = Parse<api::Includes>(Get("/api/includes", "path=sub/widget.cpp"));
  ASSERT_EQ(inc.includes_size(), 2);
  ASSERT_EQ(inc.includes(0).targets_size(), 1);  // sub/widget.h is not indexed
  EXPECT_EQ(inc.includes(0).targets(0).path(), "widget.h");

  // Several files end in the spelling: first-party first, then the fewest
  // directories in front of it.  `gen/x.pb.h` above named only one of them.
  Write("main.cpp", "#include <x.pb.h>\n");
  inc = Parse<api::Includes>(Get("/api/includes", "path=main.cpp"));
  ASSERT_EQ(inc.includes_size(), 1);
  ASSERT_EQ(inc.includes(0).targets_size(), 3);
  EXPECT_EQ(inc.includes(0).targets(0).path(), "sub/a/b/x.pb.h");
  EXPECT_EQ(inc.includes(0).targets(1).path(), "bazel-out/gen/x.pb.h");
  EXPECT_EQ(inc.includes(0).targets(2).path(), "bazel-out/other/deep/x.pb.h");

  EXPECT_EQ(Get("/api/includes", "path=nope.cpp").status, 404);
  EXPECT_EQ(Get("/api/includes", "").status, 400);
}

TEST_F(ApiTest, SymbolInfoByIdAndUsr) {
  const int32_t count = SymbolId("c:@S@Widget@FI@count");
  api::SymbolInfo info =
      Parse<api::SymbolInfo>(Get("/api/symbol/" + std::to_string(count)));
  EXPECT_EQ(info.symbol().qualified_name(), "Widget::count");
  ASSERT_EQ(info.definitions_size(), 1);
  EXPECT_EQ(info.definitions(0).path(), "widget.h");
  EXPECT_EQ(info.definitions(0).line(), 2u);
  EXPECT_EQ(info.declarations_size(), 0);
  EXPECT_EQ(info.counts().total(), 2u);
  EXPECT_EQ(info.counts().references(), 1u);
  EXPECT_EQ(info.counts().files(), 2u);
  ASSERT_EQ(info.related_size(), 1);
  EXPECT_EQ(info.related(0).kind(), cpp_index::CHILD_OF);
  EXPECT_FALSE(info.related(0).reverse());
  EXPECT_EQ(info.related(0).symbol().name(), "Widget");

  info = Parse<api::SymbolInfo>(Get("/api/symbol", "usr=c:@S@Widget"));
  EXPECT_EQ(info.symbol().name(), "Widget");
  // Widget: two members point at it (reverse), and it names Gadget as
  // derived (forward BASE_OF).
  int reverse = 0, forward = 0;
  for (const api::Related& r : info.related())
    (r.reverse() ? reverse : forward)++;
  EXPECT_EQ(reverse, 2);
  EXPECT_EQ(forward, 1);
  // Set: declared in the header, defined in the .cpp.
  info = Parse<api::SymbolInfo>(Get("/api/symbol", "usr=c:@S@Widget@F@Set#I#"));
  ASSERT_EQ(info.definitions_size(), 1);
  EXPECT_EQ(info.definitions(0).path(), "widget.cpp");
  ASSERT_EQ(info.declarations_size(), 1);
  EXPECT_EQ(info.declarations(0).path(), "widget.h");
  EXPECT_EQ(info.symbol().definition().path(), "widget.cpp");

  EXPECT_EQ(Get("/api/symbol/999").status, 404);
  EXPECT_EQ(Get("/api/symbol/x").status, 400);
  EXPECT_EQ(Get("/api/symbol", "usr=c:@nope").status, 404);
  EXPECT_EQ(Get("/api/symbol").status, 400);
}

TEST_F(ApiTest, ReferencesGroupedByFile) {
  const int32_t widget = SymbolId("c:@S@Widget");
  const std::string path = "/api/refs/" + std::to_string(widget);
  api::References refs = Parse<api::References>(Get(path));
  EXPECT_EQ(refs.total(), 3u);
  EXPECT_FALSE(refs.truncated());
  ASSERT_EQ(refs.files_size(), 3);  // sub/util.h, widget.cpp, widget.h
  EXPECT_EQ(refs.files(0).path(), "sub/util.h");
  EXPECT_EQ(refs.files(1).path(), "widget.cpp");
  ASSERT_EQ(refs.files(1).refs_size(), 1);
  const api::Reference& use = refs.files(1).refs(0);
  EXPECT_EQ(use.location().line(), 2u);
  EXPECT_EQ(use.location().column(), 6u);
  EXPECT_EQ(use.line_text(), "void Widget::Set(int v) { count = v; }");
  EXPECT_EQ(use.role_names(), "REFERENCE");
  // Role filter, paging.
  refs = Parse<api::References>(Get(path, "role=DEFINITION"));
  EXPECT_EQ(refs.total(), 1u);
  EXPECT_EQ(refs.files(0).path(), "widget.h");
  refs = Parse<api::References>(Get(path, "exclude=DEFINITION&limit=1"));
  EXPECT_EQ(refs.total(), 2u);
  EXPECT_TRUE(refs.truncated());
  EXPECT_EQ(refs.limit(), 1u);
  refs = Parse<api::References>(Get(path, "limit=1&offset=1"));
  EXPECT_EQ(refs.files_size(), 1);
  EXPECT_EQ(refs.files(0).path(), "widget.cpp");
  refs = Parse<api::References>(Get(path, "file=widget.cpp"));
  EXPECT_EQ(refs.total(), 1u);
  EXPECT_EQ(Get(path, "role=NOPE").status, 400);
  EXPECT_EQ(Get(path, "file=nope.h").status, 404);
  EXPECT_EQ(Get(path, "limit=0").status, 400);
  EXPECT_EQ(Get("/api/refs/999").status, 404);
  EXPECT_EQ(Get("/api/refs/-1").status, 400);
}

TEST_F(ApiTest, SearchAndAt) {
  const api::SearchResults hits =
      Parse<api::SearchResults>(Get("/api/search", "q=wid"));
  ASSERT_EQ(hits.hits_size(), 1);
  EXPECT_EQ(hits.hits(0).symbol().name(), "Widget");
  EXPECT_EQ(hits.hits(0).symbol().definition().line(), 1u);
  EXPECT_EQ(Get("/api/search").status, 400);
  EXPECT_EQ(Get("/api/search", "q=x&kind=NOPE").status, 400);
  const api::SearchResults structs =
      Parse<api::SearchResults>(Get("/api/search", "q=get&kind=STRUCT"));
  EXPECT_EQ(structs.hits_size(), 2);

  const api::OccurrencesAt at =
      Parse<api::OccurrencesAt>(Get("/api/at", "path=widget.cpp&offset=48"));
  ASSERT_EQ(at.spans_size(), 1);
  EXPECT_EQ(at.symbols(0).name(), "count");
  EXPECT_EQ(
      Parse<api::OccurrencesAt>(Get("/api/at", "path=widget.cpp&offset=52"))
          .spans_size(),
      0);
  EXPECT_EQ(Get("/api/at", "path=widget.cpp").status, 400);
}

TEST_F(ApiTest, TextSearch) {
  EXPECT_EQ(Get("/api/text", "q=count").status, 503);
  EXPECT_FALSE(Parse<api::RepoInfo>(Get("/api/repo")).has_text_index());

  Write("notes.md", "the count of widgets\n");
  std::string error;
  TextBuildStats stats;
  const std::unique_ptr<TextIndex> text = OpenOrBuildTextIndex(
      repo_->root(), dir_.string() + ".fts", {}, &stats, &error);
  ASSERT_TRUE(text) << error;
  handler_ = std::make_unique<ApiHandler>(*db_, *repo_, *files_, text.get());

  const api::RepoInfo info = Parse<api::RepoInfo>(Get("/api/repo"));
  EXPECT_EQ(info.text_index().files(), 4u);
  EXPECT_EQ(info.text_index().path(), dir_.string() + ".fts");

  const ApiResponse first = Get("/api/text", "q=count");
  const auto count = Parse<api::TextSearchResults>(first);
  EXPECT_EQ(count.query(), "count");
  EXPECT_TRUE(count.case_sensitive());
  EXPECT_EQ(count.total_matches(), 3u);
  EXPECT_EQ(count.total_lines(), 3u);
  EXPECT_EQ(count.files_matched(), 3u);
  ASSERT_EQ(count.files_size(), 3);
  // By path; a file the symbol index does not know has no id.
  EXPECT_EQ(count.files(0).path(), "notes.md");
  EXPECT_EQ(count.files(0).file_id(), -1);
  EXPECT_EQ(count.files(0).kind(), cpp_index::FILE_KIND_UNSPECIFIED);
  EXPECT_EQ(count.files(1).path(), "widget.cpp");
  EXPECT_GE(count.files(1).file_id(), 0);
  EXPECT_EQ(count.files(1).kind(), cpp_index::SOURCE);
  const api::TextLine& line = count.files(1).lines(0);
  EXPECT_EQ(line.line(), 2u);
  EXPECT_EQ(line.column(), 27u);
  EXPECT_EQ(line.text(), "void Widget::Set(int v) { count = v; }");
  ASSERT_EQ(line.spans_size(), 1);
  EXPECT_EQ(line.spans(0).begin(), 26u);
  EXPECT_EQ(line.spans(0).end(), 31u);
  EXPECT_EQ(count.files(2).path(), "widget.h");
  EXPECT_EQ(count.files(2).lines(0).line(), 2u);

  // The ETag covers the text index and the symbol index.
  ASSERT_FALSE(first.etag.empty());
  EXPECT_EQ(Get("/api/text", "q=count", first.etag).status, 304);

  EXPECT_EQ(Parse<api::TextSearchResults>(Get("/api/text", "q=Widget"))
                .total_matches(),
            3u);
  const auto any = Parse<api::TextSearchResults>(
      Get("/api/text", "q=widget&case=insensitive&limit=2"));
  EXPECT_FALSE(any.case_sensitive());
  EXPECT_EQ(any.total_matches(), 5u);
  EXPECT_EQ(any.total_lines(), 5u);
  EXPECT_EQ(any.files_matched(), 4u);
  EXPECT_EQ(any.next_offset(), 2u);
  const auto last = Parse<api::TextSearchResults>(
      Get("/api/text", "q=widget&case=insensitive&limit=2&offset=4"));
  EXPECT_EQ(last.offset(), 4u);
  EXPECT_EQ(last.next_offset(), 0u);
  ASSERT_EQ(last.files_size(), 1);
  EXPECT_EQ(last.files(0).path(), "widget.h");

  EXPECT_EQ(Get("/api/text").status, 400);
  EXPECT_EQ(Get("/api/text", "q=").status, 400);
  EXPECT_EQ(Get("/api/text", "q=a%0Ab").status, 400);
  EXPECT_EQ(Get("/api/text", "q=a&case=maybe").status, 400);
  EXPECT_EQ(Get("/api/text", "q=a&limit=0").status, 400);
  EXPECT_EQ(Get("/api/text", "q=a&limit=5000").status, 400);
  EXPECT_EQ(Get("/api/text", "q=a&offset=x").status, 400);
  fs::remove(dir_.string() + ".fts");
}

}  // namespace
}  // namespace code_browser
