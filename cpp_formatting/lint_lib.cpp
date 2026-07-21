#include "cpp_formatting/lint_lib.h"

#include <algorithm>
#include <fstream>
#include <ios>
#include <map>
#include <set>
#include <tuple>
#include <utility>

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

namespace {

// ---------------------------------------------------------------------------
// Myers O(ND) line diff (following James Coglan's "The Myers diff algorithm")
// ---------------------------------------------------------------------------

struct Edit {
  char Type;  // ' ' keep, '-' delete from A, '+' insert from B
  int A;      // index into A (valid for ' ' and '-')
  int B;      // index into B (valid for ' ' and '+')
};

auto myersDiff(const std::vector<llvm::StringRef>& A,
               const std::vector<llvm::StringRef>& B) -> std::vector<Edit> {
  const int N = static_cast<int>(A.size());
  const int M = static_cast<int>(B.size());
  const int Max = N + M;

  // Guard against quadratic memory use on pathological inputs (e.g. a file
  // rewritten wholesale): fall back to one big replace, which is still a
  // valid diff, just not minimal.
  if (Max > 8192) {
    std::vector<Edit> Ops;
    Ops.reserve(N + M);
    for (int I = 0; I < N; ++I) Ops.push_back({'-', I, -1});
    for (int J = 0; J < M; ++J) Ops.push_back({'+', -1, J});
    return Ops;
  }

  const int Offset = Max;
  std::vector<int> V(2 * Max + 1, 0);
  std::vector<std::vector<int>> Trace;
  int DFound = -1;
  for (int D = 0; D <= Max; ++D) {
    Trace.push_back(V);
    for (int K = -D; K <= D; K += 2) {
      int X;
      if (K == -D || (K != D && V[K - 1 + Offset] < V[K + 1 + Offset]))
        X = V[K + 1 + Offset];
      else
        X = V[K - 1 + Offset] + 1;
      int Y = X - K;
      while (X < N && Y < M && A[X] == B[Y]) {
        ++X;
        ++Y;
      }
      V[K + Offset] = X;
      if (X >= N && Y >= M) {
        DFound = D;
        break;
      }
    }
    if (DFound >= 0) break;
  }

  std::vector<Edit> RevOps;
  int X = N, Y = M;
  for (int D = DFound; D > 0; --D) {
    const std::vector<int>& VPrev = Trace[D];
    int K = X - Y;
    int PrevK =
        (K == -D || (K != D && VPrev[K - 1 + Offset] < VPrev[K + 1 + Offset]))
            ? K + 1
            : K - 1;
    int PrevX = VPrev[PrevK + Offset];
    int PrevY = PrevX - PrevK;
    while (X > PrevX && Y > PrevY) {
      RevOps.push_back({' ', X - 1, Y - 1});
      --X;
      --Y;
    }
    if (X == PrevX) {
      RevOps.push_back({'+', -1, Y - 1});
      --Y;
    } else {
      RevOps.push_back({'-', X - 1, -1});
      --X;
    }
  }
  while (X > 0 && Y > 0) {
    RevOps.push_back({' ', X - 1, Y - 1});
    --X;
    --Y;
  }
  std::reverse(RevOps.begin(), RevOps.end());
  return RevOps;
}

auto splitLines(llvm::StringRef S) -> std::vector<llvm::StringRef> {
  std::vector<llvm::StringRef> Lines;
  std::size_t Pos = 0;
  while (Pos < S.size()) {
    std::size_t NL = S.find('\n', Pos);
    if (NL == llvm::StringRef::npos) {
      Lines.push_back(S.substr(Pos));
      break;
    }
    Lines.push_back(S.substr(Pos, NL - Pos));
    Pos = NL + 1;
  }
  return Lines;
}

}  // namespace

// ---------------------------------------------------------------------------
// LintReport
// ---------------------------------------------------------------------------

void LintReport::add(LintDiagnostic Diag) {
  Diagnostics.push_back(std::move(Diag));
}

static auto sorted(const std::vector<LintDiagnostic>& Diags)
    -> std::vector<LintDiagnostic> {
  std::vector<LintDiagnostic> Out = Diags;
  std::sort(Out.begin(), Out.end(),
            [](const LintDiagnostic& L, const LintDiagnostic& R) {
              return std::tie(L.File, L.Line, L.Column) <
                     std::tie(R.File, R.Line, R.Column);
            });
  return Out;
}

