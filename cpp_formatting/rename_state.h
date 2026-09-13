#ifndef CPP_FORMATTING_RENAME_STATE_H_
#define CPP_FORMATTING_RENAME_STATE_H_

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "cpp_formatting/lint_lib.h"
#include "llvm/ADT/StringRef.h"

// The state a rename pass shares *across* translation units: dependent-token
// resolutions, vetoes, and the conflicts it reports.  Split out of
// rename_variables_lib so the TU driver (tu_driver.h) can carry it per TU and
// merge it without depending on the AST visitors.

// ---------------------------------------------------------------------------
// Cross-TU resolution of template-dependent member tokens
// ---------------------------------------------------------------------------
//
// A member access through a template parameter — e.g. `x.val` in
//   auto set_val(auto& x) { x.val = 12; }
// — is a *dependent* expression: the member it names is only known once the
// template is instantiated, and the instantiation typically lives in a
// different translation unit than the header that spells the token.  The
// per-main-file Rewriter model therefore cannot rename such a token on its own:
// the header's own TU never instantiates the template, and the instantiating
// .cpp's TU may not rewrite the header.
//
// To bridge the two, every TU records — keyed by the token's (real path, byte
// offset) — the new name each observed instantiation resolves the token to.
// When the header is later processed as its own main file, the token is
// rewritten using that agreed-upon name.  If instantiations disagree, or any
// instantiation binds the token to a member that is not being renamed (e.g. a
// type outside the FileSet), the entry is vetoed and the token is left
// untouched.  The TU driver runs every non-header TU before any header TU so
// the common case (instantiations in .cpp files, token in a header) needs no
// second parse, and re-runs a TU whose map turned out to be stale so the result
// does not depend on the order of the sources at all (see tu_driver.h).
struct DependentResolution {
  std::string NewName;   ///< the agreed-upon new name
  bool HasName = false;  ///< at least one instantiation resolved a new name
  bool Vetoed = false;   ///< conflicting / out-of-scope binding: do not rewrite
  std::string OldName;   ///< spelled member identifier (for emitting a record)
  unsigned Length = 0;   ///< length of the spelled token
  /// The member the token resolves to, keyed as in RenameVetoes, so that a
  /// veto of that member also drops this token in aggregation.
  std::string OwnerFile;
  unsigned OwnerOffset = 0;
};

/// (real path — or, for in-memory buffers, a presumed file name — and byte
/// offset within that file) -> resolution.
using DependentResolutions =
    std::map<std::pair<std::string, unsigned>, DependentResolution>;

/// Records that one instantiation resolved the token at \p Key to \p NewName.
/// A vetoed entry stays vetoed; a different name than the one already recorded
/// vetoes the entry (instantiations disagree); the same name is a no-op apart
/// from refreshing the informational fields.
void recordResolution(DependentResolutions& DepRes,
                      const std::pair<std::string, unsigned>& Key,
                      const std::string& NewName, llvm::StringRef OldName,
                      unsigned Length, std::pair<std::string, unsigned> Owner);

/// Marks the token at \p Key as not rewritable.
void vetoResolution(DependentResolutions& DepRes,
                    const std::pair<std::string, unsigned>& Key);

/// Folds every entry of \p From into \p Into with the semantics above: a veto
/// is absorbing, two different names veto, the same name is idempotent.  The
/// resolved state (HasName, NewName, Vetoed) is therefore a lattice join —
/// commutative and idempotent — so replaying a map that was seeded from \p Into
/// is safe.  The informational fields take the last writer, which under the
/// driver's source-order merge is the same TU that would have written them
/// last in a serial run.
void mergeDependentResolutions(DependentResolutions& Into,
                               const DependentResolutions& From);

