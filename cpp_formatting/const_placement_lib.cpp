#include "cpp_formatting/const_placement_lib.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Type.h"
#include "clang/AST/TypeLoc.h"
#include "clang/Basic/FileEntry.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/Lexer.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/Tooling.h"
#include "cpp_formatting/tu_driver.h"
#include "llvm/ADT/RewriteBuffer.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using namespace clang::tooling;

// ---------------------------------------------------------------------------
// ConstStyle
// ---------------------------------------------------------------------------

auto parseConstStyle(llvm::StringRef Name, ConstStyle& Out) -> bool {
  if (Name == "east") {
    Out = ConstStyle::East;
    return true;
  }
  if (Name == "west") {
    Out = ConstStyle::West;
    return true;
  }
  return false;
}

auto constStyleRuleId(ConstStyle Style) -> const char* {
  return Style == ConstStyle::East ? "east_const" : "west_const";
}

namespace {

// ---------------------------------------------------------------------------
// Which qualifiers move
// ---------------------------------------------------------------------------

/// True for the TypeLoc classes that spell a *declarator component* rather
/// than a type specifier: pointers, references, arrays, function types and the
/// parentheses that group them.
///
/// This is the whole soundness argument of the pass.  A QualifiedTypeLoc's
/// qualifiers belong to whatever its unqualified loc is, and Clang nests those
/// locs exactly the way the declarator reads:
///
///   const int* p   ->  Pointer( Qualified{const}( Builtin int ) )
///   int* const p   ->  Qualified{const}( Pointer( Builtin int ) )
///
/// The first `const` qualifies `int`, may be written on either side of it, and
/// is what this pass moves.  The second qualifies the *pointer*, has no west
/// spelling at all (`const int* p` is a different type), and is declined here.
///
/// Declining these also keeps the rewrite's source range honest: an array or
/// function declarator's TypeLoc range runs past the type specifier and over
/// the declarator-id (`const int a[3]`'s ConstantArrayTypeLoc spells
/// `int a[3]`), so appending ` const` to its end would produce
/// `int a[3] const`.  Every loc that survives this filter spells the type
/// specifier and nothing else.
auto isDeclaratorComponent(TypeLoc TL) -> bool {
  switch (TL.getTypeLocClass()) {
    case TypeLoc::Pointer:
    case TypeLoc::BlockPointer:
    case TypeLoc::MemberPointer:
    case TypeLoc::LValueReference:
    case TypeLoc::RValueReference:
    case TypeLoc::ConstantArray:
    case TypeLoc::IncompleteArray:
    case TypeLoc::VariableArray:
    case TypeLoc::DependentSizedArray:
    case TypeLoc::FunctionProto:
    case TypeLoc::FunctionNoProto:
    case TypeLoc::Paren:
    case TypeLoc::Adjusted:
    case TypeLoc::Decayed:
    // Not declarator components, but both wrap another loc whose range this
    // pass would then have to reason about through an attribute or a macro.
    // Over-declining is the safe side.
    case TypeLoc::Attributed:
    case TypeLoc::MacroQualified:
      return true;
    default:
      return false;
  }
}

/// True when \p TL -- the unqualified loc under a QualifiedTypeLoc -- spells a
/// pure type specifier, so its source range covers exactly the written type
/// and a qualifier may sit on either side of it.
auto isPlainTypeSpecifier(TypeLoc TL) -> bool {
  for (TypeLoc Cur = TL; !Cur.isNull(); Cur = Cur.getNextTypeLoc())
    if (isDeclaratorComponent(Cur)) return false;
  return true;
}

/// A run of `const`/`volatile` keywords written next to a type specifier,
/// together with the byte range that has to be deleted to take them away --
/// which includes the whitespace between them and the specifier, so that
/// `const int` loses `"const "` and `int const` loses `" const"`.
struct CvRun {
  unsigned Begin = 0;                              ///< first byte to delete
  unsigned End = 0;                                ///< one past the last byte
  llvm::SmallVector<llvm::StringRef, 2> Keywords;  ///< in source order

  auto empty() const -> bool { return Keywords.empty(); }

