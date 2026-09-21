#include "cpp_formatting/proto_index_lib.h"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "cpp_formatting/cpp_index_merge.h"
#include "cpp_formatting/index.pb.h"
#include "google/protobuf/compiler/importer.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/io/zero_copy_stream.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

namespace {

namespace gpb = google::protobuf;

using cpp_index::IndexUnit;
using cpp_index::Occurrence;
using cpp_index::Relation;
using cpp_index::Symbol;

/// A byte range `[begin, end)`.
using Range = std::pair<uint32_t, uint32_t>;

/// A file's bytes and where each of its lines starts.
struct SourceText {
  std::string Bytes;
  std::vector<size_t> LineStarts;
};

/// The byte at (\p Line, \p Column) as the protobuf tokenizer counts them:
/// both from zero, a column per byte, except that a tab advances to the next
/// multiple of eight.
auto offsetOf(const SourceText& Text, int Line, int Column)
    -> std::optional<uint32_t> {
  if (Line < 0 || Column < 0 ||
      static_cast<size_t>(Line) >= Text.LineStarts.size())
    return std::nullopt;
  constexpr int kTabWidth = 8;
  size_t Pos = Text.LineStarts[static_cast<size_t>(Line)];
  int At = 0;
  while (At < Column && Pos < Text.Bytes.size() && Text.Bytes[Pos] != '\n') {
    At += Text.Bytes[Pos] == '\t' ? kTabWidth - At % kTabWidth : 1;
    ++Pos;
  }
  // A column inside a tab, or past the end of the line, is no token's.
  if (At != Column) return std::nullopt;
  return static_cast<uint32_t>(Pos);
}

auto isSpace(char C) -> bool {
  return C == ' ' || C == '\t' || C == '\n' || C == '\r';
}

class ErrorCollector : public gpb::compiler::MultiFileErrorCollector {
 public:
  void RecordError(absl::string_view Filename, int Line, int Column,
                   absl::string_view Message) override {
    Out << std::string(Filename);
    if (Line >= 0) Out << ":" << Line + 1 << ":" << Column + 1;
    Out << ": " << std::string(Message) << "\n";
  }
  std::ostringstream Out;
};

class ProtoIndexer {
 public:
  ProtoIndexer(gpb::compiler::SourceTree& Tree,
               const ProtoIndexOptions& Options, IndexUnit& Out)
      : Tree(Tree), Options(Options), Out(Out) {
    for (const auto& [Import, Path] : Options.PathMap)
      PathByImport.emplace(Import, Path);
  }

  /// The definitions and references of \p File.
  void indexFile(const gpb::FileDescriptor* File) {
    Main = File;
    Out.add_translation_units(pathOf(std::string(File->name())));
    fileIndex(std::string(File->name()));

    const int32_t Package = packageSymbol(File);
    for (int I = 0; I < File->message_type_count(); ++I)
      indexMessage(File->message_type(I), Package);
    for (int I = 0; I < File->enum_type_count(); ++I)
      indexEnum(File->enum_type(I), Package);
    for (int I = 0; I < File->extension_count(); ++I)
      indexField(File->extension(I), Package);
    for (int I = 0; I < File->service_count(); ++I) {
      const gpb::ServiceDescriptor* Service = File->service(I);
      const int32_t S = define(Service, Package);
      for (int J = 0; J < Service->method_count(); ++J) {
        const gpb::MethodDescriptor* Method = Service->method(J);
        define(Method, S);
        reference(Method, gpb::MethodDescriptorProto::kInputTypeFieldNumber,
                  Method->input_type());
        reference(Method, gpb::MethodDescriptorProto::kOutputTypeFieldNumber,
                  Method->output_type());
      }
    }
  }

