// index_import: cpp_index protobuf (an index or a unit) -> SQLite database.
//
//   index_import index.pb                 # writes index.pb.sqlite next to it
//   index_import index.pb --out=x.sqlite --force
#include <filesystem>
#include <string>
#include <system_error>

#include "code_browser/index_schema.h"
#include "cpp_formatting/cpp_index_merge.h"
#include "cpp_formatting/index.pb.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

namespace {

namespace cl = llvm::cl;

cl::OptionCategory ImportCategory("index_import options");
cl::opt<std::string> IndexPath(cl::Positional, cl::desc("<index.pb>"),
                               cl::Required, cl::cat(ImportCategory));
cl::opt<std::string> OutPath(
    "out", cl::desc("Database to write (default: <index.pb>.sqlite)"),
    cl::cat(ImportCategory));
cl::opt<bool> Force("force", cl::desc("Replace an existing database"),
                    cl::cat(ImportCategory));

}  // namespace

int main(int argc, char** argv) {
  cl::HideUnrelatedOptions(ImportCategory);
  cl::ParseCommandLineOptions(
      argc, argv, "Imports a cpp_format symbol index into SQLite\n");
  cpp_index::IndexUnit unit;
  if (!readUnit(IndexPath, unit)) {
    llvm::errs() << "index_import: cannot read " << IndexPath << "\n";
    return 2;
  }
  const cpp_index::Index index = buildIndex(unit);

  const std::string out = OutPath.empty() ? IndexPath + ".sqlite" : OutPath;
  std::error_code ec;
  if (Force) std::filesystem::remove(out, ec);

  code_browser::ImportOptions opts;
  opts.source_path = IndexPath;
  code_browser::FillSourceInfo(opts);
  code_browser::ImportStats stats;
  const std::string error =
      code_browser::ImportIndexToFile(index, out, opts, &stats);
  if (!error.empty()) {
    llvm::errs() << "index_import: " << error << "\n";
    return 1;
  }
  llvm::outs() << "wrote " << out << ": " << stats.files << " files, "
               << stats.symbols << " symbols, " << stats.occurrences
               << " occurrences, " << stats.unresolved << " unresolved, "
               << stats.trigrams << " trigrams in " << stats.elapsed_ms
               << " ms (etag " << stats.etag << ")\n";
  return 0;
}
