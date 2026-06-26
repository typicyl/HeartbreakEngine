# Heartbreak Engine

A from-scratch 3D game engine built on a clean, backend-agnostic **RHI (Render
Hardware Interface)**. One renderer drives **three** graphics backends —
**Direct3D 12**, **Vulkan**, and **OpenGL 4.6** — behind a single bindless
interface. On top of it sits an **EnTT** ECS, **Jolt** physics, a fiber job
system, world streaming, **Recast & Detour** navigation, a photorealistic HDR
renderer, a **visual-scripting** system (schematics, transpiled to C++ for
shipping), an **adaptive music** director, a painterly **Art Editor**, and a
Unity-style **Dear ImGui + ImGuizmo** editor.

> **Status — a real engine.** All three backends render textured Cook-Torrance
> PBR with image-based lighting, cascaded shadows, an HDR post stack and a
> physically-based day/night sky. The D3D12 and Vulkan backends are at full
> feature parity; the OpenGL backend renders the lit scene, sky, IBL and UI and
> is being brought up to post-stack parity. The engine ships as five
> executables (runtime, full editor, art editor, project hub, schematic baker),
> drives gameplay with a node-graph visual scripting system, and exports a
> clean, self-contained shipping folder. Everything is authored in projects of
> `.uaf` assets packed into `.uap` containers.

---

## Table of contents

