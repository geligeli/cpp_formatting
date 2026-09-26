#include "code_browser/coverage.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace code_browser {
namespace {

namespace fs = std::filesystem;

auto MustParse(const std::string& text, const CoverageOptions& opts = {})
    -> std::unique_ptr<Coverage> {
  std::string error;
  std::unique_ptr<Coverage> c = Coverage::Parse(text, "t.lcov", opts, &error);
  EXPECT_TRUE(c) << error;
  return c;
}

auto ParseError(const std::string& text) -> std::string {
  std::string error;
  EXPECT_FALSE(Coverage::Parse(text, "t.lcov", {}, &error)) << text;
  return error;
}

// What bazel's LCOV merger writes for one file, trimmed.
constexpr char kReport[] = R"(SF:pkg/a.cpp
FN:3,_Z1fv
FNDA:2,_Z1fv
FNF:1
FNH:1
BRDA:4,0,0,1
BRDA:4,0,1,0
BRDA:9,0,0,-
BRDA:9,0,1,-
BRF:4
BRH:1
DA:3,2
DA:4,2
DA:5,0
DA:9,0
LH:2
LF:4
end_of_record
SF:pkg/sub/b.h
DA:1,7
end_of_record
SF:top.cc
DA:2,0
end_of_record
)";

TEST(CoverageTest, ReadsLinesAndBranches) {
  const std::unique_ptr<Coverage> c = MustParse(kReport);
  ASSERT_TRUE(c);
  ASSERT_EQ(c->files().size(), 3u);
  const FileCoverage* a = c->File("pkg/a.cpp");
  ASSERT_NE(a, nullptr);
  ASSERT_EQ(a->lines.size(), 4u);
  EXPECT_EQ(a->lines[0].line, 3u);
  EXPECT_EQ(a->lines[0].hits, 2u);
  EXPECT_EQ(a->lines[1].branches, 2u);  // line 4: one of two taken
  EXPECT_EQ(a->lines[1].branches_taken, 1u);
  EXPECT_EQ(a->lines[2].hits, 0u);
  EXPECT_EQ(a->lines[3].branches, 2u);  // line 9: `-` is found, not taken
  EXPECT_EQ(a->lines[3].branches_taken, 0u);
  EXPECT_EQ(a->totals.files, 1u);
  EXPECT_EQ(a->totals.lines_found, 4u);
  EXPECT_EQ(a->totals.lines_hit, 2u);
  EXPECT_EQ(a->totals.branches_found, 4u);
  EXPECT_EQ(a->totals.branches_hit, 1u);
  EXPECT_EQ(c->File("pkg/missing.cpp"), nullptr);
  EXPECT_EQ(c->totals().files, 3u);
  EXPECT_EQ(c->totals().lines_found, 6u);
  EXPECT_EQ(c->totals().lines_hit, 3u);
}

TEST(CoverageTest, DirectoriesRollUp) {
  const std::unique_ptr<Coverage> c = MustParse(kReport);
  ASSERT_TRUE(c);
  const CoverageTotals* root = c->Dir("");
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(root->files, 3u);
  const CoverageTotals* pkg = c->Dir("pkg");
  ASSERT_NE(pkg, nullptr);
  EXPECT_EQ(pkg->files, 2u);
  EXPECT_EQ(pkg->lines_found, 5u);
  EXPECT_EQ(pkg->lines_hit, 3u);
  const CoverageTotals* sub = c->Dir("pkg/sub");
  ASSERT_NE(sub, nullptr);
  EXPECT_EQ(sub->files, 1u);
  EXPECT_EQ(c->Dir("pkg/a.cpp"), nullptr);  // a file is not a directory
  EXPECT_EQ(c->Dir("other"), nullptr);

  const std::unique_ptr<Coverage> abs =
      MustParse("SF:/usr/include/x.h\nDA:1,1\nend_of_record\n");
  ASSERT_TRUE(abs);
  ASSERT_NE(abs->Dir("/usr/include"), nullptr);
  ASSERT_NE(abs->Dir("/usr"), nullptr);
  ASSERT_NE(abs->Dir("/"), nullptr);
  EXPECT_EQ(abs->Dir("")->files, 1u);
}

// Two records of one file (one per test binary) add up, line by line and
// branch by branch; the summaries in the file are not trusted.
TEST(CoverageTest, DuplicateRecordsAreSummed) {
  const std::unique_ptr<Coverage> c = MustParse(
      "SF:a.cc\nDA:1,1\nDA:2,0\nBRDA:2,0,0,0\nLF:99\nLH:99\nend_of_record\n"
      "SF:./a.cc\nDA:2,3\nDA:3,0\nBRDA:2,0,0,4\nBRDA:2,0,1,-\n"
      "end_of_record\n");
  ASSERT_TRUE(c);
  ASSERT_EQ(c->files().size(), 1u);
  const FileCoverage& a = c->files()[0];
  ASSERT_EQ(a.lines.size(), 3u);
  EXPECT_EQ(a.lines[1].hits, 3u);
  EXPECT_EQ(a.lines[1].branches, 2u);
  EXPECT_EQ(a.lines[1].branches_taken, 1u);
  EXPECT_EQ(a.totals.lines_found, 3u);
  EXPECT_EQ(a.totals.lines_hit, 2u);
}

TEST(CoverageTest, CountsSaturate) {
  const std::string max = std::to_string(std::numeric_limits<uint64_t>::max());
  const std::unique_ptr<Coverage> c = MustParse("SF:a.cc\nDA:1," + max +
                                                "\nend_of_record\nSF:a.cc\n"
                                                "DA:1,5\nend_of_record\n");
  ASSERT_TRUE(c);
  EXPECT_EQ(c->files()[0].lines[0].hits, std::numeric_limits<uint64_t>::max());
}

