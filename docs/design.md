# Key design decisions

Part of the maintainer notes indexed by [AGENTS.md](../AGENTS.md).


## `trailing_return_types`: what gets rewritten

| Input | Rewritten? | Reason |
|---|---|---|
| `int foo()` | yes | plain return type |
| `const int* foo()` | yes | leading qualifier included via backwards scan |
| `void foo()` | no | void excluded from matcher |
| `auto foo() -> int` | no | already has trailing return (`hasTrailingReturn()`) |
| `auto foo() { return 42; }` | no | deduced auto detected via `AutoTypeLoc` check |
| `decltype(auto) foo()` | no | same `AutoTypeLoc` check |
| `int foo();` (declaration only) | yes | all declarations rewritten independently |
| `int foo(); int foo() {...}` (both in same TU) | yes (both) | each declaration rewritten independently |
| a template instantiated in the same TU | pattern only | an implicit instantiation carries the pattern's source locations, so matching it too would rewrite the same place twice (`unless(isTemplateInstantiation())`) |

## `leading_return_types`: the reverse direction, and why it rejects more

`ReturnTypeStyle::Leading` (`--reverse`, or `return_types: leading`) is the
mirror rewrite, and the core of it is genuinely symmetric: for a
trailing-return declarator the parser leaves both endpoints on the
`FunctionTypeLoc` that `runToTrailing()` already uses — `getLocalRangeBegin()`
is the `auto` placeholder and `getLocalRangeEnd()` is the `->`
(`Parser::ParseFunctionDeclarator` sets `StartLoc` to the `TST_auto` decl-spec
and `LocalEndLoc` to the arrow). Both are re-checked by spelling rather than
trusted. Replacing the placeholder *in place* is what keeps `static` /
`constexpr` / attributes in their original order for free.

What is not symmetric is what may move. A trailing return type can say things
leading position cannot express, so `runToLeading()` is mostly guards:

| Input | Rewritten? | Reason |
|---|---|---|
| `auto foo() -> int` | yes | |
| `auto foo() const noexcept -> int` | yes | the qualifiers sit before the arrow and never move |
| `static constexpr auto foo() -> int` | yes | the placeholder is replaced in place |
| `auto Foo::bar() -> std::string` | yes | a name written qualified resolves the same from either position |
| `auto f(T a) -> decltype(a.size())` | no | parameters are not in scope before the declarator-id (`rangeMentionsName` on the parameter names — the dependent spellings carry no resolved `ParmVarDecl` to test against) |
| `auto make() -> int (*)(bool)` | no | leading position needs a *restructured* declarator (`int (*make())(bool)`); `isMovableTypeShape` walks `getNextTypeLoc()` for a function/array component |
| `auto get() -> int (&)[4]` | no | same |
| `auto Foo::bar() -> Inner` | no | `Inner` is found in class scope only after the declarator-id (`LookupEscapeChecker`) |
| `auto Foo::bar() -> ns::Vec<Inner>` | no | the *template argument* is unqualified even though the template name is not — `LookupEscapeChecker` traverses argument locs for exactly this |
| `auto foo() -> auto` / `-> decltype(auto)` | no | a placeholder has no type to move (`isMovableTypeShape` again) |
| `[]() -> int {}` | no | a lambda has no declarator-id to put a return type on. The Trailing direction never needed this guard: a lambda either has a trailing return (excluded by `unless(hasTrailingReturn())`) or a deduced one (excluded by its `AutoTypeLoc` check) |
| `auto foo() -> INT_T` (macro) | no | the rewrite never edits through a macro expansion |

`LookupEscapeChecker` only runs when `getLexicalDeclContext() !=
getDeclContext()`. An in-class or in-namespace declaration is looked up in the
same scope from either side of the declarator-id, so there is nothing to check
— and running the check anyway would reject ordinary `using namespace` code.

In `Emit` mode the direction mirrors the subsumption in `runToTrailing()`: a
rename edit recorded *inside the trailing type* rode along in the text that
moves to the placeholder (via `Rewriter::getRewrittenText()`), so those records
are dropped before the two new ones are pushed. Forgetting that half is how
the same offset ends up with conflicting edits after aggregation.

Both directions are fixpoints, and each is a fixpoint on the other's output
for everything it moved — see `mini_repo-return_types_roundtrip` below.

