#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "clang/Tooling/CommonOptionsParser.h"
#include "cpp_formatting/embedded_clang_resource.h"
#include "cpp_formatting/lint_lib.h"
#include "cpp_formatting/naming_convention.h"
#include "cpp_formatting/output_mode.h"
#include "cpp_formatting/rename_variables_lib.h"
#include "cpp_formatting/tu_driver.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang::tooling;
using namespace llvm;

static cl::OptionCategory NormalizeVarsCategory("normalize-variables options");

static cl::opt<std::string> StyleOpt(
    "style",
    cl::desc(
        "Target naming style. One of: snake_case, _leading, trailing_, "
        "m_prefix, camelCase, UpperCamelCase, UPPER_SNAKE_CASE, kConstant"),
    cl::Required, cl::cat(NormalizeVarsCategory));

static cl::opt<std::string> ScopeOpt(
    "scope",
    cl::desc("Scope to rename: member, local, global, method, type, namespace, "
             "or a fine-grained one ({static,const,public,protected,private}"
             "_member, {static,const}_local, {static,const}_global)"),
    cl::init("member"), cl::cat(NormalizeVarsCategory));

static cl::opt<bool> InPlace("in-place",
                             cl::desc("Overwrite modified files in place"),
                             cl::cat(NormalizeVarsCategory));
static cl::alias InPlaceAlias("i", cl::desc("Alias for -in-place"),
                              cl::aliasopt(InPlace));

static cl::opt<bool> DebugTrace(
    "debug-trace",
    cl::desc(
        "Print, per TU, every rename target and every reference site "
        "found in the AST. Makes no modifications. Output goes to stderr."),
    cl::cat(NormalizeVarsCategory));

static cl::opt<bool> LintOpt(
    "lint",
    cl::desc("Analyze only: report violations without modifying any files. "
             "Exits 1 when violations are found."),
    cl::cat(NormalizeVarsCategory));

static cl::opt<std::string> FormatOpt(
    "format",
    cl::desc("Output format for --lint: text (default), sarif, or diff. "
             "A non-default value implies --lint."),
    cl::init("text"), cl::cat(NormalizeVarsCategory));

static cl::opt<bool> ReportRenameConflictsOpt(
    "report-rename-conflicts",
    cl::desc("List every rename that was skipped because its new name was "
             "already taken in the same scope. A one-line count is printed "
             "either way."),
    cl::init(false), cl::cat(NormalizeVarsCategory));

static cl::opt<std::string> EmitEditsOpt(
    "emit-edits",
    cl::desc("Emit structured edit records (+ a template-dependent-token "
             "resolution sidecar) as JSON to the given file, for cross-TU "
             "aggregation. Modifies no source files."),
    cl::init(""), cl::cat(NormalizeVarsCategory));

static cl::opt<unsigned> JobsOpt(
    "jobs",
    cl::desc("Number of translation units to parse in parallel. 0 (the "
             "default) uses every CPU; a larger value is capped at the CPU "
             "count. The result does not depend on this value. --debug-trace "
             "always runs serially."),
    cl::init(0), cl::cat(NormalizeVarsCategory));
static cl::alias JobsAlias("j", cl::desc("Alias for --jobs"), cl::Prefix,
                           cl::aliasopt(JobsOpt));

namespace {

// Build the FileSet from the list of source paths.  We resolve each path to
// its real (canonical, absolute) form so it can be matched against the paths
// that Clang's SourceManager records for included files.
FileSet buildFileSet(const std::vector<std::string>& SourcePaths) {
  FileSet FS;
  for (const auto& P : SourcePaths) {
    SmallString<256> Real;
    if (!sys::fs::real_path(P, Real))
      FS.insert(Real.str().str());
    else
      FS.insert(P);  // best-effort if real_path fails
  }
  return FS;
}

}  // namespace

