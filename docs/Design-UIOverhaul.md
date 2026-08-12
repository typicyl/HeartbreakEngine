# Design — UI System Overhaul

> Status: **assessment + plan** (2026‑08‑11). Grounded in a 10‑subsystem source audit
> (file:line cited throughout). This document is the reference for the phased program.
> Read the "Problems" and "Risks" call‑outs before touching code — several parts of the
> current system are load‑bearing in non‑obvious ways.

## 0. Executive summary — the reframe

The prompt asks to turn "a collection of ImGui widgets" into a real retained‑mode
game‑UI framework. **That premise is already false in the codebase's favour.** The
in‑game UI is *already* a retained‑mode, ECS‑backed framework rendered by the shipped
pipeline; ImGui is only editor chrome and is compiled out of the runtime
(`hbe` links zero ImGui — `CMakeLists.txt:430`; ImGui/ImGuizmo gated `is_editor` at
`:397`). A large fraction of the target feature list already exists and is genuinely
well‑built:

- Retained‑mode scene graph = EnTT entities + `Parent`/`HierarchyOrder`, rooted by `UICanvas`.
- `.hbui` = a versioned, VFS/pack‑aware **document/template** that is *type‑level*
  separated from `.hbscene` (a document literally cannot hold world content).
- Unity `RectTransform` layout (anchors/pivot/stretch), layout groups (V/H/Grid), canvas
  groups, ScrollView clipping, nine‑slice, world‑space (diegetic) UI, gamepad/keyboard
  focus nav, `TextInput`, per‑state skinning, `.hbuianim` keyframe clips.
- A **WYSIWYG visual editor whose canvas IS the shipped renderer**, proven byte‑identical
  by `--test-uicanvas`.

So the correct strategy is **upgrade‑in‑place, not rewrite.** A rewrite would orphan the
`.hbui` assets, the migrators, the editor, the schematic integration, the world‑space
path, and a large body of hard‑won correctness (documented, tested, and defended in the
source). The overhaul is therefore about **filling real gaps** and **relieving a few
systemic structural weaknesses** — not replacing the foundation.

The whole system can be summarised in one line: **it is retained‑mode in *storage* but
immediate‑mode in *spirit*** — every frame it rebuilds the full layout + vertex stream and
polls per‑frame flags, with no events, no per‑element dirty flags, and no data/view split.
That is fine at "tens of elements" (menus) and is the root of most of the gaps versus a
"hundreds/thousands of elements, virtualized, data‑bound" target.

---

## 1. Current architecture

### 1.1 Module map (`Source/UI/`, runtime lib `HBE_UI_SOURCES`, no ImGui — `CMakeLists.txt:145`)

| File | Role |
|---|---|
| `UIDocument.{h,cpp}` (118 KB) | `.hbui` asset: `DocData`/`DocEntity` DTO, `DocumentSet` (open/instance/close), load/save via VFS, **all migrators** (scene→doc, per‑screen split, project repoint), self‑tests |
| `UISystem.{h,cpp}` (96 KB) | Layout solver (`ComputeElementRect`/`SolveElementFromRect`), the layout walk (`LayoutUIImpl`), vertex emission (`Emitter`, `BuildVerticesImpl`), interaction (`ApplyPointerPass`), `UIContext` caches, `PreloadUIAssets` |
| `UIFocus.{h,cpp}` | Keyboard/gamepad directional nav, focus ring, single‑line `TextInput` edit session (UTF‑8‑aware buffer) |
| `UIWorld.{h,cpp}` | World‑space (diegetic) UI: canvas→RTT→lit quad, ray picking w/ occlusion, `WorldText` 3D text |
| `UIAnimation.{h,cpp}` | `.hbuianim` keyframe clips (10 scalar channels, per‑key easing, triggers) |
| `UIManager.{h,cpp}` | Named‑panel stack (`Show/Push/Pop`), residency, bind to documents |
| `FontAtlas.{h,cpp}` | **stb_truetype ASCII‑only bitmap atlas** (single choke‑point for all text) |
| `Subtitles.{h,cpp}` | Priority subtitle/caption stack (render‑agnostic) |
| `Editor/UIEditor.cpp` (3.3 KLOC, editor‑only) | The dedicated visual `.hbui` editor |

