#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "clang/Tooling/CommonOptionsParser.h"
#include "cpp_formatting/const_placement_lib.h"
#include "cpp_formatting/cpp_format_lib.h"
#include "cpp_formatting/cpp_index_lib.h"
#include "cpp_formatting/cpp_index_merge.h"
#include "cpp_formatting/embedded_clang_resource.h"
#include "cpp_formatting/lint_lib.h"
#include "cpp_formatting/naming_convention.h"
#include "cpp_formatting/output_mode.h"
#include "cpp_formatting/proto_index_lib.h"
#include "cpp_formatting/rename_variables_lib.h"
#include "cpp_formatting/trailing_return_types_lib.h"
#include "cpp_formatting/tu_driver.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorOr.h"
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
  // `trailing_return_types: true` is the original spelling of
  // `return_types: trailing`, kept working because it appears in every config
  // written before the reverse direction existed.
  bool trailing_return_types = false;
  std::string return_types;     // "" | "trailing" | "leading"
  std::string const_placement;  // "" | "east" | "west"
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
    io.mapOptional("return_types", c.return_types, std::string());
    io.mapOptional("const_placement", c.const_placement, std::string());
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

static cl::opt<std::string> ReturnTypesOpt(
    "return-types",
    cl::desc("Return type style: trailing (`auto f() -> int`) or leading "
             "(`int f()`).  The reverse of --trailing-return-types; the two "
             "cannot be combined."),
    cl::init(""), cl::cat(CppFormatCategory));

static cl::opt<std::string> ConstPlacementOpt(
    "const-placement",
    cl::desc("Which side of the type specifier cv-qualifiers go on: east "
             "(`int const x`) or west (`const int x`)."),
    cl::init(""), cl::cat(CppFormatCategory));

static cl::opt<std::string> NormScopeOpt(
    "normalize-variables-scope",
    cl::desc("Scope for normalization: member, local, global, method, type, "
             "namespace, or a fine-grained one ({static,const,public,"
             "protected,private}_member, {static,const}_local, "
             "{static,const}_global)"),
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

static cl::opt<std::string> EmitIndexOpt(
    "emit-index",
    cl::desc("Index mode: parse every translation unit and write one "
             "cpp_index.IndexUnit (binary protobuf, see index.proto) to the "
             "given file -- every symbol's declarations, definitions and "
             "references, as byte ranges, in the main file and every owned "
             "file. Runs no formatting pass; combine the units with "
             "--merge-index."),
    cl::init(""), cl::cat(CppFormatCategory));

static cl::opt<std::string> OwnedFilesOpt(
    "owned-files",
    cl::desc("File holding newline-separated paths that this invocation owns "
             "in addition to the source paths. The listed files are not parsed "
             "as translation units; they only extend the file set, so a "
             "declaration in a dependency's header is renamed at its use sites "
             "here. Used by the Bazel aspect, which formats one target at a "
             "time but must rewrite uses of a dependency's declarations."),
    cl::init(""), cl::cat(CppFormatCategory));

static cl::opt<bool> ReportRenameConflictsOpt(
    "report-rename-conflicts",
    cl::desc("List every rename that was skipped because its new name was "
             "already taken in the same scope. A one-line count is printed "
             "either way."),
    cl::init(false), cl::cat(CppFormatCategory));

static cl::opt<unsigned> JobsOpt(
    "jobs",
    cl::desc("Number of translation units to parse in parallel. 0 (the "
             "default) uses every CPU; a larger value is capped at the CPU "
             "count. The result does not depend on this value."),
    cl::init(0), cl::cat(CppFormatCategory));
static cl::alias JobsAlias("j", cl::desc("Alias for --jobs"), cl::Prefix,
                           cl::aliasopt(JobsOpt));

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
  bool ReportSites = false;
  std::string Root;
  std::vector<std::string> Inputs;
  for (int i = 1; i < argc; ++i) {
    StringRef Arg(argv[i]);
    if (Arg == "--aggregate") continue;
    if (Arg == "--apply") {
      Apply = true;
    } else if (Arg == "--check") {
      Check = true;
    } else if (Arg == "--report-rename-conflicts") {
      ReportSites = true;
    } else if (Arg == "--root") {
      if (i + 1 >= argc) {
        llvm::errs() << "--root requires a directory argument\n";
        return 2;
      }
      Root = argv[++i];
    } else if (Arg.starts_with("--root=")) {
      Root = Arg.drop_front(std::string("--root=").size()).str();
    } else if (Arg == "--records-from") {
      // A list file: the Bazel rules and cpp_format.sh pass a repository's
      // worth of per-file records this way, which no command line holds.
      if (i + 1 >= argc) {
        llvm::errs() << "--records-from requires a file argument\n";
        return 2;
      }
      if (!appendRecordListFrom(argv[++i], Inputs)) return 2;
    } else if (Arg.starts_with("--records-from=")) {
      if (!appendRecordListFrom(
              Arg.drop_front(std::string("--records-from=").size()), Inputs))
        return 2;
    } else if (Arg.starts_with("-")) {
      llvm::errs() << "unknown --aggregate flag '" << Arg
                   << "' (expected --apply, --check, "
                      "--report-rename-conflicts, --root=<dir>, or "
                      "--records-from=<file>)\n";
      return 2;
    } else {
      Inputs.push_back(Arg.str());
    }
  }
  if (Inputs.empty()) {
    llvm::errs() << "--aggregate requires one or more <records.json> inputs "
                    "(positional, or listed in --records-from=<file>)\n";
    return 2;
  }
  return runEditAggregation(Inputs, Root, Apply, Check, ReportSites);
}

