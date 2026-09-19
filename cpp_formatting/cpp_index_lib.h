#ifndef CPP_FORMATTING_CPP_INDEX_LIB_H_
#define CPP_FORMATTING_CPP_INDEX_LIB_H_

// The per-translation-unit half of the symbol index: an IndexDataConsumer for
// Clang's own indexing library that turns what it reports into a
// cpp_index.IndexUnit, and the TU driver client that runs it.  See
// index.proto for the schema and cpp_index_merge.h for what happens to the
// units afterwards.

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "clang/Frontend/FrontendAction.h"
#include "clang/Index/IndexingOptions.h"
#include "clang/Tooling/Tooling.h"
#include "cpp_formatting/index.pb.h"
#include "cpp_formatting/rename_variables_lib.h"  // FileSet
#include "cpp_formatting/tu_driver.h"
#include "llvm/ADT/StringRef.h"

/// The indexing options every unit is produced with.  System symbols are NOT
/// filtered by Clang: a Bazel `cc_library(includes = [...])` becomes
/// `-isystem`, so a first-party header is a system header in every dependent's
/// TU, and the default filter would drop its occurrences before the consumer
/// ever saw them.  The consumer applies the owned-file rule instead.
auto makeIndexingOptions() -> clang::index::IndexingOptions;

/// The frontend action that indexes one TU into \p Out (the serialized
/// IndexUnit, written when the TU ends).  Occurrences are recorded for the
/// main file and for every file in \p Owned (real paths; an in-memory file
/// with no real path matches by its name); every symbol referenced from one
/// of those files is recorded whatever file declares it.
auto createIndexAction(const FileSet& Owned, std::string* Out)
    -> std::unique_ptr<clang::FrontendAction>;

/// The TU driver's client for `--emit-index`.  Purely local to one TU: no
/// cross-TU state, nothing to seed, nothing that can veto.  finish() merges
/// the slots' units in slot order (the merge is order-independent anyway).
class IndexActionFactory : public TUSlotClient {
 public:
  explicit IndexActionFactory(FileSet Owned) : Owned(std::move(Owned)) {}

  // TUSlotClient
  auto ruleCount() const -> size_t override { return 0; }
  auto createAction(TUSlot& Slot)
      -> std::unique_ptr<clang::FrontendAction> override {
    return createIndexAction(Owned, &Slot.IndexBytes);
  }
  auto shared() -> CrossTUState& override { return Shared; }
  auto rerunNeededOnVeto() const -> bool override { return false; }
  auto crossTUSeeding() const -> bool override { return false; }
  void finish(std::vector<TUSlot>& Slots) override;

  /// The merged unit; valid after finish().
  auto unit() const -> const cpp_index::IndexUnit& { return Merged; }

  /// Writes the merged unit (binary) to \p Path.  Prints a diagnostic and
  /// returns false on failure.
  auto writeUnit(llvm::StringRef Path) const -> bool;

 private:
  FileSet Owned;
  CrossTUState Shared;  // always empty
  cpp_index::IndexUnit Merged;
};

/// Test helper: indexes an in-memory TU (`input.cc`) and returns its
/// normalized unit.  \p VirtualFiles are additional in-memory files it may
/// include; \p OwnedNames are the ones (by that name) whose occurrences are
/// recorded besides the main file's.
auto indexCode(llvm::StringRef Code,
               const std::vector<std::string>& Args = {"-std=c++17"},
               const clang::tooling::FileContentMappings& VirtualFiles = {},
               const std::vector<std::string>& OwnedNames = {})
    -> cpp_index::IndexUnit;

#endif  // CPP_FORMATTING_CPP_INDEX_LIB_H_
