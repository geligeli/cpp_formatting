#include "cpp_formatting/trailing_return_types_lib.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <fstream>
#include <map>
#include <optional>
#include <string>

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/NestedNameSpecifier.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/TemplateName.h"
#include "clang/AST/Type.h"
#include "clang/AST/TypeLoc.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Basic/FileEntry.h"
#include "clang/Basic/IdentifierTable.h"
#include "clang/Basic/LLVM.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/Token.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/RewriteBuffer.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"

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
static auto getTypeLocLeftmostBegin(TypeLoc TL, SourceManager& SM)
    -> SourceLocation {
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
static auto skipQualifiersBackward(SourceLocation Start, SourceManager& SM)
    -> SourceLocation {
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

/// The mirror of skipQualifiersBackward: scan forward from the last token of
/// the written type over a run of `const`/`volatile`/`restrict` keywords, and
/// return the start of the last one (so the result is still a *token range*
/// end, like the location passed in).
///
/// East-const source spells the qualifier on the far side of the type
/// specifier -- `int const f()` -- and Clang's QualifiedTypeLoc range covers
/// only `int` there, exactly as it covers only `int` in `const int f()`.
/// Without this the qualifier is left behind by the move: the Leading
/// direction turned `auto f() -> int const` into `int f() const`, which for a
/// member function is a silently *different* declaration (a const member
/// function returning `int`, not a function returning `const int`), and the
/// Trailing direction produced `auto const f() -> int`.
///
/// The declarator-id always separates a return type from a function's own
/// cv-qualifiers (`int f() const`), so this scan can never reach them.
static auto skipQualifiersForward(SourceLocation End, SourceManager& SM,
                                  const LangOptions& LangOpts)
    -> SourceLocation {
  bool Inv = false;
  FileID FID = SM.getFileID(End);
  llvm::StringRef Buf = SM.getBufferData(FID, &Inv);
  if (Inv) return End;

  SourceLocation AfterEnd = Lexer::getLocForEndOfToken(End, 0, SM, LangOpts);
  if (AfterEnd.isInvalid() || SM.getFileID(AfterEnd) != FID) return End;

  unsigned Result = SM.getFileOffset(End);
  unsigned Pos = SM.getFileOffset(AfterEnd);
  const char* BP = Buf.data();

  while (Pos < Buf.size()) {
    // Skip whitespace forwards.
    unsigned TokStart = Pos;
    while (TokStart < Buf.size() &&
           std::isspace(static_cast<unsigned char>(BP[TokStart])))
      ++TokStart;
    if (TokStart >= Buf.size()) break;

    // The next token must be an identifier/keyword (which may not start with
    // a digit).
    if (!std::isalpha(static_cast<unsigned char>(BP[TokStart])) &&
        BP[TokStart] != '_')
      break;

    unsigned TokEnd = TokStart;
    while (TokEnd < Buf.size() &&
           (std::isalnum(static_cast<unsigned char>(BP[TokEnd])) ||
            BP[TokEnd] == '_'))
      ++TokEnd;

    llvm::StringRef Tok(BP + TokStart, TokEnd - TokStart);
    if (Tok != "const" && Tok != "volatile" && Tok != "restrict") break;
    Result = TokStart;  // extend the end rightward
    Pos = TokEnd;
  }

  if (Result == SM.getFileOffset(End)) return End;  // nothing to extend
  return SM.getLocForStartOfFile(FID).getLocWithOffset(Result);
}

/// Scan backwards from \p Start over whitespace only.  Deleting a trailing
/// return type starts here so the space before the `->` goes with it, rather
/// than leaving `auto foo() ` behind.
static auto skipWhitespaceBackward(SourceLocation Start, SourceManager& SM)
    -> SourceLocation {
  bool Inv = false;
  FileID FID = SM.getFileID(Start);
  llvm::StringRef Buf = SM.getBufferData(FID, &Inv);
  if (Inv) return Start;

  unsigned Pos = SM.getFileOffset(Start);
  const char* BP = Buf.data();
  while (Pos > 0 && std::isspace(static_cast<unsigned char>(BP[Pos - 1])))
    --Pos;
  if (Pos == SM.getFileOffset(Start)) return Start;
  return SM.getLocForStartOfFile(FID).getLocWithOffset(Pos);
}

/// True when the raw token spelled at \p Loc is \p Kind (and, for keywords,
/// spells \p Text).  Raw lexing reports keywords as tok::raw_identifier, so
/// `auto` is matched on its spelling.
static auto rawTokenAtIs(SourceLocation Loc, tok::TokenKind Kind,
                         llvm::StringRef Text, SourceManager& SM,
                         const LangOptions& LangOpts) -> bool {
  Token Tok;
  if (Lexer::getRawToken(Loc, Tok, SM, LangOpts, /*IgnoreWhiteSpace=*/true))
    return false;
  if (Kind == tok::raw_identifier)
    return Tok.is(tok::raw_identifier) && Tok.getRawIdentifier() == Text;
  return Tok.is(Kind);
}

/// A written type can only be *moved* into leading position if it is a pure
/// prefix declarator.  `auto f() -> int (*)()` in leading position is
/// `int (*f())()` -- a restructured declarator, not the same text elsewhere --
/// and a deduced placeholder (`-> auto`, `-> decltype(auto)`, `-> auto*`) has
/// no type to move at all.  getNextTypeLoc() walks exactly the declarator
/// components; template arguments are not part of that chain, so
/// `-> std::function<int()>` stays eligible.
static auto isMovableTypeShape(TypeLoc TL) -> bool {
  for (TypeLoc Cur = TL; !Cur.isNull(); Cur = Cur.getNextTypeLoc()) {
    switch (Cur.getTypeLocClass()) {
      case TypeLoc::FunctionProto:
      case TypeLoc::FunctionNoProto:
      case TypeLoc::ConstantArray:
      case TypeLoc::IncompleteArray:
      case TypeLoc::VariableArray:
      case TypeLoc::DependentSizedArray:
      case TypeLoc::Auto:
      case TypeLoc::DeducedTemplateSpecialization:
        return false;
      default:
        break;
    }
  }
  return true;
}

/// True when any identifier token in \p R spells one of \p Names.  Used for the
/// parameter check: the dependent spellings of a parameter reference
/// (DependentScopeDeclRefExpr and friends) carry no resolved ParmVarDecl to
/// test against, so the token text is what there is to go on.  Errs towards
/// "yes" whenever the range cannot be lexed.
static auto rangeMentionsName(SourceRange R, const llvm::StringSet<>& Names,
                              SourceManager& SM, const LangOptions& LangOpts)
    -> bool {
  SourceLocation Begin = R.getBegin();
  SourceLocation End = Lexer::getLocForEndOfToken(R.getEnd(), 0, SM, LangOpts);
  if (Begin.isInvalid() || End.isInvalid()) return true;
  FileID FID = SM.getFileID(Begin);
  if (FID != SM.getFileID(End)) return true;
  unsigned BeginOff = SM.getFileOffset(Begin);
  unsigned EndOff = SM.getFileOffset(End);
  if (EndOff <= BeginOff) return true;

  bool Inv = false;
  llvm::StringRef Buf = SM.getBufferData(FID, &Inv);
  if (Inv || EndOff > Buf.size()) return true;

  // The raw lexer requires its end pointer to be the buffer's NUL terminator,
  // so it runs to the end of the file and the range is enforced by offset.
  Lexer Lex(SM.getLocForStartOfFile(FID), LangOpts, Buf.begin(),
            Buf.begin() + BeginOff, Buf.end());
  Token Tok;
  while (true) {
    bool AtEnd = Lex.LexFromRawLexer(Tok);
    if (Tok.getLocation().isInvalid()) break;
    if (SM.getFileOffset(Tok.getLocation()) >= EndOff) break;
    if (Tok.is(tok::raw_identifier) && Names.contains(Tok.getRawIdentifier()))
      return true;
    if (AtEnd || Tok.is(tok::eof)) break;
  }
  return false;
}

/// The declaration a written type names, or null when the type names nothing
/// that is looked up by scope (builtins, pointers, template parameters -- a
/// template parameter is introduced by the parameter list, which precedes the
/// declarator either way).
static auto namedDeclForTypeLoc(TypeLoc TL) -> const NamedDecl* {
  QualType T = TL.getType();
  if (const auto* TT = T->getAs<TypedefType>()) return TT->getDecl();
  if (const auto* TST = T->getAs<TemplateSpecializationType>())
    return TST->getTemplateName().getAsTemplateDecl();
  if (const TagDecl* TD = T->getAsTagDecl()) return TD;
  return nullptr;
}

/// True when \p Outer is \p Inner or encloses it.
static auto declContextEncloses(const DeclContext* Outer,
                                const DeclContext* Inner) -> bool {
  if (!Outer || !Inner) return false;
  const DeclContext* O = Outer->getPrimaryContext();
  for (const DeclContext* C = Inner; C; C = C->getParent())
    if (C->getPrimaryContext() == O) return true;
  return false;
}

namespace {

/// Decides whether moving an out-of-line declaration's trailing return type
/// into leading position would change what its names resolve to.
///
/// Unqualified names in a trailing return type are looked up in the *semantic*
/// scope -- the class or namespace the declarator-id names -- because the
/// trailing type is written after it.  In leading position they are looked up
/// in the *lexical* scope the declaration sits in.  For `auto C::f() -> Inner`
/// those differ and the moved text stops compiling, so any name that is not
/// reachable from the lexical context vetoes the rewrite.
class LookupEscapeChecker : public RecursiveASTVisitor<LookupEscapeChecker> {
 public:
  explicit LookupEscapeChecker(const DeclContext* LexicalDC)
      : LexicalDC(LexicalDC) {}

  /// A name written with a qualifier (`std::string`) resolves the same from
  /// either position, so only its template arguments still need checking --
  /// `std::vector<Inner>` is qualified but `Inner` inside it is not.
  auto TraverseElaboratedTypeLoc(ElaboratedTypeLoc TL) -> bool {
    if (!TL.getQualifierLoc())
      return RecursiveASTVisitor::TraverseElaboratedTypeLoc(TL);
    if (auto TSTL = TL.getNamedTypeLoc().getAs<TemplateSpecializationTypeLoc>())
      for (unsigned I = 0, E = TSTL.getNumArgs(); I != E; ++I)
        if (!TraverseTemplateArgumentLoc(TSTL.getArgLoc(I))) return false;
    return true;
  }

  auto VisitTypeLoc(TypeLoc TL) -> bool {
    switch (TL.getTypeLocClass()) {
      case TypeLoc::Decltype:
      case TypeLoc::TypeOf:
      case TypeLoc::TypeOfExpr:
        // The expression inside is resolved in the semantic scope as well
        // (`-> decltype(member_)`).  Analysing it is not worth it for how
        // rarely it appears on an out-of-line definition.
        Escaped = true;
        return false;
      default:
        break;
    }
    if (const NamedDecl* ND = namedDeclForTypeLoc(TL)) {
      const DeclContext* DC = ND->getDeclContext();
      if (DC && (DC->isRecord() || DC->isNamespace()) &&
          !declContextEncloses(DC, LexicalDC)) {
        Escaped = true;
        return false;
      }
    }
    return true;
  }

  auto escaped() const -> bool { return Escaped; }

 private:
  const DeclContext* LexicalDC;
  bool Escaped = false;
};

}  // namespace

// ---------------------------------------------------------------------------
// TrailingReturnCallback implementation
// ---------------------------------------------------------------------------

TrailingReturnCallback::TrailingReturnCallback(Rewriter& Rewrite,
                                               ReturnTypeStyle Style)
    : Rewrite(Rewrite), Style(Style) {}

void TrailingReturnCallback::run(const MatchFinder::MatchResult& Result) {
  const FunctionDecl* Func = Result.Nodes.getNodeAs<FunctionDecl>("func");
  if (!Func) return;

  SourceManager& SM = *Result.SourceManager;
  if (Owned ? !isRewritableFile(Func->getLocation(), SM, *Owned)
            : !SM.isWrittenInMainFile(Func->getLocation()))
    return;

  TypeSourceInfo* TSI = Func->getTypeSourceInfo();
  if (!TSI) return;

  TypeLoc TL = TSI->getTypeLoc();
  FunctionTypeLoc FTL = TL.getAsAdjusted<FunctionTypeLoc>();
  if (!FTL) return;

  if (Style == ReturnTypeStyle::Trailing)
    runToTrailing(*Func, SM, FTL);
  else
    runToLeading(*Func, SM, FTL);
}

void TrailingReturnCallback::runToTrailing(const FunctionDecl& Func,
                                           SourceManager& SM,
                                           FunctionTypeLoc FTL) {
  TypeLoc ReturnLoc = FTL.getReturnLoc();
  SourceRange ReturnRange = ReturnLoc.getSourceRange();

  // Skip functions whose return type is written as plain `auto` (or
  // `decltype(auto)`) with no trailing `->`.  Rewriting them would just
  // produce `auto foo() -> auto { ... }` which is redundant noise.
  if (ReturnLoc.getAs<AutoTypeLoc>()) return;

  // A return type whose spelling comes even partly from a macro expansion
  // cannot be hoisted: its source range does not cover the text that was
  // written.  abseil declares `const ElfW(Phdr)* GetPhdr(int) const;`, where
  // ElfW is a macro -- getTypeLocLeftmostBegin() skips macro locations, so the
  // range collapses onto the `*` alone and the rewrite produces
  // `const ElfW(Phdr)auto GetPhdr(int) const -> *;`.  The reverse direction
  // already refuses these; this is the same rule for the forward one.
  if (ReturnRange.getBegin().isMacroID() || ReturnRange.getEnd().isMacroID())
    return;

  // Whatever follows the cv/ref/noexcept qualifiers has to be something the
  // trailing return type may legitimately precede.  An attribute-specifier-seq
  // there is part of `parameters-and-qualifiers`, so `-> T` belongs *after* it
  // ([dcl.fct]) -- but FunctionTypeLoc::getLocalRangeEnd() stops at the last
  // qualifier, so inserting at that point puts the arrow on the wrong side.
  // abseil's `pointer data() noexcept ABSL_ATTRIBUTE_LIFETIME_BOUND` came out
  // as `auto data() noexcept -> pointer ABSL_ATTRIBUTE_LIFETIME_BOUND`, which
  // Clang rejects; gcc accepts it, so it survives a rebuild and only surfaces
  // on the next parse.  The attribute is usually a macro, so rather than work
  // out where it ends, refuse the declaration.
  {
    const LangOptions& LO = Func.getASTContext().getLangOpts();
    std::optional<Token> Next =
        Lexer::findNextToken(FTL.getLocalRangeEnd(), SM, LO);
    if (!Next) return;
    switch (Next->getKind()) {
      case tok::l_brace:  // definition
      case tok::semi:     // declaration
      case tok::equal:    // = 0 / = default / = delete
      case tok::arrow:  // already trailing (excluded earlier, belt and braces)
      case tok::comma:  // another declarator
      case tok::r_paren:
        break;
      case tok::identifier: {
        // `override` and `final` come after the declarator, so the arrow still
        // precedes them; anything else here is an attribute macro.
        const IdentifierInfo* II = Next->getIdentifierInfo();
        if (!II || (II->getName() != "override" && II->getName() != "final"))
          return;
        break;
      }
      default:
        return;  // `[[...]]`, `requires`, or anything else unexpected
    }
  }

  // Pointer/reference TypeLocs (e.g. LValueReferenceTypeLoc for `T &`) only
  // report their sigil as their local begin; the base type lives in the next
  // TypeLoc in the chain.  Walk the chain to find the true leftmost location,
  // then extend outward past the cv-qualifiers on either side of the type
  // specifier -- QualifiedTypeLoc's range omits them whichever side they are
  // written on, so `const int f()` and the east-const `int const f()` both
  // report just `int`.  Missing the trailing one strands the qualifier on the
  // `auto` placeholder.
  const LangOptions& LangOpts = Func.getASTContext().getLangOpts();
  SourceLocation TypeBegin = getTypeLocLeftmostBegin(ReturnLoc, SM);
  SourceLocation ExtStart = skipQualifiersBackward(TypeBegin, SM);
  SourceLocation ExtEnd =
      skipQualifiersForward(ReturnRange.getEnd(), SM, LangOpts);
  SourceRange FullReturnRange(ExtStart, ExtEnd);

  // In a declarator whose return type *wraps* the function name -- a function
  // returning a function pointer, `int (*f(int))(bool)` -- the return TypeLoc's
  // source range spans the whole declarator, name and parameters included.
  // Hoisting it would duplicate the whole declarator after the `->`.  Only
  // rewrite when the return type is written entirely before the name.
  if (!SM.isBeforeInTranslationUnit(ExtEnd, Func.getLocation())) return;

  // Extract the return-type text, accounting for edits already applied to
  // this buffer.  In cpp_format's combined pass a rename rule may already
  // have rewritten an identifier inside the return type (e.g. a member in
  // `decltype(count_)`); getRewrittenText picks up that rename so the text
  // moved after `->` stays consistent — the wholesale replace below would
  // otherwise silently clobber the nested edit.  With no prior edits this
  // returns the exact original source text, as before.
  std::string OriginalTypeStr =
      Rewrite.getRewrittenText(CharSourceRange::getTokenRange(FullReturnRange));

  if (OriginalTypeStr.empty()) return;

  // Pad "auto" if the character immediately after the return-type token would
  // merge with the next token (e.g. `Foo&operator=` -> `auto operator=`).
  SourceLocation AfterReturnLoc =
      Lexer::getLocForEndOfToken(ExtEnd, 0, SM, LangOpts);
  bool Invalid = false;
  const char* NextChar = SM.getCharacterData(AfterReturnLoc, &Invalid);
  std::string AutoReplacement = "auto";
  if (!Invalid && NextChar &&
      !std::isspace(static_cast<unsigned char>(*NextChar)))
    AutoReplacement = "auto ";

  // Replace the full return type (including any leading qualifiers) with
  // "auto".  A failed replace means the location is not rewritable (e.g. it
  // comes from a macro expansion); skip the whole rewrite rather than emit a
  // trailing `->` without the `auto` replacement.
  if (Rewrite.ReplaceText(FullReturnRange, AutoReplacement)) return;

  // Insert " -> OriginalType" after the local range end of the function type
  // (closing ')' plus any cv/ref/noexcept qualifiers tracked by
  // FunctionTypeLoc).
  SourceLocation InsertLoc = FTL.getLocalRangeEnd();
  Rewrite.InsertTextAfterToken(InsertLoc, " -> " + OriginalTypeStr);

  if (Emit) {
    auto offsetOf = [&](SourceLocation L) {
      return SM.getDecomposedLoc(SM.getSpellingLoc(L)).second;
    };
    std::pair<FileID, unsigned> Begin =
        SM.getDecomposedLoc(SM.getSpellingLoc(ExtStart));
    unsigned BeginOff = Begin.second;
    unsigned EndOff =
        offsetOf(Lexer::getLocForEndOfToken(ExtEnd, 0, SM, LangOpts));
    std::string Path;
    if (const FileEntry* FE = SM.getFileEntryForID(Begin.first)) {
      StringRef RP = FE->tryGetRealPathName();
      if (!RP.empty()) Path = relativizeToCwd(RP);
    }
    if (!Path.empty() && EndOff > BeginOff) {
      // Drop rename edits already recorded inside the return type: their text
      // is carried into OriginalTypeStr (via getRewrittenText) and moved after
      // the
      // "->", so replaying them separately would double-apply / conflict.
      auto& E = Emit->Edits;
      E.erase(std::remove_if(E.begin(), E.end(),
                             [&](const EditRecord& R) {
                               return R.File == Path && R.Offset >= BeginOff &&
                                      R.Offset + R.Length <= EndOff;
                             }),
              E.end());
      std::string OldText =
          Lexer::getSourceText(CharSourceRange::getTokenRange(FullReturnRange),
                               SM, LangOpts)
              .str();
      E.push_back(
          {Path, BeginOff, EndOff - BeginOff, OldText, AutoReplacement});
      unsigned InsOff =
          offsetOf(Lexer::getLocForEndOfToken(InsertLoc, 0, SM, LangOpts));
      E.push_back({Path, InsOff, 0, "", " -> " + OriginalTypeStr});
    }
  }

  if (Report) {
    PresumedLoc PLoc = SM.getPresumedLoc(Func.getLocation());
    Report->add({PLoc.isValid() ? PLoc.getFilename() : "", PLoc.getLine(),
                 PLoc.getColumn(), RuleId,
                 "function should use trailing return type"});
  }
}

void TrailingReturnCallback::runToLeading(const FunctionDecl& Func,
                                          SourceManager& SM,
                                          FunctionTypeLoc FTL) {
  // A lambda's call operator has a trailing return type and no leading return
  // type to move it into -- `[]() -> int {}` has no declarator-id at all.  The
  // Trailing direction never had to exclude lambdas (one written without a
  // trailing return has a deduced `auto`, which its AutoTypeLoc check skips),
  // so this guard exists only on this side.
  if (const auto* MD = dyn_cast<CXXMethodDecl>(&Func))
    if (MD->getParent() && MD->getParent()->isLambda()) return;

  // For a trailing-return declarator the parser records both endpoints this
  // rewrite needs on the FunctionTypeLoc: the local range *begins* at the
  // `auto` placeholder and *ends* at the `->` (Parser::ParseFunctionDeclarator
  // sets StartLoc to the TST_auto decl-spec and LocalEndLoc to the arrow).
  // Both are verified by spelling below rather than trusted -- a declarator
  // whose placeholder is not literally `auto` is left alone.
  const LangOptions& LangOpts = Func.getASTContext().getLangOpts();
  SourceLocation AutoLoc = FTL.getLocalRangeBegin();
  SourceLocation ArrowLoc = FTL.getLocalRangeEnd();
  if (AutoLoc.isInvalid() || ArrowLoc.isInvalid()) return;
  if (AutoLoc.isMacroID() || ArrowLoc.isMacroID()) return;
  if (!rawTokenAtIs(AutoLoc, tok::raw_identifier, "auto", SM, LangOpts)) return;
  if (!rawTokenAtIs(ArrowLoc, tok::arrow, "", SM, LangOpts)) return;

  TypeLoc ReturnLoc = FTL.getReturnLoc();
  if (ReturnLoc.isNull()) return;
  SourceRange ReturnRange = ReturnLoc.getSourceRange();
  if (ReturnRange.getBegin().isInvalid() || ReturnRange.getEnd().isInvalid())
    return;
  if (ReturnRange.getBegin().isMacroID() || ReturnRange.getEnd().isMacroID())
    return;

  if (!isMovableTypeShape(ReturnLoc)) return;

  // Same leftmost-begin and cv-qualifier treatment as the Trailing direction:
  // `-> const int*` reports its range starting at `int`, and `-> int const`
  // reports it ending at `int`.  Scanning back for qualifiers is bounded by
  // the `>` of the arrow, so it can never run past the type into the
  // declarator's own `const` in `auto f() const -> int`; scanning forward runs
  // into the body or the `;`.  Leaving a trailing qualifier behind here is not
  // cosmetic: `int f() const` is a const *member function* returning `int`, a
  // different declaration from the one that was written.
  SourceLocation TypeBegin = getTypeLocLeftmostBegin(ReturnLoc, SM);
  SourceLocation ExtStart = skipQualifiersBackward(TypeBegin, SM);
  SourceLocation ExtEnd =
      skipQualifiersForward(ReturnRange.getEnd(), SM, LangOpts);
  SourceRange FullReturnRange(ExtStart, ExtEnd);

  // Parameters are not in scope before the declarator-id, so
  // `auto f(T a) -> decltype(a.size())` has nowhere to move to.
  llvm::StringSet<> ParamNames;
  for (const ParmVarDecl* P : Func.parameters())
    if (const IdentifierInfo* II = P->getIdentifier())
      ParamNames.insert(II->getName());
  if (!ParamNames.empty() &&
      rangeMentionsName(FullReturnRange, ParamNames, SM, LangOpts))
    return;

  // An out-of-line declaration looks names up in two different scopes
  // depending on which side of the declarator-id they are written on; see
  // LookupEscapeChecker.  In-class and in-namespace declarations resolve the
  // same either way and need no check.
  if (Func.getLexicalDeclContext() != Func.getDeclContext()) {
    LookupEscapeChecker Checker(Func.getLexicalDeclContext());
    Checker.TraverseTypeLoc(ReturnLoc);
    if (Checker.escaped()) return;
  }

  // Carry along any edit a rename rule already made inside the trailing type:
  // that text moves to the placeholder, and the delete below would otherwise
  // drop the rename entirely.  Mirrors the same call in runToTrailing().
  std::string TypeStr =
      Rewrite.getRewrittenText(CharSourceRange::getTokenRange(FullReturnRange));
  if (TypeStr.empty()) return;

  SourceLocation DeleteStart = skipWhitespaceBackward(ArrowLoc, SM);
  SourceLocation TypeEnd = Lexer::getLocForEndOfToken(ExtEnd, 0, SM, LangOpts);
  if (TypeEnd.isInvalid()) return;
  // Every offset below -- including the one the Emit purge measures the
  // trailing type with -- has to come from the same file to mean anything.
  FileID FID = SM.getFileID(AutoLoc);
  if (SM.getFileID(DeleteStart) != FID || SM.getFileID(TypeEnd) != FID ||
      SM.getFileID(ExtStart) != FID)
    return;
  unsigned AutoOff = SM.getFileOffset(AutoLoc);
  unsigned DeleteOff = SM.getFileOffset(DeleteStart);
  unsigned TypeBeginOff = SM.getFileOffset(ExtStart);
  unsigned TypeEndOff = SM.getFileOffset(TypeEnd);
  if (TypeEndOff <= DeleteOff || DeleteOff <= AutoOff ||
      TypeBeginOff < DeleteOff)
    return;

  // How many bytes `-> type` occupies *now*.  Rewriter::RemoveText passes its
  // length straight through to the edit buffer without mapping it, so the
  // original byte count would be wrong as soon as a rename rule has changed
  // the length of something inside the trailing type (`-> decltype(count_)`
  // becoming `-> decltype(m_count)` would leave the `)` behind).  getRangeSize
  // is the accessor that maps a range through the edits already made.
  const int DeleteLen =
      Rewrite.getRangeSize(CharSourceRange::getCharRange(DeleteStart, TypeEnd));
  if (DeleteLen <= 0) return;

  // Replace the placeholder in place -- which keeps `static`/`constexpr` and
  // any attributes in front of it exactly where they were -- then delete the
  // `-> type`.  A failed replace means the location is not rewritable; bailing
  // before the delete leaves the declaration intact rather than stripping its
  // only return type.
  if (Rewrite.ReplaceText(AutoLoc, 4, TypeStr)) return;
  Rewrite.RemoveText(DeleteStart, DeleteLen);

  if (Emit) {
    std::string Path;
    if (const FileEntry* FE = SM.getFileEntryForID(FID)) {
      StringRef RP = FE->tryGetRealPathName();
      if (!RP.empty()) Path = relativizeToCwd(RP);
    }
    if (!Path.empty()) {
      // Drop rename edits already recorded inside the trailing type: their
      // text rode along in TypeStr (via getRewrittenText) and moves to the
      // placeholder, so replaying them would double-apply / conflict.
      auto& E = Emit->Edits;
      E.erase(std::remove_if(E.begin(), E.end(),
                             [&](const EditRecord& R) {
                               return R.File == Path &&
                                      R.Offset >= TypeBeginOff &&
                                      R.Offset + R.Length <= TypeEndOff;
                             }),
              E.end());
      E.push_back({Path, AutoOff, 4, "auto", TypeStr});
      std::string Removed =
          Lexer::getSourceText(
              CharSourceRange::getCharRange(DeleteStart, TypeEnd), SM, LangOpts)
              .str();
      E.push_back({Path, DeleteOff, TypeEndOff - DeleteOff, Removed, ""});
    }
  }

  if (Report) {
    PresumedLoc PLoc = SM.getPresumedLoc(Func.getLocation());
    Report->add({PLoc.isValid() ? PLoc.getFilename() : "", PLoc.getLine(),
                 PLoc.getColumn(), RuleId,
                 "function should use leading return type"});
  }
}

// ---------------------------------------------------------------------------
// Shared matcher registration
// ---------------------------------------------------------------------------

void registerTrailingReturnMatchers(MatchFinder& Finder,
                                    TrailingReturnCallback& Callback,
                                    ReturnTypeStyle Style) {
  if (Style == ReturnTypeStyle::Leading) {
    // The mirror predicate.  `unless(isInstantiated())` is load-bearing for the
    // same reason it is below, and every other exclusion the Trailing matcher
    // needs is implied here: a conversion function, a constructor or a
    // defaulted function cannot carry a trailing return type in the first
    // place, and `-> void` is a perfectly good thing to move back.  The
    // declaration-shape guards live in runToLeading(), which has the
    // FunctionTypeLoc to inspect.
    Finder.addMatcher(
        functionDecl(hasTrailingReturn(), unless(isInstantiated()))
            .bind("func"),
        &Callback);
    return;
  }

  // `unless(isInstantiated())` is load-bearing: an instantiation carries the
  // *pattern's* source locations, so without it a template instantiated in the
  // same TU is rewritten a second time at the same place -- and by then the
  // return type reads `auto`, so the second rewrite emits `-> auto`.  Under
  // --emit-edits those land in one record file as conflicting edits at one
  // offset (this is what googletest's matcher headers hit).
  //
  // isInstantiated() rather than the narrower isTemplateInstantiation():
  // instantiating a class template re-creates its member function *templates*
  // as patterns, which keep TSK_Undeclared while still pointing back at the
  // original locations, so only the "has an instantiated ancestor" arm catches
  // them.  Explicit specializations are written out in source and keep being
  // rewritten (TSK_ExplicitSpecialization is not an instantiation).  The
  // rename side takes the same stance, via
  // shouldVisitTemplateInstantiations() == false.
  Finder.addMatcher(
      functionDecl(unless(hasTrailingReturn()), unless(returns(voidType())),
                   unless(cxxConversionDecl()), unless(isDefaulted()),
                   unless(isInstantiated()))
          .bind("func"),
      &Callback);
}

// ---------------------------------------------------------------------------
// TrailingReturnTypesAction implementation
// ---------------------------------------------------------------------------

TrailingReturnTypesAction::TrailingReturnTypesAction(OutputMode Mode,
                                                     PendingRewrites* Pending,
                                                     LintReport* Report,
                                                     std::string RuleId,
                                                     ReturnTypeStyle Style)
    : Mode(Mode), Pending(Pending), Style(Style), Callback(TheRewriter, Style) {
  Callback.setLintReport(Report, std::move(RuleId));
}

void TrailingReturnTypesAction::EndSourceFileAction() {
  // Nothing is printed or written here: several TUs may be running at once,
  // and every TU must see the original on-disk sources.  The factory's flush()
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
  // matchers only rewrite declarations written in the main file, so this is
  // the one buffer that can have any.
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

void TrailingReturnActionFactory::finish(std::vector<TUSlot>& Slots) {
  for (TUSlot& S : Slots) {
    for (auto& [Path, Content] : S.Pending) Pending[Path] = std::move(Content);
    if (Report)
      for (const LintDiagnostic& D : S.Report.diagnostics()) Report->add(D);
  }
}

void TrailingReturnActionFactory::flush() {
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

auto TrailingReturnTypesAction::CreateASTConsumer(CompilerInstance& CI,
                                                  StringRef /*File*/)
    -> std::unique_ptr<ASTConsumer> {
  TheRewriter.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
  registerTrailingReturnMatchers(Finder, Callback, Style);
  return Finder.newASTConsumer();
}

// ---------------------------------------------------------------------------
// CaptureAction — runs the rewrite and captures the result into a string
// (used by the test helper below)
// ---------------------------------------------------------------------------

namespace {

class CaptureAction : public ASTFrontendAction {
 public:
  CaptureAction(std::string& Output, ReturnTypeStyle Style)
      : Callback(TheRewriter, Style), Style(Style), Output(Output) {}

  void EndSourceFileAction() override {
    SourceManager& SM = TheRewriter.getSourceMgr();
    llvm::raw_string_ostream OS(Output);
    TheRewriter.getEditBuffer(SM.getMainFileID()).write(OS);
  }

  auto CreateASTConsumer(CompilerInstance& CI, StringRef /*File*/)
      -> std::unique_ptr<ASTConsumer> override {
    TheRewriter.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
    registerTrailingReturnMatchers(Finder, Callback, Style);
    return Finder.newASTConsumer();
  }

 private:
  Rewriter TheRewriter;
  TrailingReturnCallback Callback;  ///< must be declared after TheRewriter
  ReturnTypeStyle Style;
  MatchFinder Finder;
  std::string& Output;
};

}  // namespace

// ---------------------------------------------------------------------------
// Test helper
// ---------------------------------------------------------------------------

static auto rewriteReturnTypes(llvm::StringRef Code,
                               const std::vector<std::string>& Args,
                               ReturnTypeStyle Style) -> std::string {
  std::string Output;
  bool Success = runToolOnCodeWithArgs(
      std::make_unique<CaptureAction>(Output, Style), Code, Args);
  if (!Success || Output.empty()) return Code.str();
  return Output;
}

auto rewriteToTrailingReturnTypes(llvm::StringRef Code,
                                  const std::vector<std::string>& Args)
    -> std::string {
  return rewriteReturnTypes(Code, Args, ReturnTypeStyle::Trailing);
}

auto rewriteToLeadingReturnTypes(llvm::StringRef Code,
                                 const std::vector<std::string>& Args)
    -> std::string {
  return rewriteReturnTypes(Code, Args, ReturnTypeStyle::Leading);
}
