# Heartbreak Engine

A 3D game engine built on a clean, backend-agnostic **RHI (Render Hardware
Interface)**. The same renderer drives both **Direct3D 12** and **Vulkan**
today, with room to add more backends (Metal, D3D12 work-graphs, console APIs)
behind the same interface.

> Status: **a real game engine.** **Both** the D3D12 and Vulkan backends
> render depth-tested, **textured** Cook-Torrance PBR with **image-based
> lighting**, a **skybox**, **directional shadow maps (PCF)** and a
> **skin/subsurface** option, **punctual point/spot lights + emissive
> materials**, driven by an **EnTT** ECS with a **transform
> hierarchy** (parent/child, drag-drop reparenting in the editor) and
> **Jolt rigid-body physics** (fixed-step, editor "Simulate" toggle). A
> **platform input layer** (keyboard/mouse + XInput gamepad) feeds a shared
> fly-camera in both the editor and the runtime. The **Unity-style Dear ImGui +
> ImGuizmo editor** renders the scene to an offscreen target in a dockable
> Viewport panel (Hierarchy tree / Inspector / Stats / Assets around it).
> Everything runs identically on both backends. `--model <file>` loads a
> glTF/GLB/OBJ/FBX scene via Assimp. Shaders compile to DXIL **and** SPIR-V at
> build time.

---

## Architecture

```
Source/
  Core/        Platform-agnostic primitives
    Types.h            fixed-width aliases, NonCopyable
    Log.{h,cpp}        leveled logging (std::format)
    Window.h           platform window interface
    Window_Win32.cpp   Win32 implementation (feeds the input sink)
    Input.h            keyboard/mouse/gamepad state (engine input seam)
    Input_Win32.cpp    Win32 VK translation + XInput gamepads
  Assets/      CPU-side content
    Mesh.h             GLM Vertex, Material, MeshData, Model
    MeshGenerator.*    procedural cube / UV sphere
    ModelLoader.*      Assimp import (glTF/GLB/OBJ/FBX)
  Scene/       ECS scene graph (EnTT)
    Components.h       Transform, Parent, MeshInstance, RigidBody, Name, ...
    Scene.{h,cpp}      registry + environment; world matrices; draw/view extraction
  Physics/     Rigid-body simulation
    PhysicsWorld.*     Jolt behind a plain API (fixed step, transform sync)
  Editor/      Dear ImGui + ImGuizmo editor
    Editor.{h,cpp}     hierarchy tree (drag-drop reparent), inspector, gizmo
  RHI/         Render Hardware Interface (the backend seam)
    RHI.h              IRenderDevice, Format, SceneView, DrawItem, MeshHandle
    RHIFactory.{h,cpp} backend selection / device creation
    D3D12/D3D12Device.cpp   Direct3D 12 backend (device + geometry)
    Vulkan/VulkanDevice.cpp Vulkan backend (device + present)
  Renderer/    High-level rendering
    Camera.h           GLM perspective fly-camera (RH, [0,1] depth)
    CameraController.* input-driven fly camera (editor + runtime + gamepad)
    Renderer.{h,cpp}   owns the RHI device, shadow + scene + sky passes
    IBL.{h,cpp}        CPU-precomputed IBL + sky environment
  Engine/
    Engine.{h,cpp}     window + input + physics + renderer + main loop
  main_editor.cpp / main_runtime.cpp   the two entry points

Shaders/       Authored HLSL (compiled to DXIL + SPIR-V via DXC at build time)
  Common.hlsli       constant buffers, bindless table, shadows (PCF), ACES
  BRDF.hlsli         Cook-Torrance: GGX, Smith, Fresnel-Schlick
  MeshPBR.hlsl       textured metallic-roughness PBR + IBL + shadows
  Sky.hlsl           fullscreen environment background pass
  PBR.hlsl           full textured metallic-roughness PBR (for the textured path)

cmake/         Dependencies.cmake (GLM, Assimp, EnTT, Jolt, ...), ShaderCompile.cmake
```

The renderer talks **only** to `hbe::rhi::IRenderDevice`. Each backend is a
private implementation selected at runtime through `rhi::CreateRenderDevice`.
Math is GLM; models load through Assimp. Dependencies are fetched automatically
by CMake (FetchContent) on first configure — no manual install beyond the SDKs.

## Requirements

