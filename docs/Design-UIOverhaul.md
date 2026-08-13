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
- **P2 — Real text stack.** ✅ **DONE (2026‑08‑11).** FreeType 2.13.3 + HarfBuzz 10.1.0 +
  SheenBidi 2.6 wired (all permissive, static, runtime; HarfBuzz built w/o FT integration to
  avoid the CMake cycle; SheenBidi amalgamation gated on `SB_CONFIG_UNITY` + `extern "C"`).
  New `UI/Text/` module: `GlyphAtlas` (shared, paged, dynamically grown, white‑RGB+coverage‑α
  pages so the one `color*tex` shader is preserved; `UpdateTexture` patches, never freed) +
  `TextShaper` (UTF‑8 → SheenBidi bidi → per‑glyph font‑fallback itemization → HarfBuzz shape
  → FreeType raster → positioned `GlyphQuad`s). `FontAtlas` rewritten as a FreeType‑free
  facade; `GlyphQuad` widened (per‑glyph atlas index + source cluster + per‑glyph colour);
  3 emit sites updated (screen/world/dev‑menu). `--test-uitext` PASSES (ASCII, accented
  Latin‑1, Cyrillic, deterministic shaping, word‑wrap, `\n`, **RTL bidi visual reordering**).
  Regressions green: `--test-uisolve`/`--test-uidoc`/`--test-uiseparation` PASS; 3× clean
  `--uiworldtest` GPU runs (world‑UI target created, new font path, zero D3D12 errors);
  editor + **shipping runtime** both build & link. *Still deferred to later P2 sub‑work:*
  rich‑text span PARSER (the per‑glyph colour plumbing exists, the markup parser does not),
  ellipsis/`max‑lines` truncation, MSDF for crisp large text. *Not verifiable headless:*
  pixel‑perfect look + `--test-uicanvas` (needs a UI project, absent on this machine).
