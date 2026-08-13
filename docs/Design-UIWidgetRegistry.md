# P9.2 — WidgetVTable decomposition (D3): implementation spec

*Produced by the `ui-decomp-scope` scoping pass (2026-08-12): four parallel surface
maps (emit / layout / interact / predicates) synthesised into this plan. This is the
executable spec — another engineer (or a later session) implements from it.*

> **STATUS (2026-08-12): EMIT DECOMPOSITION COMPLETE + PROVEN.** `WidgetVTable`/`WidgetEmitCtx`
> + the dual-path dispatch (`g_uiUseVTable`, defaults FALSE) + the `--test-uivtable` gate are in
> (all in `UISystem.cpp`, internal — the registry is welded to the `Emitter`, so it did NOT become
> the separate `WidgetRegistry.{h,cpp}` the spec names; only `SetUIUseVTable`/`WidgetVTableSelfTest`
> cross `UISystem.h`). **ALL 10 emit arms extracted** (Label, Image, Panel, Button, ProgressBar,
> Slider, Toggle, Selector, ScrollView, TextInput) → gate **PASS, 1752 verts byte-identical** over a
> state cross-product corpus, on a real D3D12 session. Regression battery + runtime link green (legacy
> default untouched). Gate note: it loops legacy builds to a fixed point before capturing — the
> ScrollView's auto-measured `contentExtent` settles over several builds, not one.
>
> **INTERACT (onPointer) DONE + PROVEN (2026-08-12):** all 6 interactive types' pointer switch arms
> (Button/Toggle/Selector/Slider/TextInput/ScrollView) extracted verbatim into `onPointer` vtable slots;
> `ApplyPointerToElement`'s preamble (enabled/clip/hover/held) stays, its switch is the gated fallback.
> The struct defs + `Widget()` decl were hoisted above `ApplyPointerToElement` (defs/table stay below).
> New **`WidgetPointerParitySelfTest`** drives every interactive element both ways over 5 input probes
> and asserts identical flag mutations (hovered/held/clicked/changed/value/toggled/selected/dragging/
> scrollPos) + return; folded into `--test-uivtable` → **PASS, 40 probes**. Battery + runtime green.
>
> **✅ P9.2 COMPLETE (2026-08-12).** onNav (Slider/Selector) + onActivate (Button/Toggle/Selector/
> TextInput) extracted into vtable slots; UIFocus now calls the public dual-path `WidgetNav`/
> `WidgetActivate` (legacy switches moved there as the gated fallback; `beginEdit` stays in UIFocus as
> it owns the UIContext edit state). `IsInteractive` (UISystem) + `IsFocusable` (UIFocus, via public
> `WidgetIsFocusable(const UIElement&)`) now read the vtable flags — ONE source of truth. `--test-uivtable`
> proves **emit (1752 verts) + pointer (40 probes) + nav/activate (21 probes)** all byte/flag-identical.
> **The default is FLIPPED — `g_uiUseVTable = true`, the vtable is the shipped path.** Full battery +
> both-backend runtime link + D3D12 GPU smoke (live UI at 120fps) green. The legacy switches remain as
> the gate's comparison baseline (a later cleanup can retire them behind a checked-in golden).

## 0. Guiding constraint (verified)

`UIElement::Type` (Components.h:943–953) stays byte-for-byte on disk. The vtable is a
**runtime dispatch table indexed by that enum**, populated at static-init; nothing in
it is serialized. The refactor is a *pure code move*: every switch arm's body relocates
verbatim into a per-type function that receives the **exact same inputs** the arm reads
today, so output is provably identical (the `--test-uivtable` gate proves it).

Enum-adjacency: add a trailing `Count` sentinel after `TextInput` (changes no serialized
value 0–9) → `static_assert(std::size(kWidgets) == (size_t)Type::Count)`. If even that
edit is unwanted, use designated initializers + `static_assert` against literal `10`.