// Index modes: `--merge-index` unions per-TU index units (from --emit-index)
// into one repository index (on every CPU unless --jobs says otherwise, with
// a progress line on a terminal); `--dump-index` prints a unit or an index, or
// answers a (file, offset) lookup against it.  Like --aggregate, neither
// parses any C++, so both are dispatched before CommonOptionsParser and parse
// their own small flag sets by hand.
auto parseFormatFlag(StringRef Value, IndexFormat& Out) -> bool {
  if (parseIndexFormat(Value, Out)) return true;
  llvm::errs() << "Unknown index format '" << Value
               << "'. Valid formats: binary, text, json\n";
  return false;
}

auto runMergeIndexCli(int argc, const char** argv) -> int {
  std::string Output;
  IndexFormat Format = IndexFormat::Binary;
  MergeOptions Opts;
  std::vector<std::string> Inputs;
  for (int i = 1; i < argc; ++i) {
    StringRef Arg(argv[i]);
    if (Arg == "--merge-index") continue;
    if (Arg == "--progress") {
      Opts.Progress = MergeProgress::On;
      continue;
    }
    if (Arg == "--no-progress") {
      Opts.Progress = MergeProgress::Off;
      continue;
    }
    if (Arg.starts_with("--jobs=") || (Arg.starts_with("-j") && Arg != "-j")) {
      const StringRef N = Arg.starts_with("--jobs=")
                              ? Arg.drop_front(std::string("--jobs=").size())
                              : Arg.drop_front(2);
      if (N.getAsInteger(10, Opts.Jobs)) {
        llvm::errs() << "bad thread count '" << N << "'\n";
        return 2;
      }
      continue;
    }
    if (Arg == "--output" || Arg == "-o") {
      if (i + 1 >= argc) {
        llvm::errs() << Arg << " requires a file argument\n";
        return 2;
      }
      Output = argv[++i];
    } else if (Arg.starts_with("--output=")) {
      Output = Arg.drop_front(std::string("--output=").size()).str();
    } else if (Arg.starts_with("--format=")) {
      if (!parseFormatFlag(Arg.drop_front(std::string("--format=").size()),
                           Format))
        return 2;
    } else if (Arg == "--records-from") {
      if (i + 1 >= argc) {
        llvm::errs() << "--records-from requires a file argument\n";
        return 2;
      }
      if (!appendRecordListFrom(argv[++i], Inputs)) return 2;
    } else if (Arg.starts_with("--records-from=")) {
      if (!appendRecordListFrom(
              Arg.drop_front(std::string("--records-from=").size()), Inputs))
        return 2;
    } else if (Arg.starts_with("-")) {
      llvm::errs() << "unknown --merge-index flag '" << Arg
                   << "' (expected --output=<file>, --format=<binary|text|"
                      "json>, --records-from=<file>, --jobs=<N> or "
                      "--[no-]progress)\n";
      return 2;
    } else {
      Inputs.push_back(Arg.str());
    }
  }
  if (Output.empty()) {
    llvm::errs() << "--merge-index requires --output=<file>\n";
    return 2;
  }
  if (Inputs.empty()) {
    llvm::errs() << "--merge-index requires one or more <unit.pb> inputs "
                    "(positional, or listed in --records-from=<file>)\n";
    return 2;
  }
  return runMergeIndex(Inputs, Output, Format, Opts);
}