void LintReport::emitText(llvm::raw_ostream& OS) const {
  for (const LintDiagnostic& D : sorted(Diagnostics))
    OS << relativizeToCwd(D.File) << ":" << D.Line << ":" << D.Column
       << ": warning: " << D.Message << " [" << D.RuleId << "]\n";
}

void LintReport::emitSARIF(llvm::raw_ostream& OS,
                           llvm::StringRef ToolName) const {
  std::vector<LintDiagnostic> Diags = sorted(Diagnostics);

  std::set<std::string> RuleIds;
  for (const LintDiagnostic& D : Diags) RuleIds.insert(D.RuleId);

  llvm::json::Array Rules;
  for (const std::string& Id : RuleIds)
    Rules.push_back(llvm::json::Object{
        {"id", Id}, {"shortDescription", llvm::json::Object{{"text", Id}}}});

  llvm::json::Array Results;
  for (const LintDiagnostic& D : Diags) {
    Results.push_back(llvm::json::Object{
        {"ruleId", D.RuleId},
        {"level", "warning"},
        {"message", llvm::json::Object{{"text", D.Message}}},
        {"locations",
         llvm::json::Array{llvm::json::Object{
             {"physicalLocation",
              llvm::json::Object{
                  {"artifactLocation",
                   llvm::json::Object{{"uri", relativizeToCwd(D.File)}}},
                  {"region",
                   llvm::json::Object{{"startLine", D.Line},
                                      {"startColumn", D.Column}}}}}}}}});
  }

  llvm::json::Object Log{
      {"$schema", "https://json.schemastore.org/sarif-2.1.0.json"},
      {"version", "2.1.0"},
      {"runs",
       llvm::json::Array{llvm::json::Object{
           {"tool",
            llvm::json::Object{
                {"driver", llvm::json::Object{{"name", ToolName.str()},
                                              {"rules", std::move(Rules)}}}}},
           {"results", std::move(Results)}}}}};

  OS << llvm::formatv("{0:2}", llvm::json::Value(std::move(Log))) << "\n";
}

auto relativizeToCwd(llvm::StringRef Path) -> std::string {
  llvm::SmallString<256> Cwd;
  if (llvm::sys::fs::current_path(Cwd)) return Path.str();
  llvm::StringRef CwdRef(Cwd);
  if (Path.starts_with(CwdRef) && Path.size() > CwdRef.size() &&
      Path[CwdRef.size()] == '/')
    return Path.drop_front(CwdRef.size() + 1).str();
  return Path.str();
}

// ---------------------------------------------------------------------------
// Unified diff
// ---------------------------------------------------------------------------

void emitUnifiedDiff(llvm::StringRef Original, llvm::StringRef Modified,
                     llvm::StringRef Path, llvm::raw_ostream& OS) {
  if (Original == Modified) return;

  std::vector<llvm::StringRef> A = splitLines(Original);
  std::vector<llvm::StringRef> B = splitLines(Modified);
  // A trailing newline terminates the last line; it is not a line of its own.
  const bool ATrailingNL = Original.empty() || Original.ends_with("\n");
  const bool BTrailingNL = Modified.empty() || Modified.ends_with("\n");

  std::vector<Edit> Ops = myersDiff(A, B);

  constexpr int Context = 3;
  const int NumOps = static_cast<int>(Ops.size());

  OS << "--- a/" << Path << "\n+++ b/" << Path << "\n";

  int I = 0;
  while (I < NumOps) {
    // Find the next change.
    while (I < NumOps && Ops[I].Type == ' ') ++I;
    if (I == NumOps) break;

    // Hunk start: up to Context keeps before the first change.
    int HunkStart = I - Context;
    if (HunkStart < 0) HunkStart = 0;

    // Hunk end: extend through changes separated by at most 2*Context keeps.
    int HunkEnd = I;
    int LastChange = I;
    int J = I;
    while (J < NumOps) {
      if (Ops[J].Type != ' ') {
        LastChange = J;
      } else if (J - LastChange > 2 * Context) {
        break;
      }
      ++J;
    }
    HunkEnd = LastChange + Context + 1;
    if (HunkEnd > NumOps) HunkEnd = NumOps;

    int ACount = 0, BCount = 0;

    // Compute hunk header positions: 0-based line numbers in A and B where
    // the hunk starts.
    int APos = 0, BPos = 0;
    for (int K = 0; K < HunkStart; ++K) {
      if (Ops[K].Type != '+') ++APos;
      if (Ops[K].Type != '-') ++BPos;
    }
    for (int K = HunkStart; K < HunkEnd; ++K) {
      if (Ops[K].Type != '+') ++ACount;
      if (Ops[K].Type != '-') ++BCount;
    }
    int AStart = ACount > 0 ? APos + 1 : APos;
    int BStart = BCount > 0 ? BPos + 1 : BPos;

    OS << "@@ -" << AStart << "," << ACount << " +" << BStart << "," << BCount
       << " @@\n";
    for (int K = HunkStart; K < HunkEnd; ++K) {
      const Edit& E = Ops[K];
      if (E.Type == ' ') {
        OS << " " << A[E.A] << "\n";
        if (E.A == static_cast<int>(A.size()) - 1 && !ATrailingNL)
          OS << "\\ No newline at end of file\n";
      } else if (E.Type == '-') {
        OS << "-" << A[E.A] << "\n";
        if (E.A == static_cast<int>(A.size()) - 1 && !ATrailingNL)
          OS << "\\ No newline at end of file\n";
      } else {
        OS << "+" << B[E.B] << "\n";
        if (E.B == static_cast<int>(B.size()) - 1 && !BTrailingNL)
          OS << "\\ No newline at end of file\n";
      }
    }

    I = HunkEnd;
  }
}