  /// One GENERATES occurrence per annotation of \p Anchors that names a
  /// descriptor this producer describes.
  void addAnchors(const ProtoAnchors& Anchors) {
    gpb::GeneratedCodeInfo Info;
    if (!Info.ParseFromString(Anchors.Metadata)) {
      llvm::errs() << "cpp_format: the metadata for " << Anchors.GeneratedPath
                   << " is not a GeneratedCodeInfo; no anchors from it\n";
      return;
    }
    const int32_t Generated = fileIndexForPath(Anchors.GeneratedPath);
    for (const gpb::GeneratedCodeInfo::Annotation& A : Info.annotation()) {
      if (!A.has_begin() || !A.has_end() || A.end() <= A.begin()) continue;
      const gpb::FileDescriptor* File =
          A.source_file() == Main->name()
              ? Main
              : Main->pool()->FindFileByName(A.source_file());
      if (!File) continue;
      const int32_t S =
          symbolAt(File, std::vector<int>(A.path().begin(), A.path().end()));
      if (S < 0) continue;
      uint32_t Roles = cpp_index::GENERATES;
      // SET is a setter; ALIAS hands out a mutable reference.
      if (A.semantic() == gpb::GeneratedCodeInfo::Annotation::SET ||
          A.semantic() == gpb::GeneratedCodeInfo::Annotation::ALIAS)
        Roles |= cpp_index::WRITE;
      addOccurrence(
          Generated,
          {static_cast<uint32_t>(A.begin()), static_cast<uint32_t>(A.end())}, S,
          Roles);
    }
  }

 private:
  // --- Files ---------------------------------------------------------------

  auto pathOf(const std::string& Import) -> std::string {
    if (auto It = PathByImport.find(Import); It != PathByImport.end())
      return It->second;
    if (Options.ResolvePath) {
      std::string Path = Options.ResolvePath(Import);
      if (!Path.empty()) return Path;
    }
    return Import;
  }

  auto fileIndexForPath(const std::string& Path) -> int32_t {
    auto [It, New] = FileByPath.emplace(Path, Out.files_size());
    if (New) {
      cpp_index::File* F = Out.add_files();
      F->set_path(Path);
      F->set_kind(fileKindForPath(Path));
    }
    return It->second;
  }

  auto fileIndex(const std::string& Import) -> int32_t {
    return fileIndexForPath(pathOf(Import));
  }

  auto textOf(const gpb::FileDescriptor* File) -> const SourceText& {
    auto [It, New] = Texts.try_emplace(std::string(File->name()));
    SourceText& Text = It->second;
    if (!New) return Text;
    Text.LineStarts.push_back(0);
    const std::unique_ptr<gpb::io::ZeroCopyInputStream> In(
        Tree.Open(std::string(File->name())));
    if (!In) return Text;
    const void* Data = nullptr;
    int Size = 0;
    while (In->Next(&Data, &Size))
      Text.Bytes.append(static_cast<const char*>(Data),
                        static_cast<size_t>(Size));
    for (size_t I = 0; I < Text.Bytes.size(); ++I)
      if (Text.Bytes[I] == '\n') Text.LineStarts.push_back(I + 1);
    return Text;
  }

  /// The bytes the parser recorded for \p Path (a SourceCodeInfo path).
  auto rangeOf(const gpb::FileDescriptor* File, const std::vector<int>& Path)
      -> std::optional<Range> {
    gpb::SourceLocation Loc;
    if (!File->GetSourceLocation(Path, &Loc)) return std::nullopt;
    const SourceText& Text = textOf(File);
    const std::optional<uint32_t> Begin =
        offsetOf(Text, Loc.start_line, Loc.start_column);
    const std::optional<uint32_t> End =
        offsetOf(Text, Loc.end_line, Loc.end_column);
    if (!Begin || !End || *End <= *Begin) return std::nullopt;
    return Range{*Begin, *End};
  }

