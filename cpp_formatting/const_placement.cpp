#include <string>

#include "clang/Tooling/CommonOptionsParser.h"
#include "cpp_formatting/const_placement_lib.h"
#include "cpp_formatting/embedded_clang_resource.h"
#include "cpp_formatting/lint_lib.h"
#include "cpp_formatting/tu_driver.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang::tooling;
using namespace llvm;

static cl::OptionCategory ConstPlacementCategory("const_placement options");

static cl::opt<std::string> StyleOpt(
    "style",
    cl::desc("Which side of the type specifier cv-qualifiers go on: east "
             "(`int const x`) or west (`const int x`)."),
    cl::init("east"), cl::cat(ConstPlacementCategory));

static cl::opt<bool> InPlace("in-place",
                             cl::desc("Overwrite modified files in place"),
                             cl::cat(ConstPlacementCategory));
static cl::alias InPlaceAlias("i", cl::desc("Alias for -in-place"),
                              cl::aliasopt(InPlace));

static cl::opt<bool> LintOpt(
    "lint",
    cl::desc("Analyze only: report violations without modifying any files. "
             "Exits 1 when violations are found."),
    cl::cat(ConstPlacementCategory));

static cl::opt<std::string> FormatOpt(
    "format",
    cl::desc("Output format for --lint: text (default), sarif, or diff. "
             "A non-default value implies --lint."),
    cl::init("text"), cl::cat(ConstPlacementCategory));

static cl::opt<unsigned> JobsOpt(
    "jobs",
    cl::desc("Number of translation units to parse in parallel. 0 (the "
             "default) uses every CPU; a larger value is capped at the CPU "
             "count. The result does not depend on this value."),
    cl::init(0), cl::cat(ConstPlacementCategory));
static cl::alias JobsAlias("j", cl::desc("Alias for --jobs"), cl::Prefix,
                           cl::aliasopt(JobsOpt));

auto main(int argc, const char** argv) -> int {
  auto ExpectedParser =
      CommonOptionsParser::create(argc, argv, ConstPlacementCategory);
  if (!ExpectedParser) {
    llvm::errs() << ExpectedParser.takeError();
    return 1;
  }
  CommonOptionsParser& OptionsParser = ExpectedParser.get();

  ConstStyle Style{};
  if (!parseConstStyle(StyleOpt, Style)) {
    llvm::errs() << "Unknown style '" << StyleOpt
                 << "'. Valid styles: east, west\n";
    return 1;
  }

  const bool Lint = LintOpt || FormatOpt != "text";
  if (Lint && InPlace) {
    llvm::errs() << "--lint/--format cannot be combined with --in-place\n";
    return 1;
  }
  if (FormatOpt != "text" && FormatOpt != "sarif" && FormatOpt != "diff") {
    llvm::errs() << "Unknown format '" << FormatOpt
                 << "'. Valid formats: text, sarif, diff\n";
    return 1;
  }

  // The embedded Clang resource directory supplies the built-in headers
  // (stddef.h etc.) without a system Clang; makeStandardArgumentsAdjuster
  // skips it when the compile command already names one.
  const std::string ResourceDir = ensureClangResourceDir();

  const OutputMode Mode =
      Lint ? OutputMode::Lint
           : (InPlace ? OutputMode::InPlace : OutputMode::DryRun);
  const char* RuleId = constStyleRuleId(Style);
  ConstPlacementActionFactory Factory(Style, Mode);
  LintReport Report;
  if (Lint) Factory.setLintReport(&Report, RuleId);
  TUDriverOptions DriverOpts;
  DriverOpts.Jobs = JobsOpt;
  const int rc = runTranslationUnits(
      OptionsParser.getCompilations(), OptionsParser.getSourcePathList(),
      makeStandardArgumentsAdjuster(ResourceDir), DriverOpts, Factory);
  if (Lint) {
    if (rc != 0) return rc;
    return emitLintResults(Report, Factory.rewrites(), FormatOpt, RuleId);
  }
  // Commit whatever the TUs that parsed cleanly produced: a TU that failed
  // simply has nothing buffered.
  Factory.flush();
  return rc;
}
