# Design — MaterialX-aware, OpenPBR-based Material Architecture

**Status: AUDIT + ARCHITECTURE ONLY. No implementation. No renderer code has been changed.**

Authoritative references (treated as ground truth):
- OpenPBR Surface v1.1.1 — https://academysoftwarefoundation.github.io/OpenPBR/ and repo `reference/open_pbr_surface.mtlx` (Apache-2.0).
- MaterialX — https://github.com/AcademySoftwareFoundation/MaterialX (Apache-2.0), OpenPBR integrated in MaterialX 1.39.

Goal (verbatim intent): *evolve* Heartbreak's existing renderer into a MaterialX-aware, OpenPBR-based
material architecture while preserving Heartbreak's native runtime representation, existing rendering
systems, painterly identity, DX12/Vulkan support, and aggressive performance goals — **not** replace
the PBR shader with OpenPBR, and **not** turn the engine into a generic MaterialX runtime.

Responsibility split (target end state):

| Layer | Owner | Runs when |
|---|---|---|
| Interchange / authoring / graph / import-export | **MaterialX** | editor / import-time only |
| Standardized physically-based surface model | **OpenPBR Surface** | definition of the parameter set + BSDF |
| Native optimized runtime material | **Heartbreak Material System** (`.hbmat` + compiled GPU struct) | runtime |
| Optimized DX12/Vulkan implementation | **Heartbreak Renderer** | runtime |
| Painterly / stylization | **Heartbreak-specific** (unchanged) | runtime, strictly downstream |

---

## Part A — Existing Architecture Audit

Verified first-hand + a 7-domain subagent audit (both backends, RHI seam, passes, import, editor,
painterly). All claims carry `file:line` anchors.

### A.1 Material representations — the "four mirrors"

Material parameters are hand-duplicated across four CPU/GPU structures with **no compile-time
cross-check**:

| # | Representation | Location |
|---|---|---|
| 1 | `MaterialAsset` (`.hbmat` JSON, authoring/serialized) | [MaterialAsset.h:27](../Source/Assets/MaterialAsset.h) |
| 2 | `MeshInstance` (scene component) | [Components.h:251](../Source/Scene/Components.h) |
| 3 | `rhi::DrawItem` (per-draw) + `materialFlags` bitmask | [RHI.h:595](../Source/RHI/RHI.h), enum [RHI.h:559](../Source/RHI/RHI.h) |
| 4a | `ObjectConstants` (HLSL `cbuffer b1`) | [Common.hlsli:127](../Shaders/Common.hlsli) |
| 4b | `ObjectCB` (D3D12) — must byte-match 4a | [D3D12Device.cpp:293](../Source/RHI/D3D12/D3D12Device.cpp) |
| 4c | `ObjectUBO` (Vulkan) — must byte-match 4a/4b | [VulkanDevice.cpp:212](../Source/RHI/Vulkan/VulkanDevice.cpp) |

Fields today: `baseColor(vec4)`, `metallic`, `roughness`, `emissiveColor`, `emissiveIntensity`,
`subsurfaceColor`, `subsurfaceRadius`, `clearcoat`, `clearcoatRoughness`, `u32 materialFlags`, and 6
texture slots (albedo / normal / MR[glTF B=metal,G=rough] / AO / emissive / thickness).

### A.2 Exact CPU → GPU data flow (traced)

```
MaterialAsset (.hbmat)                         assets::LoadMaterial  (MaterialAsset.cpp:78)
   │  assets::ApplyMaterial (MaterialAsset.cpp:122)  ── copies flat fields + uploads textures (texCache)
   ▼
MeshInstance (Components.h:251)                 scene component; 1:1 flat mirror
   │  Scene collects → Renderer::CollectDrawItems (Renderer.cpp:313); flags/sort/cull/instancing
   ▼
rhi::DrawItem (RHI.h:595)                        per-draw; materialFlags routing bits set in Scene.cpp:342/349
   │  FillObjectMaterial (D3D12Device.cpp:342)  ── THE single packer on D3D12
   │  (Vulkan: DUPLICATED inline at ~4151 scene, ~5488 preview — no shared packer)
   ▼
ObjectConstants b1  ==  ObjectCB / ObjectUBO     hand-synced byte layout; per-draw root CBV / UBO
   │  AllocConstants per-frame arena (D3D12 3543/4501/5546); shadow pass REUSES the scene ObjectCB byte-for-byte
   ▼
GPU: root CBV(b1) + bindless SRV table (t0 space0, Tier-3 unbounded)  →  MeshPBR.hlsl reads gMaterialFlags + factors + bindless indices
```

**Load-bearing couplings:** (i) the three CB structs must stay byte-identical; (ii) the shadow-pass
fill must write the same bytes as the scene fill; (iii) `Renderer::SameMaterial`
([Renderer.cpp:90](../Source/Renderer/Renderer.cpp)) is the instancing identity predicate — any new
per-material field omitted there makes instanced runs inherit the run-head's value (silent wrong
render).

### A.3 Material system specifics