  // Where the parser recorded each descriptor, as a path into
  // FileDescriptorProto (descriptor.proto's SourceCodeInfo): the field number
  // of the list it is in, then its index there, from the file down.
  static auto locationOf(const gpb::Descriptor* D) -> std::vector<int> {
    if (const gpb::Descriptor* Outer = D->containing_type()) {
      std::vector<int> Path = locationOf(Outer);
      Path.push_back(gpb::DescriptorProto::kNestedTypeFieldNumber);
      Path.push_back(D->index());
      return Path;
    }
    return {gpb::FileDescriptorProto::kMessageTypeFieldNumber, D->index()};
  }
  static auto locationOf(const gpb::EnumDescriptor* E) -> std::vector<int> {
    if (const gpb::Descriptor* Outer = E->containing_type()) {
      std::vector<int> Path = locationOf(Outer);
      Path.push_back(gpb::DescriptorProto::kEnumTypeFieldNumber);
      Path.push_back(E->index());
      return Path;
    }
    return {gpb::FileDescriptorProto::kEnumTypeFieldNumber, E->index()};
  }
  static auto locationOf(const gpb::EnumValueDescriptor* V)
      -> std::vector<int> {
    std::vector<int> Path = locationOf(V->type());
    Path.push_back(gpb::EnumDescriptorProto::kValueFieldNumber);
    Path.push_back(V->index());
    return Path;
  }
  static auto locationOf(const gpb::FieldDescriptor* F) -> std::vector<int> {
    if (!F->is_extension()) {
      std::vector<int> Path = locationOf(F->containing_type());
      Path.push_back(gpb::DescriptorProto::kFieldFieldNumber);
      Path.push_back(F->index());
      return Path;
    }
    // An extension is listed where it is declared, not in what it extends.
    if (const gpb::Descriptor* Scope = F->extension_scope()) {
      std::vector<int> Path = locationOf(Scope);
      Path.push_back(gpb::DescriptorProto::kExtensionFieldNumber);
      Path.push_back(F->index());
      return Path;
    }
    return {gpb::FileDescriptorProto::kExtensionFieldNumber, F->index()};
  }
  static auto locationOf(const gpb::OneofDescriptor* O) -> std::vector<int> {
    std::vector<int> Path = locationOf(O->containing_type());
    Path.push_back(gpb::DescriptorProto::kOneofDeclFieldNumber);
    Path.push_back(O->index());
    return Path;
  }
  static auto locationOf(const gpb::ServiceDescriptor* S) -> std::vector<int> {
    return {gpb::FileDescriptorProto::kServiceFieldNumber, S->index()};
  }
  static auto locationOf(const gpb::MethodDescriptor* M) -> std::vector<int> {
    std::vector<int> Path = locationOf(M->service());
    Path.push_back(gpb::ServiceDescriptorProto::kMethodFieldNumber);
    Path.push_back(M->index());
    return Path;
  }

  /// The range of field \p Number of the descriptor proto \p Desc was built
  /// from: its name, a field's type, ...
  template <class D>
  auto rangeOfField(const D* Desc, int Number) -> std::optional<Range> {
    std::vector<int> Path = locationOf(Desc);
    Path.push_back(Number);
    return rangeOf(Desc->file(), Path);
  }

  // --- Symbols -------------------------------------------------------------

  static auto kindOf(const gpb::Descriptor*) -> cpp_index::SymbolKind {
    return cpp_index::MESSAGE;
  }
  static auto kindOf(const gpb::FieldDescriptor*) -> cpp_index::SymbolKind {
    return cpp_index::FIELD;
  }
  static auto kindOf(const gpb::EnumDescriptor*) -> cpp_index::SymbolKind {
    return cpp_index::ENUM;
  }
  static auto kindOf(const gpb::EnumValueDescriptor*) -> cpp_index::SymbolKind {
    return cpp_index::ENUM_CONSTANT;
  }
  static auto kindOf(const gpb::OneofDescriptor*) -> cpp_index::SymbolKind {
    return cpp_index::ONEOF;
  }
  static auto kindOf(const gpb::ServiceDescriptor*) -> cpp_index::SymbolKind {
    return cpp_index::SERVICE;
  }
  static auto kindOf(const gpb::MethodDescriptor*) -> cpp_index::SymbolKind {
    return cpp_index::RPC;
  }

