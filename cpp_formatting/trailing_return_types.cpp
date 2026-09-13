#include "clang/Tooling/CommonOptionsParser.h"
#include "cpp_formatting/embedded_clang_resource.h"
#include "cpp_formatting/lint_lib.h"
#include "cpp_formatting/trailing_return_types_lib.h"
#include "cpp_formatting/tu_driver.h"
#include "llvm/Support/CommandLine.h"

using namespace clang::tooling;
using namespace llvm;

static cl::OptionCategory TrailingReturnTypesCategory(
    "trailing_return_types options");
static cl::opt<bool> InPlace("in-place",
                             cl::desc("Overwrite modified files in place"),
                             cl::cat(TrailingReturnTypesCategory));
static cl::alias InPlaceAlias("i", cl::desc("Alias for -in-place"),
                              cl::aliasopt(InPlace));

static cl::opt<bool> Reverse(
    "reverse",
    cl::desc("Rewrite the other way: move trailing return types back to "
             "leading ones (`auto f() -> int` becomes `int f()`).  Many "
             "declarations cannot move and are left alone."),
    cl::cat(TrailingReturnTypesCategory));

static cl::opt<bool> LintOpt(
    "lint",
    cl::desc("Analyze only: report violations without modifying any files. "
             "Exits 1 when violations are found."),
    cl::cat(TrailingReturnTypesCategory));

static cl::opt<std::string> FormatOpt(
    "format",
    cl::desc("Output format for --lint: text (default), sarif, or diff. "
             "A non-default value implies --lint."),
    cl::init("text"), cl::cat(TrailingReturnTypesCategory));

static cl::opt<unsigned> JobsOpt(
    "jobs",
    cl::desc("Number of translation units to parse in parallel. 0 (the "
             "default) uses every CPU; a larger value is capped at the CPU "
             "count. The result does not depend on this value."),
    cl::init(0), cl::cat(TrailingReturnTypesCategory));
static cl::alias JobsAlias("j", cl::desc("Alias for --jobs"), cl::Prefix,
                           cl::aliasopt(JobsOpt));

auto main(int argc, const char** argv) -> int {
  auto ExpectedParser =
      CommonOptionsParser::create(argc, argv, TrailingReturnTypesCategory);
  if (!ExpectedParser) {
    llvm::errs() << ExpectedParser.takeError();
    return 1;
  }
  CommonOptionsParser& OptionsParser = ExpectedParser.get();

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
  const ReturnTypeStyle Style =
      Reverse ? ReturnTypeStyle::Leading : ReturnTypeStyle::Trailing;
  const char* RuleId =
      Reverse ? "leading_return_types" : "trailing_return_types";
  TrailingReturnActionFactory Factory(Mode, Style);
  LintReport Report;
  if (Lint) Factory.setLintReport(&Report, RuleId);
  TUDriverOptions DriverOpts;
  DriverOpts.Jobs = JobsOpt;
  int rc = runTranslationUnits(
      OptionsParser.getCompilations(), OptionsParser.getSourcePathList(),
      makeStandardArgumentsAdjuster(ResourceDir), DriverOpts, Factory);
  if (Lint) {
    if (rc != 0) return rc;
    return emitLintResults(Report, Factory.rewrites(), FormatOpt, RuleId);
  }
  // Commit whatever the TUs that parsed cleanly produced, as before: a TU
  // that failed simply has nothing buffered.
  Factory.flush();
  return rc;
}