  /// The keywords joined by single spaces, which is how they are re-emitted
  /// on the other side.  Original spacing inside the run is not preserved --
  /// `const    volatile` comes back as `const volatile`.
  auto text() const -> std::string {
    std::string S;
    for (llvm::StringRef K : Keywords) {
      if (!S.empty()) S += ' ';
      S += K.str();
    }
    return S;
  }
};

auto isIdentBody(char C) -> bool {
  return std::isalnum(static_cast<unsigned char>(C)) || C == '_';
}
auto isSpace(char C) -> bool {
  return std::isspace(static_cast<unsigned char>(C)) != 0;
}
auto isCvKeyword(llvm::StringRef Tok) -> bool {
  return Tok == "const" || Tok == "volatile";
}

/// Scans the raw buffer leftwards from \p From over the run of cv keywords
/// immediately preceding it.  Raw-buffer scanning (rather than a Lexer) is the
/// idiom the trailing-return pass already uses for the same job; it skips
/// whitespace but not comments, so `const /*c*/ int` is simply not recognised
/// and the declaration is left alone.
auto scanCvRunBackward(llvm::StringRef Buf, unsigned From) -> CvRun {
  CvRun Run;
  Run.End = From;
  unsigned Pos = From;
  while (Pos > 0) {
    unsigned TokEnd = Pos;
    while (TokEnd > 0 && isSpace(Buf[TokEnd - 1])) --TokEnd;
    if (TokEnd == 0 || !isIdentBody(Buf[TokEnd - 1])) break;
    unsigned TokStart = TokEnd - 1;
    while (TokStart > 0 && isIdentBody(Buf[TokStart - 1])) --TokStart;
    llvm::StringRef Tok = Buf.substr(TokStart, TokEnd - TokStart);
    if (!isCvKeyword(Tok)) break;
    Run.Keywords.insert(Run.Keywords.begin(), Tok);
    Pos = TokStart;
  }
  Run.Begin = Pos;
  return Run;
}

/// The mirror of scanCvRunBackward: the run immediately *following* \p From.
auto scanCvRunForward(llvm::StringRef Buf, unsigned From) -> CvRun {
  CvRun Run;
  Run.Begin = From;
  unsigned Pos = From;
  while (Pos < Buf.size()) {
    unsigned TokStart = Pos;
    while (TokStart < Buf.size() && isSpace(Buf[TokStart])) ++TokStart;
    if (TokStart >= Buf.size()) break;
    // A token may not start with a digit, so the identifier-start test is
    // narrower than isIdentBody.
    if (!std::isalpha(static_cast<unsigned char>(Buf[TokStart])) &&
        Buf[TokStart] != '_')
      break;
    unsigned TokEnd = TokStart;
    while (TokEnd < Buf.size() && isIdentBody(Buf[TokEnd])) ++TokEnd;
    llvm::StringRef Tok = Buf.substr(TokStart, TokEnd - TokStart);
    if (!isCvKeyword(Tok)) break;
    Run.Keywords.push_back(Tok);
    Pos = TokEnd;
  }
  Run.End = Pos;
  return Run;
}

// ---------------------------------------------------------------------------
// ConstPlacementVisitor
// ---------------------------------------------------------------------------

/// Moves each movable cv-qualifier run to the configured side.
///
/// QualifiedTypeLoc is the one node that carries "there are qualifiers written
/// at this level", so TraverseQualifiedTypeLoc is the only hook needed.  Every
/// written qualified type is reached through it, wherever it appears: a
/// declaration's type, a parameter, a return type, a template argument, a cast,
/// an alias.
///
/// shouldVisitTemplateInstantiations() stays false (the default).  An
/// instantiation carries the *pattern's* source locations, so visiting one
/// would try to move the same qualifier a second time at the same place; the
/// return-type pass declines instantiations for exactly this reason.
class ConstPlacementVisitor
    : public RecursiveASTVisitor<ConstPlacementVisitor> {
 public:
  ConstPlacementVisitor(ASTContext& Ctx, Rewriter& RW, ConstStyle Style,
                        LintReport* Report, llvm::StringRef RuleId,
                        EditReport* Edits)
      : Ctx(Ctx),
        SM(Ctx.getSourceManager()),
        RW(RW),
        Style(Style),
        Report(Report),
        RuleId(RuleId),
        Edits(Edits) {}

  void setOwnedFiles(const FileSet* O) { Owned = O; }

  /// Post-order: the children are traversed *before* this node is handled.
  ///
  /// Qualified types nest -- `const std::map<std::string const, T const*>` is
  /// three of them, and the outer one's type specifier spans both inner ones.
  /// Each move replaces the whole span of "qualifier run plus specifier" in one
  /// go, so an outer move applied first would write a specifier text that the
  /// inner moves then edit at offsets pointing into the middle of it
  /// (`std::stconst ring`).  Handling the innermost first means every outer
  /// move reads its specifier back with the inner results already in it, via
  /// getRewrittenText.
  auto TraverseQualifiedTypeLoc(QualifiedTypeLoc TL) -> bool {
    if (!RecursiveASTVisitor::TraverseQualifiedTypeLoc(TL)) return false;
    handle(TL);
    return true;
  }

 private:
  void handle(QualifiedTypeLoc TL);

  /// Records the edit record for a move.  Offsets are into the file's
  /// original content, which is what aggregation replays against.
  void emit(llvm::StringRef Path, unsigned Begin, unsigned End,
            llvm::StringRef OldText, llvm::StringRef NewText);

  ASTContext& Ctx;
  SourceManager& SM;
  Rewriter& RW;
  ConstStyle Style;
  LintReport* Report;  // null outside Lint mode
  llvm::StringRef RuleId;
  EditReport* Edits;  // non-null in Emit mode

  /// One decl-specifier-seq can be shared by several declarators
  /// (`const int a, b;` is two VarDecls over one written `const int`), so the
  /// same QualifiedTypeLoc is handled more than once.  Keyed by the type
  /// specifier's (FileID, offset), as ApplyRenamesVisitor::renameAt is.
  std::set<std::pair<unsigned, unsigned>> Done;
  const FileSet* Owned = nullptr;
};

void ConstPlacementVisitor::handle(QualifiedTypeLoc TL) {
  const QualType QT = TL.getType();
  if (!QT.isLocalConstQualified() && !QT.isLocalVolatileQualified()) return;

  // The qualifiers must belong to the type specifier, not to a pointer,
  // reference, array or function declarator built on top of it.
  if (!isPlainTypeSpecifier(TL.getUnqualifiedLoc())) return;

  // Every location involved has to be real, written source in the file this
  // TU rewrites.  A type spelled through a macro expansion has no byte range
  // holding the qualifier, so there is nothing to move.
  const SourceRange Spec = TL.getSourceRange();
  if (Spec.getBegin().isInvalid() || Spec.getEnd().isInvalid()) return;
  if (Spec.getBegin().isMacroID()) return;

  const FileID FID = SM.getFileID(Spec.getBegin());
  // Each file is rewritten by its own TU in a direct run; in Emit mode by
  // every TU that owns it (a header has no TU of its own there).
  if (Owned ? !isRewritableFile(Spec.getBegin(), SM, *Owned)
            : FID != SM.getMainFileID())
    return;

  bool Invalid = false;
  const llvm::StringRef Buf = SM.getBufferData(FID, &Invalid);
  if (Invalid) return;

  const unsigned SpecBeginOff = SM.getFileOffset(Spec.getBegin());

  // Where the written type specifier ends, one past its last byte.
  //
  // The end location needs two pieces of care, both caused by the same thing:
  // a template argument list nested inside another closes with a `>` that the
  // lexer read as a single `>>` token and the parser then split.  For that
  // synthetic `>` Clang hands out an *expansion* location, which getFileLoc
  // maps back to the real `>>` in the file, and only its first character
  // belongs to this type -- measuring the token there instead (whether via
  // Lexer::getLocForEndOfToken or a Rewriter *token* range, which expands the
  // same way) runs one character long and swallows the enclosing list's
  // bracket, turning `V<const V<const W>>` into `V<V<W const>> const>`.
  //
  // Requiring the mapped character to be `>` is what keeps this from being a
  // hole for macros: a type whose end really does come from a macro body
  // (`#define VEC V<W>`) maps to the macro's own name token, which does not
  // spell `>`, and is declined.  A specifier that genuinely ends in a lone `>`
  // is one character long there anyway, so both cases take the same branch.
  SourceLocation LastTok = Spec.getEnd();
  if (LastTok.isMacroID()) LastTok = SM.getFileLoc(LastTok);
  if (LastTok.isInvalid() || LastTok.isMacroID()) return;
  if (SM.getFileID(LastTok) != FID) return;
  const unsigned LastTokOff = SM.getFileOffset(LastTok);
  if (LastTokOff >= Buf.size()) return;

  unsigned SpecEndOff;
  if (Buf[LastTokOff] == '>') {
    SpecEndOff = LastTokOff + 1;
  } else {
    if (Spec.getEnd().isMacroID()) return;
    const SourceLocation End =
        Lexer::getLocForEndOfToken(LastTok, 0, SM, Ctx.getLangOpts());
    if (End.isInvalid() || End.isMacroID() || SM.getFileID(End) != FID) return;
    SpecEndOff = SM.getFileOffset(End);
  }
  if (SpecEndOff <= SpecBeginOff || SpecEndOff > Buf.size()) return;
  if (!Done.insert({FID.getHashValue(), SpecBeginOff}).second) return;

  const CvRun West = scanCvRunBackward(Buf, SpecBeginOff);
  const CvRun East = scanCvRunForward(Buf, SpecEndOff);

  // Whatever was found next to the specifier must really be a qualifier of
  // *this* type.  A cv keyword that is not one of this level's local
  // qualifiers means the scan read something the AST does not agree is here,
  // and the safe response is to leave the declaration alone.  (A qualifier
  // that comes from a typedef -- `typedef volatile int VI; const VI x;` --
  // is local to the type but simply not written, which is fine: only what is
  // written is ever found.)
  auto qualifiersAreLocal = [&](const CvRun& Run) {
    for (llvm::StringRef K : Run.Keywords) {
      if (K == "const" && !QT.isLocalConstQualified()) return false;
      if (K == "volatile" && !QT.isLocalVolatileQualified()) return false;
    }
    return true;
  };
  if (!qualifiersAreLocal(West) || !qualifiersAreLocal(East)) return;

  // The run that has to move.  When the declaration already has qualifiers on
  // the target side (`const int volatile x`), the moved run lands next to them
  // and the result reads in source order either way.
  const CvRun& Moving = Style == ConstStyle::East ? West : East;
  if (Moving.empty()) return;  // already on the requested side

  // Every range below is a *character* range over the offsets computed above,
  // for the reason SpecEndOff is computed by hand.
  const SourceLocation FileStart = SM.getLocForStartOfFile(FID);
  auto locAt = [&](unsigned Off) {
    return FileStart.getLocWithOffset(static_cast<int>(Off));
  };

  // Take the specifier's text *as rewritten so far*, so that a rename another
  // cpp_format rule already made inside it (a member in `decltype(count_)`),
  // or an inner qualifier this pass has already moved, survives being
  // re-emitted here rather than being clobbered.  With no prior edits this is
  // the original source text.
  const std::string SpecText = RW.getRewrittenText(
      CharSourceRange::getCharRange(locAt(SpecBeginOff), locAt(SpecEndOff)));
  if (SpecText.empty()) return;

  // The whole move is ONE contiguous replacement spanning the qualifier run
  // and the specifier together, rather than an insert on one side and a delete
  // on the other.  That matters where this pass meets the return-type pass: an
  // insertion sitting exactly on the boundary of the range runToTrailing()
  // lifts is neither carried into the moved text nor removed by the
  // replacement, so `const int Get()` came out as `auto const Get() -> int`.
  // A replacement of the exact same byte range composes instead -- the two
  // passes agree on that range because skipQualifiersBackward/Forward find the
  // same qualifier run this scan did.
  const unsigned Begin =
      Style == ConstStyle::East ? Moving.Begin : SpecBeginOff;
  const unsigned End = Style == ConstStyle::East ? SpecEndOff : Moving.End;
  const std::string Keywords = Moving.text();
  const std::string NewText = Style == ConstStyle::East
                                  ? SpecText + " " + Keywords
                                  : Keywords + " " + SpecText;
  const llvm::StringRef OldText = Buf.substr(Begin, End - Begin);

  // getRangeSize maps the span through the edits already made, which is what
  // makes this correct after an inner qualifier has changed the specifier's
  // length; ReplaceText erases exactly that many current bytes.
  const int Size =
      RW.getRangeSize(CharSourceRange::getCharRange(locAt(Begin), locAt(End)));
  if (Size <= 0) return;
  if (RW.ReplaceText(locAt(Begin), static_cast<unsigned>(Size), NewText))
    return;

  std::string Path;
  if (const FileEntry* FE = SM.getFileEntryForID(FID)) {
    llvm::StringRef RP = FE->tryGetRealPathName();
    if (!RP.empty()) Path = relativizeToCwd(RP);
  }
  if (Edits && !Path.empty()) emit(Path, Begin, End, OldText, NewText);
  if (Report) {
    const PresumedLoc PLoc = SM.getPresumedLoc(locAt(Moving.Begin));
    Report->add({PLoc.isValid() ? PLoc.getFilename() : Path, PLoc.getLine(),
                 PLoc.getColumn(), RuleId.str(),
                 Style == ConstStyle::East
                     ? "cv-qualifier should follow the type it qualifies"
                     : "cv-qualifier should precede the type it qualifies"});
  }
}

void ConstPlacementVisitor::emit(llvm::StringRef Path, unsigned Begin,
                                 unsigned End, llvm::StringRef OldText,
                                 llvm::StringRef NewText) {
  // Drop any rename edit already recorded inside the replaced span: its text
  // rode along in NewText (via getRewrittenText), so replaying it separately
  // would double-apply.  Mirrors the same purge in runToTrailing().
  auto& E = Edits->Edits;
  E.erase(std::remove_if(E.begin(), E.end(),
                         [&](const EditRecord& R) {
                           return R.File == Path && R.Offset >= Begin &&
                                  R.Offset + R.Length <= End;
                         }),
          E.end());
  // Owner fields stay empty: this is not a rename, so no veto can ever apply
  // to it -- the same stance the trailing-return records take.
  E.push_back(
      {Path.str(), Begin, End - Begin, OldText.str(), NewText.str(), "", 0});
}

}  // namespace