  /// What a panel shows next to the name: a field's type and number, an
  /// enumerator's value, an rpc's signature.
  static auto typeOf(const gpb::FieldDescriptor* F) -> std::string {
    std::string T;
    if (F->is_map()) {
      const gpb::FieldDescriptor* K = F->message_type()->map_key();
      const gpb::FieldDescriptor* V = F->message_type()->map_value();
      T = "map<" + std::string(K->type_name()) + ", " + valueTypeOf(V) + ">";
    } else {
      if (F->is_repeated())
        T = "repeated ";
      else if (F->is_required())
        T = "required ";
      else if (saysOptional(F))
        T = "optional ";
      T += valueTypeOf(F);
    }
    if (F->is_extension())
      T += " (extends " + std::string(F->containing_type()->full_name()) + ")";
    return T + " = " + std::to_string(F->number());
  }
  /// Whether the source spells `optional`: proto3 turns it into a oneof
  /// nobody wrote, proto2 requires it of every singular field outside one.
  static auto saysOptional(const gpb::FieldDescriptor* F) -> bool {
    if (F->containing_oneof()) return !F->real_containing_oneof();
    gpb::FileDescriptorProto Heading;
    F->file()->CopyHeadingTo(&Heading);
    return Heading.syntax().empty() || Heading.syntax() == "proto2";
  }
  static auto valueTypeOf(const gpb::FieldDescriptor* F) -> std::string {
    if (F->type() == gpb::FieldDescriptor::TYPE_GROUP) return "group";
    if (F->message_type()) return std::string(F->message_type()->full_name());
    if (F->enum_type()) return std::string(F->enum_type()->full_name());
    return std::string(F->type_name());
  }
  static auto typeOf(const gpb::EnumValueDescriptor* V) -> std::string {
    return "= " + std::to_string(V->number());
  }
  static auto typeOf(const gpb::MethodDescriptor* M) -> std::string {
    return std::string("(") + (M->client_streaming() ? "stream " : "") +
           std::string(M->input_type()->full_name()) + ") returns (" +
           (M->server_streaming() ? "stream " : "") +
           std::string(M->output_type()->full_name()) + ")";
  }
  template <class D>
  static auto typeOf(const D*) -> std::string {
    return "";
  }

  /// The symbol for \p Desc, with its canonical location -- in whichever file
  /// declares it, so that every unit that names the symbol says the same.
  template <class D>
  auto symbolFor(const D* Desc) -> int32_t {
    const std::string Usr = "proto:" + std::string(Desc->full_name());
    auto [It, New] = SymbolByUsr.emplace(Usr, Out.symbols_size());
    if (!New) return It->second;
    Symbol* S = Out.add_symbols();
    S->set_usr(Usr);
    S->set_name(std::string(Desc->name()));
    S->set_qualified_name(std::string(Desc->full_name()));
    S->set_kind(kindOf(Desc));
    S->set_language(cpp_index::PROTO);
    S->set_type(typeOf(Desc));
    // Every descriptor proto's `name` is field 1.
    if (const std::optional<Range> R =
            rangeOfField(Desc, gpb::DescriptorProto::kNameFieldNumber)) {
      // fileIndex() may grow `files`, never `symbols`: S stays valid.
      const int32_t F = fileIndex(std::string(Desc->file()->name()));
      cpp_index::Location* C = S->mutable_canonical();
      C->set_file(F);
      C->set_begin(R->first);
      C->set_end(R->second);
    }
    return It->second;
  }

  void addOccurrence(int32_t File, Range R, int32_t Sym, uint32_t Roles) {
    Occurrence* O = Out.add_occurrences();
    O->set_file(File);
    O->set_begin(R.first);
    O->set_end(R.second);
    O->set_symbol(Sym);
    O->set_roles(Roles);
  }

  void addChildOf(int32_t Sym, int32_t Parent) {
    if (Parent < 0) return;
    Relation* R = Out.mutable_symbols(Sym)->add_relations();
    R->set_kind(cpp_index::CHILD_OF);
    R->set_symbol(Parent);
  }

