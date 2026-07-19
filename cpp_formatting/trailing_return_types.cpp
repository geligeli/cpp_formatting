#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "cpp_formatting/embedded_clang_resource.h"
#include "cpp_formatting/lint_lib.h"
#include "cpp_formatting/trailing_return_types_lib.h"
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

  ClangTool Tool(OptionsParser.getCompilations(),
                 OptionsParser.getSourcePathList());
  Tool.appendArgumentsAdjuster(
      [](const std::vector<std::string>& Args, StringRef Filename) {
        std::vector<std::string> AdjustedArgs;
        for (const auto& Arg : Args) {
          // Strip the offending Bazel/GCC flag
          if (Arg == "-fno-canonical-system-headers") {
            continue;
          }
          AdjustedArgs.push_back(Arg);
        }
        return AdjustedArgs;
      });
  // Automatically supply the host Clang resource directory so the tool can
  // find built-in headers (stddef.h etc.) without requiring the user to pass
  // --extra-arg=-resource-dir=... manually.  Skip if the compilation database
  // or the user already provides -resource-dir.
  std::string ResourceDir = ensureClangResourceDir();
  if (!ResourceDir.empty()) {
    Tool.appendArgumentsAdjuster(
        [ResourceDir](const std::vector<std::string>& Args, StringRef) {
          for (const auto& Arg : Args)
            if (StringRef(Arg).starts_with("-resource-dir"))
              return Args;  // already present, don't override
          // Insert after Args[0] (the compiler name), matching the convention
          // used by getInsertArgumentAdjuster(..., BEGIN).
          std::vector<std::string> Adjusted = Args;
          Adjusted.insert(Adjusted.begin() + (Adjusted.empty() ? 0 : 1),
                          "-resource-dir=" + ResourceDir);
          return Adjusted;
        });
  }

  const OutputMode Mode =
      Lint ? OutputMode::Lint
           : (InPlace ? OutputMode::InPlace : OutputMode::DryRun);
  TrailingReturnActionFactory Factory(Mode);
  LintReport Report;
  if (Lint) Factory.setLintReport(&Report, "trailing_return_types");
  int rc = Tool.run(&Factory);
  if (Lint) {
    if (rc != 0) return rc;
    return emitLintResults(Report, Factory.rewrites(), FormatOpt,
                           "trailing_return_types");
  }
  return rc;
}