// ---------------------------------------------------------------------------
// runConstPlacementOnAST
// ---------------------------------------------------------------------------

void runConstPlacementOnAST(ASTContext& Ctx, Rewriter& RW, ConstStyle Style,
                            LintReport* Report, llvm::StringRef RuleId,
                            EditReport* Edits, const FileSet* Owned) {
  ConstPlacementVisitor V(Ctx, RW, Style, Report, RuleId, Edits);
  V.setOwnedFiles(Owned);
  V.TraverseDecl(Ctx.getTranslationUnitDecl());
}

// ---------------------------------------------------------------------------
// ConstPlacementAction
// ---------------------------------------------------------------------------

namespace {

class ConstPlacementConsumer : public ASTConsumer {
 public:
  ConstPlacementConsumer(Rewriter& RW, ConstStyle Style, LintReport* Report,
                         llvm::StringRef RuleId)
      : RW(RW), Style(Style), Report(Report), RuleId(RuleId) {}

  void HandleTranslationUnit(ASTContext& Ctx) override {
    runConstPlacementOnAST(Ctx, RW, Style, Report, RuleId, /*Edits=*/nullptr);
  }

 private:
  Rewriter& RW;
  ConstStyle Style;
  LintReport* Report;
  llvm::StringRef RuleId;
};

}  // namespace