  /// \p Desc is declared in the file being indexed: its symbol, a DEFINITION
  /// on its name, and its place under \p Parent.
  template <class D>
  auto define(const D* Desc, int32_t Parent) -> int32_t {
    const int32_t S = symbolFor(Desc);
    addChildOf(S, Parent);
    if (const std::optional<Range> R =
            rangeOfField(Desc, gpb::DescriptorProto::kNameFieldNumber))
      addOccurrence(fileIndex(std::string(Main->name())), *R, S,
                    cpp_index::DEFINITION);
    return S;
  }

  /// A REFERENCE to \p Target on the range of \p User's field \p Number.
  template <class D, class T>
  void reference(const D* User, int Number, const T* Target) {
    if (!Target) return;
    if (const std::optional<Range> R = rangeOfField(User, Number))
      addOccurrence(fileIndex(std::string(Main->name())), *R, symbolFor(Target),
                    cpp_index::REFERENCE);
  }

  /// `package a.b;` declares `a.b` in every file that says it, so the symbol
  /// has no canonical location -- none of them is the one.
  auto packageSymbol(const gpb::FileDescriptor* File) -> int32_t {
    if (File->package().empty()) return -1;
    const std::string Name(File->package());
    const std::string Usr = "proto:" + Name;
    auto [It, New] = SymbolByUsr.emplace(Usr, Out.symbols_size());
    if (New) {
      Symbol* S = Out.add_symbols();
      S->set_usr(Usr);
      S->set_name(Name);
      S->set_qualified_name(Name);
      S->set_kind(cpp_index::PACKAGE);
      S->set_language(cpp_index::PROTO);
    }
    // The recorded span is the whole statement; the name is the token after
    // the keyword.  Anything unusual between them (a comment) costs the
    // occurrence, not the symbol.
    if (const std::optional<Range> R =
            rangeOf(File, {gpb::FileDescriptorProto::kPackageFieldNumber})) {
      const std::string& Bytes = textOf(File).Bytes;
      size_t Begin = R->first + std::string("package").size();
      while (Begin < R->second && isSpace(Bytes[Begin])) ++Begin;
      if (Bytes.compare(Begin, Name.size(), Name) == 0)
        addOccurrence(fileIndex(std::string(File->name())),
                      {static_cast<uint32_t>(Begin),
                       static_cast<uint32_t>(Begin + Name.size())},
                      It->second, cpp_index::DECLARATION);
    }
    return It->second;
  }

  void indexMessage(const gpb::Descriptor* Message, int32_t Parent) {
    // `map<K, V>` is sugar for a nested FooEntry message nobody wrote.
    if (Message->options().map_entry()) return;
    const int32_t S = define(Message, Parent);
    for (int I = 0; I < Message->real_oneof_decl_count(); ++I)
      define(Message->oneof_decl(I), S);
    for (int I = 0; I < Message->field_count(); ++I)
      indexField(Message->field(I), S);
    for (int I = 0; I < Message->extension_count(); ++I)
      indexField(Message->extension(I), S);
    for (int I = 0; I < Message->nested_type_count(); ++I)
      indexMessage(Message->nested_type(I), S);
    for (int I = 0; I < Message->enum_type_count(); ++I)
      indexEnum(Message->enum_type(I), S);
  }

  void indexEnum(const gpb::EnumDescriptor* Enum, int32_t Parent) {
    const int32_t S = define(Enum, Parent);
    for (int I = 0; I < Enum->value_count(); ++I) define(Enum->value(I), S);
  }

  void indexField(const gpb::FieldDescriptor* Field, int32_t Parent) {
    const int32_t S = define(Field, Parent);
    if (const gpb::OneofDescriptor* Oneof = Field->real_containing_oneof())
      addChildOf(S, symbolFor(Oneof));
    if (Field->is_extension())
      reference(Field, gpb::FieldDescriptorProto::kExtendeeFieldNumber,
                Field->containing_type());

    constexpr int kTypeName = gpb::FieldDescriptorProto::kTypeNameFieldNumber;
    if (Field->is_map()) {
      referenceMapValue(Field);
    } else if (Field->type() == gpb::FieldDescriptor::TYPE_GROUP) {
      // A group's type is spelled by the same token that names it.
    } else if (Field->message_type()) {
      reference(Field, kTypeName, Field->message_type());
    } else if (Field->enum_type()) {
      reference(Field, kTypeName, Field->enum_type());
    }
  }