## 1. WidgetVTable / WidgetTraits

New files: `Source/UI/WidgetRegistry.{h,cpp}`. Runtime table (render + input) is separate
from an editor table (authoring).

**Context bundles** carry exactly the pre-switch state the arms read outside `el`/`Emitter`,
so arm bodies move unchanged:
- `WidgetEmitCtx { Renderer&; assetsDir; const LayoutItem&; Rect rect; vec2 center;
  FontAtlas& font; const string& disp; UIContext* ctx; bool disabled,pressedState,
  focusedState,selectedState,sliced; }` — from the preamble at UISystem.cpp:1252–1305.
- `WidgetPointerCtx { vec2 pointer; bool pressed,down; f32 wheel; bool over,setHover;
  const LayoutItem& item; }` — everything `ApplyPointerToElement` (829–917) reads besides `el`.

**Promote today's emit lambdas to free helpers** (they close over `WidgetEmitCtx`):
`PartTex` (1262–1264), `StateFill` (1276–1291), `QuadOrSlice` (1296–1305).
`ResolveTexture`/`UITexInfo`/`LoadUITexture` already free — unchanged.

```cpp
struct WidgetVTable {
    bool isInteractive=false;   // was IsInteractive (UISystem.cpp:819)
    bool isFocusable  =false;   // was IsFocusable   (UIFocus.cpp:17) — ScrollView=false
    bool wheelTarget  =false;   // ScrollView only   (UISystem.cpp:1027)
    void (*emit)(const UIElement&, const WidgetEmitCtx&, const Emitter&)=nullptr; // every type
    bool (*onPointer)(UIElement&, const WidgetPointerCtx&)=nullptr;               // interactive only
    void (*onNav)(UIElement&, int dir)=nullptr;                                   // dir 2=L 3=R
    void (*onActivate)(UIElement&, UIContext&)=nullptr;                           // Enter/A
    void (*childRegion)(UIElement&, const Rect& own, Rect& child, Rect& clip, bool& hasClip)=nullptr; // ScrollView 595–616
    void (*measureChildren)(UIElement&, const Rect& origin, const Rect& bbox)=nullptr;                // ScrollView 701–716
    const char* displayName=""; glm::vec2 defaultSize{100,40};
};
const WidgetVTable& Widget(UIElement::Type t); // kWidgets[(int)t], bounds-asserted
```

**Editor table** (separate, `Source/Editor/UIWidgetEditor.{h,cpp}`; migrate editor
consumers in a follow-up sub-phase — not required for the runtime vtable to land):
`WidgetEditorTraits { applyDefaults (UIEditor.cpp:515–597); treeGlyph (2835–2847);
drawInspector (Editor.cpp:9990–10385 fragments); getValue/setValue (settings contract) }`.

**Shared sub-rect producers** (dedup the emit-vs-pick geometry; consumed by BOTH):
`ComputeSliderParts` (emit 1374–1399 == pick 880–884), `SelectorCell` (1415–1427 ==
867–871), `ProgressFillRect` (1364–1366), `ComputeScrollBar` (1512–1519 == 898–904).

## 2. Switch arm → slot (verbatim moves)

Everything **above** the emit switch (UISystem.cpp:1184–1305: visible/hidden gate, world
routing, P4 styled copy, transform, clip, tint, Emitter ctor, `disp`, `font`, lambdas)
**stays in the loop** and fills `WidgetEmitCtx` + the `Emitter`. Only switch bodies move.