UI ECS components live in `Scene/Components.h`: `UICanvas`, **`UIElement`**, `UIPanel`,
`UIAnimator`, `UILayoutGroup`, `UICanvasGroup`, `UISurface` (runtime), `UIDocMember`
(runtime), `WorldText`. Easing toolkit is `Core/Easing.h` (Core‑layer, reused engine‑wide).

### 1.2 Data model

The **live** model is the ECS (entities + the six document components); `DocData`/`DocEntity`
is a flat serialization/undo DTO. `InstantiateDocument` inflates DTO→entities;
`CaptureDocument` reads entities→DTO on save (`UIDocument.cpp:503‑658`). A `.hbui` is a
**guid‑free template** addressed by `UIPanel::name` and `UIElement::action` strings.

`UIElement` (`Components.h:942‑1073`) is a **~64‑field monolithic god‑struct**: a 10‑value
`Type` enum (Panel/Label/Button/Image/ProgressBar/Slider/Toggle/Selector/ScrollView/TextInput)
plus every widget's fields side‑by‑side (slider `value`, scroll block, selector `options`,
textinput `placeholder`, 9 skin‑part textures, per‑state colors, 9‑slice) **and** per‑frame
runtime flags (`hovered/clicked/held/changed/dragging/…`). Every Label pays for ScrollView's
fields; authored and runtime state are intermixed in one serialized struct.

### 1.3 Frame pipeline (`Engine.cpp`, well‑sequenced)

```
pick pass → ui::UpdateInteraction (BEFORE scripts, hit‑tests LAST frame's layout)
          → ui::UpdateNavigation → Input::SetTextCapture
          → game flow + schematic Update
          → ui::UpdateAnimations → ui::UpdateWorldSurfaces
          → ui::BuildVertices (AFTER scripts) → Renderer::SetUIOverlay
```

