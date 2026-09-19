#ifndef CPP_FORMATTING_TU_DRIVER_H_
#define CPP_FORMATTING_TU_DRIVER_H_

#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/ArgumentsAdjusters.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "cpp_formatting/lint_lib.h"
#include "cpp_formatting/rename_state.h"
#include "llvm/ADT/StringRef.h"

// ---------------------------------------------------------------------------
// Running a rewrite over many translation units, in parallel
// ---------------------------------------------------------------------------
//
// Each translation unit is parsed by its own ClangTool on a worker thread and
// writes everything it produces into its own TUSlot: the rewritten main file,
// edit records, lint diagnostics, reported conflicts, and the two maps that
// other TUs need to see -- the vetoes it discovered and the dependent-token
// resolutions its instantiations recorded.  Nothing is shared while a TU runs.
//
// What other TUs need to see is exchanged at barriers, always in source order,
// so the result is deterministic and identical for any number of threads:
//
//   * Two phases.  Non-headers run first, then headers.  Between the two, the
//     resolutions every .cpp recorded are merged into the shared state, and
//     each header TU is seeded with a copy of it before it runs -- the header
//     is where a dependent token is spelled, the .cpp files are where it is
//     resolved.  This is the ordering orderSourcesForRename() always imposed.
//
//   * Re-run on a veto.  A veto found in one TU invalidates what another TU
//     already buffered, so when a pass discovers any, the whole pass is
//     discarded and run again with the vetoes known up front (the dependent
//     resolutions are cleared too: a member that stopped being renamed would
//     otherwise leave a stale entry that no TU overwrites).  Emit mode skips
//     this: its records carry the vetoes and aggregation drops the edits.
//
//   * Re-run a stale TU.  After a pass with no new vetoes, a TU whose main
//     file spells a dependent token that other TUs resolved differently from
//     the map it was seeded with -- a header instantiated only from another
//     header, or a .cpp whose template is instantiated only from a .cpp that
//     includes it -- is run again with the now complete map.  Only that TU is
//     re-parsed, and the map is kept.  This is what makes the outcome
//     independent of the source order.
//
// Per-TU outputs (Pending, Edits, Conflicts, Report) are merged only once, at
// the very end, by the client's finish(), so a TU that is run again simply
// replaces its slot.

/// Everything one translation unit produces.  Written only by that TU's
/// action, on whichever thread runs it; read by the driver at barriers.
struct TUSlot {
  std::string Source;            ///< the path as given on the command line
  std::string MainFileRealPath;  ///< real path of Source (absolute if unknown)
  bool IsHeader = false;

  PendingRewrites Pending;
  EditReport Edits;
  RenameConflicts Conflicts;
  /// Seeded from the shared state when the TU starts; the scan pass appends.
  RenameVetoes Vetoes;
  /// Old spellings of every declaration this TU set out to rename, so the
  /// driver can tell which never-resolved dependent tokens matter.
  std::set<std::string> RenamedNames;
  /// One map per rule (see TUSlotClient::ruleCount), seeded from the shared
  /// state when the TU starts; this TU's instantiations append.
  std::vector<DependentResolutions> DepRes;
  LintReport Report;
  /// The serialized cpp_index.IndexUnit of this TU (`--emit-index`).  Kept as
  /// bytes so the driver -- and the three binaries that never index -- do not
  /// depend on protobuf; the index client parses it back in finish().
  std::string IndexBytes;
  std::string Diagnostics;  ///< buffered Clang stderr (parallel batches)
  int Rc = 0;               ///< ClangTool::run's result for this TU

  void clearOutputs();
};

/// The state every TU contributes to and later TUs are seeded from.  Touched
/// only by the driver thread, at barriers.
struct CrossTUState {
  RenameVetoes Vetoes;
  std::vector<DependentResolutions> DepResPerRule;
  std::set<std::string> RenamedNames;  ///< union over every TU
};

/// What the driver needs from a tool: how to build a per-TU action that writes
/// into a slot, and how to fold the finished slots into the tool's outputs.
class TUSlotClient {
 public:
  virtual ~TUSlotClient() = default;