| Type | emit | onPointer | onNav/onActivate | flags |
|---|---|---|---|---|
| Panel(0) | 1308–1316 | — | — | — |
| Label(1) | 1317–1319 | — | — | — |
| Button(2) | 1320–1343 | 852–855 | onActivate 469–471 | interactive, focusable |
| Image(3) | 1344–1350 | — | — | — |
| ProgressBar(4) | 1351–1373 | — | — | — |
| Slider(5) | 1374–1400 | 877–887 | onNav 434–442 | interactive, focusable |
| Toggle(6) | 1401–1412 | 856–863 | onActivate 472–476 | interactive, focusable |
| Selector(7) | 1413–1430 | 864–876 | onNav 443–453; onActivate 477–484 | interactive, focusable |
| ScrollView(8) | 1505–1538 | 894–912 | — | interactive, **wheelTarget**, not focusable |
| TextInput(9) | 1431–1504 | 888–893 | onActivate 485–487 (→beginEdit) | interactive, focusable |

**Predicates collapse to flags:** `IsInteractive`→`Widget(t).isInteractive` (call site 962);
`IsFocusable`→`Widget(t).isFocusable` (UIFocus 42, 248); wheel filter (1027)→`wheelTarget`;
inspector predicate (Editor.cpp:10275)→`isInteractive`.

**Stays OUT of the vtable (by decision):** winner arbitration `ApplyPointerPass` (939–1051);
per-frame flag clear (1093–1130); focus ring (1544–1548, uniform post-decorator); tooltip
pass (1565–1613, frame-level decorator); TextInput edit *session* (UIFocus 280–364 — a mode
keyed on `ctx.editing==e`, not a type dispatch; `onActivate` calls `beginEdit`, the rest stays).

**Layout — ScrollView only.** `childRegion`←595–616, `measureChildren`←701–716. The
`LayoutUIImpl` walk calls the slot if non-null else default. UILayoutGroup flow (625–699)
and escape-class recursion (639–645) stay in the walk (they key on the *component*, not the
type). **Phasing:** ship D3 with the layout slots defined but ScrollView still special-cased
inline; flip ScrollView to the slots as the FINAL extraction step.

## 3. Frozen-vertex test — `--test-uivtable`

Mirrors `DocumentCanvasSelfTest`/`--test-uicanvas` (UIDocument.cpp:2409): build twice for
`contentExtent` convergence, then `std::memcmp` two `rhi::UIVertex` buffers (56-byte POD,
`static_assert` at RHI.h:651). New `Source/UI/WidgetVTableTest.cpp`, entry
`WidgetVTableSelfTest(Scene&, Renderer&)`, wired in `main_editor.cpp`. **CPU-side, no GPU.**

1. **Fuzz corpus:** one UIElement of every Type (0–9) under a canvas × a state cross-product
   per interactive type (enabled, hovered/clicked/held, toggled, focused-via-ctx, value/fill
   ∈{0,.37,1}, selected, scrollPos {top,mid,overflow}, slice on/off, styleTheme set/empty (P4),
   effect∈{0,1} (P7), rotation/scale identity+non, runtimeText set/empty, world-vs-screen host).
   Deterministic seed, ~1–2k elements. MUST include a nested clipped ScrollView and a ≥3-option
   Selector (locks `textSlot` ordering, hazard §5.1).
2. **Golden = same binary, before flip:** keep BOTH paths compiled (a TU-local `g_useVTable`,
   or `BuildVerticesLegacy`/`BuildVerticesVTable` side by side). Build twice legacy → `golden`;
   twice vtable → `candidate`; `expect(size==)` then `expect(memcmp==0)`. Keeping both behind a
   flag until green is what makes it provable render-blind.
3. **Interact parity:** drive a grid of `WidgetPointerCtx` through legacy `ApplyPointerToElement`
   vs `onPointer`, assert output flags identical (hovered,held,clicked,changed,value,toggled,
   selected,dragging,scrollPos); same for onNav/onActivate. Catches drift memcmp can't see.
4. **World-canvas:** assert parity on `WorldUIBatch::verts` too (both sides are the runtime path,
   so world verts ARE comparable here, unlike `--test-uicanvas`).
5. **Retirement:** once green and legacy deleted, keep a checked-in binary golden as the permanent
   guard — any emit-arm edit that changes a byte fails loudly.

## 4. Risk-ordered extraction (test stays green after each)

