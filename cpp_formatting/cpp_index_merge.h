#ifndef CPP_FORMATTING_CPP_INDEX_MERGE_H_
#define CPP_FORMATTING_CPP_INDEX_MERGE_H_

// The Clang-free half of the symbol index: normalizing, merging, grouping and
// serializing `cpp_index` messages (see index.proto).  Depends on protobuf and
// llvm:Support only, so it sits at the lint_lib layer -- `--merge-index` and
// `--dump-index` never parse any C++.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cpp_formatting/index.pb.h"
#include "google/protobuf/message.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

/// The `schema_version` every producer of this repository writes.
constexpr uint32_t kIndexSchemaVersion = 1;

/// How a message is written to or read from a file.
enum class IndexFormat { Binary, Text, Json };

/// Parses `binary`, `text` or `json`.
auto parseIndexFormat(llvm::StringRef Name, IndexFormat& Out) -> bool;

/// The kind of the file an index records as \p Path -- from the path alone,
/// so that no two units (or producers) ever disagree about a file: absolute is
/// SYSTEM, `bazel-out/` GENERATED, `external/` EXTERNAL, anything else SOURCE.
auto fileKindForPath(llvm::StringRef Path) -> cpp_index::FileKind;

// ---------------------------------------------------------------------------
// Normalization and merging
// ---------------------------------------------------------------------------

/// Puts a unit into canonical form: files sorted by path, symbols by USR,
/// occurrences by (file, begin, end, symbol, roles, macro), each list unique,
/// every index remapped, and duplicates merged (a symbol's empty fields are
/// filled from its duplicate, relations and attributes unioned).  An entry
/// whose file or symbol index is out of range is dropped.  Idempotent, and the
/// serialized bytes of a normalized unit depend only on its content.
void normalizeUnit(cpp_index::IndexUnit& Unit);

/// The union of several units, normalized -- so independent of their order.
auto mergeUnits(const std::vector<cpp_index::IndexUnit>& Units)
    -> cpp_index::IndexUnit;

/// The one rule that crosses languages (see index.proto): every symbol whose
/// `canonical` range is exactly the range of a `GENERATES` occurrence -- an
/// anchor a producer put on the generated file -- gets a `GENERATED_FROM`
/// relation to the anchor's symbol.  \p Unit must be normalized and stays so.
/// A pure function of the content that only ever adds relations between
/// symbols the unit already has, so it is idempotent, a linked index is a
/// valid merge input, and a unit without anchors is left untouched byte for
/// byte.  Returns the number of relations added.
auto linkGenerated(cpp_index::IndexUnit& Unit) -> size_t;

/// Groups a (normalized) unit's occurrences per file, for offset lookup.
auto buildIndex(const cpp_index::IndexUnit& Unit) -> cpp_index::Index;

/// The inverse of buildIndex: flattens `per_file` back into `occurrences`, so
/// an index can be merged with further units.
auto unitFromIndex(const cpp_index::Index& Index) -> cpp_index::IndexUnit;

/// Every occurrence in \p Path whose range contains \p Offset.  Empty when
/// the file is unknown.
auto lookup(const cpp_index::Index& Index, llvm::StringRef Path,
            uint32_t Offset) -> std::vector<const cpp_index::Occurrence*>;

/// The `|`-joined names of the Role bits set in \p Roles (`ROLE_NONE` when
/// none is).
auto roleNames(uint32_t Roles) -> std::string;

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

/// Reads a unit or an index (either message, in binary form) from \p Path
/// into a unit.  Prints a diagnostic and returns false when the file cannot be
/// read or parsed.
auto readUnit(llvm::StringRef Path, cpp_index::IndexUnit& Out) -> bool;

/// Writes \p Message to \p Path (`-` for stdout) in the given format.  Prints
/// a diagnostic and returns false on failure.
auto writeMessage(const google::protobuf::Message& Message,
                  llvm::StringRef Path, IndexFormat Format) -> bool;

// ---------------------------------------------------------------------------
// CLI entry points
// ---------------------------------------------------------------------------

/// Whether `--merge-index` reports how far it is, on stderr.
enum class MergeProgress {
  Auto,  ///< when stderr is a terminal: one line, redrawn
  On,    ///< always; a line per step when stderr is not a terminal
  Off,
};

struct MergeOptions {
  unsigned Jobs = 0;  ///< threads; 0 = one per CPU
  MergeProgress Progress = MergeProgress::Auto;
};

/// Reads and merges the units at \p Paths on `Opts.Jobs` threads.  The merge
/// is a reduction: each thread folds a contiguous run of the inputs into one
/// normalized unit, and the partial units are merged in input order -- which
/// keeps the one order-dependent rule (a symbol's empty field is filled from
/// the *first* duplicate that has it) exactly as mergeUnits() over all of them
/// applies it, so the result is byte-identical for any number of threads.
/// Returns false (after a diagnostic) when an input cannot be read.
auto mergeUnitFiles(const std::vector<std::string>& Paths,
                    const MergeOptions& Opts, cpp_index::IndexUnit& Out)
    -> bool;

/// `cpp_format --merge-index`: reads every input, merges, links generated
/// symbols to what they were generated from (linkGenerated), groups per file
/// and writes the Index to \p OutputPath.  Exit code: 0, or 2 when an input
/// cannot be read, 1 when the output cannot be written.
auto runMergeIndex(const std::vector<std::string>& InputPaths,
                   llvm::StringRef OutputPath, IndexFormat Format,
                   const MergeOptions& Opts = {}) -> int;

/// `cpp_format --dump-index`: reads one unit or index and either prints it in
/// \p Format, or, given \p Lookup = (path, offset), prints the symbol(s) at
/// that token and every occurrence of each across the index.  Exit code: 0,
/// or 2 when the input cannot be read.
auto runDumpIndex(llvm::StringRef Path, IndexFormat Format,
                  const std::optional<std::pair<std::string, uint32_t>>& Lookup,
                  llvm::raw_ostream& OS) -> int;

#endif  // CPP_FORMATTING_CPP_INDEX_MERGE_H_