- Windows 10/11, x64
- CMake ≥ 3.21
- A C++20 toolchain (MSVC / Visual Studio 2022 recommended)
- [Vulkan SDK](https://vulkan.lunarg.com/) (sets the `VULKAN_SDK` env var) —
  only needed when the Vulkan backend is enabled
- Direct3D 12 ships with the Windows SDK; no extra install

## Editor vs. runtime

The build produces **two** executables so the editor never ships in the game:

- **`HeartbreakEditor`** — runtime **+** the editor (Dear ImGui, viewport, gizmo,
  asset browser). A tool. Links `hbe_editor` (compiled with `HBE_EDITOR=1`).
- **`HeartbreakRuntime`** — the game runtime only: **no** editor, **no** ImGui
  (0 ImGui symbols in the binary). Links `hbe` (`HBE_EDITOR=0`).

A **project** is a folder with a `.hbproj` file and an `Assets/` directory. The
editor opens with a **Project Manager** (recent projects, create new, open
existing — or pass `--project path/to/file.hbproj`). Imported content (images,
models, audio) is converted to the **`.uaf` Unified Asset Format** (a binary
container, like Unreal's `.uasset`) and browsed in an **icon grid with folders**
(thumbnails for textures, drag-drop to organize); the runtime loads `.uaf`
files, never raw source files. Entities are edited as ECS components in the
Inspector: add/remove **Transform, Mesh, Rigid Body, Directional Light**, with
the scene's sun itself being an entity.

## Building

```powershell
# Configure (Visual Studio generator)
cmake -S . -B out/build/x64 -G "Visual Studio 17 2022" -A x64

# Build (produces HeartbreakEditor.exe and HeartbreakRuntime.exe)
cmake --build out/build/x64 --config Debug
```

Backends can be toggled at configure time:

```powershell
cmake -S . -B out/build/x64 -DHBE_ENABLE_VULKAN=OFF   # D3D12 only
cmake -S . -B out/build/x64 -DHBE_ENABLE_D3D12=OFF     # Vulkan only
```

## Running

```powershell
HeartbreakEditor.exe              # editor, Direct3D 12 (default)
HeartbreakEditor.exe --vulkan     # editor on Vulkan
HeartbreakRuntime.exe             # game runtime (no editor)
HeartbreakRuntime.exe --vulkan --width 1920 --height 1080
```

On **both backends** you should see a 3D grid of PBR spheres (metallic varies
vertically, roughness horizontally) over a ground plane, lit by the procedural
sky with directional shadows. In the **runtime**, physics runs immediately —
the spheres drop, bounce, and roll; fly with RMB + WASD/QE (Shift = fast) or a
gamepad (sticks + triggers). In the **editor**, select entities in the
**Hierarchy** tree (drag-drop to reparent), manipulate them with the gizmo,
edit materials in the **Inspector**, and tick **"Simulate physics"** in the
Stats panel to run the simulation. Pass `--model path/to/scene.glb` to load a
model instead. Press `Esc` to quit.

## Roadmap

Done so far: RHI seam; **both** backends present and render geometry; GLM +
Assimp + EnTT + ImGui; build-time DXC shader compilation; CPU asset pipeline;
analytic PBR mesh path; ECS scene graph with Assimp scene loading; ImGui +
ImGuizmo editor; **unified bindless texturing** (global texture array indexed by
int — D3D12 unbounded SRV table + Vulkan `VK_EXT_descriptor_indexing`); **IBL**
(CPU-precomputed irradiance / GGX-prefiltered specular / split-sum BRDF LUT as
bindless equirect maps — no cubemaps/render-targets needed).

## Concurrency & streaming

**Fiber job system** (`Core/JobSystem`, after Naughty Dog's "Parallelizing the
Naughty Dog Engine Using Fibers"). Worker threads — one per logical core by
default — are converted to fibers and run a scheduler loop; jobs execute on a
pool of 128 pre-allocated fibers. The key property is that a job can **wait on a
counter mid-execution without blocking its worker thread**: the fiber is parked,
the worker picks up other work, and the parked fiber is later resumed — possibly
on a *different* thread — once the counter is satisfied. That makes fine-grained,
deeply-nested parallelism cheap (a wait is a fiber switch, not a thread block).

```cpp
jobs::ParallelFor(count, group, [&](u32 begin, u32 end) { /* work */ });   // blocks until done
jobs::Counter* c = jobs::Kick(decls, n); jobs::Wait(c);                     // batch + wait
jobs::RunDetached(fn, arg);                                                 // fire-and-forget
```

A startup self-test exercises the whole machinery (a flat parallel-for plus jobs
that themselves wait on nested parallel-fors — the park/resume path) and logs the
result. **Skeletal animation** now poses every character across the workers.

**World streaming** (`Scene/StreamingWorld`) — distance-based world partition. A
world is a set of cells, each backed by a `.hbscene`; as a focus point (the
camera) moves, cells within `loadRadius` are loaded and cells past `unloadRadius`
are unloaded (the gap is hysteresis, so cells don't thrash at the boundary).
Loads are **asynchronous on the job system** — parse + asset IO run on workers;
only the bounded GPU instantiate of a finished load touches the main thread, so
streaming never stalls the frame. Cells are described by a `.hbworld` JSON
manifest (`--world path.hbworld`); `--worldtest` runs a built-in smoke test that
sweeps the focus across a line of cells. The scene serializer's asset caches are
now lock-protected so worker-thread staging and main-thread instantiation can run
concurrently.

## Navigation

**Navmesh + pathfinding** (`Source/Navigation/NavMesh`, built on **Recast &
Detour**). `NavMesh::Build` bakes a Detour navmesh from a world-space triangle
soup through the Recast pipeline (voxelize → filter → regions → contours → poly +
detail mesh); `FindPath` answers world-space queries (nearest-poly snap →
`findPath` → `findStraightPath`), with `NearestPoint` and `DebugTriangles`
alongside. `nav::BuildFromScene` gathers walkable geometry straight from the
scene, rebuilding CPU triangles from each mesh entity's `MeshRef` provenance
("prim:\*" / "uaf:rel#n"). The Recast/Detour types stay inside the `.cpp`
(opaque handles in the header) — the same seam used for physics and audio.
**Components.** `NavigationAgent` pathfinds to a `target` and `nav::UpdateAgents`
steers the entity's Transform along the path each frame (seek + arrival, snapped
onto the mesh, facing its motion) while softly avoiding `NavigationObstacle`s and
other agents; obstacles are dynamic local-avoidance volumes. Both are editable in
the Inspector (Add Component → Navigation Agent / Obstacle), saved with the
scene, and tick while the simulation runs (play mode / runtime), like physics.

The editor's **Navigation** panel bakes the scene's navmesh with live agent
settings, overlays it in the viewport, and tests paths (start/end from the
selection); the **Streaming** panel loads/inspects a `.hbworld`. `--navtest`
headlessly bakes a floor + obstacle, confirms a path routes around it, *and*
walks a `NavigationAgent` from corner to corner. Next: DetourCrowd-quality local
avoidance + spatial hashing for big crowds, navmesh carving (tile cache),
off-mesh links, and a C# pathfinding API.

## Toward photorealism

What the renderer has today (verified in the New Sponza scene at ~120 FPS):
textured Cook-Torrance metallic-roughness PBR (albedo / normal / metal-rough /
AO), CPU-precomputed IBL (irradiance + GGX-prefiltered specular + split-sum
BRDF LUT), a sky background, a subsurface approximation, punctual lights and
emissive materials — and, on **both backends**, a full **HDR render-pass
pipeline**:

- **HDR scene pass** — geometry + sky render linear radiance into RGBA16F with
  a sampleable depth buffer; tonemapping happens in post, not in the mesh shader
- **Cascaded shadow maps** — 4 camera-fit cascades (practical split scheme,
  texel-snapped, scene-bounds caster clamping) in a 4096² atlas, 3x3 PCF
- **SSR** — screen-space reflections composited into the HDR before bloom:
  depth-reconstructed normals + a binary-refined world-space ray march, with
  Fresnel and screen-edge fade (`--ssr`; uniform reflectivity without a G-buffer)
- **SSAO** — half-res depth-reconstructed hemispheric AO + box blur
- **Bloom** — soft-knee bright pass into a 6-mip 13-tap down / tent-up pyramid
- **Tonemap composite** — exposure (manual, plus optional **auto-exposure**: a
  256-tap log-luminance average with temporal eye adaptation, `--autoexposure`),
  ACES, AO/bloom mix, vignette, saturation/contrast grade (per-scene
  `PostSettings` on the environment)
- **TAA** — temporal anti-aliasing (both backends): the camera jitters sub-pixel
  each frame (Halton 2,3) and a depth-reprojected, neighbourhood-clamped history
  accumulates a supersampled, temporally stable image; the jitter and history
  live entirely in the backend, so the renderer is unaware
- **Depth of field** (both backends, off by default) — a single-pass depth-aware
  disk-bokeh blur: per-pixel circle-of-confusion from the reconstructed world
  distance vs. a focus distance/range, gathered over a golden-angle disk with
  foreground samples down-weighted so sharp edges don't bleed (`--dof` to try it)
- **Motion blur** (both backends, off by default) — camera motion blur: per-pixel
  velocity reprojected from depth, colour averaged along it (`--motionblur`)
- **FXAA** — post-AA resolve into the final target (viewport texture or swapchain)
- **Texture polish** — anisotropic sampling (8x) + sRGB-correct CPU mip
  generation for all loaded `.uaf` textures

Post passes read inputs through the existing bindless table, so both backends
share one shader set (`Shaders/PostCommon.hlsli` + SSAO/Bloom*/Tonemap/FXAA).
If the post shaders are missing the renderer falls back to the legacy
direct-to-LDR path (`gOutputLinear == 0` re-enables inline tonemapping).

All of the above are **on by default** (per-scene `PostSettings`) and tunable
live in the editor's **Post Process** panel — toggles plus sliders for every
effect — and the settings are **saved with the scene** (`.hbscene`).

Done on **both backends**: TAA (camera jitter + depth-reprojected history), SSR,
depth of field, camera motion blur, and auto + manual exposure.

A **photorealism pass** is now underway, adding (in dependency order) a thin
G-buffer, per-object velocity, GTAO, volumetric fog, a physically-based
atmosphere, and DDGI-style probe GI. Landed so far (**both backends**):

- **Thin G-buffer** — the forward pass writes a second MRT (`RGBA16F`:
  octahedral world normal + roughness + metalness) and a third (`RG16F`) screen
  **velocity** buffer alongside the HDR colour. The editor preview/asset-viewer
  mini-pass uses single-RT pipeline variants, and the sky pass masks off the
  extra targets so sky pixels read as "no surface".
- **Per-object velocity buffer** — the mesh VS reprojects each vertex through its
  *previous* model + bone palette (the scene caches both per entity), so **TAA
  and motion blur now track moving and skinned geometry** instead of ghosting.
- **Per-material glossy SSR** — SSR reads the G-buffer normal + roughness, so
  reflections follow the shading normal and fade out on rough/diffuse materials.
- **GTAO** — the ambient-occlusion pass is now **ground-truth AO** (Jimenez 2016):
  horizon-based, using the G-buffer shading normal instead of a depth-derivative
  estimate, with a multi-bounce term (replaces the 12-tap hemisphere SSAO; reuses
  the same half-res target, blur and `ssaoRadius`/`ssaoIntensity` controls).
- **Volumetric fog + light scattering** — a ray-march from the camera to the
  scene depth accumulates sun in-scatter sampled through the cascaded shadow map
  (so windows and gaps throw **god-rays**) plus punctual-light in-scatter with a
  Henyey-Greenstein phase over an exponential height fog, composited into the HDR
  before bloom. Tunable via the `PostSettings` `fog*` fields.
- **Physically-based atmosphere** — the sky is now a **Rayleigh + Mie
  single-scattering** ray-march (Earth atmosphere) baked into the HDR equirect;
  the irradiance and prefiltered-specular convolutions sample that baked sky, so
  the background **and** the ambient lighting share one physically-based model
  (fixes the old washed-out gradient). The bake runs across the job system, so
  the whole IBL set builds *faster* than the old procedural one (~1.4 s).
- **Screen-space global illumination (SSGI)** — one indirect diffuse bounce:
  cosine-weighted rays over the G-buffer normal are ray-marched against depth and
  each hit contributes the lit HDR colour of that surface, composited additively
  into the HDR. Gives **colour bleeding** from nearby geometry (a red wall tints
  the floor beside it); the downstream TAA denoises the gather. Screen-space, so
  off-screen light is missed — a **hardware-RT / capture-based DDGI** probe field
  is the documented upgrade for off-screen, leak-free GI.

What "ultra photorealism" still needs next, roughly in impact order:

1. **Off-screen GI** — hardware-ray-traced or capture-based DDGI probes (the SSGI
   here is the on-screen first bounce; probes add off-screen + leak-free light)
2. **Parallax occlusion mapping**, physically-based bloom energy (the G-buffer
   that unblocks per-material SSR roughness is now in place)

## Character rendering (in progress)

A character pipeline (toward TLOU2 / Detroit-quality faces) is underway. Landed so
far (**both backends**), all driven by per-material flags on the existing material
system:

- **Pre-integrated skin (SSS)** — the subsurface material is now Penner
  pre-integrated scattering: a CPU-baked diffusion-profile LUT, sampled by
  `(N·L, curvature)` (curvature reconstructed from screen-space derivatives), gives
  the soft reddened terminator of skin, plus thickness-map **transmission** so thin
  back-lit regions (ears/nose) glow.
- **Cloth (fabric sheen)** — a Charlie sheen NDF + Neubelt visibility lobe so
  garments read as cotton/velvet/silk instead of plastic.
- **Eyes** — a parallax-refracted iris (the visible iris shifts with view angle
  through the cornea) + a sharp cornea catchlight.

Still to come: **blendshapes / morph targets** (facial animation), an
**order-independent transparency** pass + **anisotropic hair**, and **simulated
cloth** (Jolt soft bodies).

Done since: textured PBR (normal/MR/AO via bindless), skin/subsurface scattering,
flythrough freecam, a Unity-style editor viewport (scene rendered to an
offscreen target shown in an ImGui panel) on both backends, a **platform input
abstraction** (keyboard/mouse/XInput gamepad behind `Core/Input`), a **skybox**
background pass, **directional shadow maps** (depth-only pass + 3x3 PCF, both
backends), a **transform hierarchy** (Parent component, world-matrix
composition, editor drag-drop reparenting), and **Jolt rigid-body physics**
(RigidBody component, fixed-step world, editor Simulate toggle), and **audio
playback** (miniaudio behind `Audio/AudioSystem`; .uaf audio assets play from
the editor's asset browser).

Also done: **scenes** (`.hbscene` JSON serialization of all components, Scene
menu, startup scene per project, **additive streaming** off a worker thread,
drag a scene into the viewport to stream it in), **keyframe animation**
(AnimationTrack + Timeline panel with scrubbing/keys/transport), **3D
positional audio** (listener follows the camera; AudioSource entities emit
spatialized .uaf audio), **drag-drop spawning** (mesh tiles drop onto the
surface under the cursor), **CPU-rasterized mesh thumbnails**, **UAP asset
packs**, and **HeartbreakHub** - a standalone project-manager exe that
launches the editor.

**Asset packs (.uap):** assets are cooked into chunked packs of **50 fixed
slots** each (`MyProject_0.uap`, `MyProject_1.uap`, ...). An asset keeps its
slot for life - assignments persist in `MyProject.uapmanifest` next to the
project - so **deleting an asset frees its slot without shifting the rest**
(untouched packs stay byte-identical across updates, which keeps patches
small), and the next imported asset fills the lowest free slot. Cook from the
Project menu ("Build Asset Pack") or headlessly with
`HeartbreakEditor --project X.hbproj --pack`. "Build Shipping Folder"
assembles `Build/` with the runtime exe, shaders, project file, assets, and
packs; all third-party dependencies link statically so the folder is
self-contained.

**Punctual lights + emissive (done):** up to 16 **point and spot lights** per
frame (ECS `PointLightComponent` / `SpotLightComponent`, windowed inverse-square
falloff, smooth spot cones) shade every PBR surface alongside the shadowed sun,
and materials carry an **emissive color/intensity + emissive map**. Lights are
created from the Hierarchy's "+ Create" menu, edited in the Inspector, and
serialized in scenes.

**Material assets (.hbmat):** a full PBR material is a small JSON asset (base
color, metallic, roughness, emissive, subsurface, and five texture-map refs).
Create one with the asset browser's **New Material** button, edit it in the
**Asset Viewer** panel (live sphere preview, texture pickers over the project's
imported textures), then **drag the material onto any object in the viewport**
to apply it — entities remember the link via a `MaterialRef` component, saving
a material re-applies it to every user in the open scene, and scenes reload
materials from the asset on load.

**Asset Viewer:** click any asset tile to inspect it — textures show a large
preview with size/format/mips, meshes show a rasterized turntable preview with
submesh/vertex/triangle stats, audio shows duration/format with a Play button,
scenes show entity counts with load/stream buttons, and materials open the
full material editor.

**Scene manager:** the **Scenes** panel lists every `.hbscene` in the project
with the **startup (default) scene** marked; set the default with one click,
and load / stream additively / rename / duplicate / delete from the context
menu. Both the editor and the runtime boot into the project's default scene.

**Build configurator + shipping:** *Project → Build Settings…* configures the
shipped game — game name (window title), company, version, **graphics backend**
(D3D12/Vulkan), resolution, startup scene, and packing options — all persisted
in the `.hbproj`. One **Build** action (menu or Build Settings window) cooks the
packs, verifies them, and assembles a **clean, minimal `Build/` folder that is
nothing but the game application, the asset packs, and DLLs**:

- **`HeartbreakRuntime.exe`** — the newest runtime exe across configs (a Release
  runtime is ~3 MB vs ~15 MB Debug).
- **`<name>_N.uap`** — **LZMS-compressed** asset packs (Windows Compression API;
  per-asset, skipped when it doesn't shrink) that now also carry the engine's
  **compiled shaders** and the **project file** as packed extras — so the build
  ships **no loose `shaders/` folder, no `.hbproj`, and no loose `Assets/`**.
- **DLLs only** — native dependencies (static today, so usually none) plus, *only
  when the project has compiled C# scripts*, the managed scripting core,
  `Scripts.dll`, and the managed DLL's two mandatory .NET hosting JSON sidecars.
  A scriptless game ships as **just the exe + packs**.

The runtime boots straight from the packs: it **auto-discovers the `.uap` packs
next to the exe, mounts them through a VFS, reads the project (and shaders) back
out of them**, and serves every scene/material/texture/audio read from the packs
(falling back to disk for anything unpacked). Shaders flow through an RHI
**shader provider** the engine wires to the VFS for packed builds, so both
backends fetch their bytecode from the packs instead of a loose folder. "Pack
only scene-referenced assets" walks every scene's mesh/material/texture/audio
references and packs just those (the shaders + project are always included).

**Undo/redo (Ctrl+Z / Ctrl+Y):** every scene mutation — gizmo drags, inspector
edits, entity create/delete/spawn, reparenting, component add/remove, material
applies, scene loads — captures a snapshot first; the Edit menu and shortcuts
walk the history (64 steps). Snapshots are in-memory scene JSON, and the
serializer keeps **process-wide GPU caches** (mesh source → handle, texture →
handle), so restoring even a multi-hundred-MB scene is instant: nothing is
re-read or re-uploaded.

**Material import from 3D files:** importing a model now **generates a
`.hbmat` material asset per material** (into `<model>_Materials/`), with
factors, emissive, and texture refs all wired up; spawned entities link to
them via `MaterialRef`, so imported models are immediately editable in the
material editor. Texture URIs are percent-decoded and searched in
conventional locations, refs are stored **relative to the Assets root** (they
survive organizing assets into folders), and the VFS gained a **filename-search
fallback** that heals refs in older assets that broke when files were moved —
existing white models re-texture themselves on the next load, no re-import
needed.

## C# scripting

The engine hosts the **.NET 8 runtime in-process** (CoreCLR via hostfxr — no
Mono). Gameplay code is plain C# deriving from `HeartbreakEngine.Script`:

```csharp
using HeartbreakEngine;

public class Spinner : Script
{
    public override void OnStart()
    {
        Debug.Log($"hello from '{Entity.Name}'");
        Entity.SetEmissive(new Color(0.2f, 1.0f, 0.5f), 3.0f);
    }

    public override void OnUpdate(float dt)
    {
        Entity.Rotation = Quaternion.FromAxisAngle(Vector3.Up, 90f * dt) * Entity.Rotation;
        if (Input.WasKeyPressed(Key.Space)) Entity.AddImpulse(Vector3.Up * 5f);
    }
}
```

- **API**: `Entity` (transform, name, find/create/destroy, base color,
  emissive, point lights, rigid-body velocity/impulse), `Input`
  (keyboard/mouse), `Debug`, `Time`, plus `Vector2/3`, `Quaternion`, `Color` —
  all marshaled through a function-pointer interop table (no reflection on the
  hot path; one native call per frame fans out to every script).
- **Workflow**: the Assets panel's **New Script** drops a templated `.cs`;
  double-click any script to open it in the **Script Editor** panel (docked
  with the Viewport). **Saving (Ctrl+S) compiles the project's scripts with
  `dotnet build` and hot-reloads them**: the old assembly unloads (collectible
  `AssemblyLoadContext`), the new one loads, and every script instance is
  recreated with its entity binding intact. Compile errors show in the panel.
- **ECS**: attach scripts with the inspector's **Script (C#)** component (a
  dropdown of compiled classes); the component serializes in scenes, works
  with undo/redo, and instances follow entity lifetime (deletes/scene loads
  clean up through registry hooks). Scripts run while **Simulate** is on in
  the editor and always in the runtime; shipped builds carry `Scripts.dll` +
  the managed core next to the exe (players need the .NET 8 runtime, not the
  SDK). Without the .NET SDK the engine just disables scripting.

**Game cameras + Game tab:** a `CameraComponent` (FOV / near / far / primary,
created from "+ Create → Camera") turns any entity into a game camera — the
**primary camera drives rendering in play mode and in the runtime**, while the
editor's Scene view keeps its own fly camera. The **Game tab**'s
Play / Pause / Stop snapshot the scene on Play and restore it exactly on Stop.

**Camera rigs (presets + rotation modes):** every `CameraComponent` has a
**behaviour preset** — `Static`, `First Person`, `Third Person` (boom that
trails a target), `Orbit` (auto-spins around a target), `Distance` (fixed
framing), or `Spline` (travels a Catmull-Rom path) — and a **rotation mode** —
`Free`, `Look At`, `Slow Follow` (damped), `Spin` (continuous yaw), or `Fixed`.
Targets are referenced by entity name and resolved each frame; position/aim
**damping** smooths follows and blends transitions. A **`CameraSpline`**
component (created from "+ Create → Camera Spline", points editable in the
inspector or "Add at Camera") supplies the path for Spline mode. The whole rig
is driven by `Scene/CameraSystem` and serialized in scenes.

**Camera Zones:** a **`CameraZone`** is a trigger volume (an oriented box from
the entity's Transform + half-extents) that switches the active camera when a
tracked entity enters it — so a level can blend between rigs as the player
moves (e.g. a first-person interior zone inside a third-person world).
Overlapping zones resolve by `priority`; with none active the scene's primary
camera is used. Zones draw their volume in the Scene view (green = active).

**In-game UI (canvas):** `UIElement` components — **Panel, Label, Button,
Image, Progress Bar / Wheel** — lay out on a configurable reference canvas
(normalized anchor + pixel offset/size, color, per-element text size,
visibility). Text is real **TrueType** (a Segoe UI/Arial atlas baked with
`stb_truetype`, anti-aliased at any size), Images and Panels/Buttons take any
imported texture (picked from a dropdown), and progress bars fill linearly or
as a **radial wheel** — all through a dedicated **textured UI overlay pass in
both backends** (the runtime has no ImGui). **Canvas scaling is
Unity-CanvasScaler-style**: Stretch, Match Height (UI keeps its size, width
follows the aspect ratio), or Pixel-Perfect, with the reference resolution
configurable in Build Settings. Buttons hover/press-shade and report clicks;
C# scripts drive everything via `Entity.SetUIText / UIClicked / UIHovered /
SetUIVisible / SetUIFill`. Created from "+ Create → UI", serialized in
scenes, live in both the Scene and Game views.

**Project Settings (environment / skybox):** a **Project Settings** panel
(Project menu) edits the project-wide environment stored in the `.hbproj`: a
**custom procedural skybox** (horizon / zenith / ground colours, sun direction
/ tint / intensity, overall sky brightness) that regenerates the sky **and the
image-based lighting** derived from it live ("Rebuild Sky"), plus the fallback
**directional sun** (colour / intensity), **ambient** and **exposure**. New
scenes inherit these defaults; per-scene values still win.

**Editor UX:** a **Window menu** shows/hides every dockable panel (and any
panel's close button hides it), with **Show All** / **Reset Layout** to restore
the default Unity-style arrangement (Hierarchy left, Inspector/Asset
Viewer/Project Settings right, content browser + tool/log tabs split across the
bottom). The **Script Editor** now auto-closes brackets/quotes, types over
matching closers, auto-indents new lines (extra level after `{`), and offers a
**completion popup** for the C# scripting API + keywords (Tab accepts the top
match; toggleable).

**Primitives + components:** the mesh library now has **plane, cylinder, cone,
capsule and torus** alongside cube/sphere ("+ Create → 3D Object" and the
inspector's "Add Component → Mesh"). New components: **Rotator** (constant spin),
**Motion Matching** (data-driven locomotion — builds a feature database from a
rig's clips and auto-selects idle/walk/run by movement speed), and the terrain
and IK systems below. **Save All** (Project menu / Ctrl+Shift+S) writes the scene
and the `.hbproj` together; every component serializes.

**Terrain editor:** a **`TerrainComponent`** materializes an editable heightfield
as a grid of independently cullable mesh **chunks** (procedurally seeded with fbm
noise). The **sculpt tool** (Terrain inspector → Sculpt) lets you drag in the
Scene view to **Raise / Lower / Smooth / Flatten** under a falloff brush, with a
projected brush ring; edits update only the affected chunk meshes **in place**
via a new RHI `UpdateMesh` (no GPU-buffer churn), and the sculpted heightmap is
saved with the scene.

**Inverse kinematics:** an **`IKConstraint`** holds two-bone IK chains (foot /
hand / etc.). After skeletal animation poses the skeleton, each chain solves
analytically so its end joint reaches a world-space target (optionally another
entity's position), with a pole hint for the bend direction and a 0..1 blend
weight — applied as corrective local rotations and recomposed so descendants
follow. Targets are resolved before the parallel pose pass.

Next:

Editor/runtime systems pass (2026-06-12): the **3D Asset Viewer** is a real
mesh editor (Unreal-style) — the RHI renders a second, independent preview
scene (HDR mini-pass + tonemap on both backends) with orbit/zoom, per-submesh
**material slot** assignment, and Save back into the `.uaf`; the asset
browser's actions moved into a **right-click context menu** (Import / Create
Folder-Material-Script-Audio Event / Refresh); the in-game UI is **Unity-style**
(UICanvas component roots + hierarchical RectTransform layout with
anchorMin/anchorMax/pivot, stretch presets, parenting via the scene hierarchy,
per-canvas scale modes and sort order — old single-anchor scenes still load);
audio grew an **FMOD/Wwise-style middleware layer**: a mixer **bus tree**
(Master/Music/SFX/Ambience + user buses, persisted in the project, edited live
in the new Audio Mixer panel) and **audio event assets** (`.hbevent`: weighted
random sound pools, volume/pitch randomization, bus routing, 3D attenuation)
posted via `AudioSystem::PostEvent`; the sphere-grid demo scene is gone (the
default world is just a sun + sky).

**Skeletal animation** (2026-06-12): full GPU-skinned character animation on
both backends. Rigged models (glTF/FBX/...) import with their skeleton +
clips intact (the flattening pre-transform only applies to static models);
vertices carry 4 joint influences, and `.uaf` v5 stores the rig (older assets
still load). At runtime the **Animator** component samples a clip into a
joint palette (uploaded per frame to a bindless bone buffer; the vertex
shader skins position/normal/tangent, shadows included), with play/loop/speed
control in the Inspector. **Real-time retargeting**: clips address joints *by
name*, so an Animator can play any other rigged asset's clips - joints match
by canonical name (DCC prefixes like `mixamorig:` stripped), unmatched joints
hold their bind pose, and translations scale by the skeletons' bone-length
ratio. **Root motion** (Animator toggle): the clip's horizontal root travel
drives the entity's Transform (loop-seam aware) so the character animates in
place yet moves through the world. FBX import collapses Assimp's pivot helper
nodes and applies the file's unit scale (Mixamo rigs import correctly), and an
over-budget skeleton degrades to a static mesh instead of hanging the GPU. The
editor's `--import <file>` flag scripts the content pipeline.

**Universal drag & drop** (2026-06-12): every asset drags from the browser and
drops where it makes sense — materials paint objects (viewport or hierarchy
row), scripts attach as components, audio clips become AudioSources, fonts
restyle UI text, meshes spawn (as children when dropped on an entity), scenes
stream additively, and **dropping a texture onto an object auto-creates a
material** (`<name>_Mat.hbmat` beside the texture; sibling
Normal/Metal-Rough/AO/Emissive maps are picked up by naming convention) and
applies it. Inspector and Asset Viewer slot widgets (UI texture/font, material
texture maps, audio-event sounds, mesh material slots) accept drops too.

1. **Embedded glTF textures** — load material maps from .glb binaries
2. **Frame graph** — automatic barriers and transient resource aliasing
3. ~~**Post** — HDR pipeline, bloom, SSAO, CSM, FXAA~~ **done** (see "Toward
   photorealism"); TAA/SSR/GI remain
4. **More backends** — the RHI seam is ready for them
5. **Scripting v2** — script properties in the inspector, OnCollision events,
   more component APIs
6. **UI v3** — nine-slice panels, layout groups, input fields, UI animation

## License

TBD.
