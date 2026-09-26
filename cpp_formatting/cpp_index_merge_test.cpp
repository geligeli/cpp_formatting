#include "cpp_formatting/cpp_index_merge.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <string>
#include <system_error>
#include <vector>

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

using cpp_index::File;
using cpp_index::Index;
using cpp_index::IndexUnit;
using cpp_index::Occurrence;
using cpp_index::Symbol;

namespace {

auto addFile(IndexUnit& U, const std::string& Path,
             cpp_index::FileKind Kind = cpp_index::SOURCE) -> int32_t {
  File* F = U.add_files();
  F->set_path(Path);
  F->set_kind(Kind);
  return U.files_size() - 1;
}

auto addSymbol(IndexUnit& U, const std::string& Usr,
               const std::string& Name = "") -> int32_t {
  Symbol* S = U.add_symbols();
  S->set_usr(Usr);
  S->set_name(Name);
  return U.symbols_size() - 1;
}

auto addOcc(IndexUnit& U, int32_t File, uint32_t Begin, uint32_t End,
            int32_t Sym, uint32_t Roles,
            cpp_index::MacroContext Macro = cpp_index::NOT_IN_MACRO)
    -> Occurrence* {
  Occurrence* O = U.add_occurrences();
  O->set_file(File);
  O->set_begin(Begin);
  O->set_end(End);
  O->set_symbol(Sym);
  O->set_roles(Roles);
  O->set_macro(Macro);
  return O;
}

auto bytes(const IndexUnit& U) -> std::string {
  std::string Out;
  U.SerializeToString(&Out);
  return Out;
}

}  // namespace

TEST(IndexMerge, NormalizeSortsTablesAndRemapsIndexes) {
  IndexUnit U;
  const int32_t B = addFile(U, "b.h");
  const int32_t A = addFile(U, "a.cpp");
  const int32_t Y = addSymbol(U, "c:@y");
  const int32_t X = addSymbol(U, "c:@x");
  addOcc(U, B, 10, 12, X, cpp_index::REFERENCE);
  addOcc(U, A, 3, 4, Y, cpp_index::DEFINITION);
  U.mutable_symbols(X)->mutable_canonical()->set_file(A);
  U.mutable_symbols(X)->add_relations()->set_kind(cpp_index::CHILD_OF);
  U.mutable_symbols(X)->mutable_relations(0)->set_symbol(Y);

  normalizeUnit(U);

  ASSERT_EQ(U.files_size(), 2);
  EXPECT_EQ(U.files(0).path(), "a.cpp");
  EXPECT_EQ(U.files(1).path(), "b.h");
  ASSERT_EQ(U.symbols_size(), 2);
  EXPECT_EQ(U.symbols(0).usr(), "c:@x");
  EXPECT_EQ(U.symbols(1).usr(), "c:@y");
  // X's canonical pointed at a.cpp (old 1) -> new 0; its relation at Y (old 0)
  // -> new 1.
  EXPECT_EQ(U.symbols(0).canonical().file(), 0);
  EXPECT_EQ(U.symbols(0).relations(0).symbol(), 1);
  ASSERT_EQ(U.occurrences_size(), 2);
  // Sorted by file: a.cpp's occurrence (of Y) first.
  EXPECT_EQ(U.occurrences(0).file(), 0);
  EXPECT_EQ(U.occurrences(0).symbol(), 1);
  EXPECT_EQ(U.occurrences(1).file(), 1);
  EXPECT_EQ(U.occurrences(1).symbol(), 0);
  EXPECT_EQ(U.schema_version(), kIndexSchemaVersion);
}

TEST(IndexMerge, NormalizeIsIdempotent) {
  IndexUnit U;
  addFile(U, "z.cpp");
  addFile(U, "a.cpp");
  addSymbol(U, "c:@b");
  addSymbol(U, "c:@a");
  addOcc(U, 0, 1, 2, 0, cpp_index::REFERENCE);
  addOcc(U, 1, 5, 6, 1, cpp_index::DEFINITION);
  normalizeUnit(U);
  const std::string Once = bytes(U);
  normalizeUnit(U);
  EXPECT_EQ(bytes(U), Once);
}

