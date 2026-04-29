#include <string>
#include <vector>

#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "cpp_formatting/embedded_clang_resource.h"
#include "cpp_formatting/naming_convention.h"
#include "cpp_formatting/rename_variables_lib.h"
#include "cpp_formatting/trailing_return_types_lib.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/YAMLTraits.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang::tooling;
using namespace llvm;

// ---------------------------------------------------------------------------
// Config structs + YAML traits
// ---------------------------------------------------------------------------

struct NormalizeVarsRule {
  std::string scope;  // "member" | "local" | "global"
  std::string style;  // naming style keyword
};

struct Config {
  bool trailing_return_types = false;
  std::vector<NormalizeVarsRule> normalize_variables;
};

LLVM_YAML_IS_SEQUENCE_VECTOR(NormalizeVarsRule)

namespace llvm {
namespace yaml {

template <>
struct MappingTraits<NormalizeVarsRule> {
  static void mapping(IO& io, NormalizeVarsRule& r) {
    io.mapRequired("scope", r.scope);
    io.mapRequired("style", r.style);
  }
};

template <>
struct MappingTraits<Config> {
  static void mapping(IO& io, Config& c) {
    io.mapOptional("trailing_return_types", c.trailing_return_types, false);
    io.mapOptional("normalize_variables", c.normalize_variables);
  }
};

}  // namespace yaml
}  // namespace llvm

// ---------------------------------------------------------------------------
// CLI options
// ---------------------------------------------------------------------------

static cl::OptionCategory CppFormatCategory("cpp_format options");

static cl::opt<std::string> ConfigFile(
    "config",
    cl::desc("YAML configuration file specifying which passes to run"),
    cl::init(""), cl::cat(CppFormatCategory));

static cl::opt<bool> TrailingReturnOpt(
    "trailing-return-types",
    cl::desc("Convert functions to trailing return type syntax"),
    cl::cat(CppFormatCategory));

static cl::opt<std::string> NormScopeOpt(
    "normalize-variables-scope",
    cl::desc("Scope for variable normalization: member, local, or global"),
    cl::init(""), cl::cat(CppFormatCategory));

static cl::opt<std::string> NormStyleOpt(
    "normalize-variables-style",
    cl::desc("Target naming style: snake_case, _leading, trailing_, m_prefix, "
             "camelCase, UpperCamelCase, UPPER_SNAKE_CASE, kConstant"),
    cl::init(""), cl::cat(CppFormatCategory));

static cl::opt<bool> InPlace("in-place",
                             cl::desc("Overwrite modified files in place"),
                             cl::cat(CppFormatCategory));
static cl::alias InPlaceAlias("i", cl::desc("Alias for --in-place"),
                              cl::aliasopt(InPlace));

// ---------------------------------------------------------------------------
// Helpers shared across passes
// ---------------------------------------------------------------------------

