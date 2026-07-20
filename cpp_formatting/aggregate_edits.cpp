// aggregate_edits — merge per-TU edit-record files (as emitted by
// `--emit-edits`) into one repository-wide change: resolve template-dependent
// tokens across TUs, merge edits per file (dedup identical, flag conflicts),
// and either print a git-apply-able unified diff (default) or apply in place.
//
// File keys in the records are resolved against --root (default: cwd), so under
// `bazel run` --root is $BUILD_WORKSPACE_DIRECTORY.

#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "cpp_formatting/lint_lib.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

static cl::opt<bool> Apply(
    "apply",
    cl::desc("Write merged edits to files in place (default: print a diff)."));
static cl::opt<bool> Check(
    "check",
    cl::desc("Report whether any edits would be made and exit 1 if so; reads "
             "no source files (hermetic lint gate)."));
static cl::opt<std::string> Root(
    "root",
    cl::desc("Directory that record file keys resolve against (default: cwd)."),
    cl::init(""));
static cl::list<std::string> Inputs(cl::Positional,
                                    cl::desc("<records.json>..."),
                                    cl::OneOrMore);

int main(int argc, char** argv) {
  cl::ParseCommandLineOptions(argc, argv,
                              "cpp_format per-TU edit-record aggregator\n");

  // Record keys are the file's real path.  Under Bazel that resolves to the
  // absolute workspace path (source symlinks), so absolute keys are used as-is;
  // relative keys (e.g. from a plain `--emit-edits` run) join with --root.
  auto resolvePath = [&](StringRef File) -> std::string {
    if (llvm::sys::path::is_absolute(File) || Root.empty()) return File.str();
    return (Root + "/" + File).str();
  };
  // A workspace-relative label for diff headers (git-apply-able from the root).
  auto displayPath = [&](StringRef File) -> std::string {
    if (!Root.empty() && File.starts_with(Root) && File.size() > Root.size() &&
        File[Root.size()] == '/')
      return File.drop_front(Root.size() + 1).str();
    return File.str();
  };
  auto readFile = [&](StringRef File) -> std::optional<std::string> {
    auto Buf = MemoryBuffer::getFile(resolvePath(File));
    if (!Buf) return std::nullopt;
    return (*Buf)->getBuffer().str();
  };

  std::vector<EditReport> Reports;
  for (const std::string& Path : Inputs) {
    auto Buf = MemoryBuffer::getFile(Path);
    if (!Buf) {
      errs() << "cannot read '" << Path << "': " << Buf.getError().message()
             << "\n";
      return 2;
    }
    EditReport R;
    if (!parseEditReport((*Buf)->getBuffer(), R)) {
      errs() << "malformed edit records: '" << Path << "'\n";
      return 2;
    }
    Reports.push_back(std::move(R));
  }

  if (Check) {
    std::map<std::string, std::vector<EditRecord>> Merged;
    std::vector<std::string> Conflicts;
    const bool Ok = mergeEditReports(Reports, Merged, Conflicts);
    for (const std::string& C : Conflicts) errs() << "conflict: " << C << "\n";
    std::size_t Total = 0;
    for (const auto& [File, Edits] : Merged) {
      if (Edits.empty()) continue;
      outs() << File << ": " << Edits.size() << " edit(s)\n";
      Total += Edits.size();
    }
    if (Total == 0 && Ok) return 0;
    errs() << Total << " formatting edit(s) would be applied across "
           << Merged.size()
           << " file(s); run `bazel run //cpp_formatting:format.fix` to apply, "
              "or `:format.diff` to preview.\n";
    return 1;
  }

  std::map<std::string, std::string> Out;
  std::vector<std::string> Conflicts;
  const bool Ok = aggregateEdits(Reports, readFile, Out, Conflicts);
  for (const std::string& C : Conflicts) errs() << "conflict: " << C << "\n";

  if (Apply) {
    for (const auto& [File, Content] : Out) {
      std::ofstream O(resolvePath(File), std::ios::trunc | std::ios::binary);
      O << Content;
    }
  } else {
    for (const auto& [File, Content] : Out) {
      std::optional<std::string> Original = readFile(File);
      emitUnifiedDiff(Original ? *Original : "", Content, displayPath(File),
                      outs());
    }
  }
  return Ok ? 0 : 1;
}
