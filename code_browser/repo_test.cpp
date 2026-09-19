#include "code_browser/repo.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "code_browser/file_cache.h"

namespace code_browser {
namespace {

namespace fs = std::filesystem;

class RepoTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() /
           ("code_browser_repo_test_" +
            std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
            "_" + std::to_string(counter_++));
    fs::remove_all(dir_);
    fs::create_directories(dir_ / "src" / "sub");
    Write("src/a.h", "int a;\n");
    Write("src/sub/b.cpp", "int b;\nint c;\n");
    Write("outside.txt", "secret\n");
  }
  void TearDown() override { fs::remove_all(dir_); }

  void Write(const std::string& rel, const std::string& text) {
    const fs::path p = dir_ / rel;
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
  }
  auto OpenRoot(RepoOptions opts = {}) -> Repo {
    opts.root = dir_ / "src";
    std::string error;
    std::optional<Repo> r = Repo::Open(opts, &error);
    EXPECT_TRUE(r) << error;
    return *r;
  }

  fs::path dir_;
  static inline int counter_ = 0;
};

TEST_F(RepoTest, NormalizeRequestPath) {
  EXPECT_EQ(Repo::NormalizeRequestPath("a/b.h"), "a/b.h");
  EXPECT_EQ(Repo::NormalizeRequestPath("./a/./b.h"), "a/b.h");
  EXPECT_EQ(Repo::NormalizeRequestPath("/usr/include/x.h"), "/usr/include/x.h");
  EXPECT_EQ(Repo::NormalizeRequestPath("", true), "");
  EXPECT_EQ(Repo::NormalizeRequestPath(".", true), "");
  EXPECT_FALSE(Repo::NormalizeRequestPath(""));
  EXPECT_FALSE(Repo::NormalizeRequestPath(".."));
  EXPECT_FALSE(Repo::NormalizeRequestPath("a/../b"));
  EXPECT_FALSE(Repo::NormalizeRequestPath("a//b"));
  EXPECT_FALSE(Repo::NormalizeRequestPath("a/"));
  EXPECT_FALSE(Repo::NormalizeRequestPath("a\\b"));
  EXPECT_FALSE(Repo::NormalizeRequestPath(std::string("a\0b", 3)));
  EXPECT_FALSE(Repo::NormalizeRequestPath("a\nb"));
}

TEST_F(RepoTest, ResolvesSourcesInsideTheRootOnly) {
  const Repo r = OpenRoot();
  ASSERT_TRUE(r.Resolve("a.h", cpp_index::SOURCE));
  ASSERT_TRUE(r.Resolve("sub/b.cpp", cpp_index::FILE_KIND_UNSPECIFIED));
  EXPECT_FALSE(r.Resolve("nope.h", cpp_index::SOURCE));
  EXPECT_FALSE(r.Resolve("../outside.txt", cpp_index::SOURCE));
  EXPECT_FALSE(r.Resolve("sub", cpp_index::SOURCE));  // a directory
  // A symlink that leads out of the root is refused even though the path
  // itself looks fine.
  fs::create_symlink(dir_ / "outside.txt", dir_ / "src" / "link.txt");
  EXPECT_FALSE(r.Resolve("link.txt", cpp_index::SOURCE));
  // System files: absolute, and only when allowed and SYSTEM.
  EXPECT_TRUE(r.Resolve((dir_ / "outside.txt").string(), cpp_index::SYSTEM));
  EXPECT_FALSE(r.Resolve((dir_ / "outside.txt").string(), cpp_index::SOURCE));
  RepoOptions no_system;
  no_system.serve_system_files = false;
  EXPECT_FALSE(OpenRoot(no_system).Resolve((dir_ / "outside.txt").string(),
                                           cpp_index::SYSTEM));
}

TEST_F(RepoTest, GeneratedFilesLiveUnderTheExecRoot) {
  // Without an exec root nothing generated resolves.
  EXPECT_FALSE(OpenRoot().exec_root());
  EXPECT_FALSE(OpenRoot().Resolve("bazel-out/x/gen.h", cpp_index::GENERATED));
  // A bazel-out convenience symlink names it.
  fs::create_directories(dir_ / "execroot" / "bazel-out" / "x");
  Write("execroot/bazel-out/x/gen.h", "gen\n");
  fs::create_directories(dir_ / "execroot" / "external" / "dep");
  Write("execroot/external/dep/d.h", "dep\n");
  fs::create_symlink(dir_ / "execroot" / "bazel-out",
                     dir_ / "src" / "bazel-out");
  const Repo r = OpenRoot();
  ASSERT_TRUE(r.exec_root());
  EXPECT_EQ(*r.exec_root(), dir_ / "execroot");
  EXPECT_TRUE(r.Resolve("bazel-out/x/gen.h", cpp_index::GENERATED));
  EXPECT_TRUE(r.Resolve("external/dep/d.h", cpp_index::EXTERNAL));
  EXPECT_FALSE(r.Resolve("bazel-out/x/missing.h", cpp_index::GENERATED));
  // Explicit exec root wins over discovery.
  RepoOptions explicit_root;
  explicit_root.exec_root = dir_;
  EXPECT_EQ(*OpenRoot(explicit_root).exec_root(), dir_);
}