  /// Number of DependentResolutions maps each slot carries (one per rule).
  virtual auto ruleCount() const -> size_t = 0;

  /// Creates the frontend action for one TU.  Called on a worker thread from
  /// inside ClangTool::run, so it may only read the tool's configuration and
  /// take the addresses of \p Slot's members.
  virtual auto createAction(TUSlot& Slot)
      -> std::unique_ptr<clang::FrontendAction> = 0;

  virtual auto shared() -> CrossTUState& = 0;

  /// Whether a pass that discovered new vetoes must be discarded and run again
  /// (false in Emit mode, where the records carry the vetoes).
  virtual auto rerunNeededOnVeto() const -> bool = 0;

  /// Whether TUs are seeded with the shared vetoes and resolutions (false in
  /// Emit mode, where every TU is independent and aggregation merges).
  virtual auto crossTUSeeding() const -> bool = 0;

  /// Folds every slot's outputs into the tool's, in slot order.  Called once
  /// on the driver thread after the last pass.
  virtual void finish(std::vector<TUSlot>& Slots) = 0;
};

struct TUDriverOptions {
  unsigned Jobs = 0;         ///< threads; 0 = all CPUs (always capped at that)
  bool ForceSerial = false;  ///< run inline on the calling thread (Debug mode)
  int MaxFullPasses = 4;     ///< veto-driven re-runs of everything
  int MaxStaleRounds = 2;    ///< re-runs of stale TUs (one suffices)
};

// ---------------------------------------------------------------------------
// What one action may rewrite
// ---------------------------------------------------------------------------

/// Real absolute paths of the files a run may rewrite.  The source list says
/// which files are *parsed* as translation units; this says which may be
/// *edited*, and the two differ: a header is edited wherever it is included.
using FileSet = std::unordered_set<std::string>;

/// True when \p Loc is spelled in a file this action may rewrite.
///
/// With an empty \p Owned -- a direct run -- that is the TU's own main file
/// and nothing else, because a direct run merges whole rewritten *file
/// contents* between TUs (PendingRewrites is path -> content, last writer
/// wins), so two TUs rewriting one header would silently keep only one of
/// their results.  In Emit mode the unit of merge is a *record*, which
/// aggregation unions per file and dedups byte-identically, so every TU may
/// edit every file it owns -- and must, because a header is not a translation
/// unit and is never parsed on its own there.
bool isRewritableFile(clang::SourceLocation Loc, clang::SourceManager& SM,
                      const FileSet& Owned);

/// True for .h/.hh/.hpp/.hxx/.h++ -- the files the driver runs after every
/// other source, and the aspect's notion of a header.
auto isHeaderSource(llvm::StringRef Path) -> bool;

/// The number of worker threads for \p Requested: 0 means every CPU the
/// process may use, and any request is capped at that count.
auto resolveJobs(unsigned Requested) -> unsigned;

/// The adjusters every binary applies to each compile command: drop
/// -fno-canonical-system-headers (a GCC flag Bazel emits), silence warnings
/// (-w) so a project's own -Werror cannot fail a run over a diagnostic this
/// tool never reads, and supply the embedded Clang resource directory unless
/// one is already given.
auto makeStandardArgumentsAdjuster(const std::string& ResourceDir)
    -> clang::tooling::ArgumentsAdjuster;

/// Runs \p Client's action over every source, as described above, and returns
/// the combined ClangTool::run result (1 if any TU failed, else 2 if any was
/// skipped, else 0).  Call the client's flush()/emitEdits() afterwards, as for
/// a plain ClangTool::run().
auto runTranslationUnits(
    const clang::tooling::CompilationDatabase& Compilations,
    const std::vector<std::string>& Sources,
    const clang::tooling::ArgumentsAdjuster& Adjuster,
    const TUDriverOptions& Opts, TUSlotClient& Client) -> int;

#endif  // CPP_FORMATTING_TU_DRIVER_H_
