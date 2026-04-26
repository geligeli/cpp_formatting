#include "cpp_formatting/trailing_return_types_lib.h"

#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/Lexer.h"
#include "clang/Tooling/Tooling.h"

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Walk the entire TypeLoc chain and return the leftmost local-range begin
/// location found.  This is necessary because pointer/reference TypeLocs
/// (e.g. `LValueReferenceTypeLoc` for `std::ostream &`) only report their
/// own sigil (`&`) as their local begin; the pointee's location is stored in
/// the next TypeLoc in the chain.  Iterating the chain and taking the minimum
/// gives the true start of the written type.
static auto getTypeLocLeftmostBegin(TypeLoc TL, SourceManager &SM) -> SourceLocation {
  SourceLocation Best;
  unsigned BestOffset = UINT_MAX;
  for (TypeLoc Cur = TL; !Cur.isNull(); Cur = Cur.getNextTypeLoc()) {
    SourceLocation Loc = Cur.getLocalSourceRange().getBegin();
    if (!Loc.isValid() || Loc.isMacroID()) continue;
    unsigned Off = SM.getFileOffset(Loc);
    if (Off < BestOffset) {
      BestOffset = Off;
      Best = Loc;
    }
  }
  return Best.isValid() ? Best : TL.getSourceRange().getBegin();
}

/// Scan backwards from \p Start in the source buffer and return a new start
/// that includes any immediately preceding `const`/`volatile`/`restrict`
/// qualifier keywords.  This is needed because Clang's QualifiedTypeLoc
/// does not extend its source range to cover leading qualifier keywords;
/// the range begins at the unqualified type (e.g. the `int` in `const int*`).
static auto skipQualifiersBackward(SourceLocation Start,
                                             SourceManager& SM) -> SourceLocation {
  bool Inv = false;
  FileID FID = SM.getFileID(Start);
  llvm::StringRef Buf = SM.getBufferData(FID, &Inv);
  if (Inv) return Start;

  unsigned Pos = SM.getFileOffset(Start);
  const char* BP = Buf.data();

  while (Pos > 0) {
    // Skip whitespace backwards.
    unsigned End = Pos;
    while (End > 0 && std::isspace((unsigned char)BP[End - 1])) --End;
    if (End == 0) break;

    // The preceding token must be an identifier/keyword character.
    if (!std::isalnum((unsigned char)BP[End - 1]) && BP[End - 1] != '_') break;

    // Find the start of that token.
    unsigned TokEnd = End;
    unsigned TokStart = TokEnd - 1;
    while (TokStart > 0 && (std::isalnum((unsigned char)BP[TokStart - 1]) ||
                            BP[TokStart - 1] == '_'))
      --TokStart;

    llvm::StringRef Tok(BP + TokStart, TokEnd - TokStart);
    if (Tok == "const" || Tok == "volatile" || Tok == "restrict") {
      Pos = TokStart;  // extend the start leftward
    } else {
      break;
    }
  }

  if (Pos == SM.getFileOffset(Start)) return Start;  // nothing to extend
  return SM.getLocForStartOfFile(FID).getLocWithOffset(Pos);
}

// ---------------------------------------------------------------------------
// TrailingReturnCallback implementation
// ---------------------------------------------------------------------------

TrailingReturnCallback::TrailingReturnCallback(Rewriter& Rewrite)
    : Rewrite(Rewrite) {}

