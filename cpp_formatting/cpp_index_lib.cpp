#include "cpp_formatting/cpp_index_lib.h"

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/Basic/FileEntry.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Index/IndexDataConsumer.h"
#include "clang/Index/IndexSymbol.h"
#include "clang/Index/IndexingAction.h"
#include "clang/Index/USRGeneration.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Tooling/Tooling.h"
#include "cpp_formatting/cpp_index_merge.h"
#include "cpp_formatting/lint_lib.h"
#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/io/zero_copy_stream_impl_lite.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using clang::index::SymbolRole;
using clang::index::SymbolRoleSet;

auto makeIndexingOptions() -> index::IndexingOptions {
  index::IndexingOptions Opts;
  Opts.SystemSymbolFilter = index::IndexingOptions::SystemSymbolFilterKind::All;
  Opts.IndexFunctionLocals = true;
  Opts.IndexParametersInDeclarations = true;
  Opts.IndexTemplateParameters = true;
  // Instantiations are reported against the pattern they came from; walking
  // them too would report the pattern's tokens once per instantiation.
  Opts.IndexImplicitInstantiation = false;
  Opts.IndexMacros = true;
  return Opts;
}

// ---------------------------------------------------------------------------
// Enum mapping: explicit switches, so a Clang enum change is a compile error
// here rather than a silent change of the format.
// ---------------------------------------------------------------------------