TEST(IndexMerge, DropsEntriesWithInvalidIndexes) {
  IndexUnit U;
  addFile(U, "a.cpp");
  addSymbol(U, "c:@x");
  addOcc(U, 5, 1, 2, 0, cpp_index::REFERENCE);  // no such file
  addOcc(U, 0, 1, 2, 7, cpp_index::REFERENCE);  // no such symbol
  addOcc(U, 0, 1, 2, 0, cpp_index::REFERENCE);  // fine
  U.mutable_symbols(0)->mutable_canonical()->set_file(9);
  normalizeUnit(U);
  ASSERT_EQ(U.occurrences_size(), 1);
  EXPECT_FALSE(U.symbols(0).has_canonical());
}

TEST(IndexMerge, MergeDedupsOccurrencesAndFillsSymbolFields) {
  // Unit 1: the header's own TU sees the definition and knows the type.
  IndexUnit U1;
  addFile(U1, "w.h");
  addSymbol(U1, "c:@S@W@FI@n", "n");
  U1.mutable_symbols(0)->set_type("int");
  U1.mutable_symbols(0)->set_kind(cpp_index::FIELD);
  addOcc(U1, 0, 20, 21, 0, cpp_index::DEFINITION);
  // Unit 2: a .cpp that includes w.h re-emits the header's definition (an
  // owned file) and adds its own reference; its symbol entry is bare.
  IndexUnit U2;
  addFile(U2, "w.cpp");
  addFile(U2, "w.h");
  addSymbol(U2, "c:@S@W@FI@n", "n");
  addOcc(U2, 1, 20, 21, 0, cpp_index::DEFINITION);
  addOcc(U2, 0, 40, 41, 0, cpp_index::REFERENCE | cpp_index::READ);

  const IndexUnit M = mergeUnits({U1, U2});
  ASSERT_EQ(M.files_size(), 2);
  ASSERT_EQ(M.symbols_size(), 1);
  EXPECT_EQ(M.symbols(0).type(), "int");
  EXPECT_EQ(M.symbols(0).kind(), cpp_index::FIELD);
  ASSERT_EQ(M.occurrences_size(), 2);
  EXPECT_EQ(M.files(M.occurrences(0).file()).path(), "w.cpp");
  EXPECT_EQ(M.occurrences(0).roles(),
            static_cast<uint32_t>(cpp_index::REFERENCE | cpp_index::READ));
  EXPECT_EQ(M.files(M.occurrences(1).file()).path(), "w.h");
  EXPECT_EQ(M.occurrences(1).roles(),
            static_cast<uint32_t>(cpp_index::DEFINITION));
}

TEST(IndexMerge, MergeIsIndependentOfInputOrder) {
  IndexUnit U1;
  addFile(U1, "b.cpp");
  addSymbol(U1, "c:@f");
  U1.mutable_symbols(0)->set_name("f");
  addOcc(U1, 0, 1, 2, 0, cpp_index::REFERENCE);
  IndexUnit U2;
  addFile(U2, "a.cpp");
  addSymbol(U2, "c:@g");
  addSymbol(U2, "c:@f");
  U2.mutable_symbols(1)->set_type("void ()");
  addOcc(U2, 0, 7, 8, 1, cpp_index::DEFINITION);
  addOcc(U2, 0, 9, 10, 0, cpp_index::DEFINITION);

  EXPECT_EQ(bytes(mergeUnits({U1, U2})), bytes(mergeUnits({U2, U1})));
  const IndexUnit M = mergeUnits({U2, U1});
  ASSERT_EQ(M.symbols_size(), 2);
  EXPECT_EQ(M.symbols(0).usr(), "c:@f");
  EXPECT_EQ(M.symbols(0).name(), "f");
  EXPECT_EQ(M.symbols(0).type(), "void ()");
}

TEST(IndexMerge, FileKindLowestWinsAndAttributesUnion) {
  IndexUnit U1;
  addFile(U1, "x.h", cpp_index::SYSTEM);
  IndexUnit U2;
  addFile(U2, "x.h", cpp_index::SOURCE);
  cpp_index::Attribute* A = U2.mutable_files(0)->add_attributes();
  A->set_key("generated_from");
  A->set_value("x.proto");
  const IndexUnit M = mergeUnits({U1, U2});
  ASSERT_EQ(M.files_size(), 1);
  EXPECT_EQ(M.files(0).kind(), cpp_index::SOURCE);
  ASSERT_EQ(M.files(0).attributes_size(), 1);
  EXPECT_EQ(M.files(0).attributes(0).value(), "x.proto");
  EXPECT_EQ(bytes(mergeUnits({U1, U2})), bytes(mergeUnits({U2, U1})));
}