void TrailingReturnCallback::run(const MatchFinder::MatchResult& Result) {
  const FunctionDecl* Func = Result.Nodes.getNodeAs<FunctionDecl>("func");
  if (!Func) return;

  SourceManager& SM = *Result.SourceManager;
  if (!SM.isWrittenInMainFile(Func->getLocation())) return;

  TypeSourceInfo* TSI = Func->getTypeSourceInfo();
  if (!TSI) return;

  TypeLoc TL = TSI->getTypeLoc();
  FunctionTypeLoc FTL = TL.getAsAdjusted<FunctionTypeLoc>();
  if (!FTL) return;

  TypeLoc ReturnLoc = FTL.getReturnLoc();
  SourceRange ReturnRange = ReturnLoc.getSourceRange();

  // Skip functions whose return type is written as plain `auto` (or
  // `decltype(auto)`) with no trailing `->`.  Rewriting them would just
  // produce `auto foo() -> auto { ... }` which is redundant noise.
  if (ReturnLoc.getAs<AutoTypeLoc>()) return;

  // Pointer/reference TypeLocs (e.g. LValueReferenceTypeLoc for `T &`) only
  // report their sigil as their local begin; the base type lives in the next
  // TypeLoc in the chain.  Walk the chain to find the true leftmost location,
  // then extend further left past any leading cv-qualifiers (needed because
  // QualifiedTypeLoc also omits leading `const`/`volatile`/`restrict`).
  SourceLocation TypeBegin = getTypeLocLeftmostBegin(ReturnLoc, SM);
  SourceLocation ExtStart = skipQualifiersBackward(TypeBegin, SM);
  SourceRange FullReturnRange(ExtStart, ReturnRange.getEnd());

  // Extract the EXACT original return-type text from the source.
  const LangOptions& LangOpts = Func->getASTContext().getLangOpts();
  StringRef OriginalTypeStr = Lexer::getSourceText(
      CharSourceRange::getTokenRange(FullReturnRange), SM, LangOpts);

  if (OriginalTypeStr.empty()) return;

  // Pad "auto" if the character immediately after the return-type token would
  // merge with the next token (e.g. `Foo&operator=` -> `auto operator=`).
  SourceLocation AfterReturnLoc =
      Lexer::getLocForEndOfToken(ReturnRange.getEnd(), 0, SM, LangOpts);
  bool Invalid = false;
  const char* NextChar = SM.getCharacterData(AfterReturnLoc, &Invalid);
  std::string AutoReplacement = "auto";
  if (!Invalid && NextChar &&
      !std::isspace(static_cast<unsigned char>(*NextChar)))
    AutoReplacement = "auto ";

  // Replace the full return type (including any leading qualifiers) with
  // "auto".
  Rewrite.ReplaceText(FullReturnRange, AutoReplacement);

  // Insert " -> OriginalType" after the local range end of the function type
  // (closing ')' plus any cv/ref/noexcept qualifiers tracked by
  // FunctionTypeLoc).
  SourceLocation InsertLoc = FTL.getLocalRangeEnd();
  Rewrite.InsertTextAfterToken(InsertLoc, " -> " + OriginalTypeStr.str());
}

// ---------------------------------------------------------------------------
// Shared matcher registration
// ---------------------------------------------------------------------------

void registerTrailingReturnMatchers(MatchFinder& Finder,
                                    TrailingReturnCallback& Callback) {
  Finder.addMatcher(
      functionDecl(unless(hasTrailingReturn()), unless(returns(voidType())),
                   unless(cxxConversionDecl()), unless(isDefaulted()))
          .bind("func"),
      &Callback);
}

// ---------------------------------------------------------------------------
// TrailingReturnTypesAction implementation
// ---------------------------------------------------------------------------

TrailingReturnTypesAction::TrailingReturnTypesAction(OutputMode Mode)
    : Mode(Mode), Callback(TheRewriter) {}

void TrailingReturnTypesAction::EndSourceFileAction() {
  SourceManager& SM = TheRewriter.getSourceMgr();
  if (Mode == OutputMode::InPlace) {
    TheRewriter.overwriteChangedFiles();
    llvm::outs() << "Modifications written to disk.\n";
  } else {
    llvm::errs() << "** Rewritten Output (Dry Run): **\n";
    TheRewriter.getEditBuffer(SM.getMainFileID()).write(llvm::outs());
  }
}

auto TrailingReturnTypesAction::CreateASTConsumer(
    CompilerInstance& CI, StringRef /*File*/) -> std::unique_ptr<ASTConsumer> {
  TheRewriter.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
  registerTrailingReturnMatchers(Finder, Callback);
  return Finder.newASTConsumer();
}

// ---------------------------------------------------------------------------
// CaptureAction — runs the rewrite and captures the result into a string
// (used by the test helper below)
// ---------------------------------------------------------------------------

namespace {

class CaptureAction : public ASTFrontendAction {
 public:
  explicit CaptureAction(std::string& Output)
      : Callback(TheRewriter), Output(Output) {}

  void EndSourceFileAction() override {
    SourceManager& SM = TheRewriter.getSourceMgr();
    llvm::raw_string_ostream OS(Output);
    TheRewriter.getEditBuffer(SM.getMainFileID()).write(OS);
  }

  auto CreateASTConsumer(CompilerInstance& CI,
                                                 StringRef /*File*/) -> std::unique_ptr<ASTConsumer> override {
    TheRewriter.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
    registerTrailingReturnMatchers(Finder, Callback);
    return Finder.newASTConsumer();
  }

 private:
  Rewriter TheRewriter;
  TrailingReturnCallback Callback;  ///< must be declared after TheRewriter
  MatchFinder Finder;
  std::string& Output;
};

}  // namespace

// ---------------------------------------------------------------------------
// Test helper
// ---------------------------------------------------------------------------

auto rewriteToTrailingReturnTypes(llvm::StringRef Code,
const std::vector<std::string> &Args) -> std::string {
  std::string Output;
  bool Success = runToolOnCodeWithArgs(std::make_unique<CaptureAction>(Output),
                                       Code, Args);
  if (!Success || Output.empty()) return Code.str();
  return Output;
}