  /// A map field's type span is all of `map<K, V>`; only V can name a type.
  void referenceMapValue(const gpb::FieldDescriptor* Field) {
    const gpb::FieldDescriptor* Value = Field->message_type()->map_value();
    if (!Value->message_type() && !Value->enum_type()) return;
    const std::optional<Range> R =
        rangeOfField(Field, gpb::FieldDescriptorProto::kTypeNameFieldNumber);
    if (!R) return;
    const std::string& Bytes = textOf(Field->file()).Bytes;
    const size_t Comma = Bytes.find(',', R->first);
    size_t End = Bytes.rfind('>', R->second - 1);
    if (Comma == std::string::npos || End == std::string::npos ||
        Comma >= R->second || End <= Comma)
      return;
    size_t Begin = Comma + 1;
    while (Begin < End && isSpace(Bytes[Begin])) ++Begin;
    while (End > Begin && isSpace(Bytes[End - 1])) --End;
    if (Begin >= End) return;
    const int32_t Target = Value->message_type()
                               ? symbolFor(Value->message_type())
                               : symbolFor(Value->enum_type());
    addOccurrence(fileIndex(std::string(Main->name())),
                  {static_cast<uint32_t>(Begin), static_cast<uint32_t>(End)},
                  Target, cpp_index::REFERENCE);
  }

  // --- Anchors -------------------------------------------------------------

  /// The symbol of the descriptor \p Path (a path into FileDescriptorProto,
  /// as GeneratedCodeInfo gives it) names; -1 when it names none we describe.
  auto symbolAt(const gpb::FileDescriptor* File, const std::vector<int>& Path)
      -> int32_t {
    auto In = [](int I, int Count) { return I >= 0 && I < Count; };
    if (Path.size() < 2) return -1;
    size_t At = 2;
    const int Index = Path[1];
    switch (Path[0]) {
      case gpb::FileDescriptorProto::kMessageTypeFieldNumber:
        if (!In(Index, File->message_type_count())) return -1;
        return symbolInMessage(File->message_type(Index), Path, At);
      case gpb::FileDescriptorProto::kEnumTypeFieldNumber:
        if (!In(Index, File->enum_type_count())) return -1;
        return symbolInEnum(File->enum_type(Index), Path, At);
      case gpb::FileDescriptorProto::kExtensionFieldNumber:
        if (!In(Index, File->extension_count()) || Path.size() != At) return -1;
        return symbolFor(File->extension(Index));
      case gpb::FileDescriptorProto::kServiceFieldNumber: {
        if (!In(Index, File->service_count())) return -1;
        const gpb::ServiceDescriptor* S = File->service(Index);
        if (Path.size() == At) return symbolFor(S);
        if (Path.size() != At + 2 ||
            Path[At] != gpb::ServiceDescriptorProto::kMethodFieldNumber ||
            !In(Path[At + 1], S->method_count()))
          return -1;
        return symbolFor(S->method(Path[At + 1]));
      }
      default:
        return -1;
    }
  }

  auto symbolInEnum(const gpb::EnumDescriptor* Enum,
                    const std::vector<int>& Path, size_t At) -> int32_t {
    if (Path.size() == At) return symbolFor(Enum);
    if (Path.size() != At + 2 ||
        Path[At] != gpb::EnumDescriptorProto::kValueFieldNumber ||
        Path[At + 1] < 0 || Path[At + 1] >= Enum->value_count())
      return -1;
    return symbolFor(Enum->value(Path[At + 1]));
  }

