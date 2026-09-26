#include "code_browser/text_index.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace code_browser {
namespace {

namespace fs = std::filesystem;

class TextIndexTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() /
           ("code_browser_text_index_test_" +
            std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
            "_" + std::to_string(counter_++));
    fs::remove_all(dir_);
    fs::create_directories(dir_ / "repo");
    dir_ = fs::canonical(dir_);
    root_ = dir_ / "repo";
    index_path_ = dir_ / "index.fts";
  }
  void TearDown() override { fs::remove_all(dir_); }

  void Write(const std::string& rel, const std::string& text) {
    const fs::path p = root_ / rel;
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
  }
  auto Paths(const TextIndexOptions& opts = {}) -> std::vector<std::string> {
    std::string error;
    const std::optional<std::vector<TextCandidate>> c =
        ListTextCandidates(root_, opts, &error);
    EXPECT_TRUE(c) << error;
    std::vector<std::string> out;
    if (c)
      for (const TextCandidate& t : *c) out.push_back(t.path);
    return out;
  }
  auto Build(TextBuildStats* stats = nullptr, TextIndexOptions opts = {})
      -> std::unique_ptr<TextIndex> {
    std::string error;
    TextBuildStats local;
    std::unique_ptr<TextIndex> index = OpenOrBuildTextIndex(
        root_, index_path_, opts, stats ? stats : &local, &error);
    EXPECT_TRUE(index) << error;
    return index;
  }
  static auto Find(const TextIndex& index, const std::string& q,
                   bool case_sensitive = true) -> TextSearchResult {
    TextSearchOptions opts;
    opts.case_sensitive = case_sensitive;
    return index.Search(q, opts);
  }

  fs::path dir_, root_, index_path_;
  static inline int counter_ = 0;
};

TEST_F(TextIndexTest, WalkSkipsBazelExternalDotDirsSymlinksAndBigFiles) {
  Write("src/a.cc", "int a;\n");
  Write("BUILD", "cc_library(name = \"a\")\n");
  Write("README.md", "# readme\n");
  Write(".git/config", "[core]\n");
  Write(".cache/x.h", "int x;\n");
  Write("sub/.hidden", "hidden\n");
  Write("external/dep/dep.h", "int dep;\n");
  Write("bazel-real/y.h", "int y;\n");  // a real directory, still Bazel's
  Write("deep/external/keep.h", "int keep;\n");
  Write("deep/bazel-out/keep.h", "int keep2;\n");
  Write("big.txt", std::string(2000, 'b'));
  Write("index.fts", "stale");
  Write("index.fts.tmp", "stale");
  fs::create_directories(dir_ / "outside");
  std::ofstream(dir_ / "outside" / "gen.h") << "int gen;\n";
  fs::create_directory_symlink(dir_ / "outside", root_ / "bazel-out");
  fs::create_directory_symlink(dir_ / "outside", root_ / "src" / "linked");
  fs::create_symlink(root_ / "src" / "a.cc", root_ / "alias.cc");

  TextIndexOptions opts;
  opts.max_file_bytes = 1000;
  opts.exclude = {root_ / "index.fts"};
  EXPECT_EQ(Paths(opts), (std::vector<std::string>{
                             "BUILD", "README.md", "deep/bazel-out/keep.h",
                             "deep/external/keep.h", "src/a.cc"}));
}

TEST_F(TextIndexTest, BinaryFilesAreListedButNotIndexed) {
  Write("a.txt", "needle\n");
  Write("b.bin", std::string("need\0le needle", 14));
  TextBuildStats stats;
  const std::unique_ptr<TextIndex> index = Build(&stats);
  ASSERT_TRUE(index);
  EXPECT_TRUE(stats.rebuilt);
  EXPECT_EQ(stats.files, 1u);
  EXPECT_EQ(stats.skipped, 1u);
  EXPECT_EQ(index->files(), 1u);
  EXPECT_EQ(index->skipped(), 1u);
  const TextSearchResult r = Find(*index, "needle");
  ASSERT_EQ(r.files.size(), 1u);
  EXPECT_EQ(r.files[0].path, "a.txt");
}

