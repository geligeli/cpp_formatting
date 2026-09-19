// code_browser: serves a git checkout with every indexed token annotated.
//
//   code_browser --db=index.sqlite --root=/path/to/checkout
//   code_browser --index=index.pb --root=. --port=8080     # imports on demand
//
// The pages are embedded; --assets-dir=code_browser/web serves them from
// disk instead while they are being developed.
#include <chrono>
#include <compare>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>

#include "code_browser/api.h"
#include "code_browser/file_cache.h"
#include "code_browser/http_server.h"
#include "code_browser/index_db.h"
#include "code_browser/index_schema.h"
#include "code_browser/repo.h"
#include "code_browser/static_assets.h"
#include "cpp_formatting/cpp_index_merge.h"
#include "cpp_formatting/index.pb.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

namespace {

namespace cl = llvm::cl;
namespace fs = std::filesystem;

cl::OptionCategory Category("code_browser options");
cl::opt<std::string> DbPath("db", cl::desc("An index database (index_import)"),
                            cl::cat(Category));
cl::opt<std::string> IndexPath(
    "index",
    cl::desc("An index .pb; imported into <index>.sqlite when that is "
             "missing or older"),
    cl::cat(Category));
cl::opt<std::string> Root("root", cl::desc("The checkout (default: cwd)"),
                          cl::init("."), cl::cat(Category));
cl::opt<std::string> ExecRoot(
    "exec-root",
    cl::desc("Where bazel-out/ and external/ resolve (default: from the "
             "root's bazel-out symlink)"),
    cl::cat(Category));
cl::opt<std::string> Address("address", cl::desc("Address to bind"),
                             cl::init("127.0.0.1"), cl::cat(Category));
cl::opt<unsigned> Port("port", cl::desc("Port to bind (0: any free port)"),
                       cl::init(8080), cl::cat(Category));
cl::opt<std::string> PortFile(
    "port-file", cl::desc("Write the bound port to this file once listening"),
    cl::cat(Category));
cl::opt<unsigned> Threads("threads", cl::desc("IO threads (0: one per CPU)"),
                          cl::init(0), cl::cat(Category));
cl::opt<std::string> AssetsDir(
    "assets-dir", cl::desc("Serve the pages from this directory (dev mode)"),
    cl::cat(Category));
cl::opt<unsigned> FileCacheMb("file-cache-mb",
                              cl::desc("Source file cache size"), cl::init(256),
                              cl::cat(Category));
cl::opt<bool> ServeSystemFiles(
    "serve-system-files",
    cl::desc("Serve absolute (SYSTEM) paths the index names"), cl::init(true),
    cl::cat(Category));
cl::opt<bool> LogRequests("log-requests", cl::desc("Log every request"),
                          cl::init(true), cl::cat(Category));
cl::opt<bool> Check("check",
                    cl::desc("Open the index, print its stats, and exit"),
                    cl::cat(Category));

auto Mtime(const fs::path& p) -> std::optional<fs::file_time_type> {
  std::error_code ec;
  const auto t = fs::last_write_time(p, ec);
  if (ec) return std::nullopt;
  return t;
}

// The database to open: --db as given, or --index imported when needed.
auto ResolveDatabase(std::string* error) -> std::string {
  if (!DbPath.empty()) return DbPath;
  if (IndexPath.empty()) {
    *error = "one of --db or --index is required";
    return "";
  }
  const std::string db = IndexPath.getValue() + ".sqlite";
  const auto index_mtime = Mtime(IndexPath.getValue());
  if (!index_mtime) {
    *error = "cannot read " + IndexPath;
    return "";
  }
  const auto db_mtime = Mtime(db);
  if (db_mtime && *db_mtime >= *index_mtime) return db;
  llvm::errs() << "code_browser: importing " << IndexPath << " into " << db
               << "\n";
  cpp_index::IndexUnit unit;
  if (!readUnit(IndexPath.getValue(), unit)) {
    *error = "cannot read " + IndexPath;
    return "";
  }
  const cpp_index::Index index = buildIndex(unit);
  std::error_code ec;
  fs::remove(db, ec);
  code_browser::ImportOptions opts;
  opts.source_path = IndexPath.getValue();
  code_browser::FillSourceInfo(opts);
  code_browser::ImportStats stats;
  if (std::string e = code_browser::ImportIndexToFile(index, db, opts, &stats);
      !e.empty()) {
    *error = e;
    return "";
  }
  llvm::errs() << "code_browser: imported " << stats.files << " files, "
               << stats.symbols << " symbols, " << stats.occurrences
               << " occurrences in " << stats.elapsed_ms << " ms\n";
  return db;
}

}  // namespace

int main(int argc, char** argv) {
  cl::HideUnrelatedOptions(Category);
  cl::ParseCommandLineOptions(
      argc, argv, "Serves a checkout with its symbol index as a web page\n");

  std::string error;
  const std::string db_path = ResolveDatabase(&error);
  if (db_path.empty()) {
    llvm::errs() << "code_browser: " << error << "\n";
    return 2;
  }
  const std::unique_ptr<code_browser::IndexDb> db =
      code_browser::IndexDb::Open(db_path, &error);
  if (!db) {
    llvm::errs() << "code_browser: " << error << "\n";
    return 2;
  }
  const code_browser::DbStats& stats = db->stats();
  llvm::errs() << "code_browser: " << db_path << ": " << stats.files
               << " files, " << stats.symbols << " symbols, "
               << stats.occurrences << " occurrences (imported "
               << stats.imported_at << " from " << stats.source_path << ")\n";
  if (Check) return 0;

  code_browser::RepoOptions ropts;
  ropts.root = Root.getValue();
  if (!ExecRoot.empty()) ropts.exec_root = ExecRoot.getValue();
  ropts.serve_system_files = ServeSystemFiles;
  std::optional<code_browser::Repo> repo =
      code_browser::Repo::Open(ropts, &error);
  if (!repo) {
    llvm::errs() << "code_browser: " << error << "\n";
    return 2;
  }
  llvm::errs() << "code_browser: root " << repo->root().string();
  if (repo->exec_root())
    llvm::errs() << ", exec root " << repo->exec_root()->string();
  llvm::errs() << "\n";

  code_browser::FileCache files(static_cast<size_t>(FileCacheMb) << 20);
  const code_browser::ApiHandler api(*db, *repo, files);
  const code_browser::StaticAssets assets =
      AssetsDir.empty()
          ? code_browser::StaticAssets::FromEmbedded()
          : code_browser::StaticAssets::FromDirectory(AssetsDir.getValue());

  code_browser::ServerOptions sopts;
  sopts.address = Address;
  sopts.port = static_cast<uint16_t>(Port);
  sopts.threads = Threads;
  sopts.log_requests = LogRequests;
  code_browser::HttpServer server(sopts, api, assets);
  if (!server.Start(&error)) {
    llvm::errs() << "code_browser: " << error << "\n";
    return 1;
  }
  llvm::errs() << "code_browser: listening on http://" << Address << ":"
               << server.bound_port() << "/\n";
  if (!PortFile.empty()) {
    std::ofstream out(PortFile.getValue());
    out << server.bound_port() << "\n";
  }
  server.InstallSignalHandlers();
  server.Join();
  llvm::errs() << "code_browser: stopped\n";
  return 0;
}