namespace {

auto mapKind(index::SymbolKind K) -> cpp_index::SymbolKind {
  using index::SymbolKind;
  switch (K) {
    case SymbolKind::Unknown:
      return cpp_index::SYMBOL_KIND_UNSPECIFIED;
    case SymbolKind::Module:
      return cpp_index::MODULE;
    case SymbolKind::Namespace:
      return cpp_index::NAMESPACE;
    case SymbolKind::NamespaceAlias:
      return cpp_index::NAMESPACE_ALIAS;
    case SymbolKind::Macro:
      return cpp_index::MACRO;
    case SymbolKind::Enum:
      return cpp_index::ENUM;
    case SymbolKind::Struct:
      return cpp_index::STRUCT;
    case SymbolKind::Class:
      return cpp_index::CLASS;
    case SymbolKind::Protocol:
      return cpp_index::PROTOCOL;
    case SymbolKind::Extension:
      return cpp_index::EXTENSION;
    case SymbolKind::Union:
      return cpp_index::UNION;
    case SymbolKind::TypeAlias:
      return cpp_index::TYPE_ALIAS;
    case SymbolKind::Function:
      return cpp_index::FUNCTION;
    case SymbolKind::Variable:
      return cpp_index::VARIABLE;
    case SymbolKind::Field:
      return cpp_index::FIELD;
    case SymbolKind::EnumConstant:
      return cpp_index::ENUM_CONSTANT;
    case SymbolKind::InstanceMethod:
      return cpp_index::INSTANCE_METHOD;
    case SymbolKind::ClassMethod:
      return cpp_index::CLASS_METHOD;
    case SymbolKind::StaticMethod:
      return cpp_index::STATIC_METHOD;
    case SymbolKind::InstanceProperty:
      return cpp_index::INSTANCE_PROPERTY;
    case SymbolKind::ClassProperty:
      return cpp_index::CLASS_PROPERTY;
    case SymbolKind::StaticProperty:
      return cpp_index::STATIC_PROPERTY;
    case SymbolKind::Constructor:
      return cpp_index::CONSTRUCTOR;
    case SymbolKind::Destructor:
      return cpp_index::DESTRUCTOR;
    case SymbolKind::ConversionFunction:
      return cpp_index::CONVERSION_FUNCTION;
    case SymbolKind::Parameter:
      return cpp_index::PARAMETER;
    case SymbolKind::Using:
      return cpp_index::USING;
    case SymbolKind::TemplateTypeParm:
      return cpp_index::TEMPLATE_TYPE_PARM;
    case SymbolKind::TemplateTemplateParm:
      return cpp_index::TEMPLATE_TEMPLATE_PARM;
    case SymbolKind::NonTypeTemplateParm:
      return cpp_index::NON_TYPE_TEMPLATE_PARM;
    case SymbolKind::Concept:
      return cpp_index::CONCEPT;
  }
  return cpp_index::SYMBOL_KIND_UNSPECIFIED;
}

auto mapSubKind(index::SymbolSubKind K) -> cpp_index::SymbolSubKind {
  using index::SymbolSubKind;
  switch (K) {
    case SymbolSubKind::None:
      return cpp_index::SUB_KIND_NONE;
    case SymbolSubKind::CXXCopyConstructor:
      return cpp_index::CXX_COPY_CONSTRUCTOR;
    case SymbolSubKind::CXXMoveConstructor:
      return cpp_index::CXX_MOVE_CONSTRUCTOR;
    case SymbolSubKind::AccessorGetter:
      return cpp_index::ACCESSOR_GETTER;
    case SymbolSubKind::AccessorSetter:
      return cpp_index::ACCESSOR_SETTER;
    case SymbolSubKind::UsingTypename:
      return cpp_index::USING_TYPENAME;
    case SymbolSubKind::UsingValue:
      return cpp_index::USING_VALUE;
    case SymbolSubKind::UsingEnum:
      return cpp_index::USING_ENUM;
  }
  return cpp_index::SUB_KIND_NONE;
}

auto mapLanguage(index::SymbolLanguage L) -> cpp_index::Language {
  using index::SymbolLanguage;
  switch (L) {
    case SymbolLanguage::C:
      return cpp_index::C;
    case SymbolLanguage::ObjC:
      return cpp_index::OBJC;
    case SymbolLanguage::CXX:
      return cpp_index::CXX;
    case SymbolLanguage::Swift:
      return cpp_index::LANGUAGE_UNSPECIFIED;
  }
  return cpp_index::LANGUAGE_UNSPECIFIED;
}

auto mapProperties(index::SymbolPropertySet Props) -> uint32_t {
  using index::SymbolProperty;
  uint32_t Out = 0;
  index::applyForEachSymbolProperty(Props, [&](SymbolProperty P) {
    switch (P) {
      case SymbolProperty::Generic:
        Out |= cpp_index::GENERIC;
        break;
      case SymbolProperty::TemplatePartialSpecialization:
        Out |= cpp_index::TEMPLATE_PARTIAL_SPECIALIZATION;
        break;
      case SymbolProperty::TemplateSpecialization:
        Out |= cpp_index::TEMPLATE_SPECIALIZATION;
        break;
      case SymbolProperty::UnitTest:
        Out |= cpp_index::UNIT_TEST;
        break;
      case SymbolProperty::Local:
        Out |= cpp_index::LOCAL;
        break;
      case SymbolProperty::ProtocolInterface:
        Out |= cpp_index::PROTOCOL_INTERFACE;
        break;
      case SymbolProperty::IBAnnotated:
      case SymbolProperty::IBOutletCollection:
      case SymbolProperty::GKInspectable:
        break;
    }
  });
  return Out;
}

/// The non-relation roles of \p Roles as Role bits.
auto mapRoles(SymbolRoleSet Roles) -> uint32_t {
  uint32_t Out = 0;
  index::applyForEachSymbolRole(Roles, [&](SymbolRole R) {
    switch (R) {
      case SymbolRole::Declaration:
        Out |= cpp_index::DECLARATION;
        break;
      case SymbolRole::Definition:
        Out |= cpp_index::DEFINITION;
        break;
      case SymbolRole::Reference:
        Out |= cpp_index::REFERENCE;
        break;
      case SymbolRole::Read:
        Out |= cpp_index::READ;
        break;
      case SymbolRole::Write:
        Out |= cpp_index::WRITE;
        break;
      case SymbolRole::Call:
        Out |= cpp_index::CALL;
        break;
      case SymbolRole::Dynamic:
        Out |= cpp_index::DYNAMIC;
        break;
      case SymbolRole::AddressOf:
        Out |= cpp_index::ADDRESS_OF;
        break;
      case SymbolRole::Implicit:
        Out |= cpp_index::IMPLICIT;
        break;
      case SymbolRole::Undefinition:
        Out |= cpp_index::UNDEFINITION;
        break;
      case SymbolRole::NameReference:
        Out |= cpp_index::NAME_REFERENCE;
        break;
      case SymbolRole::RelationChildOf:
      case SymbolRole::RelationBaseOf:
      case SymbolRole::RelationOverrideOf:
      case SymbolRole::RelationReceivedBy:
      case SymbolRole::RelationCalledBy:
      case SymbolRole::RelationExtendedBy:
      case SymbolRole::RelationAccessorOf:
      case SymbolRole::RelationContainedBy:
      case SymbolRole::RelationIBTypeOf:
      case SymbolRole::RelationSpecializationOf:
        break;
    }
  });
  return Out;
}

auto mapRelation(SymbolRole R) -> std::optional<cpp_index::RelationKind> {
  switch (R) {
    case SymbolRole::RelationChildOf:
      return cpp_index::CHILD_OF;
    case SymbolRole::RelationBaseOf:
      return cpp_index::BASE_OF;
    case SymbolRole::RelationOverrideOf:
      return cpp_index::OVERRIDE_OF;
    case SymbolRole::RelationReceivedBy:
      return cpp_index::RECEIVED_BY;
    case SymbolRole::RelationCalledBy:
      return cpp_index::CALLED_BY;
    case SymbolRole::RelationExtendedBy:
      return cpp_index::EXTENDED_BY;
    case SymbolRole::RelationAccessorOf:
      return cpp_index::ACCESSOR_OF;
    case SymbolRole::RelationContainedBy:
      return cpp_index::CONTAINED_BY;
    case SymbolRole::RelationIBTypeOf:
      return cpp_index::IB_TYPE_OF;
    case SymbolRole::RelationSpecializationOf:
      return cpp_index::SPECIALIZATION_OF;
    default:
      return std::nullopt;
  }
}

/// A relation that says something about the symbol itself (its parent, base,
/// overridden method, primary template) rather than about the occurrence (the
/// function it sits in).
auto isSymbolLevel(cpp_index::RelationKind K) -> bool {
  switch (K) {
    case cpp_index::CALLED_BY:
    case cpp_index::CONTAINED_BY:
    case cpp_index::RECEIVED_BY:
      return false;
    default:
      return true;
  }
}

auto fileKindFor(llvm::StringRef Path) -> cpp_index::FileKind {
  if (llvm::sys::path::is_absolute(Path)) return cpp_index::SYSTEM;
  if (Path.starts_with("bazel-out/")) return cpp_index::GENERATED;
  if (Path.starts_with("external/")) return cpp_index::EXTERNAL;
  return cpp_index::SOURCE;
}

/// The path a file is recorded under: its name as Clang opened it, dots
/// removed (`-I.` yields `./x.h` while the main file is `x.h`), relative to
/// the working directory where possible.
auto recordPath(FileEntryRef Ref) -> std::string {
  llvm::SmallString<256> Name(Ref.getName());
  llvm::sys::path::remove_dots(Name, /*remove_dot_dot=*/true);
  return relativizeToCwd(Name);
}

// ---------------------------------------------------------------------------
// IndexConsumer
// ---------------------------------------------------------------------------

class IndexConsumer : public index::IndexDataConsumer {
 public:
  IndexConsumer(const FileSet& Owned, std::string* Out)
      : Owned(Owned), Out(Out) {}

