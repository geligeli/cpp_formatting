#ifndef CPP_FORMATTING_CONST_PLACEMENT_LIB_H_
#define CPP_FORMATTING_CONST_PLACEMENT_LIB_H_

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "clang/AST/ASTContext.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "cpp_formatting/lint_lib.h"
#include "cpp_formatting/output_mode.h"
#include "cpp_formatting/tu_driver.h"
#include "llvm/ADT/StringRef.h"

// ---------------------------------------------------------------------------
// ConstStyle
// ---------------------------------------------------------------------------

/// Which side of the type specifier the cv-qualifiers are written on.  The two
/// directions rewrite the same declarations against each other, so this is one
/// value rather than two booleans -- "both at once" is unrepresentable, as for
/// ReturnTypeStyle.
enum class ConstStyle {
  East,  ///< `const int x` -> `int const x`   (qualifier follows the type)
  West,  ///< `int const x` -> `const int x`   (qualifier precedes the type)
};

/// Parses "east" / "west" into \p Out.  Returns false for anything else.
auto parseConstStyle(llvm::StringRef Name, ConstStyle& Out) -> bool;

/// The lint rule id for \p Style: "east_const" or "west_const".
auto constStyleRuleId(ConstStyle Style) -> const char*;

// ---------------------------------------------------------------------------
// The rewrite
// ---------------------------------------------------------------------------

/// Moves every movable cv-qualifier run in \p Ctx to the side \p Style asks
/// for, writing through \p RW.
///
/// Only a qualifier written *adjacent to the type specifier* is moved, and
/// only when it qualifies the type specifier itself rather than a declarator
/// component.  That is the whole soundness argument: `const int* p` and
/// `int const* p` denote the same type, whereas the `const` in `int* const p`
/// qualifies the pointer, has no west spelling at all, and must not move.
/// Clang draws exactly that line for us -- see the .cpp.
///
/// \p Report (Lint mode) records one diagnostic per would-be move; \p Edits
/// (Emit mode) records the moves as structured edit records for cross-TU
/// aggregation.  Both may be null.
///
/// Exposed so cpp_format can run this pass on an AST it has already parsed,
/// sharing one Rewriter with the other passes.
/// \p Owned, when non-null, is the set of files this pass may rewrite (Emit
/// mode).  Without it only the TU's own main file is rewritten, which is right
/// for a direct run; in Emit mode a header has no action of its own, so it must
/// be edited by the TUs that include it.  See isRewritableFile() in
/// tu_driver.h.
void runConstPlacementOnAST(clang::ASTContext& Ctx, clang::Rewriter& RW,
                            ConstStyle Style, LintReport* Report,
                            llvm::StringRef RuleId, EditReport* Edits,
                            const FileSet* Owned = nullptr);

// ---------------------------------------------------------------------------
// ConstPlacementAction
// ---------------------------------------------------------------------------

/// Frontend action that applies the qualifier move to one translation unit.
///
/// Like TrailingReturnTypesAction, rewritten content is buffered into \p
/// Pending -- the whole main file in DryRun mode, only a changed main file
/// otherwise -- and committed by ConstPlacementActionFactory::flush() once
/// every TU has run.  Nothing is printed or written from inside a TU.
class ConstPlacementAction : public clang::ASTFrontendAction {
 public:
  ConstPlacementAction(ConstStyle Style, OutputMode Mode,
                       PendingRewrites* Pending = nullptr,
                       LintReport* Report = nullptr, std::string RuleId = "");

  void EndSourceFileAction() override;

  auto CreateASTConsumer(clang::CompilerInstance& CI, llvm::StringRef File)
      -> std::unique_ptr<clang::ASTConsumer> override;

 private:
  ConstStyle Style;
  OutputMode Mode;
  PendingRewrites* Pending;
  LintReport* Report;
  std::string RuleId;
  clang::Rewriter TheRewriter;
};

// ---------------------------------------------------------------------------
// ConstPlacementActionFactory
// ---------------------------------------------------------------------------

/// The TU driver's client for the qualifier move.  Mirrors
/// TrailingReturnActionFactory: the rewrite is purely local to one TU, so
/// there is no cross-TU state, nothing to seed and nothing that can veto.
class ConstPlacementActionFactory : public TUSlotClient {
 public:
  ConstPlacementActionFactory(ConstStyle Style, OutputMode Mode)
      : Style(Style), Mode(Mode) {}

  void setLintReport(LintReport* Report, std::string RuleId) {
    this->Report = Report;
    this->RuleId = std::move(RuleId);
  }

  // TUSlotClient
  auto ruleCount() const -> size_t override { return 0; }
  auto createAction(TUSlot& Slot)
      -> std::unique_ptr<clang::FrontendAction> override {
    return std::make_unique<ConstPlacementAction>(
        Style, Mode, &Slot.Pending, Report ? &Slot.Report : nullptr, RuleId);
  }
  auto shared() -> CrossTUState& override { return Shared; }
  auto rerunNeededOnVeto() const -> bool override { return false; }
  auto crossTUSeeding() const -> bool override { return false; }
  void finish(std::vector<TUSlot>& Slots) override;

  auto rewrites() const -> const PendingRewrites& { return Pending; }

  /// Commits the buffered content: prints it (DryRun -- one banner, then each
  /// file behind a `=== path ===` header when there are several), writes it to
  /// disk (InPlace), or drops it (Lint, where the main has already consumed
  /// it).  Call once after every TU has run.
  void flush();

 private:
  ConstStyle Style;
  OutputMode Mode;
  PendingRewrites Pending;  // merged by finish()
  CrossTUState Shared;      // always empty
  LintReport* Report = nullptr;
  std::string RuleId;
};

// ---------------------------------------------------------------------------
// Test helper
// ---------------------------------------------------------------------------

/// Parses \p Code, applies the qualifier move for \p Style, and returns the
/// transformed source.  Returns the original string if the tool fails to parse
/// the input.
auto rewriteConstPlacement(llvm::StringRef Code, ConstStyle Style,
                           const std::vector<std::string>& Args = {
                               "-std=c++17", "-xc++"}) -> std::string;

#endif  // CPP_FORMATTING_CONST_PLACEMENT_LIB_H_