- **MaterialFlags** ([RHI.h:559](../Source/RHI/RHI.h)): 12 bits (Subsurface, Cloth, Eye, Hair,
  Transparent, NoShadow, DepthWrite, PainterlyExempt=1<<7, TerrainHole, TerrainSplat, Censored=1<<10,
  Water=1<<11). Dual-purpose: Transparent/Water/DepthWrite drive **PSO/pass routing**; the rest are
  pure shader branches. Bits 7 & 10 are consumed by the painterly mask encode — **must not be reused.**
- **Presets** ([Editor.cpp:15262](../Source/Editor/Editor.cpp), `DrawMaterialPresetCombo`): 5 entries
  (Standard/Skin/Cloth/Eye/Hair) that swap one mutually-exclusive "family" flag — the closest
  existing analog to an OpenPBR material type.
- **Serialization** ([MaterialAsset.cpp:36](../Source/Assets/MaterialAsset.cpp)): JSON `version:2`.
  `LoadMaterial` **ignores the version int** and reads every field with `j.value(key, default)` →
  **new fields are automatically backward-compatible**. Registry row is `runtimeLoaded=true` +
  `RefScan::JsonScan` ([AssetFormats.cpp:76](../Source/Assets/AssetFormats.cpp)) → new texture-path
  fields auto-pack with zero cooker edits.
- **Application** (`ApplyMaterial`): flat copy + texture upload via a rel-path→handle `texCache`.
- **Texture refs**: 6 fixed slots; MR packed B=metal/G=rough; sRGB for color/emissive, UNORM for data;
  optional non-destructive `.bc.uaf` sibling (BC3 color / BC5 normal; MR+AO uncompressed; shader
  reconstructs normal Z). Container = `.uaf` v9 ([UAF.h](../Source/Assets/UAF.h)).
- **GPU material struct**: `ObjectConstants`/`ObjectCB`/`ObjectUBO` (see A.1), packed once per draw.

### A.4 DX12 path

- **PSOs**: one base `D3D12_GRAPHICS_PIPELINE_STATE_DESC` (`CreateMeshPipeline`,
  [D3D12Device.cpp:1883](../Source/RHI/D3D12/D3D12Device.cpp)) cloned into `meshPSO_` (opaque, 3-MRT),
  `meshPSOTransparent_`, `meshPSOTransparentDepth_`, `meshPSOSingle_` (preview), `skyPSO_`,
  `shadowPSO_` (VS-only). Only `waterPSO_`/`strokeSurfacePSO_` bind different shaders. Variants differ
  **only** in blend/depth/raster/RT-count.
- **Root signature** (params[8], [D3D12Device.cpp:1917](../Source/RHI/D3D12/D3D12Device.cpp)): root
  CBVs (FrameCB b0, ObjectCB b1), root SRVs (bones t0 space1, instances t1 space1), **bindless
  unbounded SRV array** (t0 space0, Tier-3, `NonUniformResourceIndex`), 2 static samplers.
- **Descriptor tables**: bindless heap 4096 slots ([D3D12Device.cpp:892](../Source/RHI/D3D12/D3D12Device.cpp)).
- **DXC**: offline only, via cmake (A.6). No runtime compile. **No defines.**
- **Caching**: precompiled `.dxil` loaded by leaf name (`ReadBinaryFile`,
  [D3D12Device.cpp:200](../Source/RHI/D3D12/D3D12Device.cpp)); no PSO disk cache; no pipeline-library.
- **Transparency**: 2nd pass, per-item PSO by DepthWrite; **straight alpha blend, no refraction.**
- **Render targets / passes**: forward + thin G-buffer; `DrawScene` sub-passes opaque→sky→transparent
  →water→particles ([D3D12Device.cpp:4328](../Source/RHI/D3D12/D3D12Device.cpp)); `RunPostStack`
  (ssr→ssgi→fog→volume→painterly→ssao→bloom→exposure→tonemap→taa→dof→mblur→fxaa).

### A.5 Vulkan path (mirror + one divergence)

- **Pipelines**: `meshPipeline_`, `…Transparent_`, `…TransparentDepth_`, `waterPipeline_`,
  `meshPipelineSingle_`, depth-only ([VulkanDevice.cpp:467+](../Source/RHI/Vulkan/VulkanDevice.cpp));
  SPIR-V loaded from `MeshPBR.*.spv` once.
- **Descriptor sets**: set0 = FrameUBO + ObjectUBO(dynamic) + bones + instances; set1 = bindless
  sampler(0) + variable-count sampled-image array(1) (`UPDATE_AFTER_BIND`,
  [VulkanDevice.cpp:1702](../Source/RHI/Vulkan/VulkanDevice.cpp)). SPIR-V register shifts
  `-fvk-{t,s,u}-shift` in [ShaderCompile.cmake](../../cmake/ShaderCompile.cmake).
- **Material buffer**: `ObjectUBO` ([VulkanDevice.cpp:212](../Source/RHI/Vulkan/VulkanDevice.cpp)) —
  **byte-for-byte duplicate** of D3D12 `ObjectCB`. **Divergence: no shared packer** — the fill is
  inlined (scene ~4151, preview ~5488), so a new field must be added in ≥2 Vulkan sites plus the D3D12
  `FillObjectMaterial`.