auto runDumpIndexCli(int argc, const char** argv) -> int {
  std::string Input;
  IndexFormat Format = IndexFormat::Text;
  std::optional<std::pair<std::string, uint32_t>> Lookup;
  for (int i = 1; i < argc; ++i) {
    StringRef Arg(argv[i]);
    if (Arg == "--dump-index") continue;
    if (Arg.starts_with("--format=")) {
      if (!parseFormatFlag(Arg.drop_front(std::string("--format=").size()),
                           Format))
        return 2;
    } else if (Arg.starts_with("--lookup=")) {
      // <path>:<offset>; the last colon separates them, so a path may hold
      // one.
      StringRef Spec = Arg.drop_front(std::string("--lookup=").size());
      const size_t Colon = Spec.rfind(':');
      uint32_t Offset = 0;
      if (Colon == StringRef::npos || Colon == 0 ||
          Spec.drop_front(Colon + 1).getAsInteger(10, Offset)) {
        llvm::errs() << "--lookup expects <path>:<byte offset>, got '" << Spec
                     << "'\n";
        return 2;
      }
      Lookup = std::make_pair(Spec.take_front(Colon).str(), Offset);
    } else if (Arg.starts_with("-")) {
      llvm::errs() << "unknown --dump-index flag '" << Arg
                   << "' (expected --format=<binary|text|json> or "
                      "--lookup=<path>:<offset>)\n";
      return 2;
    } else if (!Input.empty()) {
      llvm::errs() << "--dump-index takes exactly one <index.pb> input\n";
      return 2;
    } else {
      Input = Arg.str();
    }
  }
  if (Input.empty()) {
    llvm::errs() << "--dump-index requires an <index.pb> input\n";
    return 2;
  }
  return runDumpIndex(Input, Format, Lookup, llvm::outs());
}

void insertRealPath(FileSet& FS, const std::string& P) {
  SmallString<256> Real;
  if (!sys::fs::real_path(P, Real))
    FS.insert(Real.str().str());
  else
    FS.insert(P);
}

FileSet buildFileSet(const std::vector<std::string>& SourcePaths) {
  FileSet FS;
  for (const auto& P : SourcePaths) insertRealPath(FS, P);
  return FS;
}