Interaction is deliberately 1‑frame‑lagged (runs on last frame's `ctx.layout`), which is
correct today because animation runs after interaction.

### 1.4 Rendering + RHI seam (clean, layer‑correct)

The whole screen UI emits into **one** `std::vector<rhi::UIVertex>` → **one** draw call
(DX12 `DrawInstanced`, Vulkan `vkCmdDraw`). Bindless `texIndex` per vertex (52‑byte
`UIVertex`, triple‑pinned by `static_assert` across RHI/DX12/Vulkan/HLSL) means solid quads,
images, and glyphs share one PSO with zero state changes. Clip = per‑vertex NDC rect via
pixel‑shader `discard`. World‑space UI and the editor authoring canvas reuse the **same**
emit path into offscreen RTs (`CreateUITarget`/`DrawUIToTexture`). The renderer never sees
ECS or UI types — the seam is a POD vertex array + a few virtuals.

### 1.5 What already satisfies the target (don't rebuild)

Retained ECS graph · `.hbui` template/format + migrators · RectTransform anchors/pivot/stretch ·
layout groups + canvas groups + scroll clipping · nine‑slice · single‑batch bindless render ·
world‑space UI + occlusion picking · topmost‑wins hit‑test · gamepad/keyboard focus nav ·
TextInput (UTF‑8 buffer) · `.hbuianim` clips + reusable easing · per‑state skinning · WYSIWYG
editor with byte‑identity guarantee · residency‑based panel manager · strong headless
self‑test battery (`--test-ui*`).

---

## 2. Problems (ranked, systemic first)

1. **`UIElement` god‑struct** (`Components.h:942‑1073`). 10 widget types in one enum switched
   by parallel `switch(el.type)` at ≥6 sites (layout, emit, interaction, nav, serializer,
   editor palette). Adding a widget is a 6‑site lockstep edit; every element carries every
   widget's memory. This is the "avoid giant UI classes / hard‑coded widget types" anti‑pattern.
2. **No fine‑grained dirty flags.** The *only* invalidation signal is one scene‑global
   counter `Scene::uiStructureVersion_` (`Scene.h:404`) that gates just the children‑map
   rebuild. Layout + vertices are **rebuilt in full every frame**. No
   `LayoutDirty/TransformDirty/StyleDirty/TextDirty/RenderDirty`. Blocks virtualization and
   "hundreds/thousands of elements".
3. **Text is the weakest subsystem — ASCII‑only bitmap.** `FontAtlas` bakes stb_truetype
   glyphs 32‑126 into one 1024² atlas at a single 48 px size; `Layout()` iterates **bytes**
   and drops anything ≥0x80 (`FontAtlas.cpp:145`). No Unicode, shaping, kerning, bidi, font
   fallback, rich‑text spans, ellipsis, or SDF. **Split‑brain hazard:** the TextInput buffer
   is UTF‑8‑correct but its rendered text is dropped — a user can type "é" and see nothing.
4. **No reusable Theme/Style asset.** Visual states are **per‑element duplicated fields**
   (`hoverColor`/part textures/`slice`) serialized into every element. "Change the theme" =
   touch every element in every `.hbui`. No `struct Theme`/`Style` exists; no
   `.hbtheme`/`.hbstyle` in the asset registry. Only hover/pressed/disabled have styled
   colors — **no focused or selected visual state**.
5. **No real event model — and interaction mutates authored, serialized fields.** Input is
   per‑frame flag polling (`hovered/clicked/changed`); no capture/target/bubble, no
   listeners, no `onFocus/onBlur/enter/leave`, no modal barrier. Worse, `ApplyPointerToElement`
   writes `el.toggled/selected/value/scrollPos` **in place** (`UISystem.cpp:764‑810`) — these
   are authored/serialized fields, so a Save‑All after runtime interaction **persists the
   player's state into the asset**. There is no model/view split.
6. **No SVG** anywhere (deps or code).
7. **No localization** — blocked twice: no string table **and** the atlas can't render
   non‑ASCII. No RTL/bidi/reflow.
8. **No data‑binding framework.** Everything keys off the free‑text `UIElement::action`
   string via **O(n) full‑pool scans** (`SchematicSystem` setters, `GetUIValue`,
   `SubstituteUITokens`) and hardcoded engine `if/else` chains (settings bind lives in two
   `Engine.cpp` functions that must be edited in lockstep). Action ids are unscoped and
   un‑indexed — collisions are silent.
9. **No virtualization.** ScrollView solves **all** children every frame; a 10 k‑row list
   pays full O(n) layout and shades‑then‑discards clipped rows.
10. **No animation timeline editor.** `.hbuianim` authoring is 3 hardcoded presets + hand‑JSON.
    Only 10 scalar channels; no bezier/pingpong/speed/events; not reusable outside UI.
11. **Editor gaps.** No multi‑select/marquee/align/distribute (explicitly declined), no
    rotate/scale gizmos, no concrete resolution/DPI/controller preview matrix, no per‑state
    authoring surface, no content‑browser drag‑drop onto the canvas.
12. **Format versioning is vestigial.** `version` is written but **never read**
    (`UIDocument.cpp:328`, no reader); back‑compat is two ad‑hoc legacy‑key shims. A breaking
    `kDocVersion=2` would mis‑parse v1 files today.
13. **GL backend not at parity** — no `CreateUITarget` (world‑space UI silently absent) and
    the GL UI shader ignores the clip rect (ScrollViews render unclipped). DX12/Vulkan are at
    parity.
14. **Silent truncation limits.** Fixed 2 MB/frame overlay budget (~6.7 k quads) truncates
    with no warning, and the fade curtain/dev overlay (appended last) drop **first**. World
    RTs are hard‑capped at **8, never freed** (structural in both backends).

---

## 3. Proposed architecture

Keep the ECS‑as‑model, single‑batch renderer, `.hbui` template, and WYSIWYG editor.
Introduce the missing layers as **new, mostly‑additive modules** and relieve the god‑struct
by **separating concerns**, not by rewriting.

### 3.1 Target module layout (adapted to the repo)

```
Source/UI/
├── Core/        UIElement decomposition helpers, runtime/authored split, dirty flags,
│                stable element ids, UIContext (existing, extended)
├── Layout/      ComputeElementRect (kept) + constraints (min/max/preferred), flex, box model,
│                DPI/scale, responsive breakpoints  [extract from UISystem.cpp]
├── Render/      Emitter (kept), dynamic atlas packer, material/blend slot, mask/stencil,
│                profiling scopes  [extract from UISystem.cpp]
├── Text/        FontAtlas → FreeType raster + HarfBuzz shaping + SheenBidi + fallback chain +
│                rich‑text span model + GlyphRun (replaces the single choke‑point internals)
├── Svg/         LunaSVG parse → rasterize‑at‑resolution → cached bindless texture
├── Style/       Theme/Style asset (.hbtheme), UIStyleRef component, state resolution
├── Anim/        keyframe core (bezier/pingpong/speed/events), shared with entity Animator
├── Widgets/     widget registry (pluggable emit/layout/interact per widget) — replaces the
│                closed Type enum switches
├── Input/       event dispatch (capture/target/bubble), focus graph, modal scope
│                (wraps the kept ApplyPointerPass)
├── Bind/        lightweight data binding + action‑id index
├── Loc/         string‑table asset + key resolution + RTL/reflow
├── World/       UIWorld (kept)
├── Assets/      UIDocument (kept) + AssetFormats registrations
└── Editor/      UIEditor (kept, extended: multi‑select, gizmos, preview matrix, timeline)
```

This is a *conceptual* target; extraction from the two big files is incremental and
test‑guarded, not a big‑bang split.

### 3.2 Key design decisions

- **D1 — Runtime/authored split (do this first in the core work).** Move the ~11 runtime
  fields (`hovered/clicked/held/changed/dragging/prevHovered/runtimeText/contentExtent/
  viewExtent/textureIndexCache/textureResolved`) off `UIElement` into a runtime‑only
  `UIWidgetState` component. Kills the "interaction corrupts the asset" hazard, shrinks the
  serialized struct, and is **safe against the frozen byte‑identity test** because those
  fields are already skipped by `WriteElement`.
- **D2 — Stable element identity (opt‑in).** Add an optional authored `id` string (or a
  document‑local stable index) so binding/animation‑retargeting/localization/accessibility
  can reference an element without abusing the non‑unique `name`/`action`. Positional parent
  indices stay for structure.
- **D3 — Widget registry, not enum.** Introduce a `WidgetVTable { layout, emit, interact,
  serialize, editorInspect }` keyed by a widget type id. The existing 10 types become table
  entries; new widgets register without editing the central switches. The `Type` enum stays
  as the on‑disk discriminator for back‑compat.
- **D4 — Dirty flags over the existing skeleton.** The `UIStructureVersion` + children‑map +
  FNV text cache is the incremental‑work skeleton; extend it to per‑element/per‑subtree dirty
  bits so static menus and virtualized lists do near‑zero work.
- **D5 — Text choke‑point preserved.** Keep `FontAtlas::Layout` + `GlyphQuad` as the single
  entry every text path funnels through (screen UI, WorldText, dev‑menu, subtitles). Replace
  the *internals* (byte loop → UTF‑8 decode → bidi → itemize → shape → dynamic glyph atlas)
  and **widen `GlyphQuad`** with a glyph id + source cluster + per‑glyph color/style. The
  existing FNV text cache becomes the shape cache, keyed the same way.
- **D6 — Style as reference, not copy.** `.hbtheme` holds named styles (colors/fonts/spacing/
  corner/border/shadow + the full state set incl. focused/selected); `UIStyleRef{theme,style}`
  on an element resolves the look with per‑element overrides preserved (the alpha‑0 "unset"
  sentinel maps cleanly to "inherit from style").
- **D7 — Events wrap, don't replace, the hit‑test.** `ApplyPointerPass` (topmost‑wins,
  screen‑beats‑world, drag‑latch, wheel‑bubbles) is preserved wholesale; a dispatch layer on
  top adds phases, modal scope, and a focus graph (auto geometric nav is the fallback under
  authored links).
