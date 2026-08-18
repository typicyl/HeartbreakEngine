# OpenPBR + MaterialX — Supported Features

This is the reference for **what the Heartbreak OpenPBR material model actually implements**, where it
approximates, and what it does not (yet) do. It is the contract the material editor, the `.hbmat`
format, and the MaterialX interchange all target. See `docs/Design-MaterialX-OpenPBR.md` for the
architecture and the phase history, and `Source/RHI/SurfaceMaterial.h` for the parameter set itself.

Roles, unchanged from the design:

- **OpenPBR Surface v1.1.1** is the primary surface/material model (the parameter set + the BSDF).
- **`.hbmat`** is the native authoring + serialised-parameter format (JSON).
- **`GpuSurfaceMaterialExt`** (176 B, `static_assert`-locked) is the compiled runtime GPU material.
- **MaterialX** is the interchange layer only — parsed at editor/import time, never a runtime graph.
- **Painterly** rendering is untouched and strictly downstream (the MeshPBR G-buffer/PSOutput contract
  is preserved byte-for-byte; see the design doc's "crown-jewel seam").

## Shading feature matrix

Legend: **Full** = implemented per the OpenPBR model; **Approx** = implemented with a documented
real-time approximation; **—** = not evaluated (parameter carried but inert), planned or out of scope.

| OpenPBR lobe / parameter | Status | Notes |
|---|---|---|
| `base_weight`, `base_color`, `base_metalness` | Full | metalness blends dielectric↔conductor response |
| `base_diffuse_roughness` | Full | energy-preserving Oren–Nayar; 0 = Lambert (default) |
| `specular_weight`, `specular_color` | Full | tints/scales the dielectric specular |
| `specular_roughness` | Full | GGX / Trowbridge–Reitz |
| `specular_ior` | Full | IOR→F0 (`IorToF0`); 1.5 → 0.04 (default) |
| metal Fresnel (`specular_color` edge tint) | Full | F82-tint conductor Fresnel (Kutz 2021); white = Schlick |
| `specular_roughness_anisotropy`, `specular_anisotropy_rotation` | Approx | anisotropic GGX NDF; **isotropic Smith G** (common real-time simplification). Routes to the FULL variant |
| `coat_weight`, `coat_roughness` | Full | second GGX dielectric lobe (the "Clearcoat" control) |
| `coat_ior`, `coat_color`, `coat_affect_color`, `coat_affect_roughness` | Full | IOR-driven coat F0; darkens/roughens the base beneath |
| `fuzz_weight`, `fuzz_color`, `fuzz_roughness` | Approx | additive Charlie/Neubelt sheen layered on top; ~0.15 energy heuristic for the base dimming |
| `subsurface_weight`, `subsurface_color`, `subsurface_radius(_scale)`, `subsurface_scatter_anisotropy` | Approx | **pre-integrated** wrapped diffusion (per-channel MFP + back-scatter), NOT random-walk |
| `transmission_weight`, `transmission_color` | Full | thin-walled + IOR-refracted; screen-space refraction (see below) |
| `thin_walled` | Full | thin surface vs. volume path selector |
| `transmission_depth` | Approx | attenuation tint for non-thin-walled glass; no true volumetric integration |
| `transmission_scatter`, `transmission_dispersion_*` (abbe) | — | carried in `.hbmat`, not evaluated |
| `thin_film_weight`, `thin_film_thickness`, `thin_film_ior` | Full | Belcour–Barla 2017 iridescence (2 orders, m=1,2). Base specular only (not the coat lobe); IBL evaluated at N·V. Thickness authored in **micrometres** |
| `emission_color`, `emission_luminance` | Full | linear HDR emitter |
| `geometry_opacity` | Approx | alpha comes from `base_color.a`; a separate geometry-opacity split is deferred |

### Transmission / refraction (P6a + P6b)

- **P6a** — the transmitted background samples the prefiltered **IBL environment** in the refracted
  direction (Fresnel-weighted, TIR-guarded), tinted by `transmission_color`. Always available.
- **P6b** — when the scene contains a transmissive material, the resolved opaque+sky HDR is copied to
  a `sceneColorCopy_` texture and the transparent pass samples the **actual geometry behind** the
  surface (true screen-space refraction). The frame constant `sceneColorIndex` selects copy-vs-IBL, so
  non-transmissive scenes are byte-identical to before and pay nothing.
- A material only refracts when it is flagged **Transparent** (alpha-blended pass) **and**
  `transmission_weight > 0`. The editor's OpenPBR panel offers a one-click "Make Transparent" when the
  flag is missing.
- Future: a ray-traced transmission path can drop in behind the same seam (`ITransmission`).

## Shader specialization (no permutation explosion)

Materials are routed to the **cheapest curated pixel-shader variant** that covers their active lobes
(`Source/RHI/MaterialCompiler.h`): `Std, Coat, Sss, Fuzz, Hair, Eye, Full`. Anisotropy, transmission
and thin-film route to `Full` (the correctness anchor, also used by the transparent/preview passes).
This is a small hand-picked table, not a 2^N matrix; the value-driven parameters live in the GPU
material, the feature-presence drives the variant. A material whose lobe combination has no dedicated
variant always falls back to `Full`, so a lobe is never silently dropped.

## Interchange

### `.hbmat` ↔ `SurfaceParams`
Full, lossless, JSON. Old files load unchanged (legacy metallic-roughness keys preserved; new OpenPBR
keys fall back to spec defaults via `j.value(...)`).

### glTF import (KHR extensions → OpenPBR)
Read at import through Assimp (`Source/Assets/ModelLoader.cpp` → `Source/Editor/Importer.cpp`):

| glTF extension | → OpenPBR |
|---|---|
| `KHR_materials_ior` | `specular_ior` (only when > 1, so an OBJ's `Ni=1` can't zero the specular) |
| `KHR_materials_transmission` | `transmission_weight` (+ auto-flags the material Transparent) |
| `KHR_materials_specular` | `specular_weight` |
| `KHR_materials_clearcoat` | `coat_weight`, `coat_roughness` |
| `KHR_materials_anisotropy` | `specular_roughness_anisotropy` |
| `KHR_materials_sheen` | `fuzz_weight`/`fuzz_color`/`fuzz_roughness` |
| `KHR_materials_volume` | `thin_walled = false` + `transmission_color`/`transmission_depth` |
| `KHR_materials_emissive_strength` | folded into `emission_*` |

`KHR_materials_iridescence` is not surfaced by the bundled Assimp, so thin-film stays default on import
(author it in the editor). Absent extensions leave OpenPBR defaults untouched — plain metallic-roughness
and non-glTF (OBJ/FBX) imports are unchanged.

### MaterialX `.mtlx` ↔ `.hbmat`  (editor only)
`Source/Assets/MaterialXInterop.cpp`, linked into `hbe_editor` only (MaterialXCore + MaterialXFormat).
The shipped runtime links **zero** MaterialX. Built when `HBE_ENABLE_MATERIALX=ON` (default); when off,
the editor's `.mtlx` buttons disable and report why.

- **Import**: reads the first surface-shader node. `open_pbr_surface` maps 1:1 (both use the OpenPBR
  names); `standard_surface` is mapped as an approximate fallback. Value inputs are read directly; a
  base-colour / emissive input connected to an `<image>` contributes a source texture path. Normal /
  metal-rough wiring through intermediate MaterialX nodes is left to the `.hbmat` authoring pass.
- **Export**: writes one `open_pbr_surface` node (its inputs from `SurfaceParams`) wired to a
  `surfacematerial`.

## DX12 ↔ Vulkan parity

The two backends are behaviourally identical by construction:

- **One** GPU material struct (`rhi::GpuSurfaceMaterialExt`, 176 B, `static_assert`) is filled by
  `FillSurfaceMaterialExt` and packed the same way into both `ObjectCB` (D3D12) and `ObjectUBO`
  (Vulkan); every `glm::vec3` sits at a 16-byte row start.
- The same 7 MeshPBR PS variants are compiled for both (DXIL + SPIR-V from one HLSL via `-D HBE_FEAT_*`),
  selected per draw by the same `ShaderVariant` enum.
- `sceneColorIndex` occupies the same appended 16-byte row of the frame constants on both backends and
  matches the HLSL `FrameConstants` layout.
- **P6b refraction copy**: D3D12 uses `CopyResource` with `RENDER_TARGET`↔`COPY_SOURCE` /
  `PIXEL_SHADER_RESOURCE`↔`COPY_DEST` barriers; Vulkan ends the main HDR render pass, does
  `vkCmdCopyImage` with explicit layout barriers, and reopens via a load render pass with writable
  depth. The Vulkan copy required `TRANSFER_SRC/DST` image-usage bits (added to the two targets only) —
  a class of defect that compiles clean and only surfaces under the validation layer / on-GPU, caught by
  the adversarial review.

OpenGL is out of scope for this program (it keeps the base metallic-roughness path).

## Known approximations (summary)

Anisotropic Smith G is isotropic; fuzz base-dimming is a heuristic; subsurface is pre-integrated (not
random-walk); transmission depth is a tint (no volumetric integration), and dispersion/scatter are
carried but inert; thin-film uses 2 interference orders on the base specular only and evaluates IBL at
N·V. All are standard real-time trades and are **default-preserving**: at OpenPBR defaults every added
term reduces to the pre-OpenPBR look, so unauthored materials are unchanged.
