# UI Data-Binding + Virtualized List — implementation spec (P9.4)

> **STATUS (2026-08-12): Phase B2 DONE + verified.** `ui::UIDataModel` (keyed tagged-`Value`
> store + per-key revision + pull providers) + `Source/UI/UIData.{h,cpp}`; `UIElement` `bind*`
> fields (bindText/bindValue/bindVisible/bindTexture, serialized — key count 71→75) →
> `ui::ResolveBindings` pushes into runtime fields (globalRev fast-skip + value-compare write
> guard); Engine owns `uiDataModel_` and calls `RefreshProviders`+`ResolveBindings` in frame
> order (inert until a project registers providers / authors a `bind*` — **zero behavior
> change**); editor inspector fields; headless **`--test-uibind`** (runs on this machine) PASS.
> **B4 primitive `InstantiateSubtree` DONE + verified (2026-08-12):** clones one template
> subtree from an in-memory `DocData` into the live registry under a parent (root re-parents),
> structural half only (headless), fixpoint collection so unordered parent links work. Headless
> **`--test-uisubtree`** PASS. (Chose `UIList` as ATTRIBUTES on the ScrollView UIElement — like
> tooltip/tab/bind* — over a 7th doc-component, to avoid the error-prone new-component lockstep.)
> **Remaining:** B4 rest (list attributes `listTemplate`/`listSource` + `ProvideList` + `UpdateLists`
> spawn/rebind + row lifecycle/save-exclude + editor), then B1 (id-index perf), B3 (migrate
> token/interact/caption/settings onto the model), B5 (virtualization). Phase plan + blockers below.


*Produced by the `ui-databind-scope` scoping pass (2026-08-12): four surface maps
(dynamic-content / scroll-layout / game-API / authoring) synthesised into this plan.
Line refs are the maps'. Executable, risk-ordered.*

## Premises (locked from the maps)
1. **`UIElement::id` + `ui::FindElementById` already exist** and are explicitly reserved
   for "future data-binding". Everything routes through `id`; make it O(1) (index it),
   don't invent a new key.
2. The de-facto model is the `UIElement` component, fed by **five inconsistent poll
   paths** (token pull, spawned choices, interact push, caption push, schematic setters).
   Unify with a **keyed data store the UI reads** + an id-index.
3. A `.hbui` subtree already round-trips + instantiates (`InstantiateDocument`). The item
   template is a **hidden `.hbui` subtree addressed by id**, NOT `.hbprefab` (hard-blocked)
   and NOT a second file. The one missing primitive is subtree-slice instantiate.

Frame order every binding must respect (`UISystem.h:8-13`, `Engine.cpp:2016-2020`):
`UpdateInteraction → schematics → ApplyChangedSettings → [new: UpdateLists → ResolveBindings]
→ SubstituteUITokens → BuildVertices`.

## Part 1 — Data-binding
- **`ui::UIDataModel`** (new `Source/UI/UIData.{h,cpp}`, one instance owned by Engine): a
  keyed tagged-union `Value {None,Number,Text,Bool,Tex}` store with a per-key **revision**
  (bumped only on change) + a **provider** table (`Provide(key, fn)` pull-on-demand,
  `RefreshProviders()` 1/frame). Generalises the hardcoded `SubstituteUITokens` switch
  (`Engine.cpp:3732-3769`) + `SeedSettingsWidgets` (`4052-4066`).
- **id-index** on `UIContext` (`std::unordered_map<std::string,entt::entity> idIndex` +
  `idIndexVersion`), rebuilt only when `UIStructureVersion` changes — the exact invalidation
  used by `BuildChildrenMap` (`UISystem.cpp:816-819`). `FindElementById` becomes O(1).
- **`bind*` fields** on `UIElement` (additive strings, `""` = none): `bindText`,`bindValue`,
  `bindVisible`,`bindTexture` → resolve into the **non-serialized** runtime fields
  (`runtimeText`/`fill`+`value`/`visible`/`texture`), the same precedent as `runtimeText`.
- **`ui::ResolveBindings(scene, model, ctx)`**: one walk; for each element with a `bind*`,
  compare the element's cached per-field rev vs `model.Rev(key)`; write the runtime field on
  mismatch. Subsumes tokens/interact/caption/settings; dialogue choices stay spawn (they're
  a variable-count list = Part 2). Token switch stays for parameterized `{item:<id>}`/`{log}`.

## Part 2 — Virtualized list
- **`UIList`** = a **7th document component** (add to `DocumentComponentKeys()`
  `UIDocument.cpp:349-354`; NOT a new `UIElement::Type`) on the ScrollView entity:
  `{templateId, dataSource, itemExtent(REQUIRED; 0⇒non-virtualized fallback), axis, buffer;
  runtime: poolCount, pool[], firstBound}`.
- **List provider** on the model: `ProvideList(key, ListCount, ListBind)` — `ListBind(index,
  rowModel)` writes the row subtree's runtime fields.
- **Window math** (reuses `scrollPos`/`viewExtent`/`contentExtent`): `contentExtent =
  count*itemExtent` computed **up front** (breaks the measure-from-children feedback loop);
  `first = max(0, floor(scrollPos/itemExtent)-buffer)`, `last = min(count-1,
  ceil((scrollPos+view)/itemExtent)+buffer)`. Rows placed via the existing forced-rect slot
  at `contentOrigin + index*itemExtent`. Only `windowN` LayoutItems reach `out`, so emit +
  hit-test touch only the window with **zero changes** to `BuildVerticesImpl`.