TEST(IndexMerge, BuildIndexGroupsPerFileAndLookupFindsContainingRanges) {
  IndexUnit U;
  addFile(U, "a.cpp");
  addFile(U, "b.h");
  addSymbol(U, "c:@x");
  addSymbol(U, "c:@y");
  addOcc(U, 0, 4, 10, 0, cpp_index::REFERENCE);
  addOcc(U, 0, 4, 10, 1, cpp_index::REFERENCE);  // two symbols, one token
  addOcc(U, 0, 20, 22, 0, cpp_index::DEFINITION);
  addOcc(U, 1, 0, 3, 1, cpp_index::DEFINITION);
  normalizeUnit(U);

  const Index Idx = buildIndex(U);
  ASSERT_EQ(Idx.per_file_size(), 2);
  EXPECT_EQ(Idx.per_file(0).file(), 0);
  EXPECT_EQ(Idx.per_file(0).occurrences_size(), 3);
  EXPECT_EQ(Idx.per_file(1).occurrences_size(), 1);

  EXPECT_EQ(lookup(Idx, "a.cpp", 4).size(), 2u);
  EXPECT_EQ(lookup(Idx, "a.cpp", 9).size(), 2u);
  EXPECT_EQ(lookup(Idx, "a.cpp", 10).size(), 0u);
  EXPECT_EQ(lookup(Idx, "a.cpp", 21).size(), 1u);
  EXPECT_EQ(lookup(Idx, "b.h", 2).size(), 1u);
  EXPECT_EQ(lookup(Idx, "nope.h", 2).size(), 0u);

  // The inverse gives back the normalized unit, byte for byte.
  EXPECT_EQ(bytes(unitFromIndex(Idx)), bytes(U));
}

TEST(IndexMerge, RoleNames) {
  EXPECT_EQ(roleNames(0), "ROLE_NONE");
  EXPECT_EQ(roleNames(cpp_index::REFERENCE | cpp_index::CALL),
            "REFERENCE|CALL");
  EXPECT_EQ(roleNames(cpp_index::DEFINITION), "DEFINITION");
}

TEST(IndexMerge, ReadUnitAcceptsBothMessages) {
  IndexUnit U;
  addFile(U, "a.cpp");
  addSymbol(U, "c:@x");
  addOcc(U, 0, 1, 2, 0, cpp_index::REFERENCE);
  normalizeUnit(U);

  llvm::SmallString<128> UnitPath;
  llvm::SmallString<128> IndexPath;
  int FD = 0;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("unit", "pb", FD, UnitPath));
  llvm::sys::fs::closeFile(FD);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("index", "pb", FD, IndexPath));
  llvm::sys::fs::closeFile(FD);
  ASSERT_TRUE(writeMessage(U, UnitPath, IndexFormat::Binary));
  ASSERT_TRUE(writeMessage(buildIndex(U), IndexPath, IndexFormat::Binary));

  IndexUnit FromUnit;
  IndexUnit FromIndex;
  ASSERT_TRUE(readUnit(UnitPath, FromUnit));
  ASSERT_TRUE(readUnit(IndexPath, FromIndex));
  EXPECT_EQ(bytes(FromUnit), bytes(U));
  EXPECT_EQ(bytes(FromIndex), bytes(U));

  IndexUnit Missing;
  EXPECT_FALSE(readUnit("/nonexistent/dir/unit.pb", Missing));
  llvm::sys::fs::remove(UnitPath);
  llvm::sys::fs::remove(IndexPath);
}

// A `.txtpb` is a unit in text form -- what the index aspect writes, with no
// tool, to say which files a testonly target owns.  Its attributes join the
// file's entry from the units that indexed it.
TEST(IndexMerge, ReadUnitInTextFormAndItsAttributesUnion) {
  llvm::SmallString<128> TextPath;
  int FD = 0;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("testonly", "txtpb", FD, TextPath));
  {
    llvm::raw_fd_ostream Out(FD, /*shouldClose=*/true);
    Out << "files { path: \"a_test.cpp\" kind: SOURCE "
           "attributes { key: \"testonly\" value: \"cc_test\" } }\n";
  }
  IndexUnit Text;
  ASSERT_TRUE(readUnit(TextPath, Text));
  ASSERT_EQ(Text.files_size(), 1);
  EXPECT_EQ(Text.files(0).path(), "a_test.cpp");
  EXPECT_EQ(Text.files(0).kind(), cpp_index::SOURCE);

  IndexUnit Parsed;
  addFile(Parsed, "a_test.cpp");
  addSymbol(Parsed, "c:@x");
  addOcc(Parsed, 0, 1, 2, 0, cpp_index::REFERENCE);
  const IndexUnit M = mergeUnits({Parsed, Text});
  ASSERT_EQ(M.files_size(), 1);
  ASSERT_EQ(M.files(0).attributes_size(), 1);
  EXPECT_EQ(M.files(0).attributes(0).key(), "testonly");
  EXPECT_EQ(M.occurrences_size(), 1);
  EXPECT_EQ(bytes(M), bytes(mergeUnits({Text, Parsed})));

  // Text that is not a unit is an error, as a bad binary unit is.
  {
    std::error_code EC;
    llvm::raw_fd_ostream Out(TextPath, EC);
    Out << "files { nonsense: 1 }\n";
  }
  IndexUnit Bad;
  EXPECT_FALSE(readUnit(TextPath, Bad));
  llvm::sys::fs::remove(TextPath);
}