  auto symbolInMessage(const gpb::Descriptor* Message,
                       const std::vector<int>& Path, size_t At) -> int32_t {
    while (true) {
      if (Message->options().map_entry()) return -1;
      if (Path.size() == At) return symbolFor(Message);
      if (Path.size() < At + 2) return -1;
      const int Index = Path[At + 1];
      const bool Last = Path.size() == At + 2;
      switch (Path[At]) {
        case gpb::DescriptorProto::kNestedTypeFieldNumber:
          if (Index < 0 || Index >= Message->nested_type_count()) return -1;
          Message = Message->nested_type(Index);
          At += 2;
          continue;
        case gpb::DescriptorProto::kEnumTypeFieldNumber:
          if (Index < 0 || Index >= Message->enum_type_count()) return -1;
          return symbolInEnum(Message->enum_type(Index), Path, At + 2);
        case gpb::DescriptorProto::kFieldFieldNumber:
          if (!Last || Index < 0 || Index >= Message->field_count()) return -1;
          return symbolFor(Message->field(Index));
        case gpb::DescriptorProto::kExtensionFieldNumber:
          if (!Last || Index < 0 || Index >= Message->extension_count())
            return -1;
          return symbolFor(Message->extension(Index));
        case gpb::DescriptorProto::kOneofDeclFieldNumber:
          // A proto3 `optional` is a oneof nobody wrote: not a symbol.
          if (!Last || Index < 0 || Index >= Message->real_oneof_decl_count())
            return -1;
          return symbolFor(Message->oneof_decl(Index));
        default:
          return -1;
      }
    }
  }

  gpb::compiler::SourceTree& Tree;
  const ProtoIndexOptions& Options;
  IndexUnit& Out;
  const gpb::FileDescriptor* Main = nullptr;
  std::map<std::string, std::string> PathByImport;
  std::map<std::string, int32_t> FileByPath;
  std::map<std::string, int32_t> SymbolByUsr;
  std::map<std::string, SourceText> Texts;
};

auto readFile(const std::string& Path, std::string& Out) -> bool {
  std::ifstream In(Path, std::ios::binary);
  if (!In) return false;
  std::ostringstream Bytes;
  Bytes << In.rdbuf();
  Out = Bytes.str();
  return true;
}

/// `--flag=value` or `--flag value`: the value, advancing \p I past it.
auto flagValue(const std::vector<std::string>& Args, size_t& I,
               llvm::StringRef Flag, std::string& Out) -> bool {
  const llvm::StringRef Arg(Args[I]);
  if (Arg.starts_with(Flag) && Arg.drop_front(Flag.size()).starts_with("=")) {
    Out = Arg.drop_front(Flag.size() + 1).str();
    return true;
  }
  if (Arg != Flag) return false;
  if (I + 1 >= Args.size()) {
    Out.clear();
    return true;
  }
  Out = Args[++I];
  return true;
}

}  // namespace

auto indexProtoFile(gpb::compiler::SourceTree& Tree,
                    const std::string& ImportName,
                    const ProtoIndexOptions& Options, IndexUnit& Out,
                    std::string& Error) -> bool {
  ErrorCollector Errors;
  gpb::compiler::Importer Importer(&Tree, &Errors);
  const gpb::FileDescriptor* File = Importer.Import(ImportName);
  if (!File) {
    Error = Errors.Out.str();
    if (Error.empty()) Error = ImportName + ": cannot be imported\n";
    return false;
  }
  Out.Clear();
  Out.set_producer("cpp_format");
  Out.set_schema_version(kIndexSchemaVersion);
  ProtoIndexer Indexer(Tree, Options, Out);
  Indexer.indexFile(File);
  for (const ProtoAnchors& A : Options.Anchors) Indexer.addAnchors(A);
  normalizeUnit(Out);
  return true;
}