TEST_F(TextIndexTest, LinesColumnsAndSpans) {
  Write("a.h", "int Foo;\nint foo = Foo + FOO;\n\n  // foo\n");
  Write("b.h", "foo");  // no trailing newline
  Write("c.h", "nothing here\r\nFoo\r\n");
  const std::unique_ptr<TextIndex> index = Build();
  ASSERT_TRUE(index);

  const TextSearchResult exact = Find(*index, "Foo");
  EXPECT_EQ(exact.total_matches, 3u);
  EXPECT_EQ(exact.total_lines, 3u);
  EXPECT_EQ(exact.files_matched, 2u);
  EXPECT_FALSE(exact.truncated);
  EXPECT_EQ(exact.next_offset, 0u);
  ASSERT_EQ(exact.files.size(), 2u);
  EXPECT_EQ(exact.files[0].path, "a.h");
  ASSERT_EQ(exact.files[0].lines.size(), 2u);
  EXPECT_EQ(exact.files[0].lines[0].line, 1u);
  EXPECT_EQ(exact.files[0].lines[0].column, 5u);
  EXPECT_EQ(exact.files[0].lines[0].text, "int Foo;");
  EXPECT_EQ(exact.files[0].lines[1].line, 2u);
  EXPECT_EQ(exact.files[0].lines[1].column, 11u);
  EXPECT_EQ(exact.files[1].path, "c.h");
  EXPECT_EQ(exact.files[1].lines[0].line, 2u);
  EXPECT_EQ(exact.files[1].lines[0].text, "Foo");  // no \r

  const TextSearchResult any = Find(*index, "foo", false);
  EXPECT_EQ(any.total_matches, 7u);
  EXPECT_EQ(any.total_lines, 5u);
  EXPECT_EQ(any.files_matched, 3u);
  const TextLineHit& two = any.files[0].lines[1];
  EXPECT_EQ(two.text, "int foo = Foo + FOO;");
  ASSERT_EQ(two.spans.size(), 3u);
  EXPECT_EQ(two.spans[0].begin, 4u);
  EXPECT_EQ(two.spans[0].end, 7u);
  EXPECT_EQ(two.spans[2].begin, 16u);
  EXPECT_EQ(any.files[0].lines[2].line, 4u);
  EXPECT_EQ(any.files[1].path, "b.h");
  EXPECT_EQ(any.files[1].lines[0].text, "foo");

  // Something with no letters is the same either way.
  EXPECT_EQ(Find(*index, " = ").total_matches, 1u);
  EXPECT_EQ(Find(*index, "zzz").total_matches, 0u);
  EXPECT_TRUE(Find(*index, "zzz").files.empty());
}

TEST_F(TextIndexTest, OverlappingMatchesMergeIntoOneSpan) {
  Write("a.txt", "xaaaax\n");
  const std::unique_ptr<TextIndex> index = Build();
  ASSERT_TRUE(index);
  const TextSearchResult r = Find(*index, "aa");
  EXPECT_EQ(r.total_matches, 3u);
  ASSERT_EQ(r.files.size(), 1u);
  ASSERT_EQ(r.files[0].lines[0].spans.size(), 1u);
  EXPECT_EQ(r.files[0].lines[0].spans[0].begin, 1u);
  EXPECT_EQ(r.files[0].lines[0].spans[0].end, 5u);
}

TEST_F(TextIndexTest, NoMatchAcrossFiles) {
  Write("a.txt", "abc");
  Write("b.txt", "def\n");
  const std::unique_ptr<TextIndex> index = Build();
  ASSERT_TRUE(index);
  EXPECT_EQ(Find(*index, "cd").total_matches, 0u);
  EXPECT_EQ(Find(*index, "abc").total_matches, 1u);
  EXPECT_EQ(Find(*index, "def").total_matches, 1u);
}