// Adds every non-blank line of \p ListFile to \p FS.  Returns false (after
// printing a diagnostic) when the file cannot be read.  A path that does not
// resolve is kept verbatim, which simply never matches a real file — the same
// benign outcome as listing a file this TU does not include.
bool addOwnedFilesFrom(StringRef ListFile, FileSet& FS) {
  auto BufOrErr = MemoryBuffer::getFile(ListFile);
  if (!BufOrErr) {
    llvm::errs() << "Cannot open owned-files list '" << ListFile
                 << "': " << BufOrErr.getError().message() << "\n";
    return false;
  }
  SmallVector<StringRef, 64> Lines;
  (*BufOrErr)->getBuffer().split(Lines, '\n', /*MaxSplit=*/-1,
                                 /*KeepEmpty=*/false);
  for (StringRef Line : Lines) {
    StringRef P = Line.trim();
    if (!P.empty()) insertRealPath(FS, P.str());
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

auto main(int argc, const char** argv) -> int {
  // Aggregate and the index sub-tools do not use the LibTooling
  // compilation-database machinery; dispatch them before CommonOptionsParser.
  for (int i = 1; i < argc; ++i) {
    const StringRef Arg(argv[i]);
    if (Arg == "--aggregate") return runAggregate(argc, argv);
    if (Arg == "--merge-index") return runMergeIndexCli(argc, argv);
    if (Arg == "--dump-index") return runDumpIndexCli(argc, argv);
    // The protobuf producer of the index: a `.proto` is not C++ either.
    if (Arg == "--emit-proto-index" || Arg.starts_with("--emit-proto-index="))
      return runEmitProtoIndex(std::vector<std::string>(argv + 1, argv + argc));
  }

  auto ExpectedParser =
      CommonOptionsParser::create(argc, argv, CppFormatCategory);
  if (!ExpectedParser) {
    llvm::errs() << ExpectedParser.takeError();
    return 1;
  }
  CommonOptionsParser& OptionsParser = ExpectedParser.get();
  const std::vector<std::string>& SourcePaths =
      OptionsParser.getSourcePathList();

  // Index mode: parse every TU and write one IndexUnit.  No formatting pass
  // runs, so no config is read and none of the rewrite/lint flags applies --
  // it is handled before the config block, whose "nothing to do" check would
  // otherwise reject a config-less run.
  if (!EmitIndexOpt.empty()) {
    if (!ConfigFile.empty() || InPlace || LintOpt || FormatOpt != "text" ||
        !EmitEditsOpt.empty() || TrailingReturnOpt || !ReturnTypesOpt.empty() ||
        !ConstPlacementOpt.empty() || !NormScopeOpt.empty() ||
        !NormStyleOpt.empty()) {
      llvm::errs() << "--emit-index runs no formatting pass and cannot be "
                      "combined with --config, a rule flag, --in-place, "
                      "--lint/--format or --emit-edits\n";
      return 1;
    }
    // The owned set decides which files' occurrences are recorded besides
    // the main file's: every source given, plus --owned-files.
    FileSet Files = buildFileSet(SourcePaths);
    if (!OwnedFilesOpt.empty() && !addOwnedFilesFrom(OwnedFilesOpt, Files))
      return 1;
    const std::string ResourceDir = ensureClangResourceDir();
    IndexActionFactory Factory(std::move(Files));
    TUDriverOptions DriverOpts;
    DriverOpts.Jobs = JobsOpt;
    if (int rc = runTranslationUnits(
            OptionsParser.getCompilations(), SourcePaths,
            makeStandardArgumentsAdjuster(ResourceDir), DriverOpts, Factory))
      return rc;
    return Factory.writeUnit(EmitIndexOpt) ? 0 : 1;
  }

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
    cfg.return_types = ReturnTypesOpt.getValue();
    cfg.const_placement = ConstPlacementOpt.getValue();
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
    if (!cfg.trailing_return_types && cfg.return_types.empty() &&
        cfg.const_placement.empty() && cfg.normalize_variables.empty()) {
      llvm::errs()
          << "Nothing to do. Provide --config=<file>, "
             "--trailing-return-types, --return-types=<trailing|leading>, "
             "--const-placement=<east|west>, or both "
             "--normalize-variables-scope and "
             "--normalize-variables-style.\n";
      return 1;
    }
  }

  // The two directions rewrite the same declarations against each other, so
  // the config cannot ask for both.  Making the direction one value rather
  // than two booleans is what keeps that unrepresentable.
  std::optional<ReturnTypeStyle> ReturnStyle;
  if (!cfg.return_types.empty()) {
    if (cfg.trailing_return_types) {
      llvm::errs() << "Set return_types or trailing_return_types, not both "
                      "(trailing_return_types: true means return_types: "
                      "trailing).\n";
      return 1;
    }
    if (cfg.return_types == "trailing") {
      ReturnStyle = ReturnTypeStyle::Trailing;
    } else if (cfg.return_types == "leading") {
      ReturnStyle = ReturnTypeStyle::Leading;
    } else {
      llvm::errs() << "Unknown return_types '" << cfg.return_types
                   << "'. Valid values: trailing, leading\n";
      return 1;
    }
  } else if (cfg.trailing_return_types) {
    ReturnStyle = ReturnTypeStyle::Trailing;
  }
  const char* ReturnRuleId = ReturnStyle == ReturnTypeStyle::Leading
                                 ? "leading_return_types"
                                 : "trailing_return_types";

  // Like the two return-type directions, east and west rewrite the same
  // declarations against each other, so the style is one value and the config
  // names it once.
  std::optional<ConstStyle> ConstPlacement;
  if (!cfg.const_placement.empty()) {
    ConstStyle Parsed{};
    if (!parseConstStyle(cfg.const_placement, Parsed)) {
      llvm::errs() << "Unknown const_placement '" << cfg.const_placement
                   << "'. Valid values: east, west\n";
      return 1;
    }
    ConstPlacement = Parsed;
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
  // (scope, style) per rule, kept alongside Rules so the combination can be
  // checked once every rule has parsed.
  struct ParsedRule {
    VariableScope Scope;
    NamingStyle Style;
    std::string Spelling;  ///< "member/snake_case", for the diagnostics
  };
  std::vector<ParsedRule> Parsed;
  for (const auto& rule : cfg.normalize_variables) {
    NamingStyle style{};
    if (!parseNamingStyle(rule.style, style)) {
      llvm::errs() << "Unknown style '" << rule.style
                   << "'. Valid styles: snake_case, _leading, trailing_, "
                      "m_prefix, camelCase, UpperCamelCase, "
                      "UPPER_SNAKE_CASE, kConstant\n";
      return 1;
    }

    const std::optional<VariableScope> parsedScope =
        parseVariableScope(rule.scope);
    if (!parsedScope) {
      llvm::errs() << "Unknown scope '" << rule.scope
                   << "'. Valid scopes: " << variableScopeNames() << "\n";
      return 1;
    }
    const VariableScope scope = *parsedScope;

    Parsed.push_back({scope, style, rule.scope + "/" + rule.style});
    Rules.push_back(
        {scope,
         [style](std::string_view name, std::string& newName) -> bool {
           newName = renameToStyle(name, style);
           return newName != name;
         },
         "normalize_variables/" + rule.scope + "/" + rule.style});
  }

  // Rules may overlap: a declaration several of them match is renamed by the
  // most specific one (scopeSpecificity() -- `local` plus `const_local` is how
  // a ruleset states a convention and its exception).  What cannot be applied
  // is the same scope twice: neither rule is more specific, both would rewrite
  // the same bytes, and the second rewrite lands on text the first replaced.
  // Two rules that can merely produce the same *name* in one scope are fine --
  // the collision resolution in runRenameRulesOnAST sees every rule's
  // candidates together, and the first rule in the list keeps a contested
  // name (the other is reported as a skip).
  for (size_t I = 0; I < Parsed.size(); ++I) {
    for (size_t J = I + 1; J < Parsed.size(); ++J) {
      const ParsedRule& A = Parsed[I];
      const ParsedRule& B = Parsed[J];
      if (A.Scope == B.Scope) {
        llvm::errs()
            << "Rules " << (I + 1) << " (" << A.Spelling << ") and " << (J + 1)
            << " (" << B.Spelling
            << ") name the same scope, so every declaration in it "
               "would be renamed twice. Keep one rule per scope; a "
               "fine-grained scope next to its broad one (const_local "
               "with local, private_member with member) is fine -- the "
               "more specific rule wins.\n";
        return 1;
      }
    }
  }

  // The file set decides which declarations may be renamed; the source list
  // decides which files are parsed as translation units.  --owned-files widens
  // the former without widening the latter, so a use of a dependency's
  // declaration is rewritten here while the declaration itself is rewritten by
  // whichever invocation actually owns (and parses) that file.
  FileSet Files = buildFileSet(SourcePaths);
  if (!OwnedFilesOpt.empty() && !addOwnedFilesFrom(OwnedFilesOpt, Files))
    return 1;

  CppFormatActionFactory Factory(std::move(Rules), ConstPlacement, ReturnStyle,
                                 ReturnRuleId, mode, std::move(Files));
  if (Lint) Factory.setLintReport(&Report);
  TUDriverOptions DriverOpts;
  DriverOpts.Jobs = JobsOpt;
  if (int rc = runTranslationUnits(OptionsParser.getCompilations(), SourcePaths,
                                   makeStandardArgumentsAdjuster(ResourceDir),
                                   DriverOpts, Factory))
    return rc;

  // Renames whose new name was already taken are skipped rather than applied:
  // renaming into an occupied name does not compile, or silently rebinds uses.
  reportRenameConflicts(Factory.conflicts(), ReportRenameConflictsOpt,
                        llvm::errs());

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