  void initialize(ASTContext& Ctx) override {
    this->Ctx = &Ctx;
    SM = &Ctx.getSourceManager();
    MainFID = SM->getMainFileID();
    Unit.set_producer("cpp_format");
    Unit.set_schema_version(kIndexSchemaVersion);
    if (OptionalFileEntryRef Ref = SM->getFileEntryRefForID(MainFID))
      Unit.add_translation_units(recordPath(*Ref));
  }

  auto handleDeclOccurrence(const Decl* D, SymbolRoleSet Roles,
                            ArrayRef<index::SymbolRelation> Relations,
                            SourceLocation Loc, ASTNodeInfo /*Node*/)
      -> bool override {
    // Only an occurrence in an owned file is recorded, and a symbol enters
    // the table only through one (or as the target of its relations): with
    // Clang's system filter off, every declaration of every included header
    // passes through here, and interning first would put all of them in.
    const std::optional<Range> R = rangeFor(Loc);
    if (!R || !R->Owned) return true;
    const int32_t Sym = symbolFor(D);
    if (Sym < 0) return true;
    // Symbol-level relations say something about the symbol wherever it is
    // declared (a base class named in an owned file's class head is a BASE_OF
    // even when the base itself is external); the target is interned for it.
    for (const index::SymbolRelation& Rel : Relations) {
      index::applyForEachSymbolRole(Rel.Roles, [&](SymbolRole Role) {
        const std::optional<cpp_index::RelationKind> K = mapRelation(Role);
        if (!K || !isSymbolLevel(*K)) return;
        const int32_t Target = symbolFor(Rel.RelatedSymbol);
        if (Target >= 0) addSymbolRelation(Sym, *K, Target);
      });
    }
    cpp_index::Occurrence* O = Unit.add_occurrences();
    O->set_file(R->File);
    O->set_begin(R->Begin);
    O->set_end(R->End);
    O->set_symbol(Sym);
    O->set_roles(mapRoles(Roles));
    O->set_macro(R->Macro);
    for (const index::SymbolRelation& Rel : Relations) {
      index::applyForEachSymbolRole(Rel.Roles, [&](SymbolRole Role) {
        const std::optional<cpp_index::RelationKind> K = mapRelation(Role);
        if (!K || isSymbolLevel(*K)) return;
        const int32_t Target = symbolFor(Rel.RelatedSymbol);
        if (Target < 0) return;
        cpp_index::Relation* Out = O->add_relations();
        Out->set_kind(*K);
        Out->set_symbol(Target);
      });
    }
    return true;
  }

