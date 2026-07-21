#include <string>
#include <vector>

#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "cpp_formatting/cpp_format_lib.h"
#include "cpp_formatting/embedded_clang_resource.h"
#include "cpp_formatting/lint_lib.h"
#include "cpp_formatting/naming_convention.h"
#include "cpp_formatting/rename_variables_lib.h"
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
    cl::desc("Scope for normalization: member, local, global, static_member, "
             "const_member, static_global, const_global, or method"),
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

static cl::opt<bool> LintOpt(
    "lint",
    cl::desc("Analyze only: report violations without modifying any files. "
             "Exits 1 when violations are found."),
    cl::cat(CppFormatCategory));

static cl::opt<std::string> FormatOpt(
    "format",
    cl::desc("Output format for --lint: text (default), sarif, or diff. "
             "A non-default value implies --lint."),
    cl::init("text"), cl::cat(CppFormatCategory));

static cl::opt<std::string> EmitEditsOpt(
    "emit-edits",
    cl::desc("Emit structured edit records (every rule's renames plus the "
             "trailing-return rewrites) and a template-dependent-token "
             "resolution sidecar as JSON to the given file, for cross-TU "
             "aggregation. Modifies no source files."),
    cl::init(""), cl::cat(CppFormatCategory));

// ---------------------------------------------------------------------------
// Helpers shared across passes
// ---------------------------------------------------------------------------

namespace {

// Aggregate mode: merge per-TU edit-record files (emitted by --emit-edits) into
// one repository change.  It shares nothing with the LibTooling parse pipeline
// (no compilation database, no `--` separator), so it is dispatched before
// CommonOptionsParser and parses its own small flag set by hand — reusing
// lint_lib's runEditAggregation, the exact code path of the standalone
// aggregate_edits binary.  This is what lets the single published cpp_format
// binary both emit and aggregate, so a Bazel integration needs only one binary.
auto runAggregate(int argc, const char** argv) -> int {
  bool Apply = false;
  bool Check = false;
  std::string Root;
  std::vector<std::string> Inputs;
  for (int i = 1; i < argc; ++i) {
    StringRef Arg(argv[i]);
    if (Arg == "--aggregate") continue;
    if (Arg == "--apply") {
      Apply = true;
    } else if (Arg == "--check") {
      Check = true;
    } else if (Arg == "--root") {
      if (i + 1 >= argc) {
        llvm::errs() << "--root requires a directory argument\n";
        return 2;
      }
      Root = argv[++i];
    } else if (Arg.starts_with("--root=")) {
      Root = Arg.drop_front(std::string("--root=").size()).str();
    } else if (Arg.starts_with("-")) {
      llvm::errs() << "unknown --aggregate flag '" << Arg
                   << "' (expected --apply, --check, or --root=<dir>)\n";
      return 2;
    } else {
      Inputs.push_back(Arg.str());
    }
  }
  if (Inputs.empty()) {
    llvm::errs() << "--aggregate requires one or more <records.json> inputs\n";
    return 2;
  }
  return runEditAggregation(Inputs, Root, Apply, Check);
}

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

}  // namespace

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

auto main(int argc, const char** argv) -> int {
  // Aggregate mode is a distinct sub-tool that does not use the LibTooling
  // compilation-database machinery; dispatch it before CommonOptionsParser.
  for (int i = 1; i < argc; ++i)
    if (StringRef(argv[i]) == "--aggregate") return runAggregate(argc, argv);

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

  const OutputMode mode = Emit      ? OutputMode::Emit
                          : Lint    ? OutputMode::Lint
                          : InPlace ? OutputMode::InPlace
                                    : OutputMode::DryRun;
  const std::string ResourceDir = ensureClangResourceDir();

  LintReport Report;

  // Build the rule list for the single combined pass: every
  // normalize_variables rule plus (optionally) trailing_return_types all run
  // on the same AST, so each TU is parsed exactly once.
  std::vector<NormalizeRule> Rules;
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
    } else if (rule.scope == "method") {
      scope = VariableScope::Method;
    } else {
      llvm::errs() << "Unknown scope '" << rule.scope
                   << "'. Valid scopes: member, local, global, "
                      "static_member, const_member, static_global, "
                      "const_global, method\n";
      return 1;
    }

    Rules.push_back(
        {scope,
         [style](std::string_view name, std::string& newName) -> bool {
           newName = renameToStyle(name, style);
           return newName != name;
         },
         "normalize_variables/" + rule.scope + "/" + rule.style});
  }

  ClangTool Tool(OptionsParser.getCompilations(),
                 orderSourcesForRename(SourcePaths));
  applyArgumentAdjusters(Tool, ResourceDir);

  CppFormatActionFactory Factory(std::move(Rules), cfg.trailing_return_types,
                                 "trailing_return_types", mode,
                                 buildFileSet(SourcePaths));
  if (Lint) Factory.setLintReport(&Report);
  if (int rc = Tool.run(&Factory)) return rc;

  if (Emit) {
    std::error_code EC;
    llvm::raw_fd_ostream OS(EmitEditsOpt, EC);
    if (EC) {
      llvm::errs() << "Cannot write '" << EmitEditsOpt << "': " << EC.message()
                   << "\n";
      return 1;
    }
    Factory.emitEdits(OS);
    return 0;
  }
  if (Lint)
    return emitLintResults(Report, Factory.rewrites(), FormatOpt, "cpp_format");
  Factory.flush();
  return 0;
}