- **D8 — Real format versioning.** Add a version‑dispatched reader so `kDocVersion` can bump
  with a migration ladder (the existing legacy‑key shims are the first rungs).

---

## 4. Existing code to preserve (the crown jewels — do not regress)

- **WYSIWYG byte‑identity** (`BuildDocumentVertices` reusing runtime `LayoutUIImpl`/
  `BuildVerticesImpl`; `--test-uicanvas` memcmp gate). The single most valuable invariant.
- **`ComputeElementRect`/`SolveElementFromRect`** — a correct, closed‑form, invertible Unity
  RectTransform, property‑tested by `--test-uisolve` (4000‑case fuzz). Build richer layout
  *on* it; never break its algebra.
- **`.hbui` document/scene TYPE separation**, `DocumentComponentKeys` whitelist, guid‑strip,
  snapshot exclusion, and the **one shared `WriteElement`/`ReadElement`** used by both
  `.hbscene` and `.hbui` with a frozen‑copy byte‑identity oracle (`--test-uidoc`).
- **Migration suite** (scene→doc, per‑screen split, project repoint) — pure‑JSON,
  non‑destructive, diagnostic‑rich. Reuse it for future format evolution.
- **Single‑batch bindless render path** + 52‑byte `UIVertex` contract + nine‑slice math +
  the RTT/world‑UI/editor‑canvas seam.