## `const_placement`: which qualifier moves, and why

`const int x` and `int const x` declare the same thing, so moving the qualifier
across the type specifier is a pure formatting change. `int* const p` is a
*different type* from `const int* p`, so moving that one is not. The whole pass
turns on telling those apart, and Clang already does it: a `QualifiedTypeLoc`'s
qualifiers belong to whatever its **unqualified loc** is, and those locs nest
the way the declarator reads.

| Written | TypeLoc nesting | What the `const` qualifies |
|---|---|---|
| `const int* p` | `Pointer( Qualified{const}( Builtin int ) )` | the `int` — movable |
| `int const* p` | the same | the `int` — movable |
| `int* const p` | `Qualified{const}( Pointer( Builtin int ) )` | the *pointer* — declined |

So `isPlainTypeSpecifier()` walks the unqualified loc's `getNextTypeLoc()` chain
and declines the whole node if any component is a pointer, reference, array,
function type or paren — the same shape test `isMovableTypeShape()` makes for
the return-type pass. Declining those also keeps the rewrite's source range
honest: an array or function declarator's TypeLoc range runs *past* the type
specifier and over the declarator-id (`const int a[3]`'s `ConstantArrayTypeLoc`
spells `int a[3]`), so appending ` const` to its end would produce
`int a[3] const`. Every loc that survives the filter spells the type specifier
and nothing else.

A member function's own `const` (`int f() const`) is not a `QualifiedTypeLoc` at
all — it lives on the `FunctionProtoType`'s method qualifiers — so the pass
never sees it, and `const int f() const` moves exactly one of its two `const`s.

**Finding the written qualifier.** `QualifiedTypeLoc::getSourceRange()` covers
only the type specifier; the qualifier keywords are outside it on one side or
the other, exactly as they are for `skipQualifiersBackward()` in the return-type
pass. `scanCvRunBackward`/`scanCvRunForward` scan the raw buffer for the run of
`const`/`volatile` keywords adjacent to the specifier. Consequences of scanning
the buffer rather than the AST, all deliberate:

- A qualifier the preprocessor produced is never found, because the buffer
  spells the macro name (`#define CONST const` leaves `CONST int x` alone), and
  a specifier whose own location is a macro expansion is declined outright.
- A run is only recognised when it is *adjacent* to the specifier. `const static
  int x` is left alone (`static` is not a cv keyword), as is
  `const /*why*/ int x` — whitespace is skipped, comments are not.
- The run is re-emitted with single spaces, so `const    int` comes back as
  `int const`. That is the one thing an east→west round trip cannot undo.

**Whole runs move, in source order.** `const volatile int x` becomes
`int const volatile x`; a declaration with qualifiers on *both* sides
(`const int volatile x`) gathers them on the requested side. `volatile` moves
with `const` because the style is about where cv-qualifiers go, and leaving
`volatile int` next to `int const` would be neither style.

**One contiguous replacement per move, innermost first.** The move is a single
`ReplaceText` spanning the qualifier run *and* the specifier, not an insert on
one side plus a delete on the other. Two things force this:

- Where the pass meets the return-type pass in `cpp_format`, an insertion
  sitting exactly on the boundary of the range `runToTrailing()` lifts is
  neither carried into the moved text nor removed by the replacement, and
  `const int Get()` came out as `auto const Get() -> int`. A replacement of the
  same byte range composes instead — the two passes agree on that range because
  `skipQualifiersBackward`/`Forward` find the same run this scan did.
- Qualified types nest (`const std::map<std::string const, T const*>` is three
  of them, and the outer one's specifier spans both inner ones), so the visitor
  handles children **before** the node (`TraverseQualifiedTypeLoc` recurses
  first). An outer move applied first would write a specifier text that the
  inner moves then edit in the middle of, giving `std::stconst ring`. Handling
  the innermost first means each outer move reads its specifier back with the
  inner results already in it, via `getRewrittenText()`.

Two smaller sharp edges, both about the closing `>` of a nested template
argument list: the lexer reads `>>` as one token that the parser splits, so the
inner list's end location is an *expansion* location (mapped back with
`getFileLoc()`), and measuring a token there runs one character long and
swallows the enclosing list's bracket. Hence every range in the pass is a
**character** range over offsets computed by hand, and the end offset is
`+1` whenever the last token spells `>`. Requiring that character to be `>` is
also what keeps the `getFileLoc()` path from being a macro hole: a type whose
end really comes from a macro body maps to the macro's own name token.