/// True when the entries whose key file is \p File differ between \p A and
/// \p B in their resolved state (HasName, NewName, Vetoed), or exist in only
/// one of the two maps.  Used by the TU driver to decide whether a TU applied
/// its dependent tokens from a map that other TUs have since changed.
auto dependentResolutionsDifferFor(const DependentResolutions& A,
                                   const DependentResolutions& B,
                                   llvm::StringRef File) -> bool;

// ---------------------------------------------------------------------------
// Renames vetoed by a reference the tool cannot rewrite
// ---------------------------------------------------------------------------
//
// A reference spelled inside a macro *body* — `#define BUMP(c) ((c).count +=
// 1)` — or synthesized by token pasting has no byte range in any source file
// that holds the name: the body token is one location shared by every
// expansion, and a pasted token exists only in Clang's scratch buffer. Renaming
// the declaration would leave such a reference spelling the old name, which is
// a build break rather than a formatting change.
//
// Renaming is therefore all-or-nothing: a single unrewritable reference vetoes
// the declaration, and it keeps its old name everywhere.  The veto is keyed by
// the declaration's (real path, byte offset) — stable across TUs *and* across
// processes, which is what lets one Bazel action's veto suppress the edits
// another action already emitted for the same declaration (see EditRecord's
// owner fields in lint_lib.h).
//
// Within one run the map is shared by every TU through the TU driver.  A veto
// found in a TU processed *after* one that already renamed the declaration
// cannot undo that TU's buffered rewrite, so the driver discards the whole
// pass and runs it again with the vetoes known up front (see tu_driver.h);
// nothing is written to disk until flush(), so discarding a pass costs
// nothing.  Emit mode skips that re-run: its output is records carrying these
// vetoes, and aggregation does the dropping.
//
// A macro *argument* is not affected: `FWD(c.count)` spells `count` at the call
// site, so it is rewritten normally (see ApplyRenamesVisitor::rewriteLoc). That
// holds for template-dependent tokens too -- `EXPECT_FALSE(this->count_)` in a
// class template -- since DependentResolutions is keyed by the spelling.
using RenameVetoes = std::map<std::pair<std::string, unsigned>, RenameVeto>;

// ---------------------------------------------------------------------------
// Declining a rename by *name*
// ---------------------------------------------------------------------------
//
// Two of the tool's backstops know only the spelling of what they could not
// rewrite, not which declaration it belongs to: a dependent token that no
// instantiation in any translation unit ever resolved (a member named in a
// template argument, which Clang folds to a value at instantiation, leaves no
// node to observe), and a use in an instantiation that could not be mapped
// back to its pattern.  Such a veto is stored in the same RenameVetoes map
// under a key no file can have -- a leading '\x01' plus the name -- so that it
// travels, seeds, merges, triggers re-runs and serializes exactly like a
// declaration veto, and every collector checks each declaration's *name*
// against it before renaming.  Aggregation drops every rename edit whose old
// spelling matches.  Over-declining -- every declaration of that name, not
// just the one meant -- is the safe side.
inline auto nameVetoKey(llvm::StringRef Name)
    -> std::pair<std::string, unsigned> {
  return {std::string("\x01") + Name.str(), 0};
}
inline auto isNameVeto(llvm::StringRef VetoFile) -> bool {
  return !VetoFile.empty() && VetoFile[0] == '\x01';
}

// ---------------------------------------------------------------------------
// Skipped renames
// ---------------------------------------------------------------------------
//
// A rename that was *not* applied because the new name is already taken in the
// same scope, or because a reference to it cannot be rewritten.  Renaming
// anyway would either be a redeclaration error or, worse, silently rebind
// existing uses to a different entity -- so the declaration and all its uses
// are left alone and the site is reported.
//
// This is lint_lib's RenameSkip: the same record is reported straight to
// stderr by a direct run and serialized into the edit records by an emit run,
// so that `--aggregate` can report the whole repository's skips at once.
using RenameConflict = RenameSkip;
using RenameConflicts = std::vector<RenameConflict>;

#endif  // CPP_FORMATTING_RENAME_STATE_H_