- **`ApplyPointerPass`** topmost‑wins hit resolver and **`PointerOverInteractive`**.
- **`UIWorld` PickWorldPage** (nearest‑front‑face + occlusion + deterministic tie‑break +
  inverse‑transpose normal) — the best‑engineered corner; reuse as the framework's 3D hit‑test.
- **UTF‑8 TextInput buffer helpers** (`CharLen/ByteOfChar/AppendUtf8`) and the ordered
  `TextEditEvent` stream in `Core/Input`.
- **`Core/Easing.h`**, the additive‑offset drift‑free animator semantics, and its
  authoring‑skip gate + `--test-scenesave` time‑independence guard.
- **Residency panel manager** + `PreloadUIAssets`/`UITexCache` no‑pop‑in contract.
- **`AssetFormats` registry** (`.hbui`/`.hbuianim` `runtimeLoaded=true`) + `--test-assetformats`.

---

## 5. New modules required

| Module | Purpose | Risk |
|---|---|---|
| `UI/Text/` (Shaper, GlyphAtlas, RichText) | FreeType raster + HarfBuzz shaping + SheenBidi + fallback + spans + ellipsis | Medium (storage‑model change inside FontAtlas) |
| `UI/Svg/` | LunaSVG parse → cached bindless raster | Low (additive) |
| `UI/Style/` + `.hbtheme` asset + `UIStyleRef` | reusable themes/styles + full state set | Low‑med (additive component; touches AssetFormats + serializer) |
| `UI/Core/UIWidgetState` (runtime component) | runtime/authored split (D1) | Low (fields already unserialized) |
| `UI/Core` dirty flags | per‑element invalidation (D4) | Medium |
| `UI/Widgets/` registry | pluggable widget vtable (D3) | **High** (touches the 6 switch sites + frozen test) |
| `UI/Input/` event + focus graph + modal | dispatch on top of `ApplyPointerPass` (D7) | Medium |
| `UI/Bind/` | data binding + action‑id index | Low‑med |
| `UI/Loc/` + string‑table asset | localization + RTL/reflow | Medium (needs Text first) |
| `UI/Anim` timeline + `Editor` timeline panel | keyframe timeline authoring | Medium |
| Editor extensions | multi‑select/align/distribute, rotate/scale gizmos, preview matrix, per‑state authoring | Medium |
| Advanced widgets | Tab/Modal/Tooltip/ContextMenu/List/Tree/Reorderable/**Virtualized list**/Viewport/RenderTexture widget | Med (after registry) |

## 6. Dependencies (recommended)

All via the existing FetchContent + static‑link + `docs/ThirdParty.md` discipline. **All are
runtime deps** (text/SVG are needed in shipped builds) and all are permissively licensed.

| Dependency | License | Why this one |
|---|---|---|
| **FreeType** | FTL / BSD | The standard TTF/OTF (incl. CFF) rasterizer; hinting + per‑size glyphs. Replaces stb_truetype's single‑size bake. |
| **HarfBuzz** | MIT | The standard text shaper (kerning, ligatures, Arabic/Indic shaping, mark positioning). Pairs with FreeType faces. |
| **SheenBidi** | Apache‑2.0 | Unicode bidi (UBA). **Chosen over FriBidi (LGPL‑2.1)** — LGPL static‑linking into a proprietary engine forces relink‑object distribution; SheenBidi is permissive, self‑contained, and purpose‑built. |
| **LunaSVG** | MIT | Self‑contained SVG parse + raster with real gradient/stroke/transform coverage. **Preferred over NanoSVG (zlib)** for icon quality; NanoSVG stays a fallback if we want a smaller footprint. |

Optional / later: **msdf‑atlas‑gen / msdfgen** (MIT) for crisp large/scaled text (SDF) — only
if the FreeType per‑size dynamic atlas proves insufficient for big titles. Not adopted up front.

Not added: ICU (too large for our needs; SheenBidi + a small case/segmentation shim suffices).

## 7. Migration strategy

Non‑negotiable: **no broken intermediate state, existing `.hbui` keep working.**

- **Format:** all additions are **additive keys** with `.value()` defaults (the reader
  already tolerates this). Before any *breaking* change, land D8 (version‑dispatched reader)
  so `kDocVersion=2+` has a real migration rung. The frozen `--test-uidoc` oracle stays green
  for every additive change.