## Execution model: parallel TUs (`tu_driver`)

Every translation unit is parsed by its own `ClangTool` on a worker thread (`--jobs`, default every CPU) and writes only into its own `TUSlot`. Nothing is shared while a TU runs; what other TUs need is exchanged at barriers by the driver thread, always in **source order** (non-headers in the order given, then headers in the order given), which is what makes the output byte-identical for any `-j`:

- **Two phases.** Non-headers run first, then headers. Between them, the dependent-token resolutions every `.cpp` recorded are merged into `CrossTUState` and each header TU is **seeded** with a copy (its slot's `Vetoes`/`DepRes` start as copies of the shared maps). This is the ordering `orderSourcesForRename()` always imposed — a header is where a dependent token is spelled, the `.cpp` files are where it is resolved — so the common case needs one parse per TU.
- **Rule (a): re-run on a veto.** A veto found in one TU invalidates what another TU already buffered. When a pass discovers any new veto (checked at each phase barrier, so the header phase is skipped on a restart), every slot is discarded, the shared `DepRes` maps are **cleared**, `Vetoes` are kept, and everything runs again (`MaxFullPasses` = 4, as `runWithVetoRerun` did; the cap now warns). `DepRes` must be cleared because the recorder is gated on `!Renames.empty()`: a member vetoed in pass 2 can leave a TU with no renames, whose stale `HasName` entry no TU would overwrite with a veto — the header token would be rewritten while the member keeps its name.
- **Rule (b): re-run a stale TU.** After a pass with no new vetoes, a TU is stale if any shared entry whose key file is its own main file differs (in `HasName`/`NewName`/`Vetoed`) from its slot's post-run map (= seed + own records, exactly what its applier consulted; `dependentResolutionsDifferFor`). Only those slots are re-run, with the shared maps **kept** (complete once no new vetoes appear) — one round converges. This is what makes resolution independent of the source order: a header instantiated only from another header, or a token spelled in `a.cpp` and instantiated only from a `b.cpp` that includes it, were silently order-dependent before.
- **Emit mode seeds nothing** (`crossTUSeeding()` false) and never re-runs (`rerunNeededOnVeto()` false): every TU is independent, aggregation drops the edits and resolutions of vetoed owners, and the JSON is identical for any `-j` and any source order. The shared maps are still merged for the sidecar.
- **Debug mode** (`--debug-trace`) forces serial, unbuffered output. The serial path (`-j1` too) leaves Clang's own stderr printer in place, so its diagnostics are exactly the old ones; parallel batches buffer them per TU (`SlotActionFactory::runInvocation` redirects the verbose stream and builds the printer from the invocation's `DiagnosticOptions`) and replay them in slot order.
- Per-TU outputs (`Pending`, `Edits`, `Conflicts`, `Report`) are merged only once, at the end, by the client's `finish()`, so a re-run TU simply replaces its slot. `LintReport` sorts on output; `reportRenameConflicts` dedups; every other collection is a `std::map` or appended in slot order.
- **Key spaces:** `Vetoes` keys are cwd-relative (`scan()`, `vetoed()`), `DependentResolutions` keys are absolute real paths (`locKey`). The staleness check compares against `sys::fs::real_path(Source)`, never a relativized path.
- Workers are `llvm::thread`s with explicit 8 MiB stacks — a pool's default stack is 1 MiB on Windows and Clang's parser/Sema/`RecursiveASTVisitor` recurse deeply. Each `ClangTool` gets `llvm::vfs::createPhysicalFileSystem()`, so the per-compile-command chdir inside `ClangTool::run` is per instance and the process cwd never changes (`relativizeToCwd()` caches it). Compile commands containing `-mllvm` would parse `llvm::cl` options concurrently — unsupported with `-j > 1`.

## `normalize_variables`: cross-file renaming

The tool processes one translation unit at a time (many at once, but each in isolation). To rename declarations in a header alongside their uses in a `.cpp`, the source list must include both files — but the order does not matter: the driver runs every non-header before any header, so every `.cpp` TU is parsed against the original on-disk header content.