// The threaded merge is a reduction over contiguous runs of the inputs, and
// has to give the bytes of the one-shot merge whatever the thread count --
// including for the one order-dependent rule (the first duplicate that has a
// field fills it) and for a pending token that a unit in *another* run
// resolves.
TEST(IndexMerge, MergeUnitFilesIsTheSameForAnyThreadCount) {
  std::vector<IndexUnit> Units;
  std::vector<std::string> Paths;
  for (int I = 0; I < 80; ++I) {
    IndexUnit U;
    U.add_translation_units("tu" + std::to_string(I) + ".cpp");
    const int32_t Common = addFile(U, "common.h");
    const int32_t Own = addFile(U, "tu" + std::to_string(I) + ".cpp");
    // Unnamed in most units; the first unit that names it wins.
    const int32_t Shared =
        addSymbol(U, "c:@shared", I % 3 == 1 ? "name" + std::to_string(I) : "");
    const int32_t Local = addSymbol(U, "c:@local" + std::to_string(I), "local");
    addOcc(U, Common, 10, 16, Shared, cpp_index::DECLARATION);
    addOcc(U, Own, 5, 10, Shared, cpp_index::REFERENCE);
    addOcc(U, Own, 20, 25, Local, cpp_index::DEFINITION);
    if (I == 70) {
      addOcc(U, Common, 40, 45, Local,
             cpp_index::REFERENCE | cpp_index::DEPENDENT);
    } else {
      cpp_index::DependentToken* P = U.add_pending();
      P->set_file(Common);
      P->set_begin(40);
      P->set_end(45);
      P->set_name("dep");
      P = U.add_pending();  // never resolved
      P->set_file(Common);
      P->set_begin(50);
      P->set_end(55);
      P->set_name("open");
    }
    llvm::SmallString<128> Path;
    int FD = 0;
    ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("unit", "pb", FD, Path));
    llvm::sys::fs::closeFile(FD);
    ASSERT_TRUE(writeMessage(U, Path, IndexFormat::Binary));
    Paths.push_back(std::string(Path));
    Units.push_back(std::move(U));
  }
  const IndexUnit Expected = mergeUnits(Units);
  ASSERT_EQ(Expected.pending_size(), 1);
  EXPECT_EQ(Expected.pending(0).name(), "open");
  for (const unsigned Jobs : {1u, 2u, 5u, 64u}) {
    MergeOptions Opts;
    Opts.Jobs = Jobs;
    Opts.Progress = MergeProgress::Off;
    IndexUnit Got;
    ASSERT_TRUE(mergeUnitFiles(Paths, Opts, Got));
    EXPECT_EQ(bytes(Got), bytes(Expected)) << "jobs=" << Jobs;
  }
  bool Named = false;
  for (const Symbol& S : Expected.symbols())
    if (S.usr() == "c:@shared") {
      EXPECT_EQ(S.name(), "name1");
      Named = true;
    }
  EXPECT_TRUE(Named);

  Paths.push_back("/nonexistent/unit.pb");
  IndexUnit Got;
  MergeOptions Opts;
  Opts.Progress = MergeProgress::Off;
  EXPECT_FALSE(mergeUnitFiles(Paths, Opts, Got));
  Paths.pop_back();
  for (const std::string& P : Paths) llvm::sys::fs::remove(P);
}

TEST(IndexMerge, UnknownEnumValueSurvivesARoundTrip) {
  IndexUnit U;
  addFile(U, "a.cpp");
  addSymbol(U, "c:@x");
  U.mutable_symbols(0)->set_kind(static_cast<cpp_index::SymbolKind>(999));
  IndexUnit Back;
  ASSERT_TRUE(Back.ParseFromString(bytes(U)));
  EXPECT_EQ(static_cast<int>(Back.symbols(0).kind()), 999);
  normalizeUnit(Back);
  EXPECT_EQ(static_cast<int>(Back.symbols(0).kind()), 999);
}