  auto handleMacroOccurrence(const IdentifierInfo* Name, const MacroInfo* MI,
                             SymbolRoleSet Roles, SourceLocation Loc)
      -> bool override {
    if (!Name || !MI) return true;
    // As above: Clang's predefined macros are "defined" in the `<built-in>`
    // buffer, and every macro of every included header passes through here.
    const std::optional<Range> R = rangeFor(Loc);
    if (!R || !R->Owned) return true;
    const int32_t Sym = symbolForMacro(Name, MI);
    if (Sym < 0) return true;
    cpp_index::Occurrence* O = Unit.add_occurrences();
    O->set_file(R->File);
    O->set_begin(R->Begin);
    O->set_end(R->End);
    O->set_symbol(Sym);
    O->set_roles(mapRoles(Roles));
    O->set_macro(R->Macro);
    return true;
  }

  void finish() override {
    indexDependentTokens();
    normalizeUnit(Unit);
    Out->clear();
    google::protobuf::io::StringOutputStream Stream(Out);
    google::protobuf::io::CodedOutputStream Coded(&Stream);
    Coded.SetSerializationDeterministic(true);
    Unit.SerializeToCodedStream(&Coded);
  }

 private:
  struct FileInfo {
    int32_t Index = -1;  ///< into Unit.files; -1 for a buffer with no file
    bool Owned = false;
  };