auto runEmitProtoIndex(const std::vector<std::string>& Args) -> int {
  std::string Output;
  std::string PathMapFile;
  std::vector<std::string> ProtoPaths;
  std::vector<std::pair<std::string, std::string>> AnchorFiles;
  std::vector<std::string> Inputs;
  for (size_t I = 0; I < Args.size(); ++I) {
    const llvm::StringRef Arg(Args[I]);
    std::string Value;
    if (flagValue(Args, I, "--emit-proto-index", Output)) continue;
    if (flagValue(Args, I, "--path-map", PathMapFile)) continue;
    if (flagValue(Args, I, "--proto-path", Value)) {
      ProtoPaths.push_back(Value);
    } else if (Arg.starts_with("-I") && Arg.size() > 2) {
      ProtoPaths.push_back(Arg.drop_front(2).str());
    } else if (flagValue(Args, I, "--anchors", Value)) {
      const size_t Eq = Value.find('=');
      if (Eq == std::string::npos || Eq == 0 || Eq + 1 == Value.size()) {
        llvm::errs() << "--anchors takes <metadata file>=<generated path>\n";
        return 2;
      }
      AnchorFiles.emplace_back(Value.substr(0, Eq), Value.substr(Eq + 1));
    } else if (Arg.starts_with("-")) {
      llvm::errs() << "Unknown flag '" << Arg
                   << "' for --emit-proto-index. Valid flags: --path-map=, "
                      "--proto-path= (-I), --anchors=<metadata>=<path>\n";
      return 2;
    } else {
      Inputs.push_back(Arg.str());
    }
  }
  if (Output.empty() || Inputs.size() != 1) {
    llvm::errs() << "Usage: cpp_format --emit-proto-index=<unit> "
                    "[--path-map=<file>] [--proto-path=<dir>]... "
                    "[--anchors=<metadata>=<generated path>]... <file.proto>\n";
    return 2;
  }

  ProtoIndexOptions Options;
  if (!PathMapFile.empty()) {
    std::ifstream In(PathMapFile);
    if (!In) {
      llvm::errs() << "Cannot read '" << PathMapFile << "'\n";
      return 2;
    }
    for (std::string Line; std::getline(In, Line);) {
      const size_t Tab = Line.find('\t');
      if (Tab == std::string::npos) continue;
      Options.PathMap.emplace_back(Line.substr(0, Tab), Line.substr(Tab + 1));
    }
  }

  // The map first: DiskSourceTree asks its mappings in order, and a listed
  // file is the one the build means even when a directory has one too.
  gpb::compiler::DiskSourceTree Tree;
  for (const auto& [Import, Path] : Options.PathMap) Tree.MapPath(Import, Path);
  for (const std::string& Dir : ProtoPaths) Tree.MapPath("", Dir);
  if (Options.PathMap.empty() && ProtoPaths.empty()) Tree.MapPath("", ".");
  Options.ResolvePath = [&Tree](const std::string& Import) {
    std::string Disk;
    if (!Tree.VirtualFileToDiskFile(Import, &Disk)) return std::string();
    const llvm::StringRef Clean(Disk);
    return (Clean.starts_with("./") ? Clean.drop_front(2) : Clean).str();
  };

  std::string ImportName;
  for (const auto& [Import, Path] : Options.PathMap)
    if (Path == Inputs.front()) ImportName = Import;
  if (ImportName.empty()) {
    std::string Shadowing;
    if (Tree.DiskFileToVirtualFile(Inputs.front(), &ImportName, &Shadowing) !=
        gpb::compiler::DiskSourceTree::SUCCESS) {
      llvm::errs() << "'" << Inputs.front()
                   << "' is not in the path map or under a --proto-path\n";
      return 2;
    }
  }

  for (const auto& [Metadata, Generated] : AnchorFiles) {
    ProtoAnchors A;
    A.GeneratedPath = Generated;
    if (!readFile(Metadata, A.Metadata)) {
      llvm::errs() << "Cannot read '" << Metadata << "'\n";
      return 2;
    }
    Options.Anchors.push_back(std::move(A));
  }

  IndexUnit Unit;
  std::string Error;
  if (!indexProtoFile(Tree, ImportName, Options, Unit, Error)) {
    llvm::errs() << Error;
    return 1;
  }
  return writeMessage(Unit, Output, IndexFormat::Binary) ? 0 : 1;
}