TEST(IndexMerge, PendingDependentTokensAreDroppedOnceResolved) {
  // The header's own unit: it spells `t.m` but instantiates nothing.
  IndexUnit Owner;
  addFile(Owner, "w.h");
  cpp_index::DependentToken* P = Owner.add_pending();
  P->set_file(0);
  P->set_begin(10);
  P->set_end(11);
  P->set_name("m");
  // A duplicate from a second owner-side unit is one entry.
  IndexUnit Owner2 = Owner;

  // A .cpp that instantiates the template binds the token.
  IndexUnit User;
  addFile(User, "w.cpp");
  addFile(User, "w.h");
  addSymbol(User, "c:@S@A@FI@m", "m");
  addOcc(User, 1, 10, 11, 0, cpp_index::REFERENCE | cpp_index::DEPENDENT);

  // Alone, the token stays pending, survives the index round trip, and is
  // reported as unresolved.
  const IndexUnit Alone = mergeUnits({Owner, Owner2});
  ASSERT_EQ(Alone.pending_size(), 1);
  EXPECT_EQ(Alone.pending(0).name(), "m");
  const Index Idx = buildIndex(Alone);
  ASSERT_EQ(Idx.unresolved_size(), 1);
  EXPECT_EQ(bytes(unitFromIndex(Idx)), bytes(Alone));

  // With the binding, the pending entry is gone whichever order the units
  // come in.
  const IndexUnit Both = mergeUnits({Owner, User});
  EXPECT_EQ(Both.pending_size(), 0);
  ASSERT_EQ(Both.occurrences_size(), 1);
  EXPECT_EQ(Both.files(Both.occurrences(0).file()).path(), "w.h");
  EXPECT_EQ(bytes(Both), bytes(mergeUnits({User, Owner})));
  EXPECT_EQ(buildIndex(Both).unresolved_size(), 0);

  // A pending entry whose file index is out of range is dropped.
  IndexUnit Bad;
  addFile(Bad, "a.cpp");
  Bad.add_pending()->set_file(3);
  normalizeUnit(Bad);
  EXPECT_EQ(Bad.pending_size(), 0);
}

namespace {

/// A proto-ish unit: `size` defined in w.proto, anchored on three ranges of
/// the generated header.
auto anchorUnit() -> IndexUnit {
  IndexUnit U;
  const int32_t Proto = addFile(U, "w.proto");
  const int32_t Header = addFile(U, "bazel-out/w.pb.h", cpp_index::GENERATED);
  const int32_t Size = addSymbol(U, "proto:demo.W.size", "size");
  U.mutable_symbols(Size)->mutable_canonical()->set_file(Proto);
  U.mutable_symbols(Size)->mutable_canonical()->set_begin(40);
  U.mutable_symbols(Size)->mutable_canonical()->set_end(44);
  addOcc(U, Proto, 40, 44, Size, cpp_index::DEFINITION);
  addOcc(U, Header, 100, 104, Size, cpp_index::GENERATES);
  addOcc(U, Header, 200, 208, Size, cpp_index::GENERATES | cpp_index::WRITE);
  addOcc(U, Header, 300, 310, Size, cpp_index::GENERATES);
  return U;
}

/// A C++-ish unit: the getter and the setter declared on two of those
/// ranges, a third symbol declared next to one, and a user of the setter.
auto generatedUnit() -> IndexUnit {
  IndexUnit U;
  const int32_t Header = addFile(U, "bazel-out/w.pb.h", cpp_index::GENERATED);
  const int32_t User = addFile(U, "user.cpp");
  auto Declared = [&](const std::string& Usr, uint32_t Begin, uint32_t End) {
    const int32_t S = addSymbol(U, Usr, Usr);
    U.mutable_symbols(S)->mutable_canonical()->set_file(Header);
    U.mutable_symbols(S)->mutable_canonical()->set_begin(Begin);
    U.mutable_symbols(S)->mutable_canonical()->set_end(End);
    return S;
  };
  Declared("c:@S@W@F@size#", 100, 104);
  const int32_t Setter = Declared("c:@S@W@F@set_size#", 200, 208);
  Declared("c:@S@W@F@swap#", 201, 208);  // overlaps an anchor, is not one
  addOcc(U, User, 7, 15, Setter, cpp_index::REFERENCE | cpp_index::CALL);
  return U;
}

auto generatedFrom(const IndexUnit& U, const std::string& Usr)
    -> std::vector<std::string> {
  std::vector<std::string> Out;
  for (const Symbol& S : U.symbols())
    if (S.usr() == Usr)
      for (const cpp_index::Relation& R : S.relations())
        if (R.kind() == cpp_index::GENERATED_FROM)
          Out.push_back(U.symbols(R.symbol()).usr());
  return Out;
}

}  // namespace

