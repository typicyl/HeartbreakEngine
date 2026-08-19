# Design — Unified Material Authoring, Layering & Surface Painting

**Status: ARCHITECTURE + PHASED IMPLEMENTATION. Grounded in a first-hand audit of the existing
material / renderer / paint / asset / editor subsystems (both backends).**

Goal (verbatim intent): add three tightly-integrated capabilities — a **node-based Material Maker**,
a **Box Brush / projection volume**, and a **direct surface painting** system — that all feed **one**
material-layer architecture and resolve down to the engine's existing native runtime material
(`hbe::SurfaceParams` → OpenPBR BSDF → painterly → renderer). This is **not** three separate material
implementations, **not** a runtime node interpreter, and **not** a second asset pipeline.

The fundamental abstraction (the request's "MOST IMPORTANT ARCHITECTURAL RULE"):

```
MATERIAL GRAPH  ──compile──▶  COMPILED MATERIAL  ──▶  MATERIAL LAYERS  ──▶  WEIGHTS / MASKS  ──▶  FINAL SURFACE
                                                            ├── PAINT MASK        (persistent, UV/box)
                                                            ├── BOX-BRUSH MASK     (spatial weight field)
                                                            └── PROCEDURAL MASK    (compiled graph)
                                                                      ▼
                                                              MATERIAL RESOLVER
                                                                      ▼
                                                          hbe::SurfaceParams (OpenPBR)
                                                                      ▼
                                                       Heartbreak renderer (DX12 / Vulkan)
```

---

## Part A — Existing architecture (what we build ON, never AROUND)

Audited first-hand. The engine already solved the material-duplication problem *and* already ships
most of the runtime substrate this feature needs — specialized, not generalized. The whole design is
"**generalize + compose the existing parts**", not "add parallel systems".

### A.1 The one authoritative material representation (do not fork it)
- **`hbe::SurfaceParams`** ([RHI/SurfaceMaterial.h:57](../Source/RHI/SurfaceMaterial.h)) — the single
  CPU material VALUE struct (OpenPBR Surface v1.1.1 names), embedded **by value** in `MaterialAsset`,
  `MeshInstance`, and `rhi::DrawItem`. `operator==` (whole-struct) is the instancing identity predicate
  (`Renderer::SameMaterial`, [Renderer.cpp:90](../Source/Renderer/Renderer.cpp)). Textures & `materialFlags`
  are deliberately *not* here. **The resolver's output type is this struct** — that is the seam.
- **`rhi::GpuSurfaceMaterialExt`** (176 B, `static_assert`-locked, [RHI/GpuMaterial.h:27](../Source/RHI/GpuMaterial.h))
  — the append-only GPU tail block; mirror `OpenPBRMaterialExt` in `Shaders/Common.hlsli:135`.
- **`material::ComputeShaderVariant`** ([RHI/MaterialCompiler.h:22](../Source/RHI/MaterialCompiler.h))
  — routes a material to the cheapest curated pixel-shader variant (`Std/Coat/Sss/Fuzz/Hair/Eye/Full`),
  never 2^N. **World features (terrain/paint/wind/alpha-test) stay runtime `MaterialFlags`, not PSO
  variants.** Material layering follows the *world-feature* rule, not the *lobe-variant* rule.
- **`.hbmat`** ([Assets/MaterialAsset.cpp](../Source/Assets/MaterialAsset.cpp)) — JSON, `version:2`,
  back-compat via null/type-tolerant `JGet`; registry row `runtimeLoaded + JsonScan` so texture refs
  auto-pack. This is the native authoring + serialized-parameter format and stays authoritative.

### A.2 The runtime layered path already exists — as terrain splat
`MaterialFlag_TerrainSplat` drives a **4-layer, world-XZ-tiled material blend** in
[MeshPBR.hlsl:466-537](../Shaders/MeshPBR.hlsl): 4 albedo/normal/MR layers + per-layer roughness + a
weight mask, sampled by **world-space tiling** with a **synthesized world-aligned tangent frame** and
geometric specular AA, overloading the bindless slots (`MaterialFlag_TerrainSplat` slot-reuse contract).
This is a working world-space tiled N-layer blend. **The generalized layered forward path is a
generalization of this block** (arbitrary projection + configurable layer count + reoriented normals),
not net-new shader architecture.

### A.3 The painting system already exists — as `hbe::paint`
[Scene/PaintSystem.{h,cpp}](../Source/Scene/PaintSystem.h) + `PaintComponent`/`PaintLayer`
([Components.h:299](../Source/Scene/Components.h)) is a full surface painter:
- **Layer stack**: each `PaintLayer` = `color` RGBA8 (albedo+coverage) + `material` RGBA8
  (metal / rough / height@0.5-neutral / coverage). `Flatten` composites → two GPU textures.
- **Brush model**: `BrushDef` → `MakeBrushTip` → `BrushTip` (deterministic) → `Dab`, single texel
  writer `WriteDabTexel` (live-paint == rebake identical).
- **Stroke DATABASE** (`PaintComponent.strokes`) is the source of truth; baked layers are a cache.
  Undo/redo = pop stroke + `BakeFromStrokes` + `Sync`. Persist to **`.hbpaint` v5** (`'HPNT'`).
- **Box + UV + 3D projection** already: `paint::BoxProjectUV` ↔ `BoxPaintUV` (MeshPBR.hlsl:37),
  `paintProjMode`. GPU upload via `Renderer::UpdateTexture` (in-place) / `UploadTexture`.
- Consumed by MeshPBR via the `gPaint*` `ObjectConstants` block (Common.hlsli:193).

**Material painting reuses this wholesale** — the paint canvas *is* the persistent-mask storage the
request asks for. We extend it to write **layer weights** (a mask channel), not just a flat overlay.

### A.4 Node-graph editors already have a template (no library)
No imgui-node-editor dependency. `Editor::DrawSchematicCanvas` ([Editor.cpp:12397](../Source/Editor/Editor.cpp))
and `DialogueEditor.cpp` are two hand-built node canvases (grid, `AddBezierCubic` links, node rects,
pins, wire-drag, right-click add-node popup). `.hbschem` (schematic visual scripting) is a directly
analogous **model + compiler + editor** precedent. The Material Maker mirrors this exactly, in its own
translation unit (`Source/Editor/MaterialGraphEditor.cpp`).

### A.5 Box volumes & spatial culling
No broad-phase; frustum cull is brute-force `Frustum::Intersects(center, halfExtent)`
([Renderer.cpp:40](../Source/Renderer/Renderer.cpp)). But box volumes are a solved pattern:
`PostVolume`/`TriggerVolume`/`ReflectionProbe` are collected per-frame with half-extents; `stream::DistanceToBox`
([StreamPolicy.h:209](../Source/Scene/StreamPolicy.h)) and `tagshard` grid+DSU exist to spatially bucket
boxes. **Box-brush volumes cull with `Frustum::Intersects` + `DistanceToBox`**, and can be gridded with
the existing DSU if counts ever grow.

### A.6 Asset cook / pack
Single registry (`AssetFormats.cpp`), `.uaf`/`.uap` v5 portable zstd cook, deterministic (byte-identical
re-cook), slot identity (`SlotIds`), BC path (`TextureCompress`, BC4 for single-channel masks). New
formats register with **one row** each. Everything loads through `vfs::ReadFile`. The `.hbmatgraph`
source + its cooked form follow the `.hbvolsim`→`.hbvol` precedent.

### A.7 The load-bearing invariants (must not break)
1. **`ObjectConstants` byte-identity** across HLSL `Common.hlsli` == D3D12 `ObjectCB` == Vulkan
   `ObjectUBO`. "The codebase's #1 silent-corruption bug class." **Append only**, std140 16-byte rows,
   vec3 never straddles a boundary, zero-init new fields.
2. **Painterly downstream contract**: MeshPBR must keep emitting the identical `PSOutput` (G-buffer
   `octN/rough/metal`, 2-bit alpha mask on `gOutputLinear`, linear-HDR). `MaterialFlags` bits 7 & 10
   reserved for the painterly mask — never reuse.
3. **`ShaderVariant` 5-way lockstep** (enum ↔ cmake ↔ 2 backend tables ↔ `ComputeShaderVariant`).
4. **`Renderer::SameMaterial`** must cover every new shading input or instanced runs wear the head's value.
5. **Cross-platform determinism**: cooked bytes LE-only, no ABI/endian coupling; two cooks byte-identical.
6. **`.hbpaint` file == collab wire** share one field order (`PutStroke`/`GetStroke`); layer identity is
   the stable `PaintLayer::id`, not array index; `strokesComplete` gates `BakeFromStrokes`.

---

## Part B — The unified architecture

### B.1 The pipeline mapped onto existing seams

| Stage | Concept | Realization | New/Reuse |
|---|---|---|---|
| Material Graph | "the material being applied" | `matgraph::Graph` (`.hbmatgraph`) + compiler | **New** module `Source/Material/` |
| Compiled Material | optimized native rep | `matgraph::CompiledGraph` → bakes to `SurfaceParams` + a small procedural-op list + texture set | **New** |
| Material Layers | stack of materials + weights | `mat::LayerStack` component; layer = material ref + mask source + opacity + blend mode + height/normal contribution | **New** component; generalizes terrain-splat |
| Paint mask | persistent UV/box mask | `hbe::paint` canvas, extended with a **weight channel** per layer | **Reuse + extend** `.hbpaint` |
| Box-brush mask | spatial weight field | `mat::BoxBrush` (`EvaluateBrush(worldPos)→0..1`) → baked into a layer weight or evaluated live | **New**, culled via existing `Frustum`/`DistanceToBox` |
| Procedural mask | graph-driven mask | a `CompiledGraph` output channel evaluated to a mask texture (offline bake) or a cheap runtime op | **New**, reuses the graph compiler |
| Material Resolver | combine layers | `mat::Resolve(...)` (CPU reference) + the generalized layered forward shader (GPU) | **New** resolver; generalizes MeshPBR splat block |
| OpenPBR surface | shading | unchanged | **Reuse** |
| Renderer | draw | unchanged draw path; one new `MaterialFlag_Layered` + append-only GPU fields | **Reuse** |

### B.2 Two resolution modes (the low-end-hardware answer)
The request demands both dynamic authoring *and* low-end performance. The resolver runs in two modes,
chosen per surface by a quality/authoring policy — **the material representation is identical in both**:

- **BAKED (default for static environment surfaces)** — the resolver runs **offline at cook time**:
  layers + masks composite into the paint canvas's flat color/material textures (and optional height),
  which feed the *existing* single-material path. Cost at runtime = today's cost. This is how a 100 m
  concrete floor with mud/moss/blood pays nothing extra. Tiling is preserved because the bake samples
  each layer with its own world/local-space tiling before compositing (no UV stretch).
- **LIVE (opt-in, bounded layer count)** — the generalized layered forward path samples up to
  `kMaxLiveLayers` (=4, matching terrain splat) per-layer materials + masks and blends in-shader, for
  surfaces that must change at runtime (gameplay decals, dynamic weather wetness already exists). Gated
  by `MaterialFlag_Layered`; a hard cap + spatial cull of box volumes keeps it cheap.

Both modes resolve to `SurfaceParams` + the existing texture slots — never a competing pipeline.

### B.3 Normal & height blending (correctness, not lerp)
- **Height blend**: `w_i' = smooth( w_i + h_i*heightContribution_i )` then normalize — a layer's height
  map biases where it wins (Unreal-style height-lerp), with an optional noise term
  (`w_i' += noise*noiseAmount`) for the "height + noise" mode.
- **Normal blend**: **reoriented normal mapping (RNM)** / UDN, never a tangent-space lerp — implemented
  in `mat::BlendNormalRNM` (CPU) and `MaterialLayered.hlsli` (GPU), matching the terrain-splat frame.

### B.4 Coordinate spaces
A layer/graph sampler chooses its space: `UV0 | UV1 | Object | World | Triplanar`. World/Object/Triplanar
tiling is size-independent (a 1 m tile stays 1 m regardless of brush/surface size) because the sampler
multiplies **world/object position** by `1/tileMeters`, never the brush dimensions. Triplanar is the
generalized 3-axis `abs(normal)`-weighted blend (net-new vs. the terrain single-plane block, modeled on it).

---

## Part C — Implementation inventory (Part 12 of the request)

### C.1 New module `Source/Material/` (backend-agnostic, headless-testable — the crown jewel)
| File | Contents |
|---|---|
| `MaterialGraph.{h,cpp}` | `matgraph::Graph`, `Node`, `Pin`, `Link`, `NodeType` catalog (Constant/Color/Float/Vector/Texture/NormalMap/Multiply/Add/Subtract/Divide/Lerp/Clamp/Remap/Power/Smoothstep/OneMinus/Noise/Voronoi/Gradient/ColorRamp/UV/WorldPos/ObjectPos/Normal/VertexColor/Height/Mask/MaterialLayer/Output). Stable `u32` ids (never pointers). `.hbmatgraph` JSON save/load, versioned. |
| `MaterialGraphCompiler.{h,cpp}` | `Compile(Graph)→CompiledGraph`: validate → topological order → constant-fold → bind Output pins to the 8 channels (BaseColor/Roughness/Metallic/Normal/Height/AO/Emissive/Opacity). Deterministic. Produces the **optimized native rep**: constant channels fold to `SurfaceParams`; non-constant channels become a compact `ProcOp` list (no generic runtime interpreter). |
| `MaterialParameters.{h,cpp}` | `ParamSet` (Color/Scalar/Vector/Texture/Bool) + `ParamOverrides` for instances (override without duplicating the graph). |
| `MaterialLayer.{h,cpp}` | `Layer` (materialRef, `MaskSource`, opacity, `BlendMode{Linear,Height,HeightNoise}`, `heightContribution`, `normalContribution`), `LayerStack`, and `Resolve(stack, sample)→SurfaceParams` (+ resolved texture-weight set). `BlendNormalRNM`. |
| `MaskSource.{h,cpp}` | `MaskSource` tagged union: `Paint`(canvas+layerId+channel), `Box`(BoxBrush), `Procedural`(CompiledGraph channel), `Constant`(w). `EvaluateMask(source, sample)→0..1`. |
| `BoxBrush.{h,cpp}` | `BoxBrush` (world transform TRS, w/h/d, `Falloff` curve, strength, projection+tiling params). `EvaluateBrush(worldPos)→0..1` (transform to local, box SDF, falloff curve), `BrushBounds()→AABB` for culling, tiling/triplanar UV helpers. |
| `FalloffCurve.{h,cpp}` | Configurable falloff (not a hardcoded smoothstep): `{Linear,Smoothstep,Smootherstep,EaseIn,EaseOut,Constant}` + `gamma`; `Eval(t)→0..1`. |
| `MaterialCook.{h,cpp}` | Offline BAKE mode: resolve a `LayerStack` over a mesh's UV into the paint canvas flat textures (reuses `paint::` composite), and bake procedural/box masks into weight channels. Emits the cooked `.hbmat` + canvas. |
| `MaterialAuthoringTest.{h,cpp}` | `SelfTest()` (headless) — every case in Part 13. |

### C.2 Modified: Scene / renderer (LIVE mode, additive)
- `Scene/Components.h`: new `MaterialLayerComponent` (a `mat::LayerStack` + resolved handles) — **new
  component, no change to `MeshInstance` fields**. Extend `PaintComponent` with an optional per-layer
  **weight buffer** (a 3rd flat texture) via the documented paint-extend recipe (new `.hbpaint` version,
  `WriteDabTexel` channel, `Sync`/`Prepare` upload, `gPaint*` append).
- `RHI/RHI.h` `DrawItem` + `RHI/GpuMaterial.h`: **append** layered-material fields (up to 4 layer
  material-param indices + mask/tiling params) to `GpuSurfaceMaterialExt` (bump `static_assert`) and the
  `gMatExt` tail — never touch the legacy `ObjectConstants` layout. Add `MaterialFlag_Layered` (next free
  bit, avoiding 7/10). Add to `Renderer::SameMaterial`.
- `Scene/Scene.cpp` `CollectDrawItems`: populate the new fields from `MaterialLayerComponent`.

### C.3 Modified: shaders (both backends, live-GPU-verification-pending)
- New `Shaders/MaterialLayered.hlsli`: `SampleLayered(...)` — generalizes the terrain-splat block to N≤4
  layers, arbitrary projection (`UV/World/Object/Triplanar`), height+noise weight blend, RNM normal blend.
- New `Shaders/Triplanar.hlsli`: 3-axis `abs(normal)`-weighted triplanar sampler + world tangent frame.
- `Shaders/MeshPBR.hlsl` PSMain: a `#if`-free runtime branch on `HBE_MAT_LAYERED` in the material-sampling
  region (:457) — a *world feature*, present in every variant (like terrain splat), so **no new PSO
  variant, no permutation growth**. `PSOutput` unchanged (painterly-safe).

### C.4 New asset types & serialization
- `.hbmatgraph` — editor source (node graph, JSON, versioned). Registry: `runtimeLoaded=false, JsonScan`.
- Cooked material: **reuse `.hbmat`** (the bake writes a normal `.hbmat` + canvas) — no new runtime
  format needed for BAKED mode. LIVE mode adds `.hbmatlayer` (JSON layer stack, `runtimeLoaded=true,
  JsonScan`) referencing child `.hbmat`s + `.hbpaint` + a `.hbbox` box-set.
- `.hbpaint` bumped a version for the weight channel (migration branch, `kStroke*Version` bump).
- All portable JSON / existing binary; all load via `vfs::ReadFile`.

### C.5 Editor
- `Source/Editor/MaterialGraphEditor.cpp` (own TU): `DrawMaterialGraphEditor` + `DrawMaterialGraphCanvas`
  (mirror `DrawSchematicCanvas`), node search, connections, parameter inspector, live sphere/plane
  preview (reuse the asset-viewer preview), texture preview, save/load, material instances. `Panel_MaterialGraph`
  enum + `kNames[]` + `SaveSurface::MaterialGraph` + `AssetHistory<matgraph::Graph>`.
- Box-brush world tool: `UpdateBoxBrushTool` (mirror `UpdateTerrainTool`), gizmo (reuse ImGuizmo on the
  box transform), tiling/falloff/strength/blend/material controls, `boxBrushConsumedClick_`.
- Paint tool: **already exists** (`UpdateArtTool`) — add layer/weight selection + eraser to its brush UI.
- Layer-stack inspector: in `DrawInspector`, per-`MaterialLayerComponent` layer list (material, mask
  source, blend mode, opacity, height/normal contribution, reorder).

### C.6 Tests (Part 13) — wired as `--test-material` (headless) + sub-asserts
Graph serialization/loading, node compilation, deterministic compilation (byte-identical recompile),
parameter overrides, layer blending, height blending, normal blending (RNM identity/known-vectors), box
weight evaluation, box rotation, box falloff (curve shapes), world-space tiling (size-independence),
local-space tiling, paint/mask serialization, painting undo/redo (reuse `BakeFromStrokes`), asset cook,
asset load, cross-platform (LE) serialization. Plus a **visual test scene** builder (`--matscene`) with a
large tiled floor, a wall, a box-brush second material, overlapping volumes, painted masks, procedural
masks, height blending, and multiple tiling scales — for the live-GPU look pass.

---

## Part D — Phase plan (each phase builds + tests green before the next)

- **P0 — This document.** ✅
- **P1 — Graph model + compiler + parameters** (`MaterialGraph`, `MaterialGraphCompiler`,
  `MaterialParameters`, `.hbmatgraph` I/O). Headless. `--test-material` (graph/compile/determinism/params).
- **P2 — Layer stack + resolver + masks + box brush + falloff** (`MaterialLayer`, `MaskSource`,
  `BoxBrush`, `FalloffCurve`). Headless. Extend `--test-material` (blend/height/normal/box/falloff/tiling).
- **P3 — Asset integration** (`AssetFormats` rows, `MaterialCook` bake mode, cross-platform serialize
  tests). Headless.
- **P4 — Paint mask extension** (`.hbpaint` weight channel via the documented recipe). Headless serialize
  test + editor upload.
- **P5 — LIVE runtime path** (`GpuSurfaceMaterialExt` append, `MaterialFlag_Layered`, `MaterialLayered.hlsli`
  + `Triplanar.hlsli`, MeshPBR hook, `Scene`/`DrawItem` wiring, `SameMaterial`). Both backends compile;
  **look = live-GPU-pending**.
- **P6 — Editor** (graph editor panel, box-brush tool, layer inspector, paint layer/weight UI). Builds;
  **UX = live-editor-pending**.
- **P7 — Visual test scene** (`--matscene`) + adversarial review + docs.

**Verified vs. pending (honesty):** P1–P4 are pure CPU/serialization and are **headless-verified** by
`--test-material` + the editor Release build. P5 shader look and P6 editor UX **compile clean on both
backends but need a live GPU/editor session to verify appearance** — the same delivery posture the
OpenPBR program used (P6b/P7 "needs live-GPU validation").

---

## Part E — Performance & limits
- **Texture memory**: BAKED mode adds zero runtime textures beyond today's paint canvas; LIVE mode caps
  at 4 layers × (albedo/normal/MR) + 1 mask, BC-compressed (BC4 masks). Mask resolution is author-chosen
  with mip generation.
- **Box volumes**: spatially culled via `Frustum::Intersects` + `DistanceToBox`; a per-frame active set,
  hard cap `kMaxActiveBoxVolumes`. Baked box masks cost nothing at runtime.
- **Shader complexity / permutations**: layering is a **runtime world-feature flag**, not a PSO variant —
  **zero permutation growth**. LIVE layered sampling is bounded and branch-gated.
- **CPU submission / batching**: no change to the draw path; `SameMaterial` keeps instancing correct.

## Part F — Accepted approximations / boundaries
- Runtime layer count capped at 4 (LIVE); unbounded stacks must use BAKED mode (composited offline).
- The graph runtime does **not** interpret arbitrary node networks: only constant-foldable channels feed
  `SurfaceParams`; non-constant channels are baked to textures (offline) or a tiny fixed `ProcOp` set.
  This mirrors the MaterialX boundary ("no general graph evaluation, ever, at runtime").
- Triplanar uses 3-tap `abs(normal)` blend (standard real-time trade), not full anisotropic projection.
- Normal blend is RNM/UDN (analytic), not a full frame re-derivation.
- OpenGL backend stays on its base path (out of scope, matching the OpenPBR program).
- Default-preserving: a mesh with no `MaterialLayerComponent` and no `MaterialFlag_Layered` is
  **byte-identical** to today.