  /// Dependent tokens: `t.m` through a template parameter names no
  /// declaration until instantiation, so Clang's indexer reports nothing for
  /// it.  The rename tool's cross-TU machinery finds such tokens and, walking
  /// this TU's instantiations, what each resolves to; here every binding
  /// becomes a REFERENCE|DEPENDENT occurrence of the resolved symbol, and a
  /// token nothing in this TU resolves is listed as pending for the merge.
  /// Bindings are recorded for a token in any first-party file, owned or not
  /// -- the TU that instantiates a template is usually not the one that owns
  /// the header spelling the token -- while pending entries come only from
  /// the owner, so a token is pending in exactly one unit.
  void indexDependentTokens() {
    struct Token {
      Range R;
      std::string Name;
      bool Bound = false;
    };
    std::vector<Token> Tokens;
    llvm::DenseMap<unsigned, size_t> ByLoc;  // spelling loc -> Tokens index
    for (const DependentToken& T : collectDependentTokens(*Ctx)) {
      const std::optional<Range> R = rangeFor(T.Loc);
      if (!R) continue;
      const cpp_index::FileKind Kind = Unit.files(R->File).kind();
      if (Kind != cpp_index::SOURCE && Kind != cpp_index::GENERATED) continue;
      const unsigned Key = SM->getSpellingLoc(T.Loc).getRawEncoding();
      if (ByLoc.try_emplace(Key, Tokens.size()).second)
        Tokens.push_back({*R, T.Name, false});
    }
    if (Tokens.empty()) return;
    forEachDependentBinding(
        *Ctx,
        [&](SourceLocation Spelling) {
          return ByLoc.count(Spelling.getRawEncoding()) > 0;
        },
        [&](SourceLocation Spelling, const Decl* D) {
          Token& T = Tokens[ByLoc[Spelling.getRawEncoding()]];
          const int32_t Sym = symbolFor(D);
          if (Sym < 0) return;
          T.Bound = true;
          cpp_index::Occurrence* O = Unit.add_occurrences();
          O->set_file(T.R.File);
          O->set_begin(T.R.Begin);
          O->set_end(T.R.End);
          O->set_symbol(Sym);
          O->set_roles(cpp_index::REFERENCE | cpp_index::DEPENDENT);
          O->set_macro(T.R.Macro);
        });
    for (const Token& T : Tokens) {
      if (T.Bound || !T.R.Owned) continue;
      cpp_index::DependentToken* P = Unit.add_pending();
      P->set_file(T.R.File);
      P->set_begin(T.R.Begin);
      P->set_end(T.R.End);
      P->set_name(T.Name);
    }
  }

  struct Range {
    int32_t File;
    uint32_t Begin;
    uint32_t End;
    cpp_index::MacroContext Macro;
    bool Owned;
  };

  /// Registers the file behind \p FID (any kind) and decides whether its
  /// occurrences are recorded: the main file, or a real path in the owned
  /// set, or -- for an in-memory file with no real path -- a name in it.
  auto fileFor(FileID FID) -> const FileInfo& {
    auto It = Files.find(FID);
    if (It != Files.end()) return It->second;
    FileInfo Info;
    if (OptionalFileEntryRef Ref = SM->getFileEntryRefForID(FID)) {
      const std::string Path = recordPath(*Ref);
      auto [PathIt, Inserted] =
          FileIndexByPath.emplace(Path, Unit.files_size());
      if (Inserted) {
        cpp_index::File* F = Unit.add_files();
        F->set_path(Path);
        F->set_kind(fileKindFor(Path));
      }
      Info.Index = PathIt->second;
      if (FID == MainFID) {
        Info.Owned = true;
      } else {
        // The owned set holds real paths.  An in-memory file (the unit
        // tests) has no real path of its own -- Clang fills the field with
        // the cwd-joined name -- so its name is accepted as well; a name
        // that equals a real path names the same file, so that costs no
        // precision on disk either.
        llvm::StringRef Real = Ref->getFileEntry().tryGetRealPathName();
        Info.Owned = (!Real.empty() && Owned.count(Real.str()) > 0) ||
                     Owned.count(Ref->getName().str()) > 0 ||
                     Owned.count(Path) > 0;
      }
    }
    return Files.try_emplace(FID, Info).first->second;
  }