// ---------------------------------------------------------------------------
// emitLintResults
// ---------------------------------------------------------------------------

auto emitLintResults(const LintReport& Report, const PendingRewrites& Rewrites,
                     llvm::StringRef Format, llvm::StringRef ToolName) -> int {
  if (Format == "sarif") {
    Report.emitSARIF(llvm::outs(), ToolName);
  } else if (Format == "diff") {
    for (const auto& [Path, Content] : Rewrites) {
      auto BufOrErr = llvm::MemoryBuffer::getFile(Path);
      if (!BufOrErr) {
        llvm::errs() << "warning: cannot read '" << Path
                     << "': " << BufOrErr.getError().message() << "\n";
        continue;
      }
      emitUnifiedDiff((*BufOrErr)->getBuffer(), Content, relativizeToCwd(Path),
                      llvm::outs());
    }
  } else {
    Report.emitText(llvm::outs());
  }
  return Report.empty() ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Structured edits (Bazel per-TU emit + aggregation)
// ---------------------------------------------------------------------------

void EditReport::emitJSON(llvm::raw_ostream& OS) const {
  llvm::json::Array Edits;
  for (const EditRecord& E : this->Edits)
    Edits.push_back(
        llvm::json::Object{{"file", E.File},
                           {"offset", static_cast<int64_t>(E.Offset)},
                           {"length", static_cast<int64_t>(E.Length)},
                           {"old", E.Old},
                           {"new", E.New}});
  llvm::json::Array Res;
  for (const ResolutionRecord& R : this->Resolutions)
    Res.push_back(llvm::json::Object{{"file", R.File},
                                     {"offset", static_cast<int64_t>(R.Offset)},
                                     {"length", static_cast<int64_t>(R.Length)},
                                     {"old", R.Old},
                                     {"new", R.New},
                                     {"veto", R.Veto}});
  llvm::json::Object Root{{"edits", std::move(Edits)},
                          {"resolutions", std::move(Res)}};
  OS << llvm::formatv("{0:2}", llvm::json::Value(std::move(Root))) << "\n";
}

auto parseEditReport(llvm::StringRef Json, EditReport& Out) -> bool {
  llvm::Expected<llvm::json::Value> Parsed = llvm::json::parse(Json);
  if (!Parsed) {
    llvm::consumeError(Parsed.takeError());
    return false;
  }
  const llvm::json::Object* Root = Parsed->getAsObject();
  if (!Root) return false;

  if (const llvm::json::Array* Edits = Root->getArray("edits")) {
    for (const llvm::json::Value& V : *Edits) {
      const llvm::json::Object* O = V.getAsObject();
      if (!O) return false;
      EditRecord E;
      E.File = O->getString("file").value_or("").str();
      E.Offset = static_cast<unsigned>(O->getInteger("offset").value_or(0));
      E.Length = static_cast<unsigned>(O->getInteger("length").value_or(0));
      E.Old = O->getString("old").value_or("").str();
      E.New = O->getString("new").value_or("").str();
      if (E.File.empty()) return false;
      Out.Edits.push_back(std::move(E));
    }
  }
  if (const llvm::json::Array* Res = Root->getArray("resolutions")) {
    for (const llvm::json::Value& V : *Res) {
      const llvm::json::Object* O = V.getAsObject();
      if (!O) return false;
      ResolutionRecord R;
      R.File = O->getString("file").value_or("").str();
      R.Offset = static_cast<unsigned>(O->getInteger("offset").value_or(0));
      R.Length = static_cast<unsigned>(O->getInteger("length").value_or(0));
      R.Old = O->getString("old").value_or("").str();
      R.New = O->getString("new").value_or("").str();
      R.Veto = O->getBoolean("veto").value_or(false);
      if (R.File.empty()) return false;
      Out.Resolutions.push_back(std::move(R));
    }
  }
  return true;
}

namespace {

// Applies non-overlapping edits (sorted ascending by offset) to \p Original.
// Returns false if any edit runs past the end of the buffer.
auto applyEditsToContent(llvm::StringRef Original,
                         const std::vector<EditRecord>& Sorted,
                         std::string& Out) -> bool {
  Out.clear();
  unsigned Pos = 0;
  for (const EditRecord& E : Sorted) {
    if (E.Offset < Pos || E.Offset + E.Length > Original.size()) return false;
    Out.append(Original.data() + Pos, Original.data() + E.Offset);
    Out.append(E.New);
    Pos = E.Offset + E.Length;
  }
  Out.append(Original.data() + Pos, Original.data() + Original.size());
  return true;
}

}  // namespace

auto mergeEditReports(
    const std::vector<EditReport>& Reports,
    std::map<std::string, std::vector<EditRecord>>& MergedByFile,
    std::vector<std::string>& Conflicts) -> bool {
  // 1. Resolve the union of dependent-token resolutions, keyed by
  // (file,offset).
  //    Mirrors recordResolution/vetoResolution in rename_variables_lib.
  struct Resolved {
    ResolutionRecord Rec;
    bool Vetoed = false;
    bool HasName = false;
  };
  std::map<std::pair<std::string, unsigned>, Resolved> ResMap;
  for (const EditReport& Rep : Reports) {
    for (const ResolutionRecord& R : Rep.Resolutions) {
      Resolved& S = ResMap[{R.File, R.Offset}];
      if (R.Veto) {
        S.Vetoed = true;
        continue;
      }
      if (S.HasName && S.Rec.New != R.New) {
        S.Vetoed = true;  // instantiations disagree.
        continue;
      }
      S.Rec = R;
      S.HasName = true;
    }
  }

  // 2. Gather all edits per file: ordinary edits + surviving resolutions.
  std::map<std::string, std::vector<EditRecord>> ByFile;
  for (const EditReport& Rep : Reports)
    for (const EditRecord& E : Rep.Edits) ByFile[E.File].push_back(E);
  for (const auto& [Key, S] : ResMap) {
    if (S.Vetoed || !S.HasName) continue;
    ByFile[S.Rec.File].push_back(
        {S.Rec.File, S.Rec.Offset, S.Rec.Length, S.Rec.Old, S.Rec.New});
  }

  // 3. Per file: sort, dedup identical, detect overlap, apply to the original.
  bool Ok = true;
  for (auto& [File, Edits] : ByFile) {
    std::sort(Edits.begin(), Edits.end(),
              [](const EditRecord& A, const EditRecord& B) {
                if (A.Offset != B.Offset) return A.Offset < B.Offset;
                return A.Length < B.Length;
              });
    std::vector<EditRecord> Merged;
    bool Conflict = false;
    for (const EditRecord& E : Edits) {
      if (!Merged.empty()) {
        const EditRecord& P = Merged.back();
        if (P.Offset == E.Offset && P.Length == E.Length && P.New == E.New)
          continue;  // byte-identical duplicate.
        if (E.Offset < P.Offset + P.Length ||
            (E.Offset == P.Offset && (P.Length == 0 || E.Length == 0) &&
             P.New != E.New)) {
          Conflicts.push_back(File + ": conflicting edits at offset " +
                              std::to_string(E.Offset));
          Conflict = true;
          break;
        }
      }
      Merged.push_back(E);
    }
    if (Conflict) {
      Ok = false;
      continue;
    }
    MergedByFile[File] = std::move(Merged);
  }
  return Ok;
}

auto aggregateEdits(
    const std::vector<EditReport>& Reports,
    const std::function<std::optional<std::string>(llvm::StringRef)>& ReadFile,
    std::map<std::string, std::string>& Out,
    std::vector<std::string>& Conflicts) -> bool {
  std::map<std::string, std::vector<EditRecord>> MergedByFile;
  bool Ok = mergeEditReports(Reports, MergedByFile, Conflicts);
  for (const auto& [File, Merged] : MergedByFile) {
    std::optional<std::string> Original = ReadFile(File);
    if (!Original) {
      Conflicts.push_back(File + ": cannot read original content");
      Ok = false;
      continue;
    }
    std::string Content;
    if (!applyEditsToContent(*Original, Merged, Content)) {
      Conflicts.push_back(File + ": edit out of range (stale records?)");
      Ok = false;
      continue;
    }
    Out[File] = std::move(Content);
  }
  return Ok;
}

auto runEditAggregation(const std::vector<std::string>& InputPaths,
                        llvm::StringRef Root, bool Apply, bool Check) -> int {
  // Record keys are the file's real path.  Under Bazel that resolves to the
  // absolute workspace path (source symlinks), so absolute keys are used as-is;
  // relative keys (e.g. from a plain `--emit-edits` run) join with Root.
  auto resolvePath = [&](llvm::StringRef File) -> std::string {
    if (llvm::sys::path::is_absolute(File) || Root.empty()) return File.str();
    return (Root + "/" + File).str();
  };
  // A workspace-relative label for diff headers (git-apply-able from the root).
  auto displayPath = [&](llvm::StringRef File) -> std::string {
    if (!Root.empty() && File.starts_with(Root) && File.size() > Root.size() &&
        File[Root.size()] == '/')
      return File.drop_front(Root.size() + 1).str();
    return File.str();
  };
  auto readFile = [&](llvm::StringRef File) -> std::optional<std::string> {
    auto Buf = llvm::MemoryBuffer::getFile(resolvePath(File));
    if (!Buf) return std::nullopt;
    return (*Buf)->getBuffer().str();
  };

  std::vector<EditReport> Reports;
  for (const std::string& Path : InputPaths) {
    auto Buf = llvm::MemoryBuffer::getFile(Path);
    if (!Buf) {
      llvm::errs() << "cannot read '" << Path
                   << "': " << Buf.getError().message() << "\n";
      return 2;
    }
    EditReport R;
    if (!parseEditReport((*Buf)->getBuffer(), R)) {
      llvm::errs() << "malformed edit records: '" << Path << "'\n";
      return 2;
    }
    Reports.push_back(std::move(R));
  }

  if (Check) {
    std::map<std::string, std::vector<EditRecord>> Merged;
    std::vector<std::string> Conflicts;
    const bool Ok = mergeEditReports(Reports, Merged, Conflicts);
    for (const std::string& C : Conflicts)
      llvm::errs() << "conflict: " << C << "\n";
    std::size_t Total = 0;
    for (const auto& [File, Edits] : Merged) {
      if (Edits.empty()) continue;
      llvm::outs() << File << ": " << Edits.size() << " edit(s)\n";
      Total += Edits.size();
    }
    if (Total == 0 && Ok) return 0;
    llvm::errs() << Total << " formatting edit(s) would be applied across "
                 << Merged.size()
                 << " file(s); run the `.fix` target to apply, or `.diff` to "
                    "preview.\n";
    return 1;
  }

  std::map<std::string, std::string> Out;
  std::vector<std::string> Conflicts;
  const bool Ok = aggregateEdits(Reports, readFile, Out, Conflicts);
  for (const std::string& C : Conflicts)
    llvm::errs() << "conflict: " << C << "\n";

  if (Apply) {
    for (const auto& [File, Content] : Out) {
      std::ofstream O(resolvePath(File), std::ios::trunc | std::ios::binary);
      O << Content;
    }
  } else {
    for (const auto& [File, Content] : Out) {
      std::optional<std::string> Original = readFile(File);
      emitUnifiedDiff(Original ? *Original : "", Content, displayPath(File),
                      llvm::outs());
    }
  }
  return Ok ? 0 : 1;
}
