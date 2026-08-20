# Rendering & VFX Overhaul — running record

Audit-driven overhaul of four systems. Each was audited against the actual codebase first (rule:
*inspect before changing*). The recurring finding: all four are architecturally sound; the leverage
is in specific additive gaps, not rewrites.

## Objective A — Particles / VFX

### Decision: keep the native module-stack system; do NOT integrate Effekseer

The brief named Effekseer. The audit found:

- **Effekseer is not vendored** anywhere in the tree (no submodule, no FetchContent, no `third_party`).
  Integrating it means importing a large external rendering library and writing a native-RHI renderer
  backend for it (DX12 **and** Vulkan) — thousands of lines, high risk.
- Heartbreak already has a **genuinely strong native particle system**: a Niagara-style module stack
  (`Source/Vfx`) with three tiers (CPU sim, CPU-sim + GPU expansion, full GPU-sim compute
  interpreter), a fixed 64-byte record, deterministic RNG, bit-exact backward compatibility, and both
  backends. It is painterly-compatible and tuned for low-end hardware — Heartbreak's identity.
- Effekseer would **fight** that identity (its own renderer assumptions, its own asset ecosystem) and
  **duplicate** a working system — exactly what the brief's own rules forbid ("do not create duplicate
  systems when an existing abstraction can be extended; do not introduce unnecessary dependencies; if a
  proposed improvement would require an enormous unrelated rewrite, document it and continue with the
  highest-value improvement that can be safely implemented").

The brief's actual **goal** for Objective A is the artist/gameplay workflow — *"artist creates an
effect → previews → saves → cooks → game loads"* and *"gameplay says `SpawnEffect("Explosion",
transform)`"*. The audit found the one real gap blocking that goal: **there was no effect asset.** An
effect could only live as fields on a scene entity — it could not be authored once, saved, shared,
previewed, or spawned by name. So the highest-value, identity-preserving move is to build that asset
on the existing system, which is what was done.

### Built (`--test-hbvfx` PASS; editor + runtime clean)

- **`.hbvfx` effect asset** (`Source/Scene/EffectAsset.{h,cpp}`, `EffectAssetJson.h`): the authored
  ParticleEmitter fields as a standalone versioned JSON asset. The field (de)serializer
  (`EmitterToJson`/`EmitterFromJson`) is the **single source of truth** — the scene serializer now
  delegates to it, so the scene format and the effect-asset format can never drift and every
  backward-compat default lives in one place. Registered in `AssetFormats` (`JsonScan`, so the sprite
  `.uaf` seeds the pack closure) → cooks/packs like any JSON asset.
- **`game::SpawnEffect(name, transform)`** (+ a `vec3` position overload): a deferred queue on the
  `game::` API (same producer/consumer pattern as `PlaySequence`/`PlayDialogue`). Gameplay/schematics
  enqueue; `spawn::Update` drains it, instantiates a `ParticleEmitter` entity from the `.hbvfx`
  (cached load), and **auto-despawns one-shot (non-looping) effects** when finished via a runtime-only
  `EffectLifetime` component. `spawn::SpawnEffect(scene, name, transform)` is the immediate form.
- **Editor**: the Particle Emitter inspector gains *Save .hbvfx* / *Load (.hbvfx)* — author an emitter,
  save it as a reusable effect, or drop one over another emitter (carrying the live pool/RNG so it
  doesn't thrash).
- Backward compat verified: `--test-vfxstack` / `--test-vfxcompat` still pass (the serializer refactor
  preserved every key + default), `--test-savedispatch` / `--test-assetformats` green.

### Deferred (documented, not blocking the goal)
- A schematic "Spawn Effect" node (visual-scripting parity with the C++ API) — the catalog/enum
  lockstep is mechanical; add when the schematic pass runs.
- A standalone effect *preview* window (the inspector already previews live on the entity).

## Objective B — Material layering  *(Layer Stack panel built; editor compiles, backend tested)*

Audit verdict: architecturally excellent, no rewrite; the layering **backend** (`mat::LayerStack`,
`MaskSource`, `mat::Resolve`, `BakeLayerStack`, `.hbmatlayer` serialization) was complete and tested
but **under-exposed** — there was no reorderable layer panel. Built:

- **`Window ▸ Material Layers`** (`Source/Editor/MaterialLayerEditor.cpp`, own TU) — a Photoshop-style
  stack editor over `.hbmatlayer`: a base material plus a top-first layer list with **add / remove /
  reorder (Up/Dn) / opacity**, a **mask** dropdown (Constant weight slider / Box / Procedural / Paint
  + invert), a **blend** dropdown (Linear / Height / Height+Noise) with height, per-layer
  normal/height-contribution toggles, and each layer's surface authored **inline** (colour /
  roughness / metallic) or picked from a **`.hbmat`**. A **live preview swatch** bakes the stack
  through the exact runtime resolver (`mat::BakeLayerStack` → `mat::Resolve`), hash-gated so it uploads
  once per edit. **Presets** (Rock+Moss, Snow-on-Rock, Sand+Puddle) give an instant editable start.
  New / Open / Save write real `.hbmatlayer` assets via the tested `SaveLayerStack`/`LoadLayerStack`.
