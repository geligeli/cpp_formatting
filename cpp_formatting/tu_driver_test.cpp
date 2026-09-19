#include "cpp_formatting/tu_driver.h"

#include <gtest/gtest.h>

#include "clang/Tooling/ArgumentsAdjusters.h"

namespace {

TEST(IsHeaderSource, RecognisesEveryHeaderExtension) {
  EXPECT_TRUE(isHeaderSource("a.h"));
  EXPECT_TRUE(isHeaderSource("dir/a.hh"));
  EXPECT_TRUE(isHeaderSource("/abs/a.hpp"));
  EXPECT_TRUE(isHeaderSource("a.hxx"));
  EXPECT_TRUE(isHeaderSource("a.h++"));
}

TEST(IsHeaderSource, EverythingElseIsASource) {
  EXPECT_FALSE(isHeaderSource("a.cpp"));
  EXPECT_FALSE(isHeaderSource("a.cc"));
  EXPECT_FALSE(isHeaderSource("a.cxx"));
  EXPECT_FALSE(isHeaderSource("a.c++"));
  EXPECT_FALSE(isHeaderSource("noext"));
  // The extension is the file's, not a directory's.
  EXPECT_FALSE(isHeaderSource("dir.h/main.cpp"));
  EXPECT_FALSE(isHeaderSource("dir.h/noext"));
}

TEST(ResolveJobs, ZeroMeansEveryCpuAndRequestsAreCapped) {
  const unsigned All = resolveJobs(0);
  EXPECT_GE(All, 1u);
  EXPECT_EQ(resolveJobs(1), 1u);
  EXPECT_EQ(resolveJobs(All), All);
  EXPECT_EQ(resolveJobs(All + 1000), All);
  EXPECT_LE(resolveJobs(2), 2u);
}

TEST(StandardArgumentsAdjuster, StripsGccFlagAndSuppliesResourceDir) {
  auto Adjust = makeStandardArgumentsAdjuster("/res");
  clang::tooling::CommandLineArguments Out = Adjust(
      {"clang++", "-fno-canonical-system-headers", "-std=c++17", "a.cpp"}, "");
  EXPECT_EQ(Out,
            (clang::tooling::CommandLineArguments{
                "clang++", "-resource-dir=/res", "-w", "-std=c++17", "a.cpp"}));
}

TEST(StandardArgumentsAdjuster, KeepsAnExplicitResourceDir) {
  auto Adjust = makeStandardArgumentsAdjuster("/res");
  clang::tooling::CommandLineArguments Out =
      Adjust({"clang++", "-resource-dir=/mine", "a.cpp"}, "");
  EXPECT_EQ(Out, (clang::tooling::CommandLineArguments{
                     "clang++", "-w", "-resource-dir=/mine", "a.cpp"}));
}

TEST(StandardArgumentsAdjuster, NoResourceDirOnlyStrips) {
  auto Adjust = makeStandardArgumentsAdjuster("");
  clang::tooling::CommandLineArguments Out =
      Adjust({"clang++", "-fno-canonical-system-headers", "a.cpp"}, "");
  EXPECT_EQ(Out,
            (clang::tooling::CommandLineArguments{"clang++", "-w", "a.cpp"}));
}

// The project's warning flags are its own business: -w goes in whatever they
// are, so a warning in a file we only parse cannot be promoted to an error and
// fail the run.  Test 12 of normalize_variables_integration_test.sh is the
// end-to-end version, including the real error that still does fail.
TEST(StandardArgumentsAdjuster, SilencesTheProjectsWarningFlags) {
  auto Adjust = makeStandardArgumentsAdjuster("");
  clang::tooling::CommandLineArguments Out =
      Adjust({"clang++", "-Wall", "-Wextra", "-Werror", "a.cpp"}, "");
  EXPECT_EQ(Out, (clang::tooling::CommandLineArguments{
                     "clang++", "-w", "-Wall", "-Wextra", "-Werror", "a.cpp"}));
}

}  // namespace