TEST_F(TextIndexTest, LongLinesAreClippedAroundTheMatch) {
  std::string line = std::string(1000, 'x') + "needle" + std::string(1000, 'y');
  // Invalid UTF-8 near the match: replaced, offsets kept.
  line[990] = static_cast<char>(0xe9);
  Write("long.txt", line + "\n");
  const std::unique_ptr<TextIndex> index = Build();
  ASSERT_TRUE(index);
  const TextSearchResult r = Find(*index, "needle");
  ASSERT_EQ(r.files.size(), 1u);
  const TextLineHit& hit = r.files[0].lines[0];
  EXPECT_EQ(hit.column, 1001u);
  EXPECT_GT(hit.text_offset, 0u);
  EXPECT_TRUE(hit.clipped_end);
  EXPECT_LE(hit.text.size(), 400u);
  ASSERT_EQ(hit.spans.size(), 1u);
  EXPECT_EQ(hit.text.substr(hit.spans[0].begin,
                            hit.spans[0].end - hit.spans[0].begin),
            "needle");
  EXPECT_EQ(hit.text_offset + hit.spans[0].begin, 1000u);
  EXPECT_EQ(hit.text[990 - hit.text_offset], '?');
}

TEST_F(TextIndexTest, PagingAndTruncation) {
  std::string text;
  for (int i = 0; i < 10; ++i) text += "hit " + std::to_string(i) + "\n";
  Write("a.txt", text);
  Write("z.txt", std::string(kMaxTextMatches + 10, 'q') + "\n");
  const std::unique_ptr<TextIndex> index = Build();
  ASSERT_TRUE(index);
  TextSearchOptions opts;
  opts.limit = 4;
  TextSearchResult page = index->Search("hit", opts);
  EXPECT_EQ(page.total_lines, 10u);
  EXPECT_EQ(page.next_offset, 4u);
  EXPECT_EQ(page.files[0].lines.front().line, 1u);
  opts.offset = 8;
  page = index->Search("hit", opts);
  EXPECT_EQ(page.next_offset, 0u);
  ASSERT_EQ(page.files[0].lines.size(), 2u);
  EXPECT_EQ(page.files[0].lines.back().line, 10u);
  opts.offset = 100;
  EXPECT_TRUE(index->Search("hit", opts).files.empty());

  const TextSearchResult many = Find(*index, "q", false);
  EXPECT_TRUE(many.truncated);
  EXPECT_EQ(many.total_matches, kMaxTextMatches + 10);
  EXPECT_EQ(many.total_lines, 1u);
  EXPECT_TRUE(Find(*index, "q").truncated);
}

TEST_F(TextIndexTest, RebuildsWhenTheCheckoutChanges) {
  Write("a.txt", "one\n");
  Write("b.txt", "two\n");
  TextBuildStats stats;
  std::unique_ptr<TextIndex> index = Build(&stats);
  ASSERT_TRUE(index);
  EXPECT_TRUE(stats.rebuilt);
  const std::string etag = index->etag();

  index = Build(&stats);
  ASSERT_TRUE(index);
  EXPECT_FALSE(stats.rebuilt);
  EXPECT_EQ(index->etag(), etag);
  EXPECT_EQ(Find(*index, "two").total_matches, 1u);

  std::string error;
  const auto candidates = [&] {
    return *ListTextCandidates(root_, {}, &error);
  };
  Write("b.txt", "three\n");
  EXPECT_FALSE(index->IsCurrent(candidates()));
  index = Build(&stats);
  EXPECT_TRUE(stats.rebuilt);
  EXPECT_EQ(Find(*index, "two").total_matches, 0u);
  EXPECT_EQ(Find(*index, "three").total_matches, 1u);

  Write("c.txt", "four\n");
  EXPECT_FALSE(index->IsCurrent(candidates()));
  index = Build(&stats);
  EXPECT_TRUE(stats.rebuilt);
  EXPECT_TRUE(index->IsCurrent(candidates()));
  fs::remove(root_ / "a.txt");
  EXPECT_FALSE(index->IsCurrent(candidates()));
  index = Build(&stats);
  EXPECT_TRUE(stats.rebuilt);
  EXPECT_EQ(Find(*index, "one").total_matches, 0u);
  EXPECT_EQ(Find(*index, "four").total_matches, 1u);
}