ConstPlacementAction::ConstPlacementAction(ConstStyle Style, OutputMode Mode,
                                           PendingRewrites* Pending,
                                           LintReport* Report,
                                           std::string RuleId)
    : Style(Style),
      Mode(Mode),
      Pending(Pending),
      Report(Report),
      RuleId(std::move(RuleId)) {}

auto ConstPlacementAction::CreateASTConsumer(CompilerInstance& CI,
                                             llvm::StringRef /*File*/)
    -> std::unique_ptr<ASTConsumer> {
  TheRewriter.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
  return std::make_unique<ConstPlacementConsumer>(TheRewriter, Style, Report,
                                                  RuleId);
}

void ConstPlacementAction::EndSourceFileAction() {
  // Nothing is printed or written here: several TUs may be running at once and
  // every TU must see the original on-disk sources.  The factory's flush()
  // commits the buffered content after the last TU.
  if (!Pending) return;
  SourceManager& SM = TheRewriter.getSourceMgr();
  const FileID MainFID = SM.getMainFileID();
  const FileEntry* FE = SM.getFileEntryForID(MainFID);
  if (!FE) return;
  std::string Path = FE->tryGetRealPathName().str();
  if (Path.empty()) return;

  if (Mode == OutputMode::DryRun) {
    // A dry run prints every file it was given, changed or not.
    std::string Content;
    llvm::raw_string_ostream OS(Content);
    TheRewriter.getEditBuffer(MainFID).write(OS);
    (*Pending)[std::move(Path)] = std::move(Content);
    return;
  }

  // InPlace / Lint: buffer the main file's content only if it has edits.  The
  // visitor only rewrites locations in the main file, so this is the one
  // buffer that can have any.
  for (auto It = TheRewriter.buffer_begin(); It != TheRewriter.buffer_end();
       ++It) {
    if (It->first != MainFID) continue;
    std::string Content;
    llvm::raw_string_ostream OS(Content);
    It->second.write(OS);
    (*Pending)[std::move(Path)] = std::move(Content);
    break;
  }
}

