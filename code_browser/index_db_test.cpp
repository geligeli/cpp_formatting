#include "code_browser/index_db.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "cpp_formatting/cpp_index_merge.h"
#include "cpp_formatting/index.pb.h"

namespace code_browser {
namespace {

using cpp_index::IndexUnit;

// ---------------------------------------------------------------------------
// A small hand-built index
//
//   widget.h  : struct Widget { int count; Widget(); void Set(int); };  (Widget
//               a CLASS, count a FIELD, Set a METHOD, all CHILD_OF Widget)
//   widget.cpp: uses of count (read, write), a call to Set, and a macro-body
//               occurrence of count on the invocation token
//   sub/util.h: struct Gadget : Widget (BASE_OF), a parameter `count`
//   /usr/include/sys.h (SYSTEM): a declaration of ::printf
//   bazel-out/gen/x.pb.h (GENERATED): nothing
// ---------------------------------------------------------------------------

auto AddFile(IndexUnit& u, const std::string& path, cpp_index::FileKind kind)
    -> int32_t {
  cpp_index::File* f = u.add_files();
  f->set_path(path);
  f->set_kind(kind);
  return u.files_size() - 1;
}

auto AddSymbol(IndexUnit& u, const std::string& usr, const std::string& name,
               const std::string& qualified, cpp_index::SymbolKind kind,
               int32_t canonical_file = -1, uint32_t begin = 0,
               uint32_t end = 0) -> int32_t {
  cpp_index::Symbol* s = u.add_symbols();
  s->set_usr(usr);
  s->set_name(name);
  s->set_qualified_name(qualified);
  s->set_kind(kind);
  s->set_language(cpp_index::CXX);
  if (canonical_file >= 0) {
    s->mutable_canonical()->set_file(canonical_file);
    s->mutable_canonical()->set_begin(begin);
    s->mutable_canonical()->set_end(end);
  }
  return u.symbols_size() - 1;
}

auto AddOcc(IndexUnit& u, int32_t file, uint32_t begin, uint32_t end,
            int32_t symbol, uint32_t roles,
            cpp_index::MacroContext macro = cpp_index::NOT_IN_MACRO)
    -> cpp_index::Occurrence* {
  cpp_index::Occurrence* o = u.add_occurrences();
  o->set_file(file);
  o->set_begin(begin);
  o->set_end(end);
  o->set_symbol(symbol);
  o->set_roles(roles);
  o->set_macro(macro);
  return o;
}

void AddRelation(IndexUnit& u, int32_t symbol, cpp_index::RelationKind kind,
                 int32_t target) {
  cpp_index::Relation* r = u.mutable_symbols(symbol)->add_relations();
  r->set_kind(kind);
  r->set_symbol(target);
}

struct Fixture {
  int32_t widget_h, widget_cpp, util_h, sys_h, gen_h;
  int32_t widget, count, set, gadget, param_count, printf_sym;
  std::unique_ptr<IndexDb> db;
};

auto MakeFixture() -> Fixture {
  Fixture f;
  IndexUnit u;
  u.set_producer("test");
  u.set_schema_version(1);
  f.widget_h = AddFile(u, "widget.h", cpp_index::SOURCE);
  f.widget_cpp = AddFile(u, "widget.cpp", cpp_index::SOURCE);
  f.util_h = AddFile(u, "sub/util.h", cpp_index::SOURCE);
  f.sys_h = AddFile(u, "/usr/include/sys.h", cpp_index::SYSTEM);
  f.gen_h = AddFile(u, "bazel-out/gen/x.pb.h", cpp_index::GENERATED);

  f.widget = AddSymbol(u, "c:@S@Widget", "Widget", "Widget", cpp_index::STRUCT,
                       f.widget_h, 7, 13);
  f.count = AddSymbol(u, "c:@S@Widget@FI@count", "count", "Widget::count",
                      cpp_index::FIELD, f.widget_h, 20, 25);
  f.set = AddSymbol(u, "c:@S@Widget@F@Set#I#", "Set", "Widget::Set",
                    cpp_index::INSTANCE_METHOD, f.widget_h, 40, 43);
  f.gadget = AddSymbol(u, "c:@S@Gadget", "Gadget", "Gadget", cpp_index::STRUCT,
                       f.util_h, 7, 13);
  f.param_count = AddSymbol(u, "c:util.h@30@F@f#I#@count", "count", "count",
                            cpp_index::PARAMETER, f.util_h, 30, 35);
  f.printf_sym = AddSymbol(u, "c:@F@printf", "printf", "printf",
                           cpp_index::FUNCTION, f.sys_h, 100, 106);
  AddRelation(u, f.count, cpp_index::CHILD_OF, f.widget);
  AddRelation(u, f.set, cpp_index::CHILD_OF, f.widget);
  AddRelation(u, f.widget, cpp_index::BASE_OF, f.gadget);

  // widget.h
  AddOcc(u, f.widget_h, 7, 13, f.widget,
         cpp_index::DEFINITION | cpp_index::DECLARATION);
  AddOcc(u, f.widget_h, 20, 25, f.count,
         cpp_index::DEFINITION | cpp_index::DECLARATION);
  AddOcc(u, f.widget_h, 40, 43, f.set, cpp_index::DECLARATION);
  // widget.cpp
  AddOcc(u, f.widget_cpp, 10, 13, f.set,
         cpp_index::DEFINITION | cpp_index::DECLARATION);
  AddOcc(u, f.widget_cpp, 30, 35, f.count,
         cpp_index::REFERENCE | cpp_index::READ);
  AddOcc(u, f.widget_cpp, 50, 55, f.count,
         cpp_index::REFERENCE | cpp_index::WRITE);
  AddOcc(u, f.widget_cpp, 60, 63, f.set,
         cpp_index::REFERENCE | cpp_index::CALL);
  // BUMP(w): the macro body names count, on the invocation token; the
  // argument w is not indexed here.
  AddOcc(u, f.widget_cpp, 70, 74, f.count,
         cpp_index::REFERENCE | cpp_index::WRITE, cpp_index::MACRO_BODY);
  // sub/util.h
  AddOcc(u, f.util_h, 7, 13, f.gadget,
         cpp_index::DEFINITION | cpp_index::DECLARATION);
  AddOcc(u, f.util_h, 16, 22, f.widget, cpp_index::REFERENCE);
  AddOcc(u, f.util_h, 30, 35, f.param_count,
         cpp_index::DEFINITION | cpp_index::DECLARATION);
  // the same token names both Widget::count (a dependent instantiation) and
  // the parameter: two occurrences, one range
  AddOcc(u, f.util_h, 50, 55, f.param_count, cpp_index::REFERENCE);
  AddOcc(u, f.util_h, 50, 55, f.count,
         cpp_index::REFERENCE | cpp_index::DEPENDENT);
  // /usr/include/sys.h (system): the declaration of printf
  AddOcc(u, f.sys_h, 100, 106, f.printf_sym, cpp_index::DECLARATION);

  cpp_index::DependentToken* t = u.add_pending();
  t->set_file(f.util_h);
  t->set_begin(80);
  t->set_end(85);
  t->set_name("value");

  // normalizeUnit() sorts files by path and symbols by USR and remaps every
  // index, so the ids above are only valid before it; re-resolve them from
  // the database.
  normalizeUnit(u);
  const cpp_index::Index index = buildIndex(u);
  ImportOptions opts;
  opts.source_path = "test.pb";
  std::string error;
  f.db = IndexDb::FromIndex(index, opts, &error);
  EXPECT_TRUE(f.db != nullptr) << error;
  if (!f.db) return f;
  f.widget_h = f.db->FileIdOf("widget.h").value_or(-1);
  f.widget_cpp = f.db->FileIdOf("widget.cpp").value_or(-1);
  f.util_h = f.db->FileIdOf("sub/util.h").value_or(-1);
  f.sys_h = f.db->FileIdOf("/usr/include/sys.h").value_or(-1);
  f.gen_h = f.db->FileIdOf("bazel-out/gen/x.pb.h").value_or(-1);
  const auto sym = [&](const char* usr) {
    const std::optional<SymbolRow> r = f.db->SymbolByUsr(usr);
    return r ? r->id : -1;
  };
  f.widget = sym("c:@S@Widget");
  f.count = sym("c:@S@Widget@FI@count");
  f.set = sym("c:@S@Widget@F@Set#I#");
  f.gadget = sym("c:@S@Gadget");
  f.param_count = sym("c:util.h@30@F@f#I#@count");
  f.printf_sym = sym("c:@F@printf");
  return f;
}

auto Ids(const std::vector<SymbolRow>& rows) -> std::vector<int32_t> {
  std::vector<int32_t> out;
  for (const SymbolRow& r : rows) out.push_back(r.id);
  return out;
}

TEST(IndexDb, StatsAndMeta) {
  Fixture f = MakeFixture();
  ASSERT_TRUE(f.db);
  EXPECT_EQ(f.db->stats().files, 5u);
  EXPECT_EQ(f.db->stats().symbols, 6u);
  EXPECT_EQ(f.db->stats().occurrences, 14u);
  EXPECT_EQ(f.db->stats().unresolved, 1u);
  EXPECT_EQ(f.db->stats().source_path, "test.pb");
  EXPECT_EQ(f.db->stats().producer, "test");
  EXPECT_EQ(f.db->etag().size(), 16u);
}

TEST(IndexDb, FilesById) {
  Fixture f = MakeFixture();
  ASSERT_TRUE(f.db);
  EXPECT_EQ(f.db->FileIdOf("widget.cpp"), f.widget_cpp);
  EXPECT_EQ(f.db->FileIdOf("/usr/include/sys.h"), f.sys_h);
  EXPECT_FALSE(f.db->FileIdOf("nope.h").has_value());
  const std::optional<FileRow> row = f.db->File(f.gen_h);
  ASSERT_TRUE(row);
  EXPECT_EQ(row->path, "bazel-out/gen/x.pb.h");
  EXPECT_EQ(row->kind, cpp_index::GENERATED);
  EXPECT_FALSE(f.db->File(99).has_value());
}

TEST(IndexDb, ListDirWalksTheTree) {
  Fixture f = MakeFixture();
  ASSERT_TRUE(f.db);
  // Root: directories first ("/", "bazel-out", "sub"), then the two files.
  const auto root = f.db->ListDir("");
  ASSERT_TRUE(root);
  std::vector<std::string> names;
  for (const DirEntry& e : *root)
    names.push_back(e.name + (e.is_dir ? "/" : ""));
  EXPECT_EQ(names, (std::vector<std::string>{"//", "bazel-out/", "sub/",
                                             "widget.cpp", "widget.h"}));
  const auto sub = f.db->ListDir("sub");
  ASSERT_TRUE(sub);
  ASSERT_EQ(sub->size(), 1u);
  EXPECT_EQ((*sub)[0].path, "sub/util.h");
  EXPECT_EQ((*sub)[0].file_id, f.util_h);
  EXPECT_EQ((*sub)[0].kind, cpp_index::SOURCE);
  const auto usr = f.db->ListDir("/usr");
  ASSERT_TRUE(usr);
  ASSERT_EQ(usr->size(), 1u);
  EXPECT_EQ((*usr)[0].path, "/usr/include");
  EXPECT_TRUE((*usr)[0].is_dir);
  const auto gen = f.db->ListDir("bazel-out/gen");
  ASSERT_TRUE(gen);
  EXPECT_EQ((*gen)[0].kind, cpp_index::GENERATED);
  EXPECT_FALSE(f.db->ListDir("nope").has_value());
  EXPECT_FALSE(f.db->ListDir("widget.h").has_value());  // a file, not a dir
}

TEST(IndexDb, FileOccurrencesAreSorted) {
  Fixture f = MakeFixture();
  ASSERT_TRUE(f.db);
  const std::vector<OccRow> occs = f.db->FileOccurrences(f.widget_cpp);
  ASSERT_EQ(occs.size(), 5u);
  for (size_t i = 1; i < occs.size(); ++i)
    EXPECT_LE(occs[i - 1].begin, occs[i].begin);
  EXPECT_EQ(occs[4].macro, cpp_index::MACRO_BODY);
  EXPECT_EQ(occs[4].symbol, f.count);
  EXPECT_TRUE(f.db->FileOccurrences(f.gen_h).empty());
}

TEST(IndexDb, OccurrencesAtAnOffset) {
  Fixture f = MakeFixture();
  ASSERT_TRUE(f.db);
  EXPECT_EQ(f.db->OccurrencesAt(f.widget_cpp, 30).size(), 1u);  // begin
  EXPECT_EQ(f.db->OccurrencesAt(f.widget_cpp, 34).size(), 1u);  // end - 1
  EXPECT_TRUE(f.db->OccurrencesAt(f.widget_cpp, 35).empty());   // end
  EXPECT_TRUE(f.db->OccurrencesAt(f.widget_cpp, 40).empty());   // between
  // One token, two symbols: both come back, sorted by symbol.
  const std::vector<OccRow> both = f.db->OccurrencesAt(f.util_h, 52);
  ASSERT_EQ(both.size(), 2u);
  EXPECT_LT(both[0].symbol, both[1].symbol);
  for (const OccRow& o : both) {
    EXPECT_TRUE(o.symbol == f.count || o.symbol == f.param_count);
    if (o.symbol == f.count) EXPECT_TRUE(o.roles & cpp_index::DEPENDENT);
  }
}

TEST(IndexDb, SymbolsAndDefinitions) {
  Fixture f = MakeFixture();
  ASSERT_TRUE(f.db);
  const std::optional<SymbolRow> count = f.db->Symbol(f.count);
  ASSERT_TRUE(count);
  EXPECT_EQ(count->qualified_name, "Widget::count");
  EXPECT_EQ(count->kind, cpp_index::FIELD);
  EXPECT_TRUE(count->has_canonical);
  EXPECT_EQ(count->canonical_file, f.widget_h);
  // definition_occ is the DEFINITION in widget.h.
  ASSERT_NE(count->definition_occ, 0);
  const std::optional<OccRow> def = f.db->Occurrence(count->definition_occ);
  ASSERT_TRUE(def);
  EXPECT_EQ(def->file, f.widget_h);
  EXPECT_EQ(def->begin, 20u);
  EXPECT_TRUE(def->roles & cpp_index::DEFINITION);
  // Set: declared in the header, defined in the .cpp -- the definition wins.
  const std::optional<SymbolRow> set = f.db->Symbol(f.set);
  ASSERT_TRUE(set);
  const std::optional<OccRow> set_def = f.db->Occurrence(set->definition_occ);
  ASSERT_TRUE(set_def);
  EXPECT_EQ(set_def->file, f.widget_cpp);
  // printf: a declaration only.
  const std::optional<SymbolRow> pf = f.db->Symbol(f.printf_sym);
  ASSERT_TRUE(pf);
  const std::optional<OccRow> pf_def = f.db->Occurrence(pf->definition_occ);
  ASSERT_TRUE(pf_def);
  EXPECT_EQ(pf_def->file, f.sys_h);
  EXPECT_FALSE(pf_def->roles & cpp_index::DEFINITION);
  EXPECT_EQ(f.db->SymbolByUsr("c:@S@Gadget")->id, f.gadget);
  EXPECT_FALSE(f.db->SymbolByUsr("c:@S@Nope").has_value());
  EXPECT_FALSE(f.db->Symbol(99).has_value());
}

TEST(IndexDb, SymbolOccurrencesWithFiltersAndPaging) {
  Fixture f = MakeFixture();
  ASSERT_TRUE(f.db);
  RefQuery q;
  std::vector<OccRow> all = f.db->SymbolOccurrences(f.count, q);
  ASSERT_EQ(all.size(), 5u);  // def, read, write, macro write, dependent
  EXPECT_EQ(f.db->CountSymbolOccurrences(f.count, q), 5u);
  for (size_t i = 1; i < all.size(); ++i)
    EXPECT_LE(std::make_pair(all[i - 1].file, all[i - 1].begin),
              std::make_pair(all[i].file, all[i].begin));
  q.role_mask = cpp_index::WRITE;
  EXPECT_EQ(f.db->SymbolOccurrences(f.count, q).size(), 2u);
  q.exclude_mask = cpp_index::WRITE;  // contradictory: nothing
  q.role_mask = 0;
  EXPECT_EQ(f.db->SymbolOccurrences(f.count, q).size(), 3u);
  q = RefQuery{};
  q.file = f.widget_cpp;
  EXPECT_EQ(f.db->CountSymbolOccurrences(f.count, q), 3u);
  q = RefQuery{};
  q.limit = 2;
  q.offset = 4;
  const std::vector<OccRow> page = f.db->SymbolOccurrences(f.count, q);
  ASSERT_EQ(page.size(), 1u);
  // Files are sorted by path, so widget.h comes last.
  EXPECT_EQ(page[0].file, f.widget_h);
}

TEST(IndexDb, RelationsBothWays) {
  Fixture f = MakeFixture();
  ASSERT_TRUE(f.db);
  const std::vector<RelationRow> fwd = f.db->Related(f.count, false);
  ASSERT_EQ(fwd.size(), 1u);
  EXPECT_EQ(fwd[0].kind, cpp_index::CHILD_OF);
  EXPECT_EQ(fwd[0].symbol, f.widget);
  // Reverse: who names Widget?  Its two members (CHILD_OF) -- BASE_OF is
  // Widget's own relation, so it is forward.
  const std::vector<RelationRow> rev = f.db->Related(f.widget, true);
  ASSERT_EQ(rev.size(), 2u);
  std::vector<int32_t> members{rev[0].symbol, rev[1].symbol};
  std::sort(members.begin(), members.end());
  std::vector<int32_t> expected{f.count, f.set};
  std::sort(expected.begin(), expected.end());
  EXPECT_EQ(members, expected);
  EXPECT_EQ(rev[0].kind, cpp_index::CHILD_OF);
  const std::vector<RelationRow> derived = f.db->Related(f.widget, false);
  ASSERT_EQ(derived.size(), 1u);
  EXPECT_EQ(derived[0].kind, cpp_index::BASE_OF);
  EXPECT_EQ(derived[0].symbol, f.gadget);
}

TEST(IndexDb, UnresolvedTokens) {
  Fixture f = MakeFixture();
  ASSERT_TRUE(f.db);
  const std::vector<UnresolvedRow> u = f.db->Unresolved(f.util_h);
  ASSERT_EQ(u.size(), 1u);
  EXPECT_EQ(u[0].name, "value");
  EXPECT_EQ(u[0].begin, 80u);
  EXPECT_TRUE(f.db->Unresolved(f.widget_h).empty());
}

TEST(IndexDb, SearchPrefixSubstringAndQualified) {
  Fixture f = MakeFixture();
  ASSERT_TRUE(f.db);
  SearchOptions opts;
  // Prefix (short query): Widget only; case-insensitive.
  SearchResult r = f.db->Search("wi", opts);
  ASSERT_EQ(r.hits.size(), 1u);
  EXPECT_EQ(r.hits[0].symbol.id, f.widget);
  // Substring on a 3+ character query: "adg" is inside Gadget.
  r = f.db->Search("ADG", opts);
  ASSERT_EQ(r.hits.size(), 1u);
  EXPECT_EQ(r.hits[0].symbol.id, f.gadget);
  // Exact beats prefix; parameters are hidden unless asked.
  r = f.db->Search("count", opts);
  ASSERT_EQ(r.hits.size(), 1u);
  EXPECT_EQ(r.hits[0].symbol.id, f.count);
  opts.include_locals = true;
  r = f.db->Search("count", opts);
  EXPECT_EQ(r.hits.size(), 2u);
  opts.include_locals = false;
  // Qualified.
  r = f.db->Search("Widget::Set", opts);
  ASSERT_EQ(r.hits.size(), 1u);
  EXPECT_EQ(r.hits[0].symbol.id, f.set);
  EXPECT_TRUE(f.db->Search("Gadget::Set", opts).hits.empty());
  // Kind filter and limit/truncation.
  opts.kind = cpp_index::STRUCT;
  r = f.db->Search("get", opts);  // widGET, gadGET
  ASSERT_EQ(r.hits.size(), 2u);
  opts.limit = 1;
  r = f.db->Search("get", opts);
  EXPECT_EQ(r.hits.size(), 1u);
  EXPECT_TRUE(r.truncated);
  EXPECT_TRUE(f.db->Search("   ", opts).hits.empty());
  EXPECT_TRUE(f.db->Search("zzz", opts).hits.empty());
}

TEST(IndexDb, ConnectionsAreReusedAcrossQueries) {
  // Many sequential queries must not open a connection each; the pool hands
  // the same one back.  (Observable only indirectly: this simply exercises
  // the lease path many times.)
  Fixture f = MakeFixture();
  ASSERT_TRUE(f.db);
  for (int i = 0; i < 200; ++i)
    EXPECT_EQ(f.db->FileIdOf("widget.h"), f.widget_h);
}

TEST(IndexDb, OpenRefusesAForeignFile) {
  std::string error;
  EXPECT_EQ(IndexDb::Open("/nonexistent/x.sqlite", &error), nullptr);
  EXPECT_FALSE(error.empty());
}

}  // namespace
}  // namespace code_browser
