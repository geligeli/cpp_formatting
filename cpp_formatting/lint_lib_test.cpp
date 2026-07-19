#include "cpp_formatting/lint_lib.h"

#include <gtest/gtest.h>

#include <string>

#include "llvm/Support/JSON.h"

namespace {

auto diff(llvm::StringRef Original, llvm::StringRef Modified,
          llvm::StringRef Path = "f.cpp") -> std::string {
  std::string Out;
  llvm::raw_string_ostream OS(Out);
  emitUnifiedDiff(Original, Modified, Path, OS);
  return Out;
}

TEST(UnifiedDiff, IdenticalInputsProduceNoOutput) {
  EXPECT_EQ(diff("int a;\nint b;\n", "int a;\nint b;\n"), "");
}

TEST(UnifiedDiff, SingleChange) {
  EXPECT_EQ(diff("line1\nline2\nline3\n", "line1\nCHANGED\nline3\n"),
            "--- a/f.cpp\n"
            "+++ b/f.cpp\n"
            "@@ -1,3 +1,3 @@\n"
            " line1\n"
            "-line2\n"
            "+CHANGED\n"
            " line3\n");
}

TEST(UnifiedDiff, DistantChangesProduceSeparateHunks) {
  EXPECT_EQ(diff("a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\n",
                 "A1\nb\nc\nd\ne\nf\ng\nh\ni\nj\nK1\n"),
            "--- a/f.cpp\n"
            "+++ b/f.cpp\n"
            "@@ -1,4 +1,4 @@\n"
            "-a\n"
            "+A1\n"
            " b\n"
            " c\n"
            " d\n"
            "@@ -8,4 +8,4 @@\n"
            " h\n"
            " i\n"
            " j\n"
            "-k\n"
            "+K1\n");
}

TEST(UnifiedDiff, NearbyChangesMergeIntoOneHunk) {
  // Changes at lines 1 and 6 are separated by 4 context lines (< 2*3), so
  // they must share a hunk.
  EXPECT_EQ(diff("a\nb\nc\nd\ne\nf\ng\n", "A\nb\nc\nd\ne\nF\ng\n"),
            "--- a/f.cpp\n"
            "+++ b/f.cpp\n"
            "@@ -1,7 +1,7 @@\n"
            "-a\n"
            "+A\n"
            " b\n"
            " c\n"
            " d\n"
            " e\n"
            "-f\n"
            "+F\n"
            " g\n");
}

TEST(UnifiedDiff, InsertionAtBeginning) {
  EXPECT_EQ(diff("b\nc\n", "a\nb\nc\n"),
            "--- a/f.cpp\n"
            "+++ b/f.cpp\n"
            "@@ -1,2 +1,3 @@\n"
            "+a\n"
            " b\n"
            " c\n");
}

TEST(UnifiedDiff, InsertionIntoEmptyFile) {
  EXPECT_EQ(diff("", "x\ny\n"),
            "--- a/f.cpp\n"
            "+++ b/f.cpp\n"
            "@@ -0,0 +1,2 @@\n"
            "+x\n"
            "+y\n");
}

TEST(UnifiedDiff, DeletionToEmptyFile) {
  EXPECT_EQ(diff("x\ny\n", ""),
            "--- a/f.cpp\n"
            "+++ b/f.cpp\n"
            "@@ -1,2 +0,0 @@\n"
            "-x\n"
            "-y\n");
}

TEST(UnifiedDiff, MissingTrailingNewline) {
  EXPECT_EQ(diff("one\ntwo", "one\nthree"),
            "--- a/f.cpp\n"
            "+++ b/f.cpp\n"
            "@@ -1,2 +1,2 @@\n"
            " one\n"
            "-two\n"
            "\\ No newline at end of file\n"
            "+three\n"
            "\\ No newline at end of file\n");
}

TEST(LintReportText, Format) {
  LintReport R;
  R.add({"b.cpp", 5, 2, "rule/b", "second"});
  R.add({"a.cpp", 3, 1, "rule/a", "first"});
  std::string Out;
  llvm::raw_string_ostream OS(Out);
  R.emitText(OS);
  EXPECT_EQ(Out,
            "a.cpp:3:1: warning: first [rule/a]\n"
            "b.cpp:5:2: warning: second [rule/b]\n");
}

TEST(LintReportSARIF, ParsesAndHasExpectedShape) {
  LintReport R;
  R.add({"foo.cpp", 3, 5, "rule/a", "'x' should be 'y'"});
  R.add({"bar.cpp", 10, 1, "rule/b", "other"});
  std::string Out;
  llvm::raw_string_ostream OS(Out);
  R.emitSARIF(OS, "normalize_variables");

  llvm::Expected<llvm::json::Value> V = llvm::json::parse(Out);
  ASSERT_TRUE(static_cast<bool>(V)) << llvm::toString(V.takeError());
  const llvm::json::Object* Root = V->getAsObject();
  ASSERT_NE(Root, nullptr);

  auto Version = Root->getString("version");
  ASSERT_TRUE(Version.has_value());
  EXPECT_EQ(*Version, "2.1.0");

  const llvm::json::Array* Runs = Root->getArray("runs");
  ASSERT_NE(Runs, nullptr);
  ASSERT_EQ(Runs->size(), 1u);

  const llvm::json::Object* Run = (*Runs)[0].getAsObject();
  ASSERT_NE(Run, nullptr);
  const llvm::json::Array* Results = Run->getArray("results");
  ASSERT_NE(Results, nullptr);
  ASSERT_EQ(Results->size(), 2u);

  const llvm::json::Object* R0 = (*Results)[1].getAsObject();  // sorted by file
  ASSERT_NE(R0, nullptr);
  auto RuleId = R0->getString("ruleId");
  ASSERT_TRUE(RuleId.has_value());
  EXPECT_EQ(*RuleId, "rule/a");
  auto Level = R0->getString("level");
  ASSERT_TRUE(Level.has_value());
  EXPECT_EQ(*Level, "warning");

  const llvm::json::Array* Locations = R0->getArray("locations");
  ASSERT_NE(Locations, nullptr);
  const llvm::json::Object* Phys =
      (*Locations)[0].getAsObject()->getObject("physicalLocation");
  ASSERT_NE(Phys, nullptr);
  const llvm::json::Object* Region = Phys->getObject("region");
  ASSERT_NE(Region, nullptr);
  auto Line = Region->getInteger("startLine");
  auto Col = Region->getInteger("startColumn");
  ASSERT_TRUE(Line.has_value());
  ASSERT_TRUE(Col.has_value());
  EXPECT_EQ(*Line, 3);
  EXPECT_EQ(*Col, 5);
}

}  // namespace