// ---------------------------------------------------------------------------
// ConstPlacementActionFactory
// ---------------------------------------------------------------------------

void ConstPlacementActionFactory::finish(std::vector<TUSlot>& Slots) {
  for (TUSlot& S : Slots) {
    for (auto& [Path, Content] : S.Pending) Pending[Path] = std::move(Content);
    if (Report)
      for (const LintDiagnostic& D : S.Report.diagnostics()) Report->add(D);
  }
}

void ConstPlacementActionFactory::flush() {
  if (Mode == OutputMode::DryRun) {
    llvm::errs() << "** Rewritten Output (Dry Run): **\n";
    const bool MultiFile = Pending.size() > 1;
    for (const auto& [Path, Content] : Pending) {
      if (MultiFile) llvm::outs() << "=== " << Path << " ===\n";
      llvm::outs() << Content;
    }
  } else if (Mode == OutputMode::InPlace) {
    for (const auto& [Path, Content] : Pending) {
      std::ofstream Out(Path, std::ios::trunc | std::ios::binary);
      Out << Content;
    }
    if (!Pending.empty()) llvm::outs() << "Modifications written to disk.\n";
  }
  Pending.clear();
}

// ---------------------------------------------------------------------------
// Test helper
// ---------------------------------------------------------------------------

namespace {

class CaptureAction : public ASTFrontendAction {
 public:
  CaptureAction(std::string& Output, ConstStyle Style)
      : Style(Style), Output(Output) {}

  void EndSourceFileAction() override {
    SourceManager& SM = TheRewriter.getSourceMgr();
    llvm::raw_string_ostream OS(Output);
    TheRewriter.getEditBuffer(SM.getMainFileID()).write(OS);
  }

  auto CreateASTConsumer(CompilerInstance& CI, llvm::StringRef /*File*/)
      -> std::unique_ptr<ASTConsumer> override {
    TheRewriter.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
    return std::make_unique<ConstPlacementConsumer>(TheRewriter, Style, nullptr,
                                                    "");
  }

 private:
  Rewriter TheRewriter;
  ConstStyle Style;
  std::string& Output;
};

}  // namespace

auto rewriteConstPlacement(llvm::StringRef Code, ConstStyle Style,
                           const std::vector<std::string>& Args)
    -> std::string {
  std::string Output;
  const bool Success = runToolOnCodeWithArgs(
      std::make_unique<CaptureAction>(Output, Style), Code, Args);
  if (!Success || Output.empty()) return Code.str();
  return Output;
}
