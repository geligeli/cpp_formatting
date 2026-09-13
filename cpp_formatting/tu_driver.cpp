#include "cpp_formatting/tu_driver.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Threading.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/thread.h"

using namespace clang;
using namespace clang::tooling;

// ---------------------------------------------------------------------------
// TUSlot
// ---------------------------------------------------------------------------

void TUSlot::clearOutputs() {
  Pending.clear();
  Edits = EditReport{};
  Conflicts.clear();
  Vetoes.clear();
  for (DependentResolutions& M : DepRes) M.clear();
  Report.clear();
  Diagnostics.clear();
  Rc = 0;
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

auto isHeaderSource(llvm::StringRef Path) -> bool {
  llvm::StringRef Ext = llvm::sys::path::extension(Path);
  return Ext == ".h" || Ext == ".hh" || Ext == ".hpp" || Ext == ".hxx" ||
         Ext == ".h++";
}

auto resolveJobs(unsigned Requested) -> unsigned {
  // hardware_concurrency() honours the affinity mask, but does not itself cap
  // an explicit request; a request above the CPU count only oversubscribes.
  unsigned Available = llvm::hardware_concurrency().compute_thread_count();
  if (Available == 0) Available = 1;
  if (Requested == 0 || Requested > Available) return Available;
  return Requested;
}

auto makeStandardArgumentsAdjuster(const std::string& ResourceDir)
    -> ArgumentsAdjuster {
  // Drop the GCC-only flag Bazel emits, and silence every warning.
  //
  // The compile command belongs to the project being formatted, not to us, and
  // a project that builds with -Werror hands us one: Clang then counts its own
  // warning as an error, CompilerInstance::ExecuteAction returns false for a
  // translation unit that parsed perfectly well, and ClangTool::run reports the
  // whole run as failed -- over a diagnostic this tool never reads.  Under the
  // Bazel aspect that fails the build.  The warning set our Clang applies is
  // not even the one the project compiles with (a different compiler version,
  // and per-target `copts` the aspect cannot recover), so honouring -Werror
  // here reports warnings the real build never sees.
  //
  // -w is tested before the -Werror promotion in DiagnosticIDs, so it outranks
  // it, and it ignores only diagnostics that are warnings by default: a real
  // error -- which does mean the AST cannot be trusted -- still fails the run.
  ArgumentsAdjuster Strip = [](const CommandLineArguments& Args,
                               llvm::StringRef) {
    CommandLineArguments Out;
    for (const std::string& Arg : Args)
      if (Arg != "-fno-canonical-system-headers") Out.push_back(Arg);
    Out.insert(Out.begin() + (Out.empty() ? 0 : 1), "-w");
    return Out;
  };
  if (ResourceDir.empty()) return Strip;
  ArgumentsAdjuster Resource = [ResourceDir](const CommandLineArguments& Args,
                                             llvm::StringRef) {
    for (const std::string& Arg : Args)
      if (llvm::StringRef(Arg).starts_with("-resource-dir")) return Args;
    // Insert after Args[0] (the compiler name), matching the convention used
    // by getInsertArgumentAdjuster(..., BEGIN).
    CommandLineArguments Adjusted = Args;
    Adjusted.insert(Adjusted.begin() + (Adjusted.empty() ? 0 : 1),
                    "-resource-dir=" + ResourceDir);
    return Adjusted;
  };
  return combineAdjusters(std::move(Strip), std::move(Resource));
}

namespace {

// ---------------------------------------------------------------------------
// Per-TU action factory
// ---------------------------------------------------------------------------

// Hands ClangTool::run the client's action for one slot.  It also owns the
// CompilerInstance setup (a copy of FrontendActionFactory::runInvocation) so
// that everything Clang prints outside the DiagnosticConsumer -- the
// "N warnings generated." summary, -v output -- can be redirected: with several
// TUs running at once, stderr must be written by the driver alone.
class SlotActionFactory : public FrontendActionFactory {
 public:
  SlotActionFactory(TUSlotClient& Client, TUSlot& Slot,
                    llvm::raw_ostream& Verbose, llvm::raw_ostream* BufferTo)
      : Client(Client), Slot(Slot), Verbose(Verbose), BufferTo(BufferTo) {}

  auto create() -> std::unique_ptr<FrontendAction> override {
    return Client.createAction(Slot);
  }

  bool runInvocation(std::shared_ptr<CompilerInvocation> Invocation,
                     FileManager* Files,
                     std::shared_ptr<PCHContainerOperations> PCHContainerOps,
                     DiagnosticConsumer* DiagConsumer) override {
    CompilerInstance Compiler(std::move(Invocation),
                              std::move(PCHContainerOps));
    Compiler.setFileManager(Files);
    Compiler.setVerboseOutputStream(Verbose);

    // The FrontendAction can have lifetime requirements for Compiler or its
    // members, and we need to ensure it's deleted earlier than Compiler.
    std::unique_ptr<FrontendAction> ScopedToolAction(create());

    // When buffering, format the diagnostics with the options the compile
    // command asked for (-fno-show-column, -ferror-limit, ...), exactly as the
    // default printer that writes to stderr would.
    std::unique_ptr<TextDiagnosticPrinter> Buffered;
    if (BufferTo) {
      Buffered = std::make_unique<TextDiagnosticPrinter>(
          *BufferTo, Compiler.getDiagnosticOpts());
      DiagConsumer = Buffered.get();
    }
    Compiler.createDiagnostics(Files->getVirtualFileSystem(), DiagConsumer,
                               /*ShouldOwnClient=*/false);
    if (!Compiler.hasDiagnostics()) return false;

    Compiler.createSourceManager(*Files);
    const bool Success = Compiler.ExecuteAction(*ScopedToolAction);
    Files->clearStatCache();
    return Success;
  }

 private:
  TUSlotClient& Client;
  TUSlot& Slot;
  llvm::raw_ostream& Verbose;
  llvm::raw_ostream* BufferTo;  // null: leave Clang's stderr printer in place
};

// ---------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------

enum class DiagMode { Print, Silent };

struct DriverContext {
  const CompilationDatabase& Compilations;
  const ArgumentsAdjuster& Adjuster;
  TUSlotClient& Client;
  std::vector<TUSlot>& Slots;
  std::mutex OutputMutex;  // every write to stderr while a batch runs
};

auto absolutePathOf(const std::string& Source) -> std::string {
  llvm::SmallString<256> Abs(Source);
  llvm::sys::fs::make_absolute(Abs);
  return std::string(Abs);
}

// Parses one TU with its own ClangTool.  \p Buffered routes every byte Clang
// would write to stderr into the slot instead, for the driver to replay in
// order; the serial path writes straight to stderr, exactly as one
// ClangTool::run over every file did.
void runOne(DriverContext& Ctx, size_t Index, DiagMode Mode, bool Buffered) {
  TUSlot& S = Ctx.Slots[Index];
  const size_t Total = Ctx.Slots.size();
  const std::string Abs = absolutePathOf(S.Source);
  if (Total > 1) {
    std::lock_guard<std::mutex> Lock(Ctx.OutputMutex);
    llvm::errs() << "[" << (Index + 1) << "/" << Total << "] Processing file "
                 << Abs << ".\n";
  }

  llvm::raw_string_ostream Buf(S.Diagnostics);
  const bool Silent = Mode == DiagMode::Silent;
  llvm::raw_ostream& Verbose =
      Silent ? llvm::nulls()
             : (Buffered ? static_cast<llvm::raw_ostream&>(Buf) : llvm::errs());
  // Driver-stage diagnostics (before the frontend's own options are parsed)
  // use default formatting when buffered; the frontend stage is re-pointed by
  // SlotActionFactory with the invocation's options.
  DiagnosticOptions DriverDiagOpts;
  TextDiagnosticPrinter BufferedPrinter(Buf, DriverDiagOpts);
  IgnoringDiagConsumer Ignore;
  DiagnosticConsumer* Consumer =
      Silent ? static_cast<DiagnosticConsumer*>(&Ignore)
             : (Buffered ? static_cast<DiagnosticConsumer*>(&BufferedPrinter)
                         : nullptr);

  // Each TU gets a file system with its own working directory, so the
  // per-compile-command chdir inside ClangTool::run never touches the process.
  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FS =
      llvm::vfs::createPhysicalFileSystem();
  ClangTool Tool(Ctx.Compilations, {S.Source},
                 std::make_shared<PCHContainerOperations>(), std::move(FS));
  if (Ctx.Adjuster) Tool.appendArgumentsAdjuster(Ctx.Adjuster);
  Tool.setDiagnosticConsumer(Consumer);
  Tool.setPrintErrorMessage(false);

  SlotActionFactory Factory(Ctx.Client, S, Verbose,
                            Buffered && !Silent ? &Buf : nullptr);
  S.Rc = Tool.run(&Factory);
  if (S.Rc == 1 && !Silent)
    Verbose << "Error while processing " << Abs << ".\n";
  if (Silent) S.Diagnostics.clear();
}

// Runs the slots in \p Batch, on \p Threads workers when there is more than
// one.  Buffered diagnostics are replayed to stderr in batch order as soon as
// every earlier TU has finished, so the output is deterministic without
// waiting for the whole batch.
void runBatch(DriverContext& Ctx, const std::vector<size_t>& Batch,
              DiagMode Mode, unsigned Threads) {
  if (Batch.empty()) return;
  const unsigned Workers =
      std::min<unsigned>(Threads, static_cast<unsigned>(Batch.size()));
  if (Workers <= 1) {
    for (size_t Index : Batch) runOne(Ctx, Index, Mode, /*Buffered=*/false);
    return;
  }

  std::atomic<size_t> Next{0};
  std::vector<bool> Done(Batch.size(), false);
  size_t NextToPrint = 0;
  auto Work = [&] {
    for (size_t K = Next.fetch_add(1); K < Batch.size();
         K = Next.fetch_add(1)) {
      runOne(Ctx, Batch[K], Mode, /*Buffered=*/true);
      std::lock_guard<std::mutex> Lock(Ctx.OutputMutex);
      Done[K] = true;
      while (NextToPrint < Batch.size() && Done[NextToPrint]) {
        TUSlot& S = Ctx.Slots[Batch[NextToPrint]];
        llvm::errs() << S.Diagnostics;
        S.Diagnostics.clear();
        ++NextToPrint;
      }
    }
  };

  // Explicit 8 MiB stacks: Clang's parser, Sema and RecursiveASTVisitor recurse
  // deeply, and a thread pool's default stack (1 MiB on Windows) is not enough.
  std::vector<llvm::thread> Pool;
  Pool.reserve(Workers);
  for (unsigned W = 0; W < Workers; ++W)
    Pool.emplace_back(std::optional<unsigned>(8u << 20), Work);
  for (llvm::thread& T : Pool) T.join();
}

}  // namespace

// ---------------------------------------------------------------------------
// runTranslationUnits
// ---------------------------------------------------------------------------

auto runTranslationUnits(const CompilationDatabase& Compilations,
                         const std::vector<std::string>& Sources,
                         const ArgumentsAdjuster& Adjuster,
                         const TUDriverOptions& Opts, TUSlotClient& Client)
    -> int {
  // Slots in source order: non-headers first, then headers, each group keeping
  // the order it was given in.  Every merge below walks slots in this order,
  // which is what makes the result independent of the thread count.
  std::vector<TUSlot> Slots;
  std::vector<size_t> NonHeaders, Headers;
  for (int Pass = 0; Pass < 2; ++Pass) {
    for (const std::string& Source : Sources) {
      const bool Header = isHeaderSource(Source);
      if (Header != (Pass == 1)) continue;
      TUSlot S;
      S.Source = Source;
      S.IsHeader = Header;
      llvm::SmallString<256> Real;
      S.MainFileRealPath = llvm::sys::fs::real_path(Source, Real)
                               ? absolutePathOf(Source)
                               : std::string(Real);
      (Header ? Headers : NonHeaders).push_back(Slots.size());
      Slots.push_back(std::move(S));
    }
  }

  const size_t RuleCount = Client.ruleCount();
  const bool Seeding = Client.crossTUSeeding();
  CrossTUState& G = Client.shared();
  G.DepResPerRule.assign(RuleCount, DependentResolutions{});
  const unsigned Threads = Opts.ForceSerial ? 1 : resolveJobs(Opts.Jobs);
  DriverContext Ctx{Compilations, Adjuster, Client, Slots, {}};

  auto reseed = [&](size_t Index) {
    TUSlot& S = Slots[Index];
    S.clearOutputs();
    S.DepRes.assign(RuleCount, DependentResolutions{});
    if (Seeding) {
      S.Vetoes = G.Vetoes;
      S.DepRes = G.DepResPerRule;
    }
  };
  auto mergeCrossTU = [&](size_t Index) {
    const TUSlot& S = Slots[Index];
    for (const auto& [Key, Veto] : S.Vetoes) G.Vetoes.try_emplace(Key, Veto);
    for (size_t R = 0; R < RuleCount; ++R)
      mergeDependentResolutions(G.DepResPerRule[R], S.DepRes[R]);
  };
  // A TU is stale when the shared map now says something different about a
  // token in its own main file than the map it applied from (its slot's map:
  // the seed plus its own records, which the merge above already folded in).
  auto isStale = [&](const TUSlot& S) {
    if (!Seeding || S.Rc != 0) return false;
    for (size_t R = 0; R < RuleCount; ++R)
      if (dependentResolutionsDifferFor(G.DepResPerRule[R], S.DepRes[R],
                                        S.MainFileRealPath))
        return true;
    return false;
  };

  const std::vector<std::vector<size_t>> Phases = {NonHeaders, Headers};
  DiagMode Mode = DiagMode::Print;
  for (int Pass = 0; Pass < Opts.MaxFullPasses; ++Pass) {
    const bool LastPass = Pass + 1 == Opts.MaxFullPasses;
    const size_t VetoesBefore = G.Vetoes.size();
    bool Restart = false;
    for (const std::vector<size_t>& Phase : Phases) {
      if (Phase.empty()) continue;
      for (size_t Index : Phase) reseed(Index);
      runBatch(Ctx, Phase, Mode, Threads);
      for (size_t Index : Phase) mergeCrossTU(Index);
      // A new veto invalidates this pass; no point parsing the headers too,
      // unless this is the last pass we are allowed and its output is final.
      if (Client.rerunNeededOnVeto() && G.Vetoes.size() > VetoesBefore) {
        Restart = true;
        if (!LastPass) break;
      }
    }
    // The first pass reported every parse diagnostic; don't repeat them.
    Mode = DiagMode::Silent;

    if (Restart) {
      if (!LastPass) {
        llvm::errs() << "re-running: " << G.Vetoes.size()
                     << " rename(s) vetoed by a reference that cannot be "
                        "rewritten\n";
        // A member that stopped being renamed leaves entries no TU overwrites
        // (the recorder only runs for a TU that still renames something), so
        // the resolutions are rebuilt from scratch; the vetoes are kept.
        for (DependentResolutions& M : G.DepResPerRule) M.clear();
        continue;
      }
      llvm::errs() << "warning: renames were still being vetoed after "
                   << Opts.MaxFullPasses << " passes; using the last pass\n";
    }

    // Rule (b): re-run only the TUs whose dependent tokens the rest of the
    // pass resolved differently.  With no new vetoes the shared map is
    // complete, so one round settles it; the cap only guards a logic error.
    for (int Round = 0; Round < Opts.MaxStaleRounds; ++Round) {
      std::vector<size_t> Stale;
      for (size_t Index = 0; Index < Slots.size(); ++Index)
        if (isStale(Slots[Index])) Stale.push_back(Index);
      if (Stale.empty()) break;
      for (size_t Index : Stale) reseed(Index);
      runBatch(Ctx, Stale, DiagMode::Silent, Threads);
      for (size_t Index : Stale) mergeCrossTU(Index);
      if (Round + 1 == Opts.MaxStaleRounds &&
          std::any_of(Slots.begin(), Slots.end(), isStale))
        llvm::errs() << "warning: template-dependent token resolution did not "
                        "converge; some tokens may be left unrenamed\n";
    }
    break;
  }

  Client.finish(Slots);

  int Rc = 0;
  for (const TUSlot& S : Slots) {
    if (S.Rc == 1) return 1;
    if (S.Rc == 2) Rc = 2;
  }
  return Rc;
}