TEST_F(TextIndexTest, ReopenedFromDiskAnswersTheSame) {
  Write("a.txt", "alpha beta\ngamma alpha\n");
  const std::unique_ptr<TextIndex> built = Build();
  ASSERT_TRUE(built);
  std::string error;
  const std::unique_ptr<TextIndex> opened =
      TextIndex::Open(index_path_, &error);
  ASSERT_TRUE(opened) << error;
  const TextSearchResult a = Find(*built, "alpha");
  const TextSearchResult b = Find(*opened, "alpha");
  EXPECT_EQ(a.total_matches, b.total_matches);
  ASSERT_EQ(b.files.size(), 1u);
  EXPECT_EQ(b.files[0].lines.size(), 2u);
  EXPECT_EQ(opened->etag(), built->etag());
  EXPECT_FALSE(opened->built_at().empty());
}

TEST_F(TextIndexTest, RefusesCorruptFiles) {
  Write("a.txt", "some text to index\n");
  ASSERT_TRUE(Build());
  std::string bytes;
  {
    std::ifstream in(index_path_, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(in), {});
  }
  const auto open_with = [&](const std::string& data) {
    const fs::path p = dir_ / "bad.fts";
    std::ofstream(p, std::ios::binary | std::ios::trunc) << data;
    std::string error;
    const bool ok = TextIndex::Open(p, &error) != nullptr;
    if (!ok) EXPECT_FALSE(error.empty());
    return ok;
  };
  EXPECT_TRUE(open_with(bytes));
  EXPECT_FALSE(open_with(bytes.substr(0, bytes.size() - 4)));
  EXPECT_FALSE(open_with(bytes.substr(0, 10)));
  EXPECT_FALSE(open_with(""));
  std::string bad_magic = bytes;
  bad_magic[0] = 'X';
  EXPECT_FALSE(open_with(bad_magic));
  // The suffix array is not read up front: garbage in it opens, and searches
  // stay inside the file.
  for (const char garbage : {'\x7f', '\xff'}) {
    std::string bad_sa = bytes;
    for (size_t i = bad_sa.size() - 16; i < bad_sa.size(); ++i)
      bad_sa[i] = garbage;
    ASSERT_TRUE(open_with(bad_sa));
    std::string e;
    const std::unique_ptr<TextIndex> index =
        TextIndex::Open(dir_ / "bad.fts", &e);
    ASSERT_TRUE(index) << e;
    for (const char* q : {"some", "t", "x", "\x7f"})
      EXPECT_LE(Find(*index, q, false).total_matches, 20u);
  }
  std::string error;
  EXPECT_FALSE(TextIndex::Open(dir_ / "missing.fts", &error));

  // A corrupt index on disk is rebuilt, not an error.
  open_with("garbage");
  fs::copy_file(dir_ / "bad.fts", index_path_,
                fs::copy_options::overwrite_existing);
  TextBuildStats stats;
  ASSERT_TRUE(Build(&stats));
  EXPECT_TRUE(stats.rebuilt);
}

TEST_F(TextIndexTest, QueriesAreOneLine) {
  EXPECT_TRUE(InvalidTextQuery(""));
  EXPECT_TRUE(InvalidTextQuery("a\nb"));
  EXPECT_TRUE(InvalidTextQuery("a\rb"));
  EXPECT_TRUE(InvalidTextQuery(std::string("a\0b", 3)));
  EXPECT_TRUE(InvalidTextQuery(std::string(kMaxTextQuery + 1, 'a')));
  EXPECT_FALSE(InvalidTextQuery("a b\tc"));
  EXPECT_FALSE(InvalidTextQuery(std::string(kMaxTextQuery, 'a')));
}

TEST_F(TextIndexTest, AnEmptyCheckoutIsAnEmptyIndex) {
  const std::unique_ptr<TextIndex> index = Build();
  ASSERT_TRUE(index);
  EXPECT_EQ(index->files(), 0u);
  EXPECT_EQ(Find(*index, "x").total_matches, 0u);
}

}  // namespace
}  // namespace code_browser