  /// The byte range of the token at \p Loc, in the file it lands in.  A
  /// location inside a macro expansion lands at the call site: on the
  /// argument's own spelling when the token was a macro argument, otherwise
  /// on the invocation's name token.
  auto rangeFor(SourceLocation Loc) -> std::optional<Range> {
    if (Loc.isInvalid()) return std::nullopt;
    const SourceLocation FileLoc = SM->getFileLoc(Loc);
    const std::pair<FileID, unsigned> Decomposed =
        SM->getDecomposedLoc(FileLoc);
    if (Decomposed.first.isInvalid()) return std::nullopt;
    const FileInfo& Info = fileFor(Decomposed.first);
    if (Info.Index < 0) return std::nullopt;
    cpp_index::MacroContext Macro = cpp_index::NOT_IN_MACRO;
    if (Loc.isMacroID())
      Macro = SM->getSpellingLoc(Loc) == FileLoc ? cpp_index::MACRO_ARGUMENT
                                                 : cpp_index::MACRO_BODY;
    const unsigned Length =
        Lexer::MeasureTokenLength(FileLoc, *SM, Ctx->getLangOpts());
    return Range{Info.Index, Decomposed.second, Decomposed.second + Length,
                 Macro, Info.Owned};
  }

  /// The symbol table entry for \p D (canonical), created on first sight; -1
  /// when it has no USR.
  auto symbolFor(const Decl* D) -> int32_t {
    if (!D) return -1;
    D = D->getCanonicalDecl();
    auto It = SymbolByDecl.find(D);
    if (It != SymbolByDecl.end()) return It->second;
    llvm::SmallString<128> Usr;
    if (index::generateUSRForDecl(D, Usr)) {
      SymbolByDecl[D] = -1;
      return -1;
    }
    const auto* ND = dyn_cast<NamedDecl>(D);
    const int32_t Index = internSymbol(Usr.str());
    SymbolByDecl[D] = Index;
    cpp_index::Symbol* S = Unit.mutable_symbols(Index);
    if (S->name().empty() && ND) {
      S->set_name(ND->getNameAsString());
      std::string Qualified;
      llvm::raw_string_ostream OS(Qualified);
      ND->printQualifiedName(OS, Ctx->getPrintingPolicy());
      S->set_qualified_name(Qualified);
    }
    const index::SymbolInfo Info = index::getSymbolInfo(D);
    S->set_kind(mapKind(Info.Kind));
    S->set_sub_kind(mapSubKind(Info.SubKind));
    S->set_language(mapLanguage(Info.Lang));
    S->set_properties(mapProperties(Info.Properties));
    if (const auto* VD = dyn_cast<ValueDecl>(D))
      S->set_type(VD->getType().getAsString(Ctx->getPrintingPolicy()));
    if (const std::optional<Range> R = rangeFor(D->getLocation())) {
      S->mutable_canonical()->set_file(R->File);
      S->mutable_canonical()->set_begin(R->Begin);
      S->mutable_canonical()->set_end(R->End);
    }
    return Index;
  }