namespace {

FileSet buildFileSet(const std::vector<std::string>& SourcePaths) {
  FileSet FS;
  for (const auto& P : SourcePaths) {
    SmallString<256> Real;
    if (!sys::fs::real_path(P, Real))
      FS.insert(Real.str().str());
    else
      FS.insert(P);
  }
  return FS;
}

void applyArgumentAdjusters(ClangTool& Tool, const std::string& ResourceDir) {
  Tool.appendArgumentsAdjuster(
      [](const std::vector<std::string>& Args, StringRef) {
        std::vector<std::string> Out;
        for (const auto& Arg : Args)
          if (Arg != "-fno-canonical-system-headers") Out.push_back(Arg);
        return Out;
      });
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
}

struct TrailingReturnFactory : FrontendActionFactory {
  OutputMode Mode;
  explicit TrailingReturnFactory(OutputMode M) : Mode(M) {}
  auto create() -> std::unique_ptr<clang::FrontendAction> override {
    return std::make_unique<TrailingReturnTypesAction>(Mode);
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

auto main(int argc, const char** argv) -> int {
  auto ExpectedParser =
      CommonOptionsParser::create(argc, argv, CppFormatCategory);
  if (!ExpectedParser) {
    llvm::errs() << ExpectedParser.takeError();
    return 1;
  }
  CommonOptionsParser& OptionsParser = ExpectedParser.get();
  const std::vector<std::string>& SourcePaths =
      OptionsParser.getSourcePathList();

  // Build config: from YAML file if --config given, else from CLI flags.
  Config cfg;
  if (!ConfigFile.empty()) {
    auto BufOrErr = MemoryBuffer::getFile(ConfigFile);
    if (!BufOrErr) {
      llvm::errs() << "Cannot open config file '" << ConfigFile
                   << "': " << BufOrErr.getError().message() << "\n";
      return 1;
    }
    yaml::Input YIn((*BufOrErr)->getBuffer());
    YIn >> cfg;
    if (YIn.error()) {
      llvm::errs() << "Failed to parse config file '" << ConfigFile << "'\n";
      return 1;
    }
  } else {
    cfg.trailing_return_types = TrailingReturnOpt.getValue();
    const bool hasScope = !NormScopeOpt.empty();
    const bool hasStyle = !NormStyleOpt.empty();
    if (hasScope && hasStyle) {
      cfg.normalize_variables.push_back(
          {NormScopeOpt.getValue(), NormStyleOpt.getValue()});
    } else if (hasScope || hasStyle) {
      llvm::errs()
          << "--normalize-variables-scope and --normalize-variables-style "
             "must be specified together\n";
      return 1;
    }
    if (!cfg.trailing_return_types && cfg.normalize_variables.empty()) {
      llvm::errs()
          << "Nothing to do. Provide --config=<file>, "
             "--trailing-return-types, or both "
             "--normalize-variables-scope and --normalize-variables-style.\n";
      return 1;
    }
  }

  const OutputMode mode = InPlace ? OutputMode::InPlace : OutputMode::DryRun;
  const std::string ResourceDir = ensureClangResourceDir();

  // Pass 1+: normalize_variables rules applied in order.
  for (const auto& rule : cfg.normalize_variables) {
    NamingStyle style{};
    if (!parseNamingStyle(rule.style, style)) {
      llvm::errs() << "Unknown style '" << rule.style
                   << "'. Valid styles: snake_case, _leading, trailing_, "
                      "m_prefix, camelCase, UpperCamelCase, "
                      "UPPER_SNAKE_CASE, kConstant\n";
      return 1;
    }

    VariableScope scope{};
    if (rule.scope == "member") {
      scope = VariableScope::Member;
    } else if (rule.scope == "local") {
      scope = VariableScope::Local;
    } else if (rule.scope == "global") {
      scope = VariableScope::Global;
    } else if (rule.scope == "static_member") {
      scope = VariableScope::StaticMember;
    } else if (rule.scope == "const_member") {
      scope = VariableScope::ConstMember;
    } else if (rule.scope == "static_global") {
      scope = VariableScope::StaticGlobal;
    } else if (rule.scope == "const_global") {
      scope = VariableScope::ConstGlobal;
    } else {
      llvm::errs() << "Unknown scope '" << rule.scope
                   << "'. Valid scopes: member, local, global, "
                      "static_member, const_member, static_global, "
                      "const_global\n";
      return 1;
    }

    FileSet collectFrom = buildFileSet(SourcePaths);
    VariableRenameCallback cb = [style](std::string_view name,
                                        std::string& newName) -> bool {
      newName = renameToStyle(name, style);
      return newName != name;
    };

    ClangTool Tool(OptionsParser.getCompilations(),
                   orderSourcesForRename(SourcePaths));
    applyArgumentAdjusters(Tool, ResourceDir);

    std::unique_ptr<RenameActionFactory> factory;
    switch (scope) {
      case VariableScope::Member:
        factory = RenameAllMemberVariables(std::move(cb), mode,
                                           std::move(collectFrom));
        break;
      case VariableScope::Local:
        factory = RenameAllLocalVariables(std::move(cb), mode,
                                          std::move(collectFrom));
        break;
      case VariableScope::Global:
        factory = RenameAllGlobalVariables(std::move(cb), mode,
                                           std::move(collectFrom));
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
    if (int rc = Tool.run(factory.get())) return rc;
    factory->flush();
  }

  // Final pass: trailing_return_types (runs after variable renames so it sees
  // already-renamed identifiers, though in practice these passes are
  // independent).
  if (cfg.trailing_return_types) {
    ClangTool Tool(OptionsParser.getCompilations(), SourcePaths);
    applyArgumentAdjusters(Tool, ResourceDir);
    TrailingReturnFactory Factory(mode);
    if (int rc = Tool.run(&Factory)) return rc;
  }

  return 0;
}