- **P3 — SVG.** ✅ **DONE (2026‑08‑11).** LunaSVG 2.4.1 (+bundled plutovg, MIT) wired
  (INTERFACE `LUNASVG_BUILD_STATIC` so consumers don't hit the dllimport trap). New
  `UI/Svg/SvgCache`: VFS/pack‑aware load → parse once → **rasterize on demand at the
  element's draw size** (supersampled, clamped) → cached bindless texture keyed by
  (path,w,h); never import‑baked. `.svg` routed through `ResolveTexture`/`LoadUITexture`
  (zero call‑site churn) + warmed by `PreloadUIAssets`; registered in `AssetFormats` as a
  runtime‑loaded Leaf (ships in packs, `--test-assetformats` PASS). `--test-uisvg` PASS
  (parse, multi‑res raster, RGBA byte order, malformed‑reject); regressions green; editor +
  **shipping runtime** build & link. *Deferred:* true draw‑rect (device‑pixel) rasterization,
  editor drag‑drop/thumbnail of `.svg` onto the canvas (folded into the P10 editor phase),
  SVG fill‑override/mask. Basic multiplicative tint already works via `el.color`.
- **P4 — Theme/Style asset.** ✅ **DONE (2026‑08‑11).** `.hbtheme` = named reusable styles;
  a `UIElement` references one via `styleTheme`+`styleName` (kept as element FIELDS, not a new
  component, to confine the change to the ONE shared `WriteElement`/`ReadElement`+frozen‑twin
  lockstep — safest first serialization‑touching phase). New `UI/Style/Theme` (parse/cache/
  resolve/overlay): at emit the style fills the element's UNSET skin fields (element‑set wins),
  applied to a local copy so the authored component is untouched. Added the two missing visual
  states **focused/selected** (`focusedColor`/`selectedColor`, wired into `stateFill`). `.hbtheme`
  registered (`JsonScan`, ships themed fonts/textures). **Adversarially reviewed (3‑lens
  workflow) → 6 findings, all fixed:** the frozen key‑count guard (53→57), themed sounds not
  playing (wired into `PlayUISounds`), a `ParseTheme` type_error crash on wrong‑typed JSON
  (hardened + full try/catch), a `selectedColor`/Toggle doc mismatch, and a per‑element alloc
  nit (`std::optional`). `--test-uitheme` + frozen **`--test-uidoc`** + all regressions PASS;
  editor + shipping runtime build & link. *Deferred to P10:* a dedicated Theme editor panel +
  inspector `styleTheme`/`styleName` fields (today a `.hbtheme` is hand‑authored / set in the
  `.hbui`). *Follow‑up:* broaden focused/selected theming beyond the Button state‑fill path.

  `.hbtheme` shape:
  ```json
  { "version": 1, "kind": "hbtheme",
    "styles": {
      "PrimaryButton": {
        "hoverColor": [0.9,0.3,0.35,1], "pressedColor": [0.7,0.2,0.25,1],
        "focusedColor": [0.4,0.6,1,1], "font": "Fonts/Inter.uaf",
        "hoverTexture": "UI/btn_hover.png", "slice": [12,12,12,12]
      }
    } }
  ```
- **P5 — Core representation (SAFE SUBSET).** ✅ **DONE (2026‑08‑11).** SCOPE DECISION (user‑
  approved): after reading the interaction core, the D1 runtime/authored field‑split proved
  pervasive (value/toggled/selected/scrollPos/text read across interaction/focus/animation/emit/
  schematic/settings/editor) AND its hazard is already mitigated (documents excluded from scene
  snapshots; the editor Play/Interact preview snapshots+restores those fields; the shipped runtime
  never saves documents). So **D1 is bundled into P9's decomposition** (where those access sites get
  restructured anyway), and P5 delivered the safe, additive pieces:
  • **D8 — version‑dispatched `.hbui` reader**: `version` was written but never read (a latent trap);
  now read, a newer‑than‑engine file warns + loads best‑effort, and `MigrateDocument(doc, fromVersion)`
  is the dispatch hook every future breaking `kDocVersion` bump appends to.
  • **D2 — stable opt‑in element `id`**: an additive `UIElement.id` (serialized through the same
  Write/Read/Frozen/Fuzz lockstep + count guard 57→58) + `ui::FindElementById`. A stable handle for
  future binding/localization/animation‑targeting/a11y (the `.hbui` entity still carries no guid).
  • **Dirty‑flag foundation**: runtime `layoutDirty`/`styleDirty` on `UIElement` + `ui::MarkElementDirty`/
  `MarkAllDirty` (wired into project‑switch) — advisory today; **P11's incremental layout is the consumer**.
  Frozen **`--test-uidoc`** (58 keys) + all regressions PASS; editor + shipping runtime build & link.