- Lean by design: explicit New/Open/Save buttons instead of hooking the Ctrl+S document-dispatch
  system, so it needed no save-dispatch lockstep — only the panel enum + Window-menu entry + one
  BuildUI dispatch. Backend correctness stays covered by `--test-material` (layer serialize + resolve).

Deferred (documented): drag-to-reorder (Up/Dn buttons ship now), per-layer thumbnails, and the
BAKED→LIVE per-pixel GPU path (wire `MaterialLayered.hlsli` into `MeshPBR.hlsl` behind
`HBE_MAT_LAYERED` — the recipe exists but it touches the hot path and wants GPU verification).

## Objective C — Decals  *(C1 built; both backends compile, runtime clean, regression green)*

Audit: solid MVP; leverage in material-channel control, projection quality, and placement. Built (all
using the previously-**unused** `DecalData.flags` field + the free `params.w`, so **no GPU struct-size
change** = no byte-layout hazard):

- **Channel-enable decals** — `flags` bits gate which surface channels a decal writes: base colour /
  normal / roughness-metallic independently. Authors can now make a normal-only detail decal, a
  colour-only stain, or an MR-only wet patch instead of always overriding everything.
- **Two-sided projection** (`flags` bit 3) — project onto back faces too.
- **Hard projection cone** (`params.w` = cos(maxAngle)) — reject surfaces turned more than *maxAngle*
  from the projector, killing the smear a box decal leaves as it wraps a corner / onto steep faces.
- **Z soft-fade** — the shader edge fade now also fades along the projection-depth axis, not just XY.
- **Editor**: inspector channel checkboxes + two-sided + max-angle; a purple projector-box wireframe
  with a yellow **+Z projection-direction line** so orientation is obvious. Serialized (defaults =
  legacy behaviour, so old scenes are unchanged).

- **Emissive decals** (C2) — an **append-only** `DecalData`/HLSL-`Decal` growth (`float4 emissive`,
  keeping the 16-byte cbuffer alignment; both are memcpy'd so a tail append is byte-safe), gated by
  `flags` bit 4 (off by default → existing content unchanged). MeshPBR accumulates a coverage-shaped
  glow and adds it to the surface emission, so glowing signs / runes / embers follow the decal's
  alpha. Inspector: an Emissive toggle + colour + intensity. MeshPBR recompiles clean to DXIL+SPIR-V.
- **Drop-on-surface placement** (the audit's "biggest decal UX gap") — a *Decal Drop Tool* (Create
  menu toggle): click any surface and a decal is created there, auto-oriented so its projection +Z
  points **into** the surface (basis built from the raycast normal). Reuses the paint tools' mesh
  raycast + the world-tool click-suppression pattern.
- **Reusable `.hbdecal` asset / decal library** (the brief's "save it as a reusable asset") —
  `Source/Scene/DecalAsset.{h,cpp}` + `DecalAssetJson.h`, mirroring `.hbvfx`: the authored
  DecalComponent fields as a standalone versioned JSON asset; `DecalToJson`/`DecalFromJson` are the
  single source of truth the scene serializer now delegates to. Registered in AssetFormats (JsonScan,
  so its albedo/normal/mr `.uaf` textures seed the pack closure). Inspector Save/Load. `--test-hbdecal`
  PASS.

Deferred (documented): **clustered/culled decal binning** to replace the O(pixels×16) global loop +
16 cap (a bigger perf project, lower priority than the authoring wins above).

## Objective D — Volumetrics  *(empty-space skip built; both backends compile clean)*

Audit: architecture good, raymarch feature-complete for a stylized look; the leverage is **throughput
on weak hardware**. Built:

- **Energy-correct empty-space skipping** in `VolumeRaymarch.hlsl` — smoke fills a tiny fraction of
  its AABB, so the uniform march wasted most steps sampling background. The march now grows the step
  (up to 3×) while it samples empty voxels and snaps back to the base step the instant density
  appears. This is exact in empty regions (`d==0 → exp(-0·step)==1`, so a bigger step changes nothing)
  and only risks under-sampling the leading edge of a feature thinner than ~3 steps, which the 3× cap
  bounds. The iteration budget is unchanged, so a dense volume behaves as before. Compiles clean to
  DXIL + SPIR-V.

Deferred (documented, needs on-GPU visual validation — there is no headless raymarch test): **full
NanoVDB HDDA tile-leaping** (zero-fidelity-loss skipping via the tree topology), **CPU adaptive step
COUNT** scaled to AABB size + quality levels (changes existing volumes' step count, so it wants a
visual A/B), and **temporal accumulation/reprojection**. The empty-space skip already delivers the
dominant win (a large mostly-empty fog box is now cheap) without changing the look.

## Renderer integration
Audit: no rewrite. Three additive shared seams — a unified HDR-forward injection list (kills the
particle/grass/decal setter sprawl), a reusable half-res volumetric march+composite (de-dups fog vs
NanoVDB volume), and a shared blue-noise/temporal-index + G-buffer fetch helper.