Edits are buffered per TU (`TUSlot::Pending`), merged into `RenameActionFactory::Pending` (a path → content map) by `finish()`, and only committed by `flush()` after `runTranslationUnits()` returns. This is what makes multi-file in-place renaming correct: every TU compiles against the original on-disk source regardless of how many headers are in the list.

`FileSet` (in `rename_variables_lib.h`) holds the real absolute paths of all source files. `CollectRenamesVisitor` collects declarations from any file in the set (not just the main file), enabling the header's declarations to be found when compiling the `.cpp`.

## `normalize_variables`: template-dependent tokens

Two spellings need this, and they share one mechanism:

- **A member access** — `x.val` where `x` is dependent (`CXXDependentScopeMemberExpr`).
- **A qualified name** — `Helper<T>::val` (`DependentScopeDeclRefExpr`), which is a *name* whose lookup is deferred, not an access on an object. Its resolved form in an instantiation is an ordinary `DeclRefExpr`, so the recorder matches on that. googletest's `&(TypeIdHelper<T>::dummy_)` is the motivating case.

A member accessed through a template parameter — e.g. `x.val` in `auto set_val(auto& x) { x.val = 12; }` (or the explicit `template <class T> void set_val(T& x)`) — is a **dependent** expression (`CXXDependentScopeMemberExpr`): which member `val` names is unknown until the template is instantiated, and the instantiations usually live in the `.cpp` files, not in the header that spells the token. The per-main-file `Rewriter` model can't rename it on its own — the header's own TU never instantiates the template, and an instantiating `.cpp`'s TU doesn't rewrite the header.

The tool bridges this with a **cross-TU resolution map** (`DependentResolutions` in `rename_state.h`), keyed by the token's `(real path, byte offset)`. Each TU works on its own copy (seeded from the shared `CrossTUState::DepResPerRule` — one map **per rule**, so a token resolved by one rule is never re-applied by another) and the driver merges every TU's copy back at the barriers with `mergeDependentResolutions()`. Per TU, `runRenameRuleOnAST` does three things:

1. `DependentTokenCollector` (cheap, no instantiations) finds dependent token locations in owned files — both spellings above. If there are none, the rest is skipped — the feature is zero-cost for non-template code. A token written as a macro *argument* counts: it is spelled at the call site, and `ownedKey()` keys on the spelling, so the collector, the recorder and the applier all agree on that location. This is what makes googletest's `EXPECT_FALSE(this->table_)` inside a `TYPED_TEST` renameable — the single largest class of missed references that corpus had.
2. `RecordDependentResolutionsVisitor` (`shouldVisitTemplateInstantiations() == true`) walks this TU's instantiations and, for each resolved member access (`MemberExpr`) or qualified reference (`DeclRefExpr`) landing on a known dependent-token location, records the new name it resolves to. A binding to a member that is **not** being renamed (e.g. a type outside the `FileSet`) or a disagreement between instantiations **vetoes** the location.
3. `ApplyRenamesVisitor::VisitCXXDependentScopeMemberExpr` / `VisitDependentScopeDeclRefExpr` rewrite the token in its **own** main-file TU using the agreed name (skipping vetoed/unresolved entries).

Because the driver runs headers after every other source, every instantiating `.cpp` TU runs before the header TU that consumes its resolutions; a TU whose tokens were resolved by a TU of the *same* phase is re-run (rule (b) above), so the order within a phase does not matter either. `ApplyRenamesVisitor` itself keeps `shouldVisitTemplateInstantiations() == false`, so all non-dependent paths are unchanged — the feature is purely additive. **Soundness is bounded by the passed source set:** an instantiation in a TU that is *not* passed to the tool is invisible, so its member may be renamed while its dependent use is missed; conversely a visible out-of-scope binding is vetoed conservatively (leaving the token), which can produce an incomplete rename. Both cases surface as a compile error on the next build — the intended review gate — rather than a silent miscompile. Full all-or-nothing propagation (skipping a member's declaration rename when a dependent use can't be safely rewritten) is a possible future hardening for *those* two cases; the third — a dependent token with no rewritable spelling at all — is already handled that way by the macro veto above.

## `normalize_variables`: names spelled through macros

Whether a reference can be renamed depends on what the preprocessor did to the token, which `ApplyRenamesVisitor::rewriteLoc()` decides with one test: `SM.getFileLoc(Loc) == SM.getSpellingLoc(Loc)`. `getFileLoc()` walks macro-*argument* levels down to the spelling and macro-*body* levels up to the expansion, so the two agree exactly when the token was typed by the user.