// The one rule that crosses languages: a symbol declared on exactly the range
// of a GENERATES anchor was generated from the anchor's symbol.
TEST(IndexMerge, LinkGeneratedJoinsCanonicalRangesWithAnchors) {
  IndexUnit U = mergeUnits({anchorUnit(), generatedUnit()});
  EXPECT_EQ(linkGenerated(U), 2u);
  EXPECT_EQ(generatedFrom(U, "c:@S@W@F@size#"),
            std::vector<std::string>{"proto:demo.W.size"});
  EXPECT_EQ(generatedFrom(U, "c:@S@W@F@set_size#"),
            std::vector<std::string>{"proto:demo.W.size"});
  EXPECT_TRUE(generatedFrom(U, "c:@S@W@F@swap#").empty());
  // The anchor's own symbol is declared in the .proto, not generated.
  EXPECT_TRUE(generatedFrom(U, "proto:demo.W.size").empty());

  // Still canonical, and a fixpoint: linking again, or merging the linked
  // unit again (an index is a merge input), changes nothing.
  const std::string Linked = bytes(U);
  IndexUnit Again = U;
  normalizeUnit(Again);
  EXPECT_EQ(bytes(Again), Linked);
  EXPECT_EQ(linkGenerated(Again), 0u);
  EXPECT_EQ(bytes(Again), Linked);
  IndexUnit Remerged = mergeUnits({unitFromIndex(buildIndex(U))});
  linkGenerated(Remerged);
  EXPECT_EQ(bytes(Remerged), Linked);
}

TEST(IndexMerge, LinkGeneratedLeavesAUnitWithoutAnchorsAlone) {
  IndexUnit U = mergeUnits({generatedUnit()});
  const std::string Before = bytes(U);
  EXPECT_EQ(linkGenerated(U), 0u);
  EXPECT_EQ(bytes(U), Before);

  // Anchors with nothing declared on them are not an error either.
  IndexUnit Anchors = mergeUnits({anchorUnit()});
  const std::string AnchorsBefore = bytes(Anchors);
  EXPECT_EQ(linkGenerated(Anchors), 0u);
  EXPECT_EQ(bytes(Anchors), AnchorsBefore);
}

// Linking only adds relations between symbols that are there, so it does not
// matter when it happens: linking a partial index and merging the rest later
// ends where merging everything and linking once does, in any order.
TEST(IndexMerge, LinkGeneratedIsMonotoneAndOrderIndependent) {
  auto Link = [](IndexUnit U) {
    linkGenerated(U);
    return U;
  };
  const std::string All =
      bytes(Link(mergeUnits({anchorUnit(), generatedUnit()})));
  EXPECT_EQ(bytes(Link(mergeUnits({generatedUnit(), anchorUnit()}))), All);
  EXPECT_EQ(bytes(Link(mergeUnits(
                {Link(mergeUnits({anchorUnit()})), generatedUnit()}))),
            All);
  EXPECT_EQ(bytes(Link(mergeUnits(
                {Link(mergeUnits({generatedUnit()})), anchorUnit()}))),
            All);

  // Two producers anchoring one range (a field and the oneof case it is) give
  // two relations, sorted.
  IndexUnit Second;
  const int32_t Header =
      addFile(Second, "bazel-out/w.pb.h", cpp_index::GENERATED);
  const int32_t Case = addSymbol(Second, "proto:demo.W.choice", "choice");
  addOcc(Second, Header, 100, 104, Case, cpp_index::GENERATES);
  IndexUnit Both = mergeUnits({anchorUnit(), Second, generatedUnit()});
  EXPECT_EQ(linkGenerated(Both), 3u);
  EXPECT_EQ(
      generatedFrom(Both, "c:@S@W@F@size#"),
      (std::vector<std::string>{"proto:demo.W.choice", "proto:demo.W.size"}));
}

TEST(IndexMerge, GeneratesIsARoleName) {
  EXPECT_EQ(roleNames(cpp_index::GENERATES | cpp_index::WRITE),
            "WRITE|GENERATES");
}
