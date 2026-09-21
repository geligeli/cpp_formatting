#include "cpp_formatting/cpp_index_merge.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/io/zero_copy_stream_impl_lite.h"
#include "google/protobuf/json/json.h"
#include "google/protobuf/text_format.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"

using cpp_index::Attribute;
using cpp_index::File;
using cpp_index::FileOccurrences;
using cpp_index::Index;
using cpp_index::IndexUnit;
using cpp_index::Occurrence;
using cpp_index::Relation;
using cpp_index::Symbol;

auto parseIndexFormat(llvm::StringRef Name, IndexFormat& Out) -> bool {
  if (Name == "binary") {
    Out = IndexFormat::Binary;
    return true;
  }
  if (Name == "text") {
    Out = IndexFormat::Text;
    return true;
  }
  if (Name == "json") {
    Out = IndexFormat::Json;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Normalization
// ---------------------------------------------------------------------------

auto fileKindForPath(llvm::StringRef Path) -> cpp_index::FileKind {
  if (llvm::sys::path::is_absolute(Path)) return cpp_index::SYSTEM;
  if (Path.starts_with("bazel-out/")) return cpp_index::GENERATED;
  if (Path.starts_with("external/")) return cpp_index::EXTERNAL;
  return cpp_index::SOURCE;
}

namespace {

/// Sorts \p Attrs by key and keeps the first value of each key.
void normalizeAttributes(google::protobuf::RepeatedPtrField<Attribute>& Attrs) {
  std::map<std::string, std::string> ByKey;
  for (const Attribute& A : Attrs) ByKey.emplace(A.key(), A.value());
  Attrs.Clear();
  for (auto& [Key, Value] : ByKey) {
    Attribute* A = Attrs.Add();
    A->set_key(Key);
    A->set_value(Value);
  }
}

/// Appends every attribute of \p From that \p To lacks (by key).
void unionAttributes(const google::protobuf::RepeatedPtrField<Attribute>& From,
                     google::protobuf::RepeatedPtrField<Attribute>& To) {
  for (const Attribute& A : From) {
    bool Present = false;
    for (const Attribute& B : To)
      if (B.key() == A.key()) {
        Present = true;
        break;
      }
    if (!Present) *To.Add() = A;
  }
}

auto relationKey(const Relation& R) -> std::pair<int, int32_t> {
  return {static_cast<int>(R.kind()), R.symbol()};
}

/// Remaps every relation's symbol through \p SymbolMap (dropping those whose
/// target is unknown), then sorts by (kind, symbol) and dedups.
void normalizeRelations(google::protobuf::RepeatedPtrField<Relation>& Rels,
                        const std::vector<int32_t>& SymbolMap) {
  std::vector<Relation> Kept;
  for (const Relation& R : Rels) {
    if (R.symbol() < 0 || static_cast<size_t>(R.symbol()) >= SymbolMap.size())
      continue;
    Relation Copy = R;
    Copy.set_symbol(SymbolMap[static_cast<size_t>(R.symbol())]);
    Kept.push_back(std::move(Copy));
  }
  std::sort(Kept.begin(), Kept.end(), [](const Relation& A, const Relation& B) {
    return relationKey(A) < relationKey(B);
  });
  Kept.erase(std::unique(Kept.begin(), Kept.end(),
                         [](const Relation& A, const Relation& B) {
                           return relationKey(A) == relationKey(B);
                         }),
             Kept.end());
  Rels.Clear();
  for (Relation& R : Kept) *Rels.Add() = std::move(R);
}

/// Appends every relation of \p From that \p To lacks.
void unionRelations(const google::protobuf::RepeatedPtrField<Relation>& From,
                    google::protobuf::RepeatedPtrField<Relation>& To) {
  for (const Relation& R : From) {
    bool Present = false;
    for (const Relation& S : To)
      if (relationKey(R) == relationKey(S)) {
        Present = true;
        break;
      }
    if (!Present) *To.Add() = R;
  }
}

/// Fills \p To's empty fields from \p From; unions the rest.
void mergeSymbolInto(const Symbol& From, Symbol& To) {
  if (To.name().empty()) To.set_name(From.name());
  if (To.qualified_name().empty()) To.set_qualified_name(From.qualified_name());
  if (To.kind() == cpp_index::SYMBOL_KIND_UNSPECIFIED) To.set_kind(From.kind());
  if (To.sub_kind() == cpp_index::SUB_KIND_NONE)
    To.set_sub_kind(From.sub_kind());
  if (To.language() == cpp_index::LANGUAGE_UNSPECIFIED)
    To.set_language(From.language());
  To.set_properties(To.properties() | From.properties());
  if (To.type().empty()) To.set_type(From.type());
  if (!To.has_canonical() && From.has_canonical())
    *To.mutable_canonical() = From.canonical();
  unionRelations(From.relations(), *To.mutable_relations());
  unionAttributes(From.attributes(), *To.mutable_attributes());
}

/// Fills \p To's unspecified kind from \p From; unions the attributes.
void mergeFileInto(const File& From, File& To) {
  // The lowest specified kind wins, so two units that classified one path
  // differently agree whatever their order.
  if (From.kind() != cpp_index::FILE_KIND_UNSPECIFIED &&
      (To.kind() == cpp_index::FILE_KIND_UNSPECIFIED ||
       From.kind() < To.kind()))
    To.set_kind(From.kind());
  unionAttributes(From.attributes(), *To.mutable_attributes());
}

/// One occurrence per (range, symbol, macro context): two reports of the same
/// token naming the same symbol are one occurrence whose roles are the union
/// -- a macro-body token read in one expansion and written in another, or a
/// dependent name Clang's indexer resolved through the primary template and
/// an instantiation then confirmed (REFERENCE from the one, DEPENDENT from
/// the other).
auto occurrenceKey(const Occurrence& O)
    -> std::tuple<int32_t, uint32_t, uint32_t, int32_t, int> {
  return {O.file(), O.begin(), O.end(), O.symbol(),
          static_cast<int>(O.macro())};
}

/// Sorts occurrences by their key, merges duplicates (unioning relations).
/// Every occurrence must already have valid, remapped indexes.
void sortAndDedupOccurrences(std::vector<Occurrence>& Occs) {
  std::stable_sort(Occs.begin(), Occs.end(),
                   [](const Occurrence& A, const Occurrence& B) {
                     return occurrenceKey(A) < occurrenceKey(B);
                   });
  std::vector<Occurrence> Out;
  for (Occurrence& O : Occs) {
    if (!Out.empty() && occurrenceKey(Out.back()) == occurrenceKey(O)) {
      Out.back().set_roles(Out.back().roles() | O.roles());
      unionRelations(O.relations(), *Out.back().mutable_relations());
      continue;
    }
    Out.push_back(std::move(O));
  }
  Occs = std::move(Out);
}

}  // namespace

void normalizeUnit(IndexUnit& Unit) {
  // 1. Files: sort by path, merge duplicates, map old index -> new index.
  std::map<std::string, File> FilesByPath;
  for (const File& F : Unit.files()) {
    auto [It, Inserted] = FilesByPath.emplace(F.path(), F);
    if (!Inserted) mergeFileInto(F, It->second);
  }
  // (Not std::distance on the map's iterators: that is linear per entry, and
  // a merge of a repository's units has millions of entries.)
  std::vector<int32_t> FileMap;
  {
    llvm::StringMap<int32_t> NewIndex;
    for (const auto& [Path, F] : FilesByPath)
      NewIndex.try_emplace(Path, static_cast<int32_t>(NewIndex.size()));
    FileMap.reserve(static_cast<size_t>(Unit.files_size()));
    for (const File& F : Unit.files())
      FileMap.push_back(NewIndex.lookup(F.path()));
  }
  const auto mapFile = [&](int32_t Old) -> std::optional<int32_t> {
    if (Old < 0 || static_cast<size_t>(Old) >= FileMap.size())
      return std::nullopt;
    return FileMap[static_cast<size_t>(Old)];
  };

  // 2. Symbols: sort by USR, merge duplicates, map old index -> new index.
  std::map<std::string, Symbol> SymbolsByUsr;
  for (const Symbol& S : Unit.symbols()) {
    auto [It, Inserted] = SymbolsByUsr.emplace(S.usr(), S);
    if (!Inserted) mergeSymbolInto(S, It->second);
  }
  std::vector<int32_t> SymbolMap;
  {
    llvm::StringMap<int32_t> NewIndex;
    for (const auto& [Usr, S] : SymbolsByUsr)
      NewIndex.try_emplace(Usr, static_cast<int32_t>(NewIndex.size()));
    SymbolMap.reserve(static_cast<size_t>(Unit.symbols_size()));
    for (const Symbol& S : Unit.symbols())
      SymbolMap.push_back(NewIndex.lookup(S.usr()));
  }

  // 3. Occurrences: remap, drop the unmappable, sort, dedup.
  std::vector<Occurrence> Occs;
  Occs.reserve(static_cast<size_t>(Unit.occurrences_size()));
  for (const Occurrence& O : Unit.occurrences()) {
    std::optional<int32_t> F = mapFile(O.file());
    if (!F || O.symbol() < 0 ||
        static_cast<size_t>(O.symbol()) >= SymbolMap.size())
      continue;
    Occurrence Copy = O;
    Copy.set_file(*F);
    Copy.set_symbol(SymbolMap[static_cast<size_t>(O.symbol())]);
    normalizeRelations(*Copy.mutable_relations(), SymbolMap);
    Occs.push_back(std::move(Copy));
  }
  sortAndDedupOccurrences(Occs);

  // 3b. Pending dependent tokens: remap, drop the unmappable and every one
  // some DEPENDENT occurrence has since resolved, sort, dedup.
  std::set<std::tuple<int32_t, uint32_t, uint32_t>> Resolved;
  for (const Occurrence& O : Occs)
    if (O.roles() & cpp_index::DEPENDENT)
      Resolved.emplace(O.file(), O.begin(), O.end());
  std::vector<cpp_index::DependentToken> Pending;
  for (const cpp_index::DependentToken& P : Unit.pending()) {
    std::optional<int32_t> F = mapFile(P.file());
    if (!F || Resolved.count({*F, P.begin(), P.end()})) continue;
    cpp_index::DependentToken Copy = P;
    Copy.set_file(*F);
    Pending.push_back(std::move(Copy));
  }
  const auto pendingKey = [](const cpp_index::DependentToken& P) {
    return std::make_tuple(P.file(), P.begin(), P.end(), P.name());
  };
  std::sort(Pending.begin(), Pending.end(), [&](const auto& A, const auto& B) {
    return pendingKey(A) < pendingKey(B);
  });
  Pending.erase(std::unique(Pending.begin(), Pending.end(),
                            [&](const auto& A, const auto& B) {
                              return pendingKey(A) == pendingKey(B);
                            }),
                Pending.end());

  // 4. Rebuild the tables in their new order, remapping what they refer to.
  Unit.clear_files();
  for (auto& [Path, F] : FilesByPath) {
    normalizeAttributes(*F.mutable_attributes());
    *Unit.add_files() = std::move(F);
  }
  Unit.clear_symbols();
  for (auto& [Usr, S] : SymbolsByUsr) {
    if (S.has_canonical()) {
      std::optional<int32_t> F = mapFile(S.canonical().file());
      if (F)
        S.mutable_canonical()->set_file(*F);
      else
        S.clear_canonical();
    }
    normalizeRelations(*S.mutable_relations(), SymbolMap);
    normalizeAttributes(*S.mutable_attributes());
    *Unit.add_symbols() = std::move(S);
  }
  Unit.clear_occurrences();
  for (Occurrence& O : Occs) *Unit.add_occurrences() = std::move(O);
  Unit.clear_pending();
  for (cpp_index::DependentToken& P : Pending)
    *Unit.add_pending() = std::move(P);

  // 5. The scalar fields.
  std::vector<std::string> TUs(Unit.translation_units().begin(),
                               Unit.translation_units().end());
  std::sort(TUs.begin(), TUs.end());
  TUs.erase(std::unique(TUs.begin(), TUs.end()), TUs.end());
  Unit.clear_translation_units();
  for (std::string& T : TUs) Unit.add_translation_units(std::move(T));
  if (Unit.schema_version() == 0) Unit.set_schema_version(kIndexSchemaVersion);
}

auto mergeUnits(const std::vector<IndexUnit>& Units) -> IndexUnit {
  // Concatenate with the indexes offset, then let normalizeUnit sort and
  // merge: the result is a pure function of the union.
  IndexUnit Out;
  for (const IndexUnit& U : Units) {
    const int32_t FileBase = Out.files_size();
    const int32_t SymbolBase = Out.symbols_size();
    for (const std::string& T : U.translation_units())
      Out.add_translation_units(T);
    for (const File& F : U.files()) *Out.add_files() = F;
    for (const Symbol& S : U.symbols()) {
      Symbol* Copy = Out.add_symbols();
      *Copy = S;
      if (Copy->has_canonical())
        Copy->mutable_canonical()->set_file(Copy->canonical().file() +
                                            FileBase);
      for (Relation& R : *Copy->mutable_relations())
        R.set_symbol(R.symbol() + SymbolBase);
    }
    for (const Occurrence& O : U.occurrences()) {
      Occurrence* Copy = Out.add_occurrences();
      *Copy = O;
      Copy->set_file(O.file() + FileBase);
      Copy->set_symbol(O.symbol() + SymbolBase);
      for (Relation& R : *Copy->mutable_relations())
        R.set_symbol(R.symbol() + SymbolBase);
    }
    for (const cpp_index::DependentToken& P : U.pending()) {
      cpp_index::DependentToken* Copy = Out.add_pending();
      *Copy = P;
      Copy->set_file(P.file() + FileBase);
    }
    if (Out.producer().empty()) Out.set_producer(U.producer());
    if (Out.schema_version() == 0) Out.set_schema_version(U.schema_version());
  }
  normalizeUnit(Out);
  return Out;
}

auto linkGenerated(IndexUnit& Unit) -> size_t {
  // Anchors by range.  Occurrences are sorted by (file, begin, end, ...), so
  // the anchors are too, and a range's symbols come out in symbol order.
  using Range = std::tuple<int32_t, uint32_t, uint32_t>;
  std::vector<std::pair<Range, int32_t>> Anchors;
  for (const Occurrence& O : Unit.occurrences())
    if (O.roles() & cpp_index::GENERATES)
      Anchors.push_back({Range{O.file(), O.begin(), O.end()}, O.symbol()});
  if (Anchors.empty()) return 0;

  size_t Added = 0;
  for (int32_t I = 0; I < Unit.symbols_size(); ++I) {
    Symbol& S = *Unit.mutable_symbols(I);
    if (!S.has_canonical()) continue;
    const Range Key{S.canonical().file(), S.canonical().begin(),
                    S.canonical().end()};
    auto It = std::lower_bound(Anchors.begin(), Anchors.end(), Key,
                               [](const std::pair<Range, int32_t>& A,
                                  const Range& K) { return A.first < K; });
    bool Changed = false;
    for (; It != Anchors.end() && It->first == Key; ++It) {
      // The anchor's own symbol is declared where it is declared, not
      // generated from itself.
      if (It->second == I) continue;
      Relation R;
      R.set_kind(cpp_index::GENERATED_FROM);
      R.set_symbol(It->second);
      const bool Present = std::any_of(
          S.relations().begin(), S.relations().end(),
          [&](const Relation& X) { return relationKey(X) == relationKey(R); });
      if (Present) continue;
      *S.add_relations() = R;
      Changed = true;
      ++Added;
    }
    if (Changed)
      std::sort(S.mutable_relations()->begin(), S.mutable_relations()->end(),
                [](const Relation& A, const Relation& B) {
                  return relationKey(A) < relationKey(B);
                });
  }
  return Added;
}

auto buildIndex(const IndexUnit& Unit) -> Index {
  Index Out;
  Out.set_producer(Unit.producer());
  Out.set_schema_version(Unit.schema_version() ? Unit.schema_version()
                                               : kIndexSchemaVersion);
  for (const std::string& T : Unit.translation_units())
    Out.add_translation_units(T);
  for (const File& F : Unit.files()) *Out.add_files() = F;
  for (const Symbol& S : Unit.symbols()) *Out.add_symbols() = S;
  for (int32_t I = 0; I < Unit.files_size(); ++I)
    Out.add_per_file()->set_file(I);
  // A normalized unit's occurrences are already sorted by (file, begin, ...),
  // so each file's run is in the order FileOccurrences wants.
  for (const Occurrence& O : Unit.occurrences()) {
    if (O.file() < 0 || O.file() >= Unit.files_size()) continue;
    *Out.mutable_per_file(O.file())->add_occurrences() = O;
  }
  for (const cpp_index::DependentToken& P : Unit.pending())
    *Out.add_unresolved() = P;
  return Out;
}

auto unitFromIndex(const Index& Index) -> IndexUnit {
  IndexUnit Out;
  Out.set_producer(Index.producer());
  Out.set_schema_version(Index.schema_version());
  for (const std::string& T : Index.translation_units())
    Out.add_translation_units(T);
  for (const File& F : Index.files()) *Out.add_files() = F;
  for (const Symbol& S : Index.symbols()) *Out.add_symbols() = S;
  for (const FileOccurrences& FO : Index.per_file())
    for (const Occurrence& O : FO.occurrences()) {
      Occurrence* Copy = Out.add_occurrences();
      *Copy = O;
      Copy->set_file(FO.file());
    }
  for (const cpp_index::DependentToken& P : Index.unresolved())
    *Out.add_pending() = P;
  return Out;
}

auto lookup(const Index& Index, llvm::StringRef Path, uint32_t Offset)
    -> std::vector<const Occurrence*> {
  std::vector<const Occurrence*> Out;
  const auto& Files = Index.files();
  const auto It = std::lower_bound(
      Files.begin(), Files.end(), Path,
      [](const File& F, llvm::StringRef P) { return F.path() < P; });
  if (It == Files.end() || It->path() != Path) return Out;
  const auto FileIndex = static_cast<int32_t>(std::distance(Files.begin(), It));
  for (const FileOccurrences& FO : Index.per_file()) {
    if (FO.file() != FileIndex) continue;
    for (const Occurrence& O : FO.occurrences()) {
      if (O.begin() > Offset) break;  // sorted by begin
      if (Offset < O.end()) Out.push_back(&O);
    }
  }
  return Out;
}

auto roleNames(uint32_t Roles) -> std::string {
  static constexpr std::pair<cpp_index::Role, const char*> kNames[] = {
      {cpp_index::DECLARATION, "DECLARATION"},
      {cpp_index::DEFINITION, "DEFINITION"},
      {cpp_index::REFERENCE, "REFERENCE"},
      {cpp_index::READ, "READ"},
      {cpp_index::WRITE, "WRITE"},
      {cpp_index::CALL, "CALL"},
      {cpp_index::DYNAMIC, "DYNAMIC"},
      {cpp_index::ADDRESS_OF, "ADDRESS_OF"},
      {cpp_index::IMPLICIT, "IMPLICIT"},
      {cpp_index::UNDEFINITION, "UNDEFINITION"},
      {cpp_index::NAME_REFERENCE, "NAME_REFERENCE"},
      {cpp_index::DEPENDENT, "DEPENDENT"},
      {cpp_index::PASTED, "PASTED"},
      {cpp_index::GENERATES, "GENERATES"},
  };
  std::string Out;
  for (const auto& [Bit, Name] : kNames) {
    if (!(Roles & static_cast<uint32_t>(Bit))) continue;
    if (!Out.empty()) Out += '|';
    Out += Name;
  }
  return Out.empty() ? "ROLE_NONE" : Out;
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

auto readUnit(llvm::StringRef Path, IndexUnit& Out) -> bool {
  auto BufOrErr = llvm::MemoryBuffer::getFile(Path);
  if (!BufOrErr) {
    llvm::errs() << "Cannot read '" << Path
                 << "': " << BufOrErr.getError().message() << "\n";
    return false;
  }
  llvm::StringRef Bytes = (*BufOrErr)->getBuffer();
  // The two messages share every field number but one, so parse as a unit
  // first; a file that turns out to carry `per_file` was an index.
  IndexUnit Unit;
  if (!Unit.ParseFromArray(Bytes.data(), static_cast<int>(Bytes.size()))) {
    llvm::errs() << "Cannot parse '" << Path << "' as a cpp_index message\n";
    return false;
  }
  if (Unit.occurrences_size() == 0) {
    Index Idx;
    if (Idx.ParseFromArray(Bytes.data(), static_cast<int>(Bytes.size())) &&
        Idx.per_file_size() > 0)
      Unit = unitFromIndex(Idx);
  }
  Out = std::move(Unit);
  return true;
}

auto writeMessage(const google::protobuf::Message& Message,
                  llvm::StringRef Path, IndexFormat Format) -> bool {
  std::string Bytes;
  switch (Format) {
    case IndexFormat::Binary: {
      google::protobuf::io::StringOutputStream Stream(&Bytes);
      google::protobuf::io::CodedOutputStream Coded(&Stream);
      Coded.SetSerializationDeterministic(true);
      if (!Message.SerializeToCodedStream(&Coded)) {
        llvm::errs() << "Cannot serialize the index\n";
        return false;
      }
      break;
    }
    case IndexFormat::Text:
      if (!google::protobuf::TextFormat::PrintToString(Message, &Bytes)) {
        llvm::errs() << "Cannot print the index as text\n";
        return false;
      }
      break;
    case IndexFormat::Json: {
      google::protobuf::json::PrintOptions Opts;
      Opts.add_whitespace = true;
      if (!google::protobuf::json::MessageToJsonString(Message, &Bytes, Opts)
               .ok()) {
        llvm::errs() << "Cannot print the index as JSON\n";
        return false;
      }
      break;
    }
  }
  if (Path == "-") {
    llvm::outs() << Bytes;
    llvm::outs().flush();
    return true;
  }
  std::ofstream Out(Path.str(), std::ios::trunc | std::ios::binary);
  Out << Bytes;
  Out.close();
  if (!Out) {
    llvm::errs() << "Cannot write '" << Path << "'\n";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// CLI entry points
// ---------------------------------------------------------------------------

namespace {

/// The merge's progress line.  Called from the merging threads.
class ProgressReporter {
 public:
  ProgressReporter(MergeProgress Mode, size_t Total)
      : Total(Total), Tty(llvm::sys::Process::StandardErrIsDisplayed()) {
    Enabled = Mode == MergeProgress::On || (Mode == MergeProgress::Auto && Tty);
  }

  /// One more input has been read and folded.
  void unitDone() {
    if (!Enabled) return;
    const std::lock_guard<std::mutex> Lock(Mutex);
    ++Done;
    // A terminal redraws one line; a log gets a line per tenth.
    if (!Tty && Done != Total &&
        (Done * 10) / Total == ((Done - 1) * 10) / Total)
      return;
    std::string Line =
        "cpp_format: merging the index: " + std::to_string(Done) + "/" +
        std::to_string(Total) + " units";
    draw(Line);
  }

  /// A step that is not counted in units.
  void step(const std::string& What) {
    if (!Enabled) return;
    const std::lock_guard<std::mutex> Lock(Mutex);
    draw("cpp_format: merging the index: " + What);
  }

  /// Ends the redrawn line, so what is printed next starts on its own.
  void finish() {
    if (!Enabled || !Tty) return;
    const std::lock_guard<std::mutex> Lock(Mutex);
    llvm::errs() << "\r\033[K";
    llvm::errs().flush();
  }

 private:
  void draw(const std::string& Line) {
    if (Tty)
      llvm::errs() << "\r\033[K" << Line;
    else
      llvm::errs() << Line << "\n";
    llvm::errs().flush();
  }

  size_t Total;
  bool Tty;
  bool Enabled = false;
  std::mutex Mutex;
  size_t Done = 0;
};

/// How many inputs a thread reads before it folds them into its partial unit:
/// enough that the sort in normalizeUnit() is not repeated per input, few
/// enough that a thread never holds more than this many parsed units.
constexpr size_t kFoldBatch = 16;

}  // namespace

auto mergeUnitFiles(const std::vector<std::string>& Paths,
                    const MergeOptions& Opts, IndexUnit& Out) -> bool {
  ProgressReporter Progress(Opts.Progress, Paths.size());
  const unsigned Cpus = std::max(1u, std::thread::hardware_concurrency());
  const size_t Threads = std::max<size_t>(
      1, std::min<size_t>(Opts.Jobs == 0 ? Cpus : Opts.Jobs,
                          (Paths.size() + kFoldBatch - 1) / kFoldBatch));

  // Contiguous runs of the inputs, of about equal size in bytes (a unit's cost
  // is its size, and they differ by orders of magnitude).
  std::vector<uint64_t> Sizes;
  uint64_t TotalBytes = 0;
  for (const std::string& P : Paths) {
    uint64_t Size = 0;
    if (llvm::sys::fs::file_size(P, Size)) Size = 0;  // readUnit() will say why
    Sizes.push_back(Size + 1);
    TotalBytes += Size + 1;
  }
  std::vector<std::pair<size_t, size_t>> Runs;  // [begin, end)
  {
    size_t Begin = 0;
    uint64_t Seen = 0;
    for (size_t I = 0; I < Paths.size(); ++I) {
      Seen += Sizes[I];
      if (Runs.size() + 1 < Threads &&
          Seen >= TotalBytes * (Runs.size() + 1) / Threads) {
        Runs.emplace_back(Begin, I + 1);
        Begin = I + 1;
      }
    }
    if (Begin < Paths.size() || Runs.empty())
      Runs.emplace_back(Begin, Paths.size());
  }

  std::vector<IndexUnit> Partial(Runs.size());
  std::atomic<bool> Failed{false};
  const auto foldRun = [&](size_t R) {
    std::vector<IndexUnit> Batch;  // [0] is what this run has folded so far
    Batch.emplace_back();
    const auto fold = [&] {
      if (Batch.size() == 1) return;
      IndexUnit Folded = mergeUnits(Batch);
      Batch.clear();
      Batch.push_back(std::move(Folded));
    };
    for (size_t I = Runs[R].first; I < Runs[R].second && !Failed; ++I) {
      IndexUnit U;
      if (!readUnit(Paths[I], U)) {
        Failed = true;
        return;
      }
      Batch.push_back(std::move(U));
      if (Batch.size() > kFoldBatch) fold();
      Progress.unitDone();
    }
    fold();
    Partial[R] = std::move(Batch.front());
  };
  if (Runs.size() == 1) {
    foldRun(0);
  } else {
    std::vector<std::thread> Workers;
    for (size_t R = 0; R < Runs.size(); ++R) Workers.emplace_back(foldRun, R);
    for (std::thread& W : Workers) W.join();
  }
  if (Failed) {
    Progress.finish();
    return false;
  }
  if (Partial.size() == 1) {
    Out = std::move(Partial.front());
  } else {
    Progress.step("combining " + std::to_string(Partial.size()) +
                  " partial indexes");
    Out = mergeUnits(Partial);
  }
  Progress.finish();
  return true;
}

auto runMergeIndex(const std::vector<std::string>& InputPaths,
                   llvm::StringRef OutputPath, IndexFormat Format,
                   const MergeOptions& Opts) -> int {
  IndexUnit Unit;
  if (!mergeUnitFiles(InputPaths, Opts, Unit)) return 2;
  linkGenerated(Unit);
  const Index Merged = buildIndex(Unit);
  return writeMessage(Merged, OutputPath, Format) ? 0 : 1;
}

namespace {

void describe(const Index& Idx, const Occurrence& O, llvm::raw_ostream& OS) {
  OS << Idx.files(O.file()).path() << ":" << O.begin() << "-" << O.end() << " "
     << roleNames(O.roles());
  if (O.macro() == cpp_index::MACRO_ARGUMENT) OS << " macro-argument";
  if (O.macro() == cpp_index::MACRO_BODY) OS << " macro-body";
}

}  // namespace

auto runDumpIndex(llvm::StringRef Path, IndexFormat Format,
                  const std::optional<std::pair<std::string, uint32_t>>& Lookup,
                  llvm::raw_ostream& OS) -> int {
  IndexUnit Unit;
  if (!readUnit(Path, Unit)) return 2;
  normalizeUnit(Unit);
  const Index Idx = buildIndex(Unit);
  if (!Lookup) return writeMessage(Idx, "-", Format) ? 0 : 1;

  const std::vector<const Occurrence*> At =
      lookup(Idx, Lookup->first, Lookup->second);
  if (At.empty()) {
    // A dependent token no indexed TU instantiates is known but unresolved;
    // say so rather than "no symbol".
    for (const cpp_index::DependentToken& P : Idx.unresolved()) {
      if (Idx.files(P.file()).path() != Lookup->first ||
          Lookup->second < P.begin() || Lookup->second >= P.end())
        continue;
      OS << "dependent token '" << P.name() << "' at " << Lookup->first << ":"
         << P.begin() << "-" << P.end()
         << " is unresolved: no indexed translation unit instantiates its "
            "template\n";
      return 0;
    }
    OS << "no symbol at " << Lookup->first << ":" << Lookup->second << "\n";
    return 0;
  }
  // One block per distinct symbol at the token: the symbol, then every
  // occurrence of it in the index.
  std::vector<int32_t> Seen;
  for (const Occurrence* O : At) {
    if (std::find(Seen.begin(), Seen.end(), O->symbol()) != Seen.end())
      continue;
    Seen.push_back(O->symbol());
    const Symbol& S = Idx.symbols(O->symbol());
    OS << S.usr() << "\n";
    OS << "  " << cpp_index::SymbolKind_Name(S.kind()) << " "
       << S.qualified_name();
    if (!S.type().empty()) OS << " : " << S.type();
    OS << "\n";
    if (S.has_canonical())
      OS << "  canonical " << Idx.files(S.canonical().file()).path() << ":"
         << S.canonical().begin() << "-" << S.canonical().end() << "\n";
    // Provenance, both ways: what this was generated from, and what was
    // generated from this.
    for (const Relation& R : S.relations())
      if (R.kind() == cpp_index::GENERATED_FROM)
        OS << "  generated from " << Idx.symbols(R.symbol()).usr() << "\n";
    for (const Symbol& Other : Idx.symbols())
      for (const Relation& R : Other.relations())
        if (R.kind() == cpp_index::GENERATED_FROM && R.symbol() == O->symbol())
          OS << "  generates " << Other.usr() << "\n";
    for (const FileOccurrences& FO : Idx.per_file())
      for (const Occurrence& Occ : FO.occurrences()) {
        if (Occ.symbol() != O->symbol()) continue;
        OS << "  ";
        describe(Idx, Occ, OS);
        OS << "\n";
      }
  }
  return 0;
}