  auto symbolForMacro(const IdentifierInfo* Name, const MacroInfo* MI)
      -> int32_t {
    auto It = SymbolByMacro.find(MI);
    if (It != SymbolByMacro.end()) return It->second;
    llvm::SmallString<128> Usr;
    if (index::generateUSRForMacro(Name->getName(), MI->getDefinitionLoc(), *SM,
                                   Usr)) {
      SymbolByMacro[MI] = -1;
      return -1;
    }
    const int32_t Index = internSymbol(Usr.str());
    SymbolByMacro[MI] = Index;
    cpp_index::Symbol* S = Unit.mutable_symbols(Index);
    if (S->name().empty()) {
      S->set_name(Name->getName().str());
      S->set_qualified_name(Name->getName().str());
    }
    const index::SymbolInfo Info = index::getSymbolInfoForMacro(*MI);
    S->set_kind(mapKind(Info.Kind));
    S->set_language(mapLanguage(Info.Lang));
    if (const std::optional<Range> R = rangeFor(MI->getDefinitionLoc())) {
      S->mutable_canonical()->set_file(R->File);
      S->mutable_canonical()->set_begin(R->Begin);
      S->mutable_canonical()->set_end(R->End);
    }
    return Index;
  }

  auto internSymbol(llvm::StringRef Usr) -> int32_t {
    auto [It, Inserted] = SymbolByUsr.emplace(Usr.str(), Unit.symbols_size());
    if (Inserted) Unit.add_symbols()->set_usr(Usr.str());
    return It->second;
  }

  void addSymbolRelation(int32_t Sym, cpp_index::RelationKind K,
                         int32_t Target) {
    if (!SymbolRelations.emplace(Sym, static_cast<int>(K), Target).second)
      return;
    cpp_index::Relation* R = Unit.mutable_symbols(Sym)->add_relations();
    R->set_kind(K);
    R->set_symbol(Target);
  }

  const FileSet& Owned;
  std::string* Out;
  ASTContext* Ctx = nullptr;
  SourceManager* SM = nullptr;
  FileID MainFID;
  cpp_index::IndexUnit Unit;
  llvm::DenseMap<FileID, FileInfo> Files;
  std::map<std::string, int32_t> FileIndexByPath;
  llvm::DenseMap<const Decl*, int32_t> SymbolByDecl;
  llvm::DenseMap<const MacroInfo*, int32_t> SymbolByMacro;
  std::map<std::string, int32_t> SymbolByUsr;
  std::set<std::tuple<int32_t, int, int32_t>> SymbolRelations;
};

}  // namespace

auto createIndexAction(const FileSet& Owned, std::string* Out)
    -> std::unique_ptr<FrontendAction> {
  return index::createIndexingAction(
      std::make_shared<IndexConsumer>(Owned, Out), makeIndexingOptions());
}

// ---------------------------------------------------------------------------
// IndexActionFactory
// ---------------------------------------------------------------------------

void IndexActionFactory::finish(std::vector<TUSlot>& Slots) {
  std::vector<cpp_index::IndexUnit> Units;
  Units.reserve(Slots.size());
  for (const TUSlot& S : Slots) {
    if (S.IndexBytes.empty()) continue;
    cpp_index::IndexUnit U;
    if (U.ParseFromString(S.IndexBytes)) Units.push_back(std::move(U));
  }
  Merged = mergeUnits(Units);
  if (Merged.producer().empty()) Merged.set_producer("cpp_format");
}

auto IndexActionFactory::writeUnit(llvm::StringRef Path) const -> bool {
  return writeMessage(Merged, Path, IndexFormat::Binary);
}

// ---------------------------------------------------------------------------
// Test helper
// ---------------------------------------------------------------------------

auto indexCode(llvm::StringRef Code, const std::vector<std::string>& Args,
               const clang::tooling::FileContentMappings& VirtualFiles,
               const std::vector<std::string>& OwnedNames)
    -> cpp_index::IndexUnit {
  FileSet Owned(OwnedNames.begin(), OwnedNames.end());
  std::string Bytes;
  cpp_index::IndexUnit Unit;
  if (!clang::tooling::runToolOnCodeWithArgs(
          createIndexAction(Owned, &Bytes), Code, Args, "input.cc",
          "clang-tool", std::make_shared<PCHContainerOperations>(),
          VirtualFiles))
    return Unit;
  Unit.ParseFromString(Bytes);
  return Unit;
}