TEST(CoverageTest, ToleratesWhatWriterVariantsEmit) {
  // CRLF, blank lines, TN/VER, a DA checksum, and line 0 (skipped).
  const std::unique_ptr<Coverage> c = MustParse(
      "TN:\r\nVER:2\r\n\r\nSF:a.cc\r\nDA:0,1\r\nDA:1,2,abcdef\r\n"
      "FNL:0,1,2\r\nFNA:0,1,f\r\nend_of_record\r\n");
  ASSERT_TRUE(c);
  ASSERT_EQ(c->files()[0].lines.size(), 1u);
  EXPECT_EQ(c->files()[0].lines[0].hits, 2u);

  const std::unique_ptr<Coverage> empty = MustParse("");
  ASSERT_TRUE(empty);
  EXPECT_TRUE(empty->files().empty());
  EXPECT_EQ(empty->Dir(""), nullptr);
}

TEST(CoverageTest, ErrorsNameTheLine) {
  EXPECT_EQ(ParseError("SF:a.cc\nDA:x,1\nend_of_record\n"),
            "t.lcov:2: DA: expected <line>,<count>, got 'DA:x,1'");
  EXPECT_EQ(ParseError("SF:a.cc\nDA:1,-3\nend_of_record\n"),
            "t.lcov:2: DA: expected <line>,<count>, got 'DA:1,-3'");
  EXPECT_EQ(ParseError("DA:1,1\n"), "t.lcov:1: DA outside a record");
  EXPECT_EQ(ParseError("SF:a.cc\nSF:b.cc\n"), "t.lcov:2: SF inside a record");
  EXPECT_EQ(ParseError("TN:\nSF:a.cc\nDA:1,1\n"),
            "t.lcov:2: record has no end_of_record");
  EXPECT_EQ(ParseError("end_of_record\n"),
            "t.lcov:1: end_of_record outside a record");
  EXPECT_EQ(ParseError("SF:a.cc\nXX:1\nend_of_record\n"),
            "t.lcov:2: not an LCOV record: 'XX:1'");
  EXPECT_EQ(ParseError("garbage\n"), "t.lcov:1: not an LCOV record: 'garbage'");
  EXPECT_EQ(ParseError("SF:\n"), "t.lcov:1: SF with no path");
  EXPECT_EQ(ParseError("SF:a.cc\nBRDA:1,0,x\nend_of_record\n"),
            "t.lcov:2: BRDA: expected <line>,<block>,<branch>,<taken>, got "
            "'BRDA:1,0,x'");
}

TEST(CoverageTest, PathsAreSpelledAsTheIndexSpellsThem) {
  CoverageOptions opts;
  opts.strip_prefixes = {"/work/repo", "/cache/execroot/_main/"};
  EXPECT_EQ(NormalizeCoveragePath("pkg/a.cc", opts), "pkg/a.cc");
  EXPECT_EQ(NormalizeCoveragePath("./pkg//a.cc", opts), "pkg/a.cc");
  EXPECT_EQ(NormalizeCoveragePath("/proc/self/cwd/pkg/a.cc", opts), "pkg/a.cc");
  EXPECT_EQ(NormalizeCoveragePath("/work/repo/pkg/a.cc", opts), "pkg/a.cc");
  EXPECT_EQ(NormalizeCoveragePath("/work/repository/a.cc", opts),
            "/work/repository/a.cc");  // a prefix is a whole directory
  EXPECT_EQ(NormalizeCoveragePath("/cache/execroot/_main/external/x/y.h", opts),
            "external/x/y.h");
  EXPECT_EQ(NormalizeCoveragePath(
                "/tmp/sandbox/7/execroot/_main/bazel-out/k8/bin/g.pb.cc", opts),
            "bazel-out/k8/bin/g.pb.cc");
  EXPECT_EQ(NormalizeCoveragePath("/usr/include/stdio.h", opts),
            "/usr/include/stdio.h");
}

TEST(CoverageTest, LoadTakesTheEtagAndTimeFromTheFile) {
  const fs::path dir =
      fs::temp_directory_path() /
      ("code_browser_coverage_test_" +
       std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
  fs::create_directories(dir);
  const fs::path file = dir / "r.lcov";
  std::ofstream(file, std::ios::binary) << kReport;
  std::string error;
  const std::unique_ptr<Coverage> c = Coverage::Load(file, {}, &error);
  ASSERT_TRUE(c) << error;
  EXPECT_EQ(c->path(), file.string());
  EXPECT_NE(c->mtime_ns(), 0);
  EXPECT_EQ(c->collected_at().size(), 20u);  // 2026-09-26T12:00:00Z
  const std::unique_ptr<Coverage> again = Coverage::Load(file, {}, &error);
  ASSERT_TRUE(again) << error;
  EXPECT_EQ(again->etag(), c->etag());
  fs::last_write_time(file,
                      fs::last_write_time(file) + std::chrono::seconds(10));
  const std::unique_ptr<Coverage> later = Coverage::Load(file, {}, &error);
  ASSERT_TRUE(later) << error;
  EXPECT_NE(later->etag(), c->etag());

  EXPECT_FALSE(Coverage::Load(dir / "missing.lcov", {}, &error));
  EXPECT_EQ(error, "cannot read " + (dir / "missing.lcov").string());
  std::ofstream(dir / "bad.lcov", std::ios::binary) << "SF:a\nDA:1\n";
  EXPECT_FALSE(Coverage::Load(dir / "bad.lcov", {}, &error));
  EXPECT_EQ(error, (dir / "bad.lcov").string() +
                       ":2: DA: expected <line>,<count>, got 'DA:1'");
  fs::remove_all(dir);
}

}  // namespace
}  // namespace code_browser
