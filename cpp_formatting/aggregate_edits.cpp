// aggregate_edits — merge per-TU edit-record files (as emitted by
// `--emit-edits`) into one repository-wide change: resolve template-dependent
// tokens across TUs, merge edits per file (dedup identical, flag conflicts),
// and either print a git-apply-able unified diff (default) or apply in place.
//
// File keys in the records are resolved against --root (default: cwd), so under
// `bazel run` --root is $BUILD_WORKSPACE_DIRECTORY.
//
// This is a thin CLI over lint_lib's runEditAggregation; `cpp_format
// --aggregate` exposes the same logic from the single published binary.

#include <string>
#include <vector>

#include "cpp_formatting/lint_lib.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"
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
static cl::list<std::string> RecordsFrom(
    "records-from",
    cl::desc("File listing record files, one per line (may repeat). A "
             "repository's worth of per-file records does not fit on a "
             "command line, so the Bazel rules pass them this way."),
    cl::value_desc("file"));
static cl::opt<bool> ReportRenameConflicts(
    "report-rename-conflicts",
    cl::desc("List every rename the run declined, with its site and reason. A "
             "one-line count is printed either way."));
static cl::list<std::string> Inputs(cl::Positional,
                                    cl::desc("<records.json>..."),
                                    cl::ZeroOrMore);

int main(int argc, char** argv) {
  cl::ParseCommandLineOptions(argc, argv,
                              "cpp_format per-TU edit-record aggregator\n");
  std::vector<std::string> InputPaths(Inputs.begin(), Inputs.end());
  for (const std::string& List : RecordsFrom)
    if (!appendRecordListFrom(List, InputPaths)) return 2;
  if (InputPaths.empty()) {
    llvm::errs() << "aggregate_edits: no record files given (positional, or "
                    "listed in --records-from=<file>)\n";
    return 2;
  }
  return runEditAggregation(InputPaths, Root, Apply, Check,
                            ReportRenameConflicts);
}