TEST_F(RepoTest, ReadHeadInEveryGitLayout) {
  Repo r = OpenRoot();
  EXPECT_EQ(r.ReadHead().commit, "");  // not a checkout
  const std::string sha = "0123456789abcdef0123456789abcdef01234567";
  // Symbolic HEAD + loose ref.
  Write("src/.git/HEAD", "ref: refs/heads/main\n");
  Write("src/.git/refs/heads/main", sha + "\n");
  GitHead h = r.ReadHead();
  EXPECT_EQ(h.commit, sha);
  EXPECT_EQ(h.ref, "refs/heads/main");
  // Packed refs only.
  fs::remove(dir_ / "src/.git/refs/heads/main");
  Write("src/.git/packed-refs",
        "# pack-refs with: peeled fully-peeled sorted\n" + sha +
            " refs/heads/main\n^deadbeef\n");
  EXPECT_EQ(r.ReadHead().commit, sha);
  // Detached.
  Write("src/.git/HEAD", sha + "\n");
  h = r.ReadHead();
  EXPECT_EQ(h.commit, sha);
  EXPECT_EQ(h.ref, "");
  // A worktree: .git is a file pointing at a gitdir with a commondir.
  fs::remove_all(dir_ / "src/.git");
  Write("main/.git/HEAD", "ref: refs/heads/other\n");
  Write("main/.git/packed-refs", sha + " refs/heads/wt\n");
  Write("main/.git/worktrees/wt/HEAD", "ref: refs/heads/wt\n");
  Write("main/.git/worktrees/wt/commondir", "../..\n");
  Write("src/.git",
        "gitdir: " + (dir_ / "main/.git/worktrees/wt").string() + "\n");
  h = r.ReadHead();
  EXPECT_EQ(h.commit, sha);
  EXPECT_EQ(h.ref, "refs/heads/wt");
}

TEST_F(RepoTest, FileCacheLinesAndRefresh) {
  const Repo r = OpenRoot();
  FileCache cache(1 << 20);
  const fs::path b = *r.Resolve("sub/b.cpp", cpp_index::SOURCE);
  std::shared_ptr<const CachedFile> f = cache.Get(r, b);
  ASSERT_TRUE(f);
  EXPECT_EQ(f->data, "int b;\nint c;\n");
  EXPECT_EQ(f->line_starts, (std::vector<uint32_t>{0, 7}));
  EXPECT_EQ(f->LineOf(0), std::make_pair(1u, 1u));
  EXPECT_EQ(f->LineOf(4), std::make_pair(1u, 5u));
  EXPECT_EQ(f->LineOf(7), std::make_pair(2u, 1u));
  EXPECT_EQ(f->LineOf(11), std::make_pair(2u, 5u));
  EXPECT_EQ(f->LineText(1), "int b;");
  EXPECT_EQ(f->LineText(2), "int c;");
  EXPECT_EQ(f->LineText(3), "");
  EXPECT_FALSE(f->etag.empty());
  // The same object comes back while the file is unchanged.
  EXPECT_EQ(cache.Get(r, b), f);
  // A changed size re-reads.
  Write("src/sub/b.cpp", "int bb;\n");
  std::shared_ptr<const CachedFile> g = cache.Get(r, b);
  ASSERT_TRUE(g);
  EXPECT_NE(g, f);
  EXPECT_EQ(g->data, "int bb;\n");
  EXPECT_EQ(cache.bytes(), g->size);
}

TEST_F(RepoTest, FileCacheEvictsByBytes) {
  const Repo r = OpenRoot();
  FileCache cache(10);  // room for one small file
  const fs::path a = *r.Resolve("a.h", cpp_index::SOURCE);
  const fs::path b = *r.Resolve("sub/b.cpp", cpp_index::SOURCE);
  ASSERT_TRUE(cache.Get(r, a));
  ASSERT_TRUE(cache.Get(r, b));
  EXPECT_LE(cache.bytes(), 14u);  // the most recent file survives
}

}  // namespace
}  // namespace code_browser