- **Recycle** (`ui::UpdateLists`, before layout): size the pool once (`min(count,
  ceil(view/itemExtent)+2*buffer+1)`) via the new **`InstantiateSubtree`** (reuses
  `InstantiateDocument` pass 1/2 against a sub-range); rebind only when integer `first`
  changes → O(visible), never per-sub-pixel. Spawns go through `DocumentSet::Track`.
- **`InstantiateSubtree`** (new, `UIDocument`): clone a template subtree from the in-memory
  `DocData`, re-parent its root to the ScrollView; structural half stays headless (`renderer`
  may be null) — that's what makes the CPU test possible.

## Part 3 — Headless test gates (mostly CPU, run on this machine)
- **`--test-uibind`** (pure CPU): drive a `UIDataModel`, run `ResolveBindings`, assert
  runtime fields match + **rev-guard** (no-op `Set` ⇒ zero writes, instrument a counter) +
  `idIndex` parity vs the old linear scan + one rebuild per structure-version bump.
- **`--test-uilist`** (pure CPU, structural): pool size, `CaptureDocument` row order ==
  `Track` order, window math table (`scrollPos → [first,last]`, forced rects), `contentExtent
  == count*itemExtent` (measure pass skipped), thumb size, two-instance independent pools,
  `UIStructureVersion` stable across a scroll sweep.
- **`--test-uilist-emit`** (CPU emit, no swapchain): reuse the `--test-uicanvas` harness;
  emit touches exactly `windowN` rows; reshape count == rows-newly-revealed (no thrash).

## Part 4 — Risk-ordered phases (each shippable + testable)
- **B1 — id-index + O(1) `FindElementById`.** Pure perf plumbing, zero behavior change.
  Gate: `--test-uibind` index-vs-scan parity. *Lowest risk; unblocks everything.*
- **B2 — `UIDataModel` + providers + `ResolveBindings` + `bind*` fields.** Migrate scalar
  tokens (`{objective}/{equipped}/{progress}`) through the model (token switch kept as
  fallback). Additive serializer + key-count bump. *Low risk.*
- **B3 — migrate interact/caption/settings onto the model** (keep anchor/fade pushes).
  Retires 3 of the 5 poll paths. *Medium risk, path-by-path revertible.*
- **B4 — non-virtualized bound list.** `UIList` + `InstantiateSubtree` + list providers +
  `UpdateLists` spawning ALL rows (no window). Inventory/quest/save-slots data-driven.
  Extent still measured (no feedback break yet). *Medium risk, correctness-only.*
- **B5 — virtualization (optimization).** ScrollView-branch fork in `walk`, up-front
  `contentExtent`, window math, pool + rebind-on-`first`. Guarded by `itemExtent>0`; `==0`
  falls back to B4. *Highest risk; fully gated; ship after soak.*
- **B6 (optional)** — reframe dialogue choices onto `UIList` (consolidation, not capability).

## Part 5 — Blockers / hazards (codebase-specific)
1. **Entity churn vs the structure-version cache.** Rebind data, NEVER churn entities on
   scroll (each spawn/destroy bumps `UIStructureVersion` → full cache + idIndex rebuild).
   Pool once, rebind in place. `--test-uilist` asserts `UIStructureVersion` stable across scroll.
2. **Per-element glyph cache keyed by entity** (`slotKey=entityKey<<4|textSlot`): rebinding
   reshapes only `windowN` rows on `first`-change (acceptable). Re-check the prune threshold
   `>4*(layout.size()+16)` — virtualization shrinks `layout.size()`; may need `+poolCount`.
3. **Serialization key-count guards are a hard contract.** `bind*` (+4 keys) bumps
   `WriteElement(de).size()` and must mirror into `FrozenElement` + `SceneSerializer`. `UIList`
   as the 7th doc component moves in lockstep with `DocumentComponentKeys()` + the paste guard
   (`Editor.cpp:2218-2239`) + SaveScene refusal, or paste/save silently forks.
4. **Editor Play snapshot/restore.** Pooled rows are runtime-spawned `UIDocMember` entities —
   must be EXCLUDED from the authored save (like dialogue-choice/interact entities) and torn
   down on Play-stop (mirror `ResetDialogueRuntime`). Either don't `Track` pooled rows or skip
   them in `CaptureDocument`; cover in `--test-uilist` round-trip.
5. **Cross-document scoping** (like `ProcessTabs`): a `.hbui` opened twice has two `DocHandle`s;
   `UpdateLists` must filter `view<UIList>` by handle so pools don't cross-contaminate. Provider
   is global (model key); row entities + `firstBound` are per-`UIList`-instance.
6. **The feedback-loop inversion is load-bearing.** `itemExtent==0` ⇒ documented fallback to
   B4 (never guess an extent) — a wrong/zero extent silently breaks thumb + wheel clamp.
7. **Frame order** (Part 0): `UpdateLists → ResolveBindings` run after schematics/settings and
   before tokens/`BuildVertices`; `UpdateLists` before `walk`. Wrong order = one-frame-late rows.

## Files
- New: `Source/UI/UIData.{h,cpp}`; test gates in `main_editor.cpp`.
- `Components.h` (`UIElement` `bind*`; `UIList`), `UISystem.{h,cpp}` (`idIndex`; O(1)
  `FindElementById`; `walk` virtualization fork; `UpdateLists`), `UIDocument.{h,cpp}`
  (`InstantiateSubtree`; `UIList` in `DocumentComponentKeys` + serializer + key-count),
  `SceneSerializer.cpp` (mirror), `Engine.cpp` (own the model; providers; frame order; Play
  teardown), `UIEditor.cpp` (List catalog entry).
