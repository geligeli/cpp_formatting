#include <cstdio>

#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "cpp_formatting/naming_convention.h"
#include "cpp_formatting/rename_variables_lib.h"
#include "llvm/Support/CommandLine.h"
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
    cl::desc("Variable scope to rename: member, local, global, "
             "static_member, const_member, static_global, const_global"),
    cl::init("member"), cl::cat(NormalizeVarsCategory));

static cl::opt<bool> InPlace("in-place",
                             cl::desc("Overwrite modified files in place"),
                             cl::cat(NormalizeVarsCategory));
static cl::alias InPlaceAlias("i", cl::desc("Alias for -in-place"),
                              cl::aliasopt(InPlace));

namespace {

auto detectClangResourceDir() -> std::string {
  FILE* F = popen("clang -print-resource-dir 2>/dev/null", "r");
  if (!F) return {};
  char Buf[512];
  std::string Result;
  while (fgets(Buf, sizeof(Buf), F)) Result += Buf;
  pclose(F);
  while (!Result.empty() && (Result.back() == '\n' || Result.back() == '\r'))
    Result.pop_back();
  return Result;
}

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

  VariableScope scope{};
  if (ScopeOpt == "member") {
    scope = VariableScope::Member;
  } else if (ScopeOpt == "local") {
    scope = VariableScope::Local;
  } else if (ScopeOpt == "global") {
    scope = VariableScope::Global;
  } else if (ScopeOpt == "static_member") {
    scope = VariableScope::StaticMember;
  } else if (ScopeOpt == "const_member") {
    scope = VariableScope::ConstMember;
  } else if (ScopeOpt == "static_global") {
    scope = VariableScope::StaticGlobal;
  } else if (ScopeOpt == "const_global") {
    scope = VariableScope::ConstGlobal;
  } else {
    llvm::errs()
        << "Unknown scope '" << ScopeOpt
        << "'. Valid scopes: member, local, global, "
           "static_member, const_member, static_global, const_global\n";
    return 1;
  }

  const OutputMode mode = InPlace ? OutputMode::InPlace : OutputMode::DryRun;

  // Resolve source paths to real paths so shouldCollect() can match them
  // against the file entries recorded in Clang's SourceManager when an
  // included header is part of the source list.
  FileSet collectFrom = buildFileSet(OptionsParser.getSourcePathList());

  VariableRenameCallback cb = [style](std::string_view name,
                                      std::string& newName) -> bool {
    newName = renameToStyle(name, style);
    return newName != name;
  };

  ClangTool Tool(OptionsParser.getCompilations(),
                 orderSourcesForRename(OptionsParser.getSourcePathList()));

  Tool.appendArgumentsAdjuster(
      [](const std::vector<std::string>& Args, StringRef) {
        std::vector<std::string> Out;
        for (const auto& Arg : Args)
          if (Arg != "-fno-canonical-system-headers") Out.push_back(Arg);
        return Out;
      });

  const std::string ResourceDir = detectClangResourceDir();
  if (!ResourceDir.empty()) {
    Tool.appendArgumentsAdjuster(
        [ResourceDir](const std::vector<std::string>& Args, StringRef) {
          for (const auto& Arg : Args)
            if (StringRef(Arg).starts_with("-resource-dir")) return Args;
          std::vector<std::string> Adjusted = Args;
          Adjusted.insert(Adjusted.begin() + (Adjusted.empty() ? 0 : 1),
                          "-resource-dir=" + ResourceDir);
          return Adjusted;
        });
  }

  std::unique_ptr<RenameActionFactory> factory;
  switch (scope) {
    case VariableScope::Member:
      factory =
          RenameAllMemberVariables(std::move(cb), mode, std::move(collectFrom));
      break;
    case VariableScope::Local:
      factory =
          RenameAllLocalVariables(std::move(cb), mode, std::move(collectFrom));
      break;
    case VariableScope::Global:
      factory =
          RenameAllGlobalVariables(std::move(cb), mode, std::move(collectFrom));
      break;
    case VariableScope::StaticMember:
      factory = RenameAllStaticMemberVariables(std::move(cb), mode,
                                               std::move(collectFrom));
      break;
    case VariableScope::ConstMember:
      factory = RenameAllConstMemberVariables(std::move(cb), mode,
                                              std::move(collectFrom));
      break;
    case VariableScope::StaticGlobal:
      factory = RenameAllStaticGlobalVariables(std::move(cb), mode,
                                               std::move(collectFrom));
      break;
    case VariableScope::ConstGlobal:
      factory = RenameAllConstGlobalVariables(std::move(cb), mode,
                                              std::move(collectFrom));
      break;
  }
  int rc = Tool.run(factory.get());
  factory->flush();
  return rc;
}