1. **Label** — pure Text, no state; proves the plumbing.
2. **Image** — one QuadOrSlice + failed-load skip.
3. **Panel** — QuadOrSlice + optional centered text (first disp+font).
4. **ProgressBar** — first shared sub-rect (`ProgressFillRect`) + radial branch; no interaction.
5. **Button** — first interactive: stateFill, sprite swap, auto-contrast, onPointer, onActivate.
6. **Toggle** — write-back flip across pointer+activate.
7. **Selector** — shared sub-rect used by emit AND interact (`SelectorCell`) + multi-Text/textSlot + onNav.
8. **Slider** — `ComputeSliderParts` shared, latched `dragging` across frames, onNav nudge.
9. **ScrollView** — interactive+wheel+only layout type; flip `childRegion`/`measureChildren` LAST.
10. **TextInput** — riskiest: reads/writes `ctx` (editing/caret/preEditText), its own tightened-clip
    Emitter, owns the multi-frame edit session; extra begin/commit/cancel/click-hop parity cases.

Rationale: state ownership + cross-pass sharing rise monotonically; the two types touching other
subsystems (ScrollView→layout walk, TextInput→UIContext session) land last.

## 5. Hazards to preserve EXACTLY

1. **Glyph cache / `textSlot` ordering** (UISystem.h:169–174; Emitter L42–44,143). Key mixes
   `entityKey` (from `item.entity`, set at 1250) + a per-element counter incremented in `Text()`
   call order. Keep `entityKey` in the preamble; call `Text()` in the SAME order per arm —
   critically Selector's per-option loop (1415–1427). Corpus MUST include a ≥3-option Selector.
2. **P4 styled-copy `std::optional<UIElement>`** (1203–1214). `emit` gets the styled COPY by
   const-ref (can't mutate — enforced); `onPointer` gets the REAL component. Never apply style in
   interact. Corpus MUST include styleTheme-set elements.
3. **World-canvas routing** (1188–1202). `dest` (screen `out` vs `worldOut[batch].verts`) chosen
   in the preamble, baked into `Emitter::out`; vtable never sees routing. Preserve `allToOut`
   bypass + `continue` on missing RT. Corpus MUST include a world canvas.
4. **P7 `effect`/`fx`** (RHI.h:649; Emitter L54–56,75; ctor arg `el.effect` at 1251). Stamped by
   `Emitter::Vertex`; reused Emitter propagates it — no arm touches it. Corpus MUST vary effect∈{0,1}.
5. **P9 tooltip pass** (1565–1613) + focus ring (1544–1548): once-per-frame document decorators,
   stay OUTSIDE the loop and the vtable. Tooltip candidate recording stays in `ApplyPointerPass`.

Also: ScrollView `contentExtent` side-effect (the editor snapshot/restore at 1663–1690 must keep
wrapping the layout slots — the only slots allowed to write component fields). Keep
`--test-uicanvas`/`--test-uisolve` green alongside the new `--test-uivtable`.

## Files touched

- **New:** `Source/UI/WidgetRegistry.{h,cpp}`, `WidgetVTableTest.cpp`; `Source/Editor/UIWidgetEditor.{h,cpp}`.
- **Edit:** `UISystem.cpp` (loop keeps preamble/ring/tooltip; switch bodies→registry; IsInteractive→flag),
  `UIFocus.cpp` (IsFocusable→flag; nav/activate switches→onNav/onActivate), `Components.h` (trailing
  `Type::Count`), `main_editor.cpp` (`--test-uivtable`).
- **Follow-up sub-phase:** editor consumers (UIEditor.cpp:397/515/2835, Editor.cpp:9984–10385) → `WidgetEditorTraits`.

**Behavior-preserving by construction** (verbatim moves + identical inputs) and **proven** by
`--test-uivtable` (byte-for-byte UIVertex memcmp + field-by-field interact parity), all render-blind.