- [Executables](#executables)
- [Rendering backends & the RHI](#rendering-backends--the-rhi)
- [Building](#building)
- [Running & command-line flags](#running--command-line-flags)
- [Source layout](#source-layout)
- [Rendering & photorealism](#rendering--photorealism)
- [Day / night / weather sky](#day--night--weather-sky)
- [Scenes, levels & streaming](#scenes-levels--streaming)
- [Schematics (visual scripting)](#schematics-visual-scripting)
- [Gameplay systems](#gameplay-systems)
- [The Art Editor (painting)](#the-art-editor-painting)
- [Audio & adaptive music](#audio--adaptive-music)
- [Assets & shipping](#assets--shipping)
- [Editor UX](#editor-ux)
- [Concurrency, navigation & physics](#concurrency-navigation--physics)
- [Requirements](#requirements)
- [License](#license)

---

## Executables

The build produces **five** programs from one shared engine library, so the
editor never ships inside a game:

| Exe | What it is |
|-----|------------|
| **`HeartbreakRuntime`** | The game runtime only — **no** editor, **no** ImGui symbols. Links `hbe` (`HBE_EDITOR=0`). Boots straight from `.uap` packs. |
| **`HeartbreakEditor`** | The full editor: viewport, gizmo, hierarchy, inspector, asset browser, every authoring panel (incl. painting, music, schematics). Links `hbe_editor` (`HBE_EDITOR=1`). |
| **`HeartbreakArtEditor`** | The same editor in a painting-focused layout (wide Art Editor + viewport, paint mode on at boot, engine/tech UI hidden) for 2D artists. |
| **`HeartbreakHub`** | A standalone project manager that launches the editor (recent/create/open projects). |
| **`HeartbreakBaker`** | A headless tool that **transpiles a project's schematics to C++** so shipped runtimes execute compiled gameplay instead of interpreting graphs. |

A **project** is a folder with a `.hbproj` file and an `Assets/` directory. The
editor opens with a Project Manager (recent projects, create new, open existing,
or `--project path/to/file.hbproj`).

## Rendering backends & the RHI

The renderer talks **only** to `hbe::rhi::IRenderDevice`; each backend is a
private implementation selected at runtime by `rhi::CreateRenderDevice`. The
core abstraction is **unified bindless texturing** — a global texture array
indexed by integer (D3D12 unbounded SRV table / Vulkan `VK_EXT_descriptor_
indexing` / per-draw binding on GL), so all three backends share one HLSL shader
set compiled to **DXIL + SPIR-V** at build time (the GL backend uses
hand-written GLSL, since GL has no runtime descriptor arrays).

- **Direct3D 12** and **Vulkan** — full feature parity (the reference path).
- **OpenGL 4.6** — a WGL core-context backend (`clipControl` for RH zero-to-one
  depth, mirrored winding). Renders the textured, IBL-lit scene, the
  physically-based sky and the in-game UI overlay; the full HDR post stack /
  shadows / GI are being ported to parity.

**Boot-time backend selection.** Before any rendering, the engine resolves a
**backend fallback chain** from the project's per-platform **build profile**
(e.g. *D3D12 → Vulkan → OpenGL*) and initializes the first one that succeeds, so
a game still launches on a machine where the preferred API is unavailable.
Backends can also be forced per run (`--d3d12` / `--vulkan` / `--opengl`) or
disabled at configure time.

## Building

```powershell
# Configure (Visual Studio generator — no Ninja)
cmake -S . -B out/build/x64 -G "Visual Studio 17 2022" -A x64

# Build all five exes
cmake --build out/build/x64 --config Debug
```

Backends are toggled at configure time (all ON by default):

```powershell
cmake -S . -B out/build/x64 -DHBE_ENABLE_VULKAN=OFF   # drop Vulkan
cmake -S . -B out/build/x64 -DHBE_ENABLE_OPENGL=OFF   # drop OpenGL
```

Dependencies (GLM, Assimp, EnTT, Jolt, Dear ImGui, ImGuizmo, nlohmann/json, stb,
miniaudio, Recast/Detour) are fetched automatically by CMake (FetchContent) on
first configure and linked **statically**, so a shipped build is self-contained.
Shaders compile to DXIL (DXC from the Windows SDK) and SPIR-V (DXC from the
Vulkan SDK) as part of the build.

## Running & command-line flags

```powershell
HeartbreakEditor.exe                      # editor, Direct3D 12 (default)
HeartbreakEditor.exe --vulkan             # editor on Vulkan
HeartbreakRuntime.exe --opengl            # runtime on OpenGL
HeartbreakRuntime.exe --width 1920 --height 1080
```

Useful flags (editor/runtime): `--project <file.hbproj>`, `--model <file>` (load
a glTF/GLB/OBJ/FBX), `--world <file.hbworld>` (stream a world), `--play` (boot
straight into play), `--time <hours>` / `--daynight` / `--clouds` (force
time-of-day and weather), `--import <file>` (headless content import),
`--pack` (cook asset packs), plus headless self-tests (`--navtest`,
`--worldtest`). `Esc` quits.

## Source layout

```
Source/
  Core/        Types, Log, Win32 Window/Input (kbd/mouse + XInput), JobSystem (fibers)
  Assets/      Mesh, MeshGenerator, ModelLoader (Assimp), UAF/UAP (.uaf/.uap), VFS,
               MaterialAsset (.hbmat), AudioEvent (.hbevent), MusicGraph (.hbmusic)
  Scene/       EnTT ECS: Components, Scene, Level, SceneSerializer, SceneStreamer,
               StreamingWorld, AnimationSystem, CameraSystem, CharacterController,
               TerrainSystem, PaintSystem (painting), ParticleSystem
  Physics/     PhysicsWorld (Jolt behind a plain API)
  Navigation/  GridNav (real-time grid A*) + Recast/Detour navmesh
  Game/        GameSystems (objectives, checkpoints, deferred music commands)
  Audio/       AudioSystem (miniaudio: bus tree, events, 3D voices, music director)
  Schematic/   Schematic (graph + node catalog), SchematicSystem (interpreter),
               SchematicTranspile (graph -> C++)
  RHI/         IRenderDevice + RHIFactory; D3D12/, Vulkan/, GL/ backends
  Renderer/    Camera, Renderer (shadow/scene/sky/post passes), IBL, probes/GI
  Editor/      Dear ImGui + ImGuizmo editor (all panels), Importer, thumbnails
  Engine/      Engine (window + input + physics + audio + renderer + main loop)
  Tools/       SchematicBaker (the HeartbreakBaker exe)
  main_runtime / main_editor / main_hub / main_arteditor

Shaders/       HLSL -> DXIL + SPIR-V (Common, BRDF, MeshPBR, Sky, post stack, UI,
               paint, particles, brush strokes, ...)
cmake/         Dependencies.cmake, ShaderCompile.cmake
```

## Rendering & photorealism

A full **HDR render-pass pipeline** (D3D12 + Vulkan; OpenGL in progress):

- **HDR scene pass** into RGBA16F with a thin **G-buffer** (octahedral world
  normal + roughness + metalness) and a screen **velocity** buffer; tonemapping
  happens in post.
- **Cascaded shadow maps** — 4 camera-fit cascades, texel-snapped, 3×3 PCF in a
  4096² atlas.
- **Per-object / skinned velocity** — the mesh VS reprojects through the
  previous model + bone palette, so TAA and motion blur track moving and
  animated geometry.
- **GTAO** — ground-truth horizon-based ambient occlusion (Jimenez 2016) using
  the G-buffer normal, with a multi-bounce term.
- **SSR** — per-material glossy screen-space reflections (follow the shading
  normal, fade on rough surfaces).
- **SSGI** — one indirect diffuse bounce for screen-space colour bleeding.
- **Volumetric fog + light scattering** — a camera-to-depth ray-march samples
  the sun through the cascaded shadow map (god-rays) plus punctual-light
  in-scatter over exponential height fog.
- **Bloom** (soft-knee 6-mip pyramid), **TAA** (Halton jitter + depth-reprojected
  neighbourhood-clamped history), **depth of field** (disk bokeh from a
  reconstructed CoC), **camera motion blur**, **FXAA**, **auto/manual exposure**
  (256-tap log-luminance with eye adaptation), ACES tonemap, vignette, grade.
- **Probe-based global illumination** — local light/reflection probes bake into
  a **full SH-irradiance GI volume** (DDGI-style octahedral depth + Chebyshev
  visibility, BVH bake, area/emissive lights), cached to `.hbgi`, sampled
  trilinearly in the mesh shader — fixes sealed-room sky leak and adds off-screen
  bounce the on-screen SSGI can't see.

All post effects are per-scene `PostSettings`, tunable live in the **Post
Process** panel and saved with the scene; **Post Volumes** apply a per-region
look (the camera blends between them) with a project-global AA/GTAO stamp.

**Character rendering.** Per-material flags drive: **pre-integrated skin** (Penner
diffusion-profile LUT by `N·L`/curvature + thickness transmission), **cloth**
(Charlie sheen + Neubelt visibility), and **eyes** (parallax-refracted iris +
cornea catchlight). Blendshapes, OIT hair and Jolt soft-body cloth are next.

**Painterly rendering.** The oil-on-canvas look is produced by *authored* paint,
not an automatic filter: the Art Editor's painted impasto/bristle relief lit by
PBR, and free-floating 3D brush strokes (see the Art Editor). Earlier automatic
painterly passes — a 2D post filter and an auto stroke-splatting layer — were
removed in favour of this.

## Day / night / weather sky

The sky is a real-time **analytic atmosphere** (Rayleigh + Mie) in `Sky.hlsl`,
driven by a **time-of-day** system that moves the sun across the day. Night is a
navy-graded gradient with sharp three-layer **stars** and a **moon** (real night
isn't black). **Weather** adds FBM **clouds** that drift with a **wind**
direction, plus overcast/haze. The same atmosphere model feeds the IBL bake, so
the background and the ambient light share one physically-based source. Authored
in **Project Settings** (time of day, day length, dynamic sky, cloud
coverage/density, overcast, wind) and serialized in the `.hbproj`; forceable per
run with `--time` / `--daynight` / `--clouds`.

## Scenes, levels & streaming

- **Scenes** (`.hbscene`) — JSON serialization of every component; a Scene
  manager lists them, marks the startup scene, and loads / streams / renames /
  duplicates / deletes.
- **Levels** — a Naughty-Dog-style **level** is a pair of scene files (static +
  dynamic) with the UI separate; `Engine::LoadLevel` composes them, and the
  navmesh bakes the static half.
- **World streaming** — distance-based world partition: cells (each a `.hbscene`)
  load within `loadRadius` and unload past `unloadRadius` (hysteresis), loaded
  **asynchronously on the job system** so streaming never stalls the frame.
  Described by a `.hbworld` manifest.
- **Prefabs** (`.hbprefab`) — save a subtree as a reusable prefab; browse and
  instantiate it (reuses the copy/paste plumbing).
- **Keyframe animation** — AnimationTrack + a Timeline panel with
  scrubbing/keys/transport.

## Schematics (visual scripting)

Gameplay is authored as **node graphs** (`.hbschem`) in an ImGui node editor —
**this replaced the previous C# scripting subsystem.** A graph has event nodes
(Start/Update), flow (Branch/Sequence/Delay), variables, math/logic, transform
and gameplay actions. In the editor and during development the graphs run through
an **interpreter**; for shipping, **HeartbreakBaker transpiles them to C++** so
the runtime executes compiled gameplay (the interpreter is the fallback for
unbaked graphs).

Node categories include **Game** (Reach Checkpoint, Set/Complete Objective) and
**Music** (Set Music State, Set Music Param, Play Stinger) — gameplay drives the
adaptive score directly from a graph. Side-effecting nodes that need engine
systems route through a small deferred command queue the engine drains each
frame.

## Gameplay systems

- **Player movement** — a `CharacterController` (move/jump/gravity, camera-
  relative) for kinematic player control; the runtime uses the game camera (no
  freecam in shipped play).
- **Game cameras & rigs** — a `CameraComponent` with behaviour presets (Static,
  First/Third Person, Orbit, Distance, Spline) and rotation modes (Free, Look At,
  Slow Follow, Spin, Fixed), damped follows, `CameraSpline` paths, and
  **Camera Zones** (trigger volumes that switch the active rig by priority).
- **Game flow** — a runtime state machine: **main menu → loading → playing**,
  with cursor lock and project slots for the menu / HUD / loading / studio-splash
  scenes; `UIElement` action buttons drive transitions.
- **Checkpoints & objectives** — a `Checkpoint` component + objective tracker
  (HUD `{objective}` token), `.hbsave` save/load, and schematic game nodes. A
  shipped-build **dev overlay** (Ctrl+`, gated by `BuildSettings.devMenu`) aids
  debugging.
- **Particles** — a pooled CPU simulation + a GPU billboard pass (both backends).
- **In-game UI** — Unity-CanvasScaler-style canvases with Panel / Label / Button
  / Image / Progress-bar-or-wheel elements, real TrueType text (stb_truetype
  atlas), through a textured UI overlay pass in every backend (the runtime has no
  ImGui).
- **Terrain** — an editable chunked heightfield with a sculpt brush
  (Raise/Lower/Smooth/Flatten) updating chunk meshes in place.
- **IK & locomotion** — two-bone IK constraints (foot/hand), a **Rotator**
  (constant spin), and **Motion Matching** (data-driven locomotion from a rig's
  clips).

## The Art Editor (painting)

A painting workflow for surfacing and 2.5D scene painting, available as the
dedicated `HeartbreakArtEditor` exe **and** as the **Art Editor** panel in the
full editor (with a one-click **Paint** toggle in the menu bar).

**Surface painting** — paint pigment + PBR material + relief directly onto a
mesh's UV texture:

- A **stroke database** is the editable source of truth; the painted layer
  textures are a baked cache (replay strokes → flatten), so undo/redo is a cheap
  stroke pop + rebake. Persists to `.hbpaint`.
- **Projection (3D-aware) painting** stamps by surface proximity in mesh-local
  space, so a stroke crosses UV-island seams and never stretches.
- **UV-seam edge dilation** pads painted texels into the gutter so filtering and
  mips don't bleed across island boundaries.
- An **oil-paint look** — directional **bristle** micro-relief baked into the
  height (impasto ridges that catch raking light), bristle value-streaks in the
  pigment, and bold **broken colour** — produced by the painted data under PBR
  lighting, **not** a post filter.
- Paint **layers** (composited with opacity/transparency), per-object box vs
  mesh-UV unwrap, distance-LOD mip averaging, and a brush library + brush editor
  (custom procedural or imported/hand-painted tips).

**3D brush strokes (grease-pencil)** — draw in screen space and the stroke
becomes its **own free-floating object** in real space, placed on a plane
tangent to the surface you start on (depth + orientation from the first hit, not
the camera). Strokes are double-sided, **shadeless** (they show the colour you
picked), with a baked oil bristle-streak texture, and carry **Photoshop/GIMP-
style brush dynamics**: start/end **taper**, procedural **size jitter** and path
**wobble**, and path **smoothing**.

## Audio & adaptive music

A FMOD/Wwise-style audio middleware over **miniaudio**:

- **Mixer bus tree** — Master → Music/SFX/Ambience plus user buses, persisted in
  the project and edited live in the **Audio Mixer** panel (per-bus volume/mute).
- **Audio events** (`.hbevent`) — weighted random sound pools with volume/pitch
  randomization, bus routing and 3D attenuation, posted by name.
- **3D positional audio** — the listener follows the camera; `AudioSource`
  entities emit spatialized `.uaf` audio.
- **Adaptive music** — a graph (`.hbmusic`) of **states** (sections), each made
  of synced looping **layers** (stems) that **crossfade** on a state change, plus
  runtime **parameters** (e.g. *intensity*) that fade individual layers in/out
  over a range. Authored in the **Music** panel (drag stems onto layers, bind
  them to parameters, set start state + fade) with **live preview** (play a
  state, drag a parameter and hear the mix adapt). The runtime auto-starts the
  project's score when the game begins; gameplay drives it through the **Music**
  schematic nodes (or stingers — one-shot accents). Beat-quantized transitions
  and a dialogue duck are the documented next steps.

## Assets & shipping

- **Unified Asset Format (`.uaf`)** — imported content (images, models, audio) is
  cooked into a binary container (like `.uasset`); the runtime loads `.uaf`, not
  raw files. Skeletal rigs + clips are stored in the mesh asset.
- **Material assets (`.hbmat`)** — a full PBR material as JSON (base colour,
  metal, roughness, emissive, subsurface, five texture refs), edited in the
  **Asset Viewer** with a live preview and drag-applied onto objects (entities
  remember the link via `MaterialRef`). Importing a model generates a `.hbmat`
  per material.
- **Asset packs (`.uap`)** — assets cook into chunked packs of 50 fixed slots; a
  slot is kept for an asset's life (deleting one frees its slot without shifting
  the rest, so patches stay small). Packs are **LZMS-compressed** and also carry
  the **compiled shaders** and the **project file** as packed extras.
- **Shipping** — *Project → Build Settings…* configures the game (name, version,
  backend/build profile, resolution, startup scene, dev menu). **Build**
  assembles a clean `Build/` folder that is **just the runtime exe + `.uap`
  packs + any DLLs** (no loose `shaders/`, `.hbproj` or `Assets/`). The runtime
  auto-discovers the packs next to the exe, mounts them through the VFS, and
  reads the project, shaders and every asset back out of them.

## Editor UX

A Unity-style dockable layout: **Viewport** (scene to an offscreen target) +
Hierarchy (drag-drop reparenting), Inspector, Asset browser (icon grid + folders,
right-click context menu, universal drag-drop), Asset Viewer, Scenes, Audio
Mixer, **Music**, **Art Editor**, **Schematic Editor**, Post Process, Navigation,
Streaming, Stats, Timeline, Project Settings. A **Window** menu shows/hides every
panel with Show All / Reset Layout.

- **Undo/redo** (Ctrl+Z / Ctrl+Y, 64 steps) over every scene mutation — gizmo
  drags, inspector edits, create/delete/spawn, reparenting, component add/remove,
  material applies, scene loads. Snapshots are in-memory scene JSON with
  process-wide GPU caches, so restoring even a large scene is instant.
- **Gizmo grid snapping** (Ctrl override) and **prefabs** (`.hbprefab`).
- **Play / Pause / Stop** in the Game tab snapshot the scene on Play and restore
  it on Stop; the primary `CameraComponent` drives the view in play mode and the
  runtime.

## Concurrency, navigation & physics

- **Fiber job system** (`Core/JobSystem`, after Naughty Dog's fiber talk) — one
  worker per core; jobs run on a pool of 128 fibers and can **wait on a counter
  mid-execution without blocking the worker** (the fiber parks and resumes later,
  possibly on another thread). `jobs::ParallelFor` / `Kick`+`Wait` /
  `RunDetached`. Skeletal animation poses every character across the workers, and
  streaming loads run on them.
- **Navigation** — real-time grid A* (`GridNav`, auto-rebuilt from static
  geometry) plus a **Recast & Detour** navmesh; `NavigationAgent`s steer along
  paths each frame with local avoidance around `NavigationObstacle`s. The
  **Navigation** panel bakes/overlays the navmesh and tests paths; `--navtest`
  runs a headless bake-and-walk.
- **Physics** — **Jolt** rigid bodies behind a plain API (fixed step, transform
  sync), an editor *Simulate* toggle, and raycasts used by the camera/gameplay.

## Requirements

- Windows 10/11, x64
- CMake ≥ 3.21 and a C++20 toolchain (Visual Studio 2022 / MSVC recommended)
- [Vulkan SDK](https://vulkan.lunarg.com/) (sets `VULKAN_SDK`) — only when the
  Vulkan backend is enabled (also provides the SPIR-V DXC)
- Direct3D 12 and OpenGL ship with Windows; no extra install

## License

TBD.
