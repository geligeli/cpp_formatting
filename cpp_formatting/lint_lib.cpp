#include "cpp_formatting/lint_lib.h"

#include <algorithm>
#include <set>
#include <tuple>
#include <utility>

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

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