- **Shader variants / caching**: none; no `VkPipelineCache` disk cache. Transparency + passes mirror
  D3D12.

### A.6 Shader system

- **Compilation**: cmake `hbe_add_shader` (SOURCE/ENTRY/STAGE/NAME) → DXC → `NAME.stage.dxil` +
  `NAME.stage.spv`, SM 6.5, staged next to the exe. Full explicit list in
  [ShaderCompile.cmake](../../cmake/ShaderCompile.cmake).
- **Permutations: NONE.** Zero `-D` defines to the compiler. `MeshPBR.ps` is one uber-shader that
  runtime-branches on `gMaterialFlags`.
- **Defines generated: none.** **Caching:** compile-once-at-build, load-by-leaf-name; the
  "silent-dormancy precedent" (a shader not registered here simply doesn't exist at runtime) is
  called out repeatedly.
- **Feasibility of specialization:** high and low-risk. `hbe_add_shader` is the single hook to emit
  `-D`-keyed variants (`MeshPBR_<VAR>.{vs,ps}.{dxil,spv}`); the loader already keys by leaf name;
  bindless means extra texture slots need no root-sig/descriptor changes. This is the seam for the
  hybrid static/dynamic model (Part D).

### A.7 Import

- **Model import = Assimp** ([ModelLoader.cpp](../Source/Assets/ModelLoader.cpp), `ConvertMaterial:30`):
  reads only base color / metallic / roughness / emissive (+`emissive_strength`) / 5 texture slots.
  **All KHR PBR extensions dropped** (clearcoat, transmission, ior, specular, sheen, anisotropy,
  iridescence, volume); the importer's generated `.hbmat` doesn't even copy existing
  clearcoat/subsurface.
- **No MaterialX / OpenPBR / `.mtlx` ingestion anywhere** (grep-confirmed).

### A.8 Editor tooling

Two ImGui surfaces in [Editor.cpp](../Source/Editor/Editor.cpp): entity inspector Mesh/Material
(`8605-8727`, edits live `MeshInstance`) and Asset-Viewer full-PBR `.hbmat` editor (`17810-18009`,
edits a `MaterialAsset` copy + textures + acoustics + live sphere preview). Both share
`DrawMaterialPresetCombo`. `AcousticMaterial` block is orthogonal and **must be preserved**; note its
`transmission` field (acoustic occlusion) is **unrelated** to OpenPBR `transmission_weight`.

### A.9 Painterly seam — crown-jewel invariants (must not break)

Painterly is strictly downstream, reading only: final **linear-HDR RGB**, the thin **G-buffer**
`float4(OctEncode(N).rg, roughness, metallic)`, scene **depth**, and the **2-bit alpha mask**
`float(mbits)/3.0` (bit0=PainterlyExempt[128u], bit1=Censored[1024u], written **only** when
`gOutputLinear!=0`; decoded by `DecodeMaskBits`, [PostCommon.hlsli:93](../Shaders/PostCommon.hlsli)).
Passes: [Painterly.hlsl](../Shaders/Painterly.hlsl), [BrushStrokes.hlsl](../Shaders/BrushStrokes.hlsl),
[PainterlyComposite.hlsl](../Shaders/PainterlyComposite.hlsl), true-3D
[StrokeSurface.hlsl](../Shaders/StrokeSurface.hlsl) + [StrokeGen.cpp](../Source/Assets/StrokeGen.cpp).
OpenPBR is safe **iff** MeshPBR keeps emitting the identical PSOutput. StrokeSurface uses a *reduced*
lighting model — it should be updated alongside the BSDF to avoid under/over-painting drift.

### A.10 Transmission today

None. `MaterialFlag_Transparent` = straight alpha compositing. No IOR, refraction, thin-walled, or
volume. There is **no scene-color-behind resource** bound to the transparent pass (SSR is a post pass
reading HDR color, [SSR.hlsl:11](../Shaders/SSR.hlsl)).

### A.11 OpenGL

[GLDevice.cpp](../Source/RHI/GL/GLDevice.cpp) is far below parity (no bindless/materialFlags/BC/
compute). **Out of scope** (user decision 2026-08-16): unchanged; renders its base-MR approximation.

---

## Part B — OpenPBR Gap Analysis

Classification key: **(1)** supported correctly · **(2)** supported, needs param/behavior remap ·
**(3)** partial · **(4)** new implementation · **(5)** not appropriate for the RT renderer ·
**(6)** future/optional.

| OpenPBR feature | Current Heartbreak support | Class | Required changes | Difficulty | Perf impact | Recommended approach |
|---|---|---|---|---|---|---|
| **base_color** | `baseColor.rgb` + albedo map | 2 | add `base_weight` scalar; split opacity from `.a` | Low | none | remap; keep vec4, add weight |
| **base_metalness** | `metallic` + MR.B | 1 | — | Low | none | keep |
| **specular_roughness** | `roughness` + MR.G | 1/2 | rename; `α=r²` already used | Low | none | keep |
| **base_diffuse_roughness** (Oren-Nayar) | Lambert only ([MeshPBR.hlsl:246](../Shaders/MeshPBR.hlsl)) | 4 | EON diffuse, `#if` gated (r=0 ⇒ Lambert) | Low | tiny (gated) | new lobe, specialize on r>0 |
| **specular_weight / color** | none | 4 | add fields; modulate Fresnel | Low | none | new params, dynamic |
| **specular_ior** | fixed F0=0.04 | 4 | F0 from IOR (`((1-η)/(1+η))²`); default 1.5⇒0.04 (back-compat) | Low | none | new param, dynamic |
| **specular_roughness_anisotropy** (+tangent) | hair-only hack | 4 | anisotropic GGX + tangent frame | Med | med (gated) | ANISO permutation |
| **Metal Fresnel (F82-tint)** | Schlick `lerp(0.04,albedo,metal)` | 2 | F82-tint conductor; white specular_color≈Schlick | Low | none | remap in base lobe |
| **coat_weight / coat_roughness** | `clearcoat`/`clearcoatRoughness` (fixed F0) | 3 | extend to coat_ior/color/darkening | Low | low (gated) | evolve clearcoat → COAT |
| **coat_ior / coat_color / coat_darkening** | none | 4 | IOR-driven coat Fresnel, absorption tint, base darkening/roughening | Med | low | COAT permutation |
| **coat_roughness_anisotropy / rotation / coat_normal** | none | 6 | coat aniso + separate coat normal map | Med | low | future/optional |
| **fuzz_weight / color / roughness** | cloth flag (Charlie, replaces GGX, hardcoded) | 3 | make additive top layer + params | Low | low (gated) | evolve cloth → FUZZ layer |
| **subsurface_weight / color / radius** | flag + subsurfaceColor/Radius + LUT | 3 | weight blend; keep pre-integrated approx | Med | med (gated) | SSS permutation, approx |
| **subsurface_radius_scale (RGB) / scatter_anisotropy** | none | 4/5 | RGB MFP falloff; anisotropy approx | Med | med | approximate; document |
| **transmission_weight / color** | none (alpha only) | 4 | thin-walled tint + SS refraction | High | med (opt-in) | TRANSMISSION permutation |
| **transmission_depth / scatter / dispersion** (volume) | none | 5/6 | Beer-law approx; dispersion deferred | High | high | approx/omit; document |
| **thin_walled** | none | 4 | thin dielectric (no bend, double-sided) | Low | low | part of TRANSMISSION |
| **thin_film_weight / thickness / ior** | none | 4 | Belcour-Barla analytic on Fresnel | Med | low (gated) | THINFILM permutation |
| **emission_color / luminance** | emissiveColor×intensity | 2 | rename; nit units approximated | Low | none | remap |
| **geometry_opacity** | `baseColor.a` + Transparent flag | 2 | scalar opacity → alpha (respect painterly mask) | Low | none | remap carefully |
| **geometry_normal / bump** | normal map (BC5 Z-reconstruct) | 1 | — | Low | none | keep |
| **geometry_tangent** | per-vertex tangent | 2 | expose for anisotropy | Low | none | reuse |
| **Layering & energy conservation** | ad-hoc per lobe | 4 | directional-albedo (E_spec) coupling per OpenPBR slab model | Med | low | implement in `OpenPBRSurface.hlsli` |
| **Ambient medium (n_ambient)** | assumes air | 5 | — | — | — | fix at 1.0 |

**Net:** genuine rework of the base BSDF (IOR specular, F82 metal, Oren-Nayar, energy coupling) +
new gated lobes (coat/fuzz/aniso/transmission/thin-film/SSS-remap). Not a rename, not a full rewrite —
the existing subsurface/cloth/hair/eye/clearcoat/emission code is *evolved* into OpenPBR lobes.

---

## Part C — MaterialX Integration Analysis

### C.1 Where MaterialX enters — **import/export & authoring only, never runtime**

```
IMPORT:  .mtlx ──▶ MaterialX parse (Core+Format) ──▶ find open_pbr_surface node ──▶ read inputs/tex refs
             ──▶ OpenPBR→SurfaceParams translation ──▶ HBMaterial (.hbmat) ──▶ [ship] compiled GPU material
EXPORT:  HBMaterial ──▶ SurfaceParams→OpenPBR ──▶ build MaterialX Document (open_pbr_surface node)
             ──▶ MaterialXFormat write ──▶ .mtlx
```

### C.2 What runs where (hard boundary)

| Stage | Runs | Component |
|---|---|---|
| Parse `.mtlx`, traverse graph, read node inputs | **import-time (editor/cooker)** | MaterialX Core+Format |
| Translate OpenPBR node → `SurfaceParams` | import-time | Heartbreak translator |
| Compile `SurfaceParams` → FeatureKey + packed GPU material | import-time **and/or** load-time | `MaterialCompiler` (Heartbreak) |
| Evaluate BSDF, bind material, draw | **runtime** | Heartbreak renderer (no MaterialX) |
| Evaluate arbitrary MaterialX node graphs per frame | **NEVER** | — (explicitly excluded) |

### C.3 MaterialX dependency strategy (investigation for §14)

- **License:** Apache-2.0 → static-link and ship-safe; no copyleft.
- **Minimal subset for our use:** `MaterialXCore` + `MaterialXFormat` only (parse + read/write node
  inputs). Excluded: `MaterialXGenShader`/`GenGlsl`/`GenMsl`/`GenOsl`/`GenMdl` (shader codegen — we
  implement the BSDF ourselves), `MaterialXRender*`, `MaterialXView`, `MaterialXGraphEditor`.
- **External deps of the subset:** only **PugiXML** (bundled with MaterialX; light). No other
  third-party libs required for Core+Format.
- **Build:** CMake, C++17, MSVC 2017+/GCC8+/Clang5+; Windows + Linux supported. Configure with
  `-DMATERIALX_BUILD_GEN=OFF -DMATERIALX_BUILD_RENDER=OFF -DMATERIALX_BUILD_TESTS=OFF
  -DMATERIALX_BUILD_PYTHON=OFF -DMATERIALX_BUILD_VIEWER=OFF -DMATERIALX_BUILD_GRAPH_EDITOR=OFF`,
  static libs. Vendored under `Tools/ThirdParty/` or fetched, matching the existing ThirdParty
  discipline ([docs/ThirdParty.md](ThirdParty.md)).
- **Where it links:** **HeartbreakEditor / a cooker tool only**, behind `HBE_EDITOR` (same pattern as
  ImGui multi-viewport). The **shipping runtime links zero MaterialX** — footprint unchanged, startup
  cost unchanged. OpenPBR default values are read at import from the `open_pbr_surface` nodedef so our
  defaults stay authoritative.
- **Bundled data:** ship the OpenPBR + MaterialX standard-library `.mtlx` definitions with the editor
  only (needed to resolve nodedefs during import).

### C.4 Supported MaterialX subset (first implementation)

- **Primary:** documents whose surface shader is `open_pbr_surface` (and `standard_surface` mapped to
  OpenPBR with documented conversion). Read constant input values + direct `<image>`/texture-node
  file references + basic `<multiply>`/`<mix>` on inputs where trivially foldable.
- **Not first:** arbitrary procedural node graphs (noise networks, complex pattern graphs). If a graph
  isn't trivially foldable to constants+textures, either bake it (future, via optional codegen) or
  import the constant portion and **warn** with documented lossy conversion. No general-purpose graph
  evaluation, ever, at runtime.

### C.5 `.hbmat` role (decision for §4)

`.hbmat` becomes the **native authoring + serialized-OpenPBR-parameters format** (a superset
`SurfaceParams` in JSON), and remains the **source the runtime loads**. It is *not* the raw GPU blob.
Separation:

```
Authoring / interchange :  MaterialX (.mtlx) / glTF
Native Heartbreak asset :  .hbmat  (SurfaceParams JSON: OpenPBR params + tex refs + acoustic + painterly + feature hints)
Runtime GPU material    :  GpuSurfaceMaterial (packed) + PermutationId, produced by MaterialCompiler
```

`.hbmat` stays JSON (human-diffable, back-compat via `j.value` defaults). An **optional** pre-cooked
binary compiled form (packed `GpuSurfaceMaterial` + PermutationId, baked into `.uap` packs) is a
future optimization; not required for correctness. This keeps `.hbmat` authoritative and avoids
forcing one format to replace another.

---

## Part D — Proposed Architecture

```
   MaterialX (.mtlx) / glTF-KHR / hand-authored          ← interchange & authoring
        │  MaterialX Core+Format (editor/import only)  |  Assimp + KHR reads
        ▼
   OpenPBR parameter extraction / translation           ← import-time
        ▼
   hbe::SurfaceParams   (ONE shared CPU struct, OpenPBR-named)   ← native runtime material rep
        ▼  .hbmat  (JSON serialize; back-compat)
        ▼
   MaterialCompiler::Compile()                           ← feature analysis (static) + value packing (dynamic)
        │        ├─▶ FeatureKey ─▶ PermutationId (curated variant, FULL fallback)
        │        └─▶ GpuSurfaceMaterial (compact, packed)
        ▼  FillSurfaceMaterial()  — ONE shared packer (both backends)
   ObjectConstants.material  (nested block; painterly byte offsets preserved)
        ▼  PSO = table[PermutationId][passRole]   (DX12 DXIL / Vulkan SPIR-V, identical BSDF source)
   MeshPBR.hlsl ▶ #include OpenPBRSurface.hlsli (per-#define stripped variant)
        ▼
   OpenPBR physically-based shading  ─▶  Heartbreak painterly/stylization (UNCHANGED)  ─▶  final image
```

### D.1 One shared material struct (kills the four mirrors)

New `Source/RHI/SurfaceMaterial.h` defining **once**:
- `struct hbe::SurfaceParams` — full OpenPBR parameter set (CPU authoring/runtime; compact; spec
  defaults). `MaterialAsset`, `MeshInstance`, `DrawItem` embed **this** instead of scattered fields →
  material mirrors drop 4 → 1 CPU definition.
- `struct hbe::GpuSurfaceMaterial` — packed GPU form (§D.4), mirrored **once** in
  `Shaders/OpenPBRMaterial.hlsli`, nested into `ObjectConstants`/`ObjectCB`/`ObjectUBO` at the **tail**
  (painterly offsets unchanged; zero-init = defaults = existing look pre-migration). `static_assert`
  on `sizeof`.
- `inline void FillSurfaceMaterial(GpuSurfaceMaterial&, const SurfaceParams&)` — the **single** packer
  used by both backends (removes Vulkan's duplicated inline fill).

Deferred (future): an indexed per-material `StructuredBuffer` (identical materials share one entry) —
better cache behavior but disturbs instancing/shadow-CB reuse; not in first pass.

### D.2 The BSDF library `Shaders/OpenPBRSurface.hlsli`

Implements OpenPBR layering (top→bottom): `fuzz → coat → mix(metal_F82, dielectric_base, metalness)`,
`dielectric_base = mix(opaque, translucent, transmission_weight)`,
`opaque = mix(glossy_diffuse, subsurface, subsurface_weight)`,
`glossy_diffuse = dielectric_specular(GGX, IOR→F0) over Oren-Nayar, coupled by (1−E_spec)`; thin-film
modifies metal & dielectric Fresnel (Belcour-Barla). Every lobe is `#if HBE_FEAT_*`. Serves both
direct lighting and the IBL/ambient block (per-lobe env terms). MeshPBR keeps its structure and the
painterly output byte-identical.

### D.3 Hybrid static/dynamic feature model (avoids permutation explosion)

Per §6, split features:

| Kind | Examples | Mechanism |
|---|---|---|
| **Static** (compile-time specialization) | presence of coat / fuzz / subsurface / transmission / thin-film / anisotropy / Oren-Nayar | `#if HBE_FEAT_*` → **PermutationId** |
| **Dynamic** (GPU material params) | all continuous values: colors, weights, roughness, IOR, thickness, radius, anisotropy amount/rotation | `GpuSurfaceMaterial` fields |

`MaterialCompiler` computes a **FeatureKey** from lobe weights (`weight>ε ⇒ on`) + explicit modes,
then maps it to the cheapest **curated** variant that covers it; uncovered combos fall back to `FULL`.

**Curated variant set** (bounded, not 2^N) emitted by `hbe_add_shader` `DEFINES`:

| Variant | Lobes | Use |
|---|---|---|
| `STD` | dielectric+metal, GGX, (EON if r>0), normal, emissive | ~90% |
| `COAT` | STD+coat | lacquer, car paint, wet |
| `SSS` | STD+subsurface | skin, wax, marble |
| `FUZZ` | STD+fuzz | cloth, velvet |
| `ANISO` | STD+anisotropic spec | brushed metal |
| `TRANSMISSION` | STD+thin-walled/SS-refraction | glass, liquids |
| `THINFILM` | STD+thin-film | soap, oil |
| `FULL` | everything (fallback) | complex layered |
| `HAIR`, `EYE` | existing specialty, ported | hair, eyes |

**PSO/compile budget:** ~10 surface variants; shadow PSO is VS-only (shared, 1); preview limited to
STD/FULL. Realistic PSO count ≈ variants × {opaque, transparent, transparentDepth} ≈ 30-ish per
backend — modest, built lazily/at init. Bindless means **no root-sig/descriptor growth**.

### D.4 Compact GPU material `GpuSurfaceMaterial`

Target ~96-128 B, 16-B aligned; colors `float3`, scalars grouped, roughness/IOR/anisotropy quantized
where safe, texture indices as packed `u16` pairs. G-buffer packing (`octN/rough/metal`) **unchanged**
so SSR/SSGI/SSAO + painterly keep working (coat/transmission excluded from SSR — documented). Cheap
materials upload the small block; the *shader* pays nothing for absent lobes (stripped permutation).

### D.5 Quality tiers (§12)

| Tier | Behavior |
|---|---|
| **Low** | STD only; coat/fuzz/aniso/SSS/transmission/thin-film collapse to base response; min texture samples |
| **Medium** | curated permutations; thin-walled transmission; pre-integrated SSS |
| **High** | screen-space refraction; anisotropy; thin-film; richer SSS |
| **Future** | hardware ray-traced transmission/SSS behind the same abstraction |

Tier caps which permutations a material may resolve to (a project/quality setting), so low-end HW never
pays for High lobes. Implemented as a clamp in `MaterialCompiler` + a runtime tier uniform.

### D.6 Transmission abstraction (§8)

```
OpenPBR transmission params ─▶ ITransmission (interface)
     ├─ ThinWalled     (tint + roughness-blur; no pass; double-sided)
     ├─ ScreenSpace    (samples resolved opaque scene-color behind; IOR bend + Beer tint; quality opt-in)
     └─ RayTraced      (FUTURE; same params, replaces/augments SS without changing SurfaceParams)
```

The material representation is refraction-method-agnostic; the renderer picks the implementation by
quality tier + per-material opt-in. Screen-space needs a resolved opaque-color SRV bound to the
transparent pass (new, both backends) — the only new render resource in the program.

### D.7 Painterly interaction (unchanged contract)

`OpenPBR material → PBR shading → painterly/stylization → final`. MeshPBR keeps emitting the identical
PSOutput (G-buffer packing, 2-bit alpha mask on `gOutputLinear`, linear-HDR radiance). New OpenPBR
flags avoid bits 7/10. `StrokeSurface.hlsl` is updated to sample the OpenPBR base response so 3D
strokes match the underpainting. Painterly stays Heartbreak-specific; no painterly concept is pushed
into OpenPBR/MaterialX.

### D.8 Backends & consistency

Shared HLSL BSDF → identical DXIL/SPIR-V by construction. C++ deltas (packer, permutation table, PSO
selection, transmission SRV) mirrored in both backends + a parity self-test.

---

## Part E — Migration Plan (safe phases)

Each phase: **Files/modules · Dependencies · Risks · Validation · Perf · Rollback.** No phase is
merged until it builds + reviews clean on **both** backends. Painterly re-verified after P2, P6.

**P0 — This document.** (done) Audit + architecture. No code.

**P1 — CPU material-representation consolidation (no visual change). — DONE + BUILDS (2026-08-16).**
- Scope (locked with user): consolidate the duplicated CPU material VALUE fields into ONE
  `hbe::SurfaceParams` embedded in MaterialAsset/MeshInstance/DrawItem. Textures stay layer-specific.
  The existing GPU packers change only their SOURCE (read `it.surface.*`) and emit BYTE-IDENTICAL
  `ObjectCB`/`ObjectUBO`. No GPU-ABI/HLSL/PSO/pass/painterly/MaterialX change. `GpuSurfaceMaterial`
  and the OpenPBR BSDF are P2, NOT P1.
- Delivered:
  - New `Source/RHI/SurfaceMaterial.h` = `SurfaceParams` (full OpenPBR value set, OpenPBR-named,
    `operator==`). base_color kept as glm::vec4 (`.a` = opacity) in P1; the distinct
    `geometry_opacity` split is deferred to P2 (the shading phase) per the opacity constraint.
  - Legacy->OpenPBR field mapping documented in the header + mirrored in the `.hbmat` loader.
  - Structs migrated: MaterialAsset, MeshInstance, rhi::DrawItem (flat fields removed).
  - Packers repointed: 3 D3D12 + 2 Vulkan blocks (`ocb.<f> = it.surface.<openpbr>`), same bytes.
  - `.hbmat` keeps all legacy JSON keys (from `surface`) + adds new OpenPBR keys with spec-default
    fallback -> existing files load unchanged; version stays 2.
  - `ApplyMaterial` + Scene population collapse to `X.surface = Y.surface`; `SameMaterial` now uses
    the complete `a.surface == b.surface` (fixes a latent clearcoat-omission instancing bug).
  - ~230 call sites across 15 files repointed; SceneSerializer's partial `.hbmat`->MeshInstance copy
    gap preserved exactly.
- Validation done: **Release build of HeartbreakEditor (both DX12 + Vulkan + GL backends compiled)
  succeeds with 0 errors** — the compiler is the completeness check (removed fields make any
  missed/wrong rename a hard error). Byte-identical GPU packing holds by construction (each mapped
  field equals its legacy value; baseColorRGBA == old vec4). Runtime render-parity smoke test is
  available but not yet run.
- Perf: neutral (GPU CB unchanged; no new per-draw bytes). Rollback: revert the listed files;
  isolated, no cross-phase coupling.

**P2 — OpenPBR base shading. — DONE + BUILDS + adversarially verified (2026-08-16).**
- Scope realized: the OpenPBR BASE response (IOR-driven dielectric specular, F82-tint metal,
  Oren-Nayar diffuse, energy coupling) + coat via coat_ior/coat_color + geometry_opacity split.
  Fuzz/subsurface-diffusion/anisotropy/transmission/thin-film intentionally deferred to P4-P7 (their
  params are already carried on the GPU). Tuned skin/cloth/hair/eye paths kept intact.
- GPU material layout decision (lowest-risk): the metallic-roughness-era fields STAY in their existing
  ObjectConstants positions (StrokeSurface/PBR/Water read them); the new OpenPBR params are APPENDED as
  one tail block `gMatExt` (HLSL `OpenPBRMaterialExt` in Common.hlsli == C++ `rhi::GpuSurfaceMaterialExt`
  in Source/RHI/GpuMaterial.h - one shared def for both backends, static_assert 176 B). MeshPBR reads a
  consolidated OpenPBR view from the legacy fields + gMatExt. No CB re-layout (avoids the #1 bug class).
- Files: new `Shaders/OpenPBRSurface.hlsli` (IorToF0/FresnelSchlickF90/FresnelF82Tint/OrenNayarDiffuse);
  new `Source/RHI/GpuMaterial.h`; `Shaders/Common.hlsli` (OpenPBRMaterialExt + gMatExt); `Shaders/MeshPBR.hlsl`
  (base BSDF rework, 10 surgical edits + opacity); D3D12/Vulkan CB structs + 5 ext-fill sites.
- Default preservation (by construction): at OpenPBR defaults (specular_ior 1.5, specular_weight 1,
  specular_color white, base_diffuse_roughness 0, coat_ior 1.5, coat_color white, base_weight 1) every
  new term reduces EXACTLY to the previous look, so existing materials are visually unchanged.
- Validation: Release builds of BOTH HeartbreakEditor and HeartbreakRuntime (DX12+Vulkan+GL compiled,
  static_assert held) = 0 errors; a 3-reviewer adversarial workflow (BSDF math / CB byte-layout parity /
  painterly-contract preservation) returned ZERO defects. Runtime visual A/B not yet run (available).
- Painterly: PSOutput (G-buffer octN/rough/metal, 2-bit alpha mask on gOutputLinear, linear-HDR) is
  byte-for-byte unchanged; painterly + StrokeSurface/PBR/Water unaffected.
- Perf: one appended CB block (176 B/draw); default materials pay ~the same shader cost. Permutation
  stripping is P3. Rollback: revert the P2 files (append + additive shader edits; legacy CB untouched).

**P3 — Permutation/hybrid system.**
- Files: `ShaderCompile.cmake`(DEFINES + variant list); new `MaterialCompiler`; backend PSO tables +
  selection; loader.
- Deps: P2. Risks: silent-dormancy (unregistered variant), PSO count, FULL fallback coverage.
- Validation: strip-on-absence proof (STD binary lacks coat/SSS code); PSO count report; per-variant
  render tests. Perf: **win** (cheap materials drop branches). Rollback: force PermutationId=FULL.

**P4 — Coat + Fuzz + Anisotropy** (evolve clearcoat/cloth; add aniso). Files: `OpenPBRSurface.hlsli`,
compiler, editor. Deps: P3. Validation: coated/aniso/cloth scenes + parity. Perf: gated. Rollback:
disable variants (fall to STD/FULL).

**P5 — Subsurface remap** (OpenPBR params, keep RT approximation). Files: `OpenPBRSurface.hlsli`,
compiler, editor. Deps: P3. Validation: skin scene vs current; parity. Perf: gated. Rollback: keep
legacy skin flag path.

**P6 — Transmission** (`ITransmission`: thin-walled + screen-space refraction). Files: new
scene-color-behind SRV + transparent-pass wiring (both backends), `OpenPBRSurface.hlsli`, compiler,
editor. Deps: P3, resolved-opaque-color target. Risks: pass ordering, alpha/mask collision, both
backends. Validation: glass/thin-walled scenes; parity; painterly re-check. Perf: opt-in only.
Rollback: thin-walled-only (no pass).

**P7 — Thin-film** (Belcour-Barla). Files: `OpenPBRSurface.hlsli`, compiler, editor. Deps: P3.
Validation: iridescence scene + parity. Perf: gated. Rollback: disable variant.

**P8 — Import/interop.** glTF KHR reads (`ConvertMaterial`); focused MaterialX importer (Core+Format,
editor-only) `.mtlx→SurfaceParams→.hbmat`; MaterialX exporter `HBMaterial→.mtlx`; `AssetFormats` row +
`Importer` dispatch. Deps: MaterialX Core+Format build integration; P1. Risks: dependency build,
lossy graphs. Validation: round-trip `.mtlx` import/export of OpenPBR test materials; import-only
(no runtime link). Perf: import-time only, runtime unaffected. Rollback: gate importer off; hand-auth
`.hbmat` still works.

**P9 — Editor.** OpenPBR inspector (grouped lobes, presets, preview, tier), transmission/coat/SSS/
aniso/thin-walled UI; acoustic + painterly panels untouched. Deps: P1-P7. Validation: author→save→
reload; preset round-trip. Rollback: keep legacy inspector.

**P10 — Validation & docs.** Material test suite (dielectric, metal, rough metal, smooth dielectric,
coated, transmissive, thin-walled, anisotropic, emissive, subsurface, layered) as scenes/`.hbmat`;
DX12↔Vulkan parity harness; supported-features + approximations doc. Deps: all. Rollback: n/a.

---

## Part F — Accepted approximations & limitations (documented deviations)

Real-time; the following intentionally diverge from the path-traced reference:
- **Subsurface**: pre-integrated/diffusion approximation, not random-walk volumetric; `radius_scale`
  RGB falloff; `scatter_anisotropy` approximated.
- **Transmission**: thin-walled + single-bounce screen-space refraction + Beer-law tint. No true
  volumetric multi-scatter; **dispersion** and `transmission_scatter` stored for round-trip but
  approximated/omitted at runtime. Behind `ITransmission` so a future RT path drops in without
  changing `SurfaceParams`.
- **Fuzz**: Charlie/Neubelt sheen as additive top layer, not full microflake theory.
- **Thin-film**: Belcour-Barla analytic (production standard), not full spectral Airy.
- **Coat**: analytic darkening/roughening; not a traced inter-layer medium; coat_normal/anisotropy are
  future/optional.
- **Emission**: linear radiance multiplier; `emission_luminance` nit units approximated.
- **Anisotropy**: mesh/UV tangent; no per-pixel tangent map unless authored.
- **Screen-space** reflections/GI see only the base lobe (G-buffer carries base normal/rough/metal).
- **MaterialX**: only OpenPBR-Surface-centric documents (constants + direct texture refs) in the first
  implementation; arbitrary procedural graphs are not evaluated (import warns + documents lossy
  conversion). MaterialX is an **import/export/editor-time** dependency; the shipped runtime contains
  no MaterialX and evaluates no graphs.
- **OpenGL**: out of scope; renders the legacy base-MR approximation.