- **P6 — Layout richness (FOCUSED SUBSET).** ✅ **DONE (2026‑08‑11).** Delivered the two highest‑
  value, most‑bounded items well, as default‑no‑op additive steps so `ComputeElementRect` stays
  verbatim (`--test-uisolve`) and existing content is byte‑identical (`--test-uicanvas`):
  • **DPI / UI‑scale factor** — a global `ui::SetUIScale`/`GetUIScale` (clamp [0.25,4]) applied in
  `EffectiveCanvas` (the choke for every canvas path); persisted in `UserSettings.uiScale` (clamped
  on load), applied at boot (runtime‑only so the editor/tests stay at 1.0), and a `setting:uiscale`
  slider bound over the FULL [0.25,4] range (seed/apply exact inverses). The key responsive/accessibility
  win (Steam Deck / density).
  • **min/max/aspect constraints** — additive `UIElement.minSize/maxSize/aspectRatio` (serialized
  through the shared lockstep, count 58→61) clamped by a new `ApplyLayoutConstraints` post‑step:
  resized about the pivot for free elements, **pinned to the slot top‑left for layout‑group children**,
  and re‑applied after a `fitContent` grow so they compose. **Adversarially reviewed (3‑lens workflow)
  → 4 findings, all fixed:** the group‑child pivot bug (found by 2 agents), `fitContent`+maxSize compose,
  and the uiScale slider‑range/load‑clamp inconsistency. `--test-uisolve` (constraints + top‑left pinning)
  + `--test-uidoc` (61) + all regressions PASS; editor + shipping runtime build & link.
  *Deferred* (each needs monolithic‑walk surgery best paired with P9's layout modularization): flex
  grow/shrink, named margins, content‑size‑to‑TEXT for leaves (touches the layout↔text boundary),
  richer grid (auto‑flow/spanning/track‑sizing), responsive breakpoints.
- **P7 — Rendering (FOCUSED SUBSET; GL out of scope per user).** ✅ **DONE (2026‑08‑12).**
  • **CPU profiling scopes** — `UIFrameStats` gained `layoutUs`/`emitUs`/`shapeUs` (chrono‑timed in
  the cached `BuildVertices` + `Emitter::Text`), logged in the perf line. Addresses the audit's
  "no CPU profiling scopes" finding; the observability P11 needs.
  • **Per‑element material/effect slot** (the flagship "custom UI materials") — `UIVertex` gained a
  `u32 fx` (52→56 B), mirrored in the DX12 input layout, Vulkan attrs, and `UI.hlsl` (a shader
  BRANCH, so it's still ONE bindless draw — no blend‑state/pipeline change). Authored `UIElement.effect`
  (serialized, count 61→62); effect 1 = grayscale/desaturate; 0 = pixel‑identical. **Adversarially
  reviewed (3‑lens) → 1 nit** (a stale vertex‑budget comment; the cap correctly derives from
  `sizeof(UIVertex)`) — clean serialization/emitter/profiling. Verified: `--test-uidoc` (62) + all
  regressions PASS; **both backends smoke‑clean** (`--uiworldtest --d3d12`/`--vulkan`: device init,
  world‑UI page + text through the new 56‑B format, zero validation/device‑removed). Editor + runtime
  build & link. *Visual look of the grayscale effect is unverified headless — your eye.*
  *Deferred* (render‑blind RHI/both‑backend work, best paired with a visual pass / P11 perf): dynamic
  atlas packer, stencil/shape masking beyond the axis‑aligned clip, indexed geometry, UI‑GPU‑timer split.
- **P8 — Focus graph + modal scope (FOCUSED SUBSET).** ✅ **DONE (2026‑08‑12).** Delivered the
  bounded, additive (no‑op by default) half of "events + focus graph":
  • **Authored focus graph** — `UIElement.navUp/navDown/navLeft/navRight` (target element `id`,
  using P5's stable ids; serialized, count 62→66) override the geometric directional pick in
  `PickInDirection`; unresolvable → geometric fallback. `UIPanel.firstFocus` (+ new `PickInitial`)
  sets the initial focus target, **scoped to its own panel's subtree** and chosen from the topmost
  active panel (so a background HUD can't steal it).
  • **Modal scope** — `UIPanel.modal`; `ui::ActiveModalPanel(scene, layout)` picks the topmost
  **shown** modal (requires a laid‑out descendant, so an active‑but‑unshown modal never locks the
  UI) and `ui::IsDescendantOf` traps both pointer hit‑testing (`ApplyPointerPass`) and focus
  (`Focusable`/`UpdateNavigation`, which also drops focus a newly‑opened modal now blocks) to its
  subtree. `UIPanel` serialized fields (count 2→4).
  **Adversarially reviewed (3‑lens) → 6 findings, all fixed:** a **major modal lockout** (trapping to
  an unshown modal — fixed by the layout‑aware `ActiveModalPanel`), stacked‑modal + first‑focus
  pool‑order/scoping bugs, a doc gap, and panel‑fuzz coverage. `--test-uidoc` (66/panel‑4) +
  **`--test-uipick`** (interaction no‑regression) + all regressions PASS; editor + runtime build & link.
  *Deferred to P9's decomposition:* the full capture/target/bubble event‑dispatch graph +
  focus/blur/enter/leave listener events + eager focus‑on‑Show (needs UIManager↔UIContext wiring).
- **P9 — Widget registry + advanced widgets.** D3 vtable; then Tab/Modal/Tooltip/ContextMenu/
  List/Tree/Reorderable/**Virtualized list**/Viewport/RenderTexture widget. *Riskiest core change;
  done after P5/P8 clean the seams.* **Being delivered in risk-ordered SUB-SLICES** (the phase is
  ~3-4 phases of work; each slice builds + is adversarially reviewed on its own):
  - **P9.1 — Tooltip.** ✅ **DONE (2026-08-12).** A hover tooltip delivered as a composable
    ATTRIBUTE (`UIElement.tooltip` + `tooltipDelay`) that works on ANY element — deliberately NOT a
    new `Type` enum value, so it grows no emit-switch arm (composition over monolithic widget types,
    the overhaul's stated aim). Pipeline: the pointer pass (`ApplyPointerPass`) records the topmost
    tooltip-bearing element under the cursor into `UIContext.tooltipCandidate` (independent of the
    interaction gate, so a *disabled* control can still explain itself); `ui::UpdateTooltipTimer`
    advances a dwell timer on the **presentation clock** and unconditionally (see review below);
    `BuildVerticesImpl` emits a wrapped, screen-clipped popup below the element once the delay passes.
    Serialized (element key count 66→68, both writers + fuzz + guard), authored via a Tooltip field
    in the editor inspector. **Adversarially reviewed (3 lenses: timer-lifecycle / emit-positioning /
    serialization-lockstep) → 1 major + 1 minor + nits, all fixed:** the **major** — the dwell timer
    ran on the pause-scaled simulation `dt`, so tooltips never appeared on the *paused* pause/settings
    menu (a primary surface); fixed by moving it to a dedicated `UpdateTooltipTimer` on the unscaled
    presentation clock, called every frame (also fixes the minor: a popup freezing while the dev menu
    owns input). Emit + serialization lenses came back clean (wrap-consistency invariant, byte-identity,
    coordinate space, clamp all verified). `--test-uidoc` (68) + `--test-uipick` + full battery PASS;
    editor + runtime build & link both clean; D3D12 GPU smoke clean.
  - **P9.2 — WidgetVTable decomposition (D3).** ✅ **DONE (2026-08-12)** — spec at
    [Design-UIWidgetRegistry.md](Design-UIWidgetRegistry.md). The god-struct's per-type `switch`es (emit,
    pointer, nav, activate) are all a `WidgetVTable` indexed by the unchanged `Type` enum; `IsInteractive`/
    `IsFocusable` collapsed to its flags. Proven byte/flag-identical by `--test-uivtable` (emit 1752 verts +
    pointer 40 + nav/activate 21 probes) and **the default is flipped — the vtable is the shipped path**
    (battery + both-backend link + D3D12 live-UI smoke green). *Detail log:* **Foundation LANDED + PROVEN
    (2026-08-12):** the `WidgetVTable`/`WidgetEmitCtx` registry (UISystem.cpp, internal — welded to the
    `Emitter`), the additive dual-path emit dispatch (`g_uiUseVTable`, **defaults FALSE** so the shipped
    runtime is byte-for-byte the legacy switch), and the **`--test-uivtable` frozen-vertex gate** — which
    boots a real GPU session with its OWN corpus (no project needed, so it runs on this machine), emits
    legacy vs vtable, and `memcmp`s the streams. **ALL 10 emit widgets now extracted (Label, Image,
    Panel, Button, ProgressBar, Slider, Toggle, Selector, ScrollView, TextInput) → gate PASS, 1752 verts
    byte-identical** over a state cross-product corpus (selector options, radial bar, toggle on/off,
    slider value, hovered button, textinput text+placeholder, overflowing scrollview). The whole ~230-line
    emit switch is decomposed. (Gate note: the corpus loops legacy builds to a fixed point first — the
    ScrollView's auto-measured `contentExtent` settles over several builds, so a naive capture would drift.)
    Full regression battery + runtime link green (legacy default untouched). **Interact `onPointer` also
    DONE + PROVEN (2026-08-12):** all 6 interactive types' pointer arms extracted into `onPointer` slots
    (ApplyPointerToElement's preamble stays; its switch is the gated fallback), verified by a new
    `WidgetPointerParitySelfTest` (drives every interactive widget both ways, asserts identical flag
    mutations — 40 probes) folded into `--test-uivtable`. *Remaining:* onNav/onActivate (UIFocus keyboard
    switches, entangled with UpdateNavigation), the IsInteractive/IsFocusable→flag collapse, then flipping
    the default to vtable.
  - **P9.3 — advanced widgets (composable-first).** 🔨 IN PROGRESS.
    - **Tabs** ✅ **DONE (2026-08-12).** Composable ATTRIBUTES (`UIElement.tabGroup` + `tabTarget`,
      using P5 ids) — NOT a new type/component. `ui::ProcessTabs` (per-frame, after interaction) shows the
      clicked tab's target, hides the other targets in its group, and sets `toggled` on the active tab (so
      a P4 selectedColor highlights it). Serialized (key count 68→70), authored via inspector fields, works
      in the editor Interact preview. **Adversarially reviewed → 3 fixes:** doc-scoped the group/id lookup
      (cross-screen safety — two resident screens can reuse a `tabGroup` name), ran ProcessTabs in the
      preview, and added `visible` to the preview snapshot (corruption guard). Play-mode pollution already
      safe (CaptureSnapshot/RestoreSnapshot covers it). Battery + both-backend link green.
    - **Foldout / collapsible** ✅ **DONE (2026-08-12).** `UIElement.collapseTarget` (a P5 id) — clicking
      toggles that element's visibility (accordion sections, tree nodes) and sets `toggled` to the expanded
      state. Handled in the same `ui::ProcessTabs` (doc-scoped, preview-safe — inherits the tab review
      fixes). Serialized (key count 70→71) + inspector field. Independent of tabs (tab SETS, foldout TOGGLES).
    - *Remaining:* List (≈ ScrollView+VerticalLayoutGroup already, may need only a convenience), Tree
      (≈ nested foldouts now), Viewport/RenderTexture. **Virtualized list** is the real perf gap but is
      coupled to **data-binding** (nothing to virtualize without a data source) — treat as a joint effort.
  - **P9.4 — data-binding + virtualized list.** 🔨 IN PROGRESS — spec at
    [Design-UIDataBinding.md](Design-UIDataBinding.md) (scoped by the `ui-databind-scope` workflow).
    - **B2 data-binding model** ✅ **DONE (2026-08-12).** `ui::UIDataModel` (keyed store + revisions +
      providers) + `UIElement.bind{Text,Value,Visible,Texture}` (serialized, key count 71→75) →
      `ui::ResolveBindings` (rev fast-skip + value-compare guard) wired into the Engine frame order;
      inert until used (**zero behavior change**); headless `--test-uibind` PASS. Unifies the ad-hoc
      dynamic-content paths behind one channel.
    - *Remaining:* B1 (id-index O(1) `FindElementById`, perf), B3 (migrate token/interact/caption/
      settings onto the model), B4 (non-virtualized bound list = `UIList` + `InstantiateSubtree`), B5
      (virtualization). B4/B5 are the audit's perf gap; each phase is independently shippable + testable.
  - **P9.5+ (remaining):** full event-dispatch graph (capture/target/bubble + listeners). (D1
    runtime/authored split is lower-value — serialization already excludes runtime fields.)
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