auto main(int argc, const char** argv) -> int {
  auto ExpectedParser =
      CommonOptionsParser::create(argc, argv, NormalizeVarsCategory);
  if (!ExpectedParser) {
    llvm::errs() << ExpectedParser.takeError();
    return 1;
  }
  CommonOptionsParser& OptionsParser = ExpectedParser.get();

  NamingStyle style{};
  if (!parseNamingStyle(StyleOpt, style)) {
    llvm::errs() << "Unknown style '" << StyleOpt
                 << "'. Valid styles: snake_case, _leading, trailing_, "
                    "m_prefix, camelCase, UpperCamelCase, UPPER_SNAKE_CASE, "
                    "kConstant\n";
    return 1;
  }

  const std::optional<VariableScope> parsedScope = parseVariableScope(ScopeOpt);
  if (!parsedScope) {
    llvm::errs() << "Unknown scope '" << ScopeOpt
                 << "'. Valid scopes: " << variableScopeNames() << "\n";
    return 1;
  }
  const VariableScope scope = *parsedScope;

  const bool Lint = LintOpt || FormatOpt != "text";
  const bool Emit = !EmitEditsOpt.empty();
  if (Lint && InPlace) {
    llvm::errs() << "--lint/--format cannot be combined with --in-place\n";
    return 1;
  }
  if (Emit && (InPlace || Lint)) {
    llvm::errs() << "--emit-edits cannot be combined with --in-place or "
                    "--lint/--format\n";
    return 1;
  }
  if (FormatOpt != "text" && FormatOpt != "sarif" && FormatOpt != "diff") {
    llvm::errs() << "Unknown format '" << FormatOpt
                 << "'. Valid formats: text, sarif, diff\n";
    return 1;
  }

  const OutputMode mode = DebugTrace ? OutputMode::Debug
                          : Emit     ? OutputMode::Emit
                          : Lint     ? OutputMode::Lint
                          : InPlace  ? OutputMode::InPlace
                                     : OutputMode::DryRun;

  // Resolve source paths to real paths so shouldCollect() can match them
  // against the file entries recorded in Clang's SourceManager when an
  // included header is part of the source list.
  FileSet collectFrom = buildFileSet(OptionsParser.getSourcePathList());

  VariableRenameCallback cb = [style](std::string_view name,
                                      std::string& newName) -> bool {
    newName = renameToStyle(name, style);
    return newName != name;
  };

  const std::string ResourceDir = ensureClangResourceDir();

  std::unique_ptr<RenameActionFactory> factory =
      RenameAllInScope(std::move(cb), scope, mode, std::move(collectFrom));
  LintReport Report;
  if (Lint)
    factory->setLintReport(&Report, "normalize_variables/" +
                                        ScopeOpt.getValue() + "/" +
                                        StyleOpt.getValue());
  TUDriverOptions DriverOpts;
  DriverOpts.Jobs = JobsOpt;
  // The debug trace is printed from inside each TU; keep it readable.
  DriverOpts.ForceSerial = mode == OutputMode::Debug;
  int rc = runTranslationUnits(
      OptionsParser.getCompilations(), OptionsParser.getSourcePathList(),
      makeStandardArgumentsAdjuster(ResourceDir), DriverOpts, *factory);
  reportRenameConflicts(factory->conflicts(), ReportRenameConflictsOpt,
                        llvm::errs());
  if (Emit) {
    if (rc != 0) return rc;
    std::error_code EC;
    llvm::raw_fd_ostream OS(EmitEditsOpt, EC);
    if (EC) {
      llvm::errs() << "Cannot write '" << EmitEditsOpt << "': " << EC.message()
                   << "\n";
      return 1;
    }
    factory->emitEdits(OS);
    return rc;
  }
  if (Lint) {
    if (rc != 0) return rc;
    return emitLintResults(Report, factory->rewrites(), FormatOpt,
                           "normalize_variables");
  }
  factory->flush();
  return rc;
}