| Reference | `rewriteLoc` | Outcome |
|---|---|---|
| `c.itemCount` (ordinary) | the token | renamed |
| `FWD(c.itemCount)` (macro **argument**) | the call site | renamed — the token is written once, in ordinary source |
| `OUTER(c.itemCount)` (argument forwarded through macros) | the call site | renamed — the walk is recursive |
| `TWICE(c.itemCount)` (argument expanded twice) | one call site, two AST nodes | renamed **once** (`renameAt` dedups by `(FileID, offset)`) |
| `struct S { FIELD(itemCount) };` (declaration as an argument) | the call site | renamed (`shouldCollect` resolves through the expansion) |
| `EXPECT_FALSE(this->itemCount)` in a class template (**dependent**, in an argument) | the call site | renamed via the dependent-token path, which keys on the spelling |
| `#define BUMP(c) ((c).itemCount += 1)` (macro **body**) | invalid | **vetoes the rename** |
| `c.PASTE(item)` where `#define PASTE(p) p##Count` | invalid (`<scratch space>`) | **vetoes the rename** |

**Renaming is all-or-nothing.** A macro body spells its token once, at a location every expansion shares — expansions need not agree on what it names, and rewriting it would change all of them. A `##`-pasted token exists only in Clang's scratch buffer, so no byte range of any file holds the name. Neither can be rewritten, and renaming the declaration without them does not compile. So one unrewritable reference vetoes the declaration, which keeps its old name everywhere and is reported through the existing `RenameConflict` channel (`--report-rename-conflicts` for the sites). Turning a build break into a reported skip is the point: the tool declines the rename instead of half-applying it. The scan deliberately ignores file ownership — a macro in a third-party header that names one of our members is just as fatal and just as unrewritable.

The scan pass is the one place `ApplyRenamesVisitor` sets `shouldVisitTemplateInstantiations() == true`. It has to: a **dependent** token spelled in a macro body has no `MemberExpr` in the pattern at all — which member it names is known only in an instantiation — so nothing else would ever discover that the member has an unrewritable reference, and it would be renamed at its declaration with the macro left behind. The rewrite pass keeps `shouldVisitTemplateInstantiations() == false` as before, since instantiation locations point back into the pattern, which is rewritten once through the pattern's own nodes.

`RenameVetoes` is keyed by the declaration's `(real path, byte offset)`, stable across TUs *and* processes. For a virtual function the key is the **base-most** declaration of the override hierarchy (`renameOwnerKey()`), so a veto found on any override suppresses the whole family — `collectOverrideFamily()` walks upwards only, so every override agrees on that key without needing to see its siblings.

Two mechanisms make the veto order-independent, one per driver path:

- **Direct runs** (`normalize_variables`, `cpp_format` without `--emit-edits`): a veto discovered while processing one TU can invalidate a rename another TU already buffered, and a `Rewriter`'s edits cannot be taken back. Nothing reaches disk before `flush()`, so the driver discards the whole pass and runs again with the vetoes known (rule (a) in "Execution model"). It costs nothing when no macro names anything being renamed, which is the common case, and nothing in Emit mode either (`rerunNeededOnVeto()` is false there — records carry their own vetoes, so aggregation does the dropping and the Bazel path never pays for a second parse). Per TU, `runRenameRuleOnAST` runs `ApplyRenamesVisitor` twice — `ApplyMode::Scan` collects vetoes and rewrites nothing, then `ApplyMode::Rewrite` applies what survived — so within one TU no edit is ever buffered before every reference in it has been seen.
- **The Bazel path**: each target's action is its own process, and the library's action never sees the macro its dependent expands. So every rename `EditRecord` (and every `ResolutionRecord`) names its owning declaration in `owner_file`/`owner_offset`, each report carries a `vetoes` list, and `mergeEditReports` drops every edit whose owner *any* report vetoed. That is what stops the library's action renaming a member while the binary's action is left spelling the old name.

**The bound is the same one the dependent-token feature has:** a veto can only come from a TU that is actually parsed. A macro expanded only in a repo you do not format, in a `no-cpp-format` target, or under an `#if` branch this build does not take is invisible, and its reference is missed. As before, that surfaces as a compile error on the next build rather than a silent miscompile.