- **Runtime/authored split (D1):** `UIWidgetState` is emplaced at instantiate; no file change
  (runtime fields were never serialized). Old assets load byte‑identically.
- **Theme/Style (D6):** elements with no `UIStyleRef` render exactly as today (inline fields
  remain the fallback). A theme is opt‑in; an optional migrator can *extract* repeated inline
  skins into a generated `.hbtheme` (mirroring the existing scene→doc migrator pattern),
  non‑destructively.
- **Text:** the widened `GlyphQuad`/`Layout` keep the same call contract; all four callers
  (Emitter, TextInput caret, WorldText, dev‑menu) update together. ASCII content is
  pixel‑stable; non‑ASCII that previously rendered blank now renders (surfacing latent
  content, not breaking it).
- **Widget registry (D3):** the on‑disk `Type` enum is unchanged; the registry maps enum→vtable.
- **Editor:** every new gesture funnels through the existing `SolveElementFromRect` invariant.
- **Every phase ends** with: build (Release, both backends) → run the `--test-ui*` battery →
  `--test-uicanvas` on both reference documents → fix regressions. The suite gets CTest
  integration in the final phase.

## 8. Phased implementation plan

Resequenced from the prompt's literal order for **risk**: additive, high‑leverage work first;
the risky god‑struct decomposition is moved **late**, behind the runtime/authored split, dirty
flags, and the event layer that make its seams clean. Each phase is independently shippable and
test‑gated.

- **P1 — Audit** ✅ (this document).
- **P2 — Real text stack.** FreeType + HarfBuzz + SheenBidi; dynamic paged glyph atlas;
  widen `GlyphQuad` (glyph id + cluster + per‑glyph color); font fallback chain; rich‑text
  span parser; ellipsis/truncation + max‑lines. *Highest leverage, unblocks localization,
  no format break.* New self‑test `--test-uitext` (shaping/bidi/measure/wrap/ellipsis).
- **P3 — SVG.** LunaSVG; `.svg` asset + cached rasterization keyed by path+target size →
  bindless; `UIElement` image source accepts SVG; editor preview. `--test-uisvg`.
- **P4 — Theme/Style asset.** `.hbtheme` + `UIStyleRef` + full state set (add focused/selected);
  AssetFormats registration; shared serializer discipline; optional extract‑migrator. `--test-uistyle`.
- **P5 — Core representation.** D1 runtime/authored split (`UIWidgetState`) · D2 optional stable
  id · D4 per‑element dirty flags · D8 version‑dispatched reader. *The safe half of "core
  refactor"; leaves the widget enum intact.*
- **P6 — Layout richness.** min/max/preferred, margins, flex grow/shrink, aspect fitter,
  content‑sized leaves, richer grid, DPI/UI‑scale factor, responsive breakpoints — all on the
  kept `ComputeElementRect`.
- **P7 — Rendering.** per‑element material/blend slot on `UIVertex`, dynamic atlas packer,
  stencil/shape mask, indexed geometry, **GL clip + world‑UI parity**, profiling scopes,
  isolate the UI GPU timer, raise/soft‑warn the vertex budget.
- **P8 — Events + focus graph.** dispatch (capture/target/bubble, focus/blur/enter/leave),
  modal scope, authored + auto focus graph — wrapping `ApplyPointerPass`.
- **P9 — Widget registry + advanced widgets.** D3 vtable; then Tab/Modal/Tooltip/ContextMenu/
  List/Tree/Reorderable/**Virtualized list**/Viewport/RenderTexture widget. *Riskiest core change;
  done after P5/P8 clean the seams.*
- **P10 — Editor.** multi‑select + align/distribute, rotate/scale gizmos, responsive/DPI/
  controller preview matrix, per‑state authoring, content‑browser drag‑drop, **animation timeline panel**.
- **P11 — Animation.** timeline model (bezier/pingpong/speed/events), generalized targets,
  state‑driven transitions, shared keyframe core with entity `Animator`.
- **P12 — Localization + data binding + accessibility + perf + tests/docs.** string‑table asset +
  RTL/reflow (now possible) · binding + action‑id index · a11y roles/labels/live‑regions +
  reduced‑motion/high‑contrast theme hooks · virtualization + hit‑test spatial index + whole‑batch
  skip · CTest integration + migration ladder + docs.

**Per‑phase gate:** build Release + both backends → `--test-ui*` + `--test-uicanvas` → fix
regressions → (visual checks a human must sign off, since headless can't verify "looks right").
```
