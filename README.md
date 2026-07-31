# Heartbreak Engine

A from-scratch 3D game engine built on a clean, backend-agnostic **RHI (Render
Hardware Interface)**. One renderer drives **three** graphics backends —
**Direct3D 12**, **Vulkan**, and **OpenGL 4.6** — behind a single bindless
interface. On top of it sits an **EnTT** ECS, **Jolt** physics, a fiber job
system, real-time grid-A\* navigation, a photorealistic HDR
renderer, a **visual-scripting** system (schematics, transpiled to C++ for
shipping), a branching **narrative** stack (dialogue graphs, cutscenes,
interaction), **modular characters**, **destruction**, an **adaptive music**
director, a painterly **Art Editor**, and a Unity-style **Dear ImGui + ImGuizmo**
editor.

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
- [Performance & measurement](#performance--measurement)
- [Source layout](#source-layout)
- [Rendering & photorealism](#rendering--photorealism)
- [The painterly look](#the-painterly-look)
- [Day / night / weather sky](#day--night--weather-sky)
- [Scenes & levels](#scenes--levels)
- [Schematics (visual scripting)](#schematics-visual-scripting)
- [Gameplay systems](#gameplay-systems)
- [Narrative: dialogue, cutscenes & interaction](#narrative-dialogue-cutscenes--interaction)
- [Characters](#characters)
- [Destruction](#destruction)
- [VFX & particles](#vfx--particles)
- [In-game UI & input](#in-game-ui--input)
- [The Art Editor (painting)](#the-art-editor-painting)
- [Audio & adaptive music](#audio--adaptive-music)
- [Cinematics & movie rendering](#cinematics--movie-rendering)
- [Assets & shipping](#assets--shipping)
- [Editor UX](#editor-ux)
- [Concurrency, navigation & physics](#concurrency-navigation--physics)
- [Design documents](#design-documents)
- [Requirements](#requirements)
- [License](#license)

---

## Executables

The build produces **five** programs from one shared engine library, so the
editor never ships inside a game:

| Exe | What it is |
|-----|------------|
| **`HeartbreakRuntime`** | The game runtime only — **no** editor, **no** ImGui symbols. Links `hbe` (`HBE_EDITOR=0`). Boots straight from `.uap` packs. |
| **`HeartbreakEditor`** | The full editor: viewport, gizmo, hierarchy, inspector, asset browser, every authoring panel (incl. painting, music, schematics, dialogue, cutscenes). Links `hbe_editor` (`HBE_EDITOR=1`). |
| **`HeartbreakArtEditor`** | The same editor in a painting-focused layout (wide Art Editor + viewport, paint mode on at boot, engine/tech UI hidden) for 2D artists. |
| **`HeartbreakHub`** | A standalone project manager that launches the editor (recent/create/open projects). |
| **`HeartbreakBaker`** | A headless tool that **transpiles a project's schematics to C++** so shipped runtimes execute compiled gameplay instead of interpreting graphs. |

A **project** is a folder with a `.hbproj` file and an `Assets/` directory. The
editor opens with a Project Manager (recent projects, create new, open existing,
or `--project path/to/file.hbproj`).

> `--project` wants the **`.hbproj` file path**, not the containing folder.

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

**The RHI rule.** Any GPU-visible feature must be implemented in **both** the
D3D12 and Vulkan backends. A change landed in one backend only is a bug, not a
partial feature. Frame constants are a **three-site lockstep** — `FrameCB`
(`D3D12Device.cpp`), `FrameUBO` (`VulkanDevice.cpp`) and `cbuffer FrameConstants`
(`Shaders/Common.hlsli`) are one layout; change all three or none.

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
miniaudio) are fetched automatically by CMake (FetchContent) on first configure
and linked **statically**, so a shipped build is self-contained.
Shaders compile to DXIL (DXC from the Windows SDK) and SPIR-V (DXC from the
Vulkan SDK) as part of the build.

> **Shaders are compiled at CMake time, not at runtime**, and are staged per
> config into `bin/<Config>/shaders/` before being folded into the `.uap` packs
> at ship time. Building only one config therefore leaves the *other* configs —
> and any previously shipped `Build/` folder — running **stale shaders**. After
> changing a `.hlsl`, rebuild every config you care about and re-ship. A new
> shader must also be added to `cmake/ShaderCompile.cmake`, or its pass is
> silently dormant.

## Running & command-line flags

```powershell
HeartbreakEditor.exe                      # editor, Direct3D 12 (default)
HeartbreakEditor.exe --vulkan             # editor on Vulkan
HeartbreakRuntime.exe --opengl            # runtime on OpenGL
HeartbreakRuntime.exe --width 1920 --height 1080
```

**Project & content**

| Flag | Effect |
|---|---|
| `--project <file.hbproj>` | Open a project directly (automation / file association) |
| `--model <file>` | Load a glTF/GLB/OBJ/FBX/DAE/PLY/STL directly |
| `--import <file>` | Headless content import into the project's `Assets/` |
| `--play` | Boot straight into play mode |
| `--pack` | Cook asset packs and exit |
| `--ship` | Cook the full shipping folder and exit |

**Display & backend**

`--d3d12` / `--dx12`, `--vulkan` / `--vk`, `--opengl` / `--gl`,
`--width`, `--height`, `--fullscreen`, `--windowed`, `--winpos <x> <y>`,
`--vsync`, `--novsync`, `--validation`, `--no-validation`

**Look & post**

`--time <hours>`, `--daynight`, `--clouds`, `--painterly [radius]`, `--ssr`,
`--dof`, `--motionblur`, `--autoexposure` (`--autoexp`), `--shadowcascades <n>`,
`--nocull`

**Measurement** — see [Performance & measurement](#performance--measurement):
`--benchmark <frames>`, `--benchmark-warmup <frames>`, `--benchmark-csv <path>`,
`--gpuprofile`, `--stress <n>`, `--stress-shared <n>`, `--stress-particles <n>`

**Headless self-tests**

`--navtest`, `--uiworldtest`, `--fracturetest`,
`--destructiontest`, `--test-seamweld`, `--test-readback`, `--test-vfxstack`,
`--test-vfxcompat`, `--test-vfxsim`, `--test-gpucompute`, `--test-entityguid`,
`--test-noleveltypes`

**Offline capture**: `--render-movie` (see
[Cinematics & movie rendering](#cinematics--movie-rendering)). `Esc` quits.

## Performance & measurement

The engine ships its own measurement rig, because every performance intuition
about it has been wrong at least once.

- **`--benchmark <frames>`** runs N frames, prints mean / median / p95 / 1 % low
  plus draw stats, and exits. It forces **vsync off** (a present cap measures
  your monitor, not the engine), discards `--benchmark-warmup` frames (default
  120) for PSO compilation, and uses the **raw** frame delta — the engine's
  normal `dt` is clamped and EMA-smoothed, which would average away exactly the
  spikes you are hunting. `--benchmark-csv <path>` dumps per-frame times.
- **`--gpuprofile`** prints a per-pass GPU breakdown every ~2 s (`shadow`,
  `scene`, `particles`, `ssr`, `ssgi`, `fog`, `volparticles`, `strokefilter`,
  `strokes`, `bloom`, `taa`, `dof`, `fxaa`, `ui`, …).
- **Stress rigs**: `--stress <n>` (unique meshes — vertex-bandwidth bound),
  `--stress-shared <n>` (one shared mesh — draw-sort / instancing rig),
  `--stress-particles <n>` (overlapping additive sprites — particle fill rig).

**Things worth knowing before optimizing anything:**

- **Build config dominates.** Debug adds roughly **23 ms of CPU** over
  RelWithDebInfo on an identical scene with identical GPU time. An FPS number
  from a Debug build measures MSVC's debug runtime, not the engine. Always
  confirm the config first.
- **Release is GPU-bound.** At 6000 casters the CPU headroom is ~0.4 ms, so
  multithreading the per-frame systems wins approximately nothing at that scale.
- **Profile the real scene.** A synthetic 6000-mesh stress test pointed at the
  shadow pass; on the actual game scene shadow was identical across backends and
  the whole cross-backend gap was one post-process pass.
- **Take 3–5 samples.** Single GPU-profiler readings vary meaningfully run to
  run.

## Source layout

```
Source/
  Core/        Types, Log, Win32 Window/Input (kbd/mouse + XInput), JobSystem (fibers),
               InputActions (data-driven actions + rebinding + device glyphs)
  Assets/      Mesh, MeshGenerator, ModelLoader (Assimp), UAF/UAP (.uaf/.uap), VFS,
               AssetFormats (the format registry), MaterialAsset (.hbmat),
               AudioEvent (.hbevent), MusicGraph (.hbmusic), Fracture (.hbfrac)
  Scene/       EnTT ECS: Components, Scene, SceneSerializer, SceneStreamer,
               EntityGuid, AnimationSystem, CameraSystem, CameraRig,
               CharacterController, TerrainSystem, PaintSystem, ParticleSystem,
               WorldState
  Vfx/         VfxTypes (attribute model), VfxStack (module stack), VfxLegacy
  Dialogue/    Branching dialogue graphs (.hbdialogue) + conversation runtime
  UI/          In-game UI (canvases, widgets, layout, skinning), Subtitles
  Project/     Project + ProjectSettings (.hbproj)
  Physics/     PhysicsWorld (Jolt behind a plain API), contact events, raycasts
  Navigation/  GridNav (real-time grid A* pathfinder + dynamic obstacles)
  Game/        GameSystems (objectives, checkpoints, deferred music commands),
               DestructionSystem, gameplay core (combat, AI, spawning, inventory)
  Audio/       AudioSystem (miniaudio: bus tree, events, 3D voices, music director,
               spatial occlusion)
  Schematic/   Schematic (graph + node catalog), SchematicSystem (interpreter),
               SchematicTranspile (graph -> C++)
  RHI/         IRenderDevice + RHIFactory; D3D12/, Vulkan/, GL/ backends
  Renderer/    Camera, Renderer (shadow/scene/sky/post passes), IBL, probes/GI
  Editor/      Dear ImGui + ImGuizmo editor (all panels), Importer, thumbnails
  Engine/      Engine (window + input + physics + audio + renderer + main loop)
  Tools/       SchematicBaker (the HeartbreakBaker exe)
  main_runtime / main_editor / main_hub / main_arteditor

Shaders/       HLSL -> DXIL + SPIR-V (Common, BRDF, MeshPBR, Sky, post stack, UI,
               paint, particles, Painterly, BrushStrokes, StrokeSurface, ...)
cmake/         Dependencies.cmake, ShaderCompile.cmake
docs/          Design documents and system guides
```

## Rendering & photorealism

A full **HDR render-pass pipeline** (D3D12 + Vulkan; OpenGL in progress):

- **HDR scene pass** into RGBA16F with a thin **G-buffer** (octahedral world
  normal + roughness + metalness) and a screen **velocity** buffer; tonemapping
  happens in post.
- **Cascaded shadow maps** — 4 camera-fit cascades, texel-snapped, 3×3 PCF in a
  4096² atlas. Each draw item carries a **`cascadeMask`**, so an object is
  skipped in cascades its world AABB cannot reach — the win grows with scene
  complexity (6000 unique casters: 61 → 86 FPS).
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

## The painterly look

The oil-on-canvas look comes from **four independent layers**, each toggleable:

1. **Directional brush-stroke filter** (`Shaders/Painterly.hlsl`) — a screen-space
   pass that integrates colour **along the structure-tensor tangent**, i.e. the
   direction a brush would actually drag. This **replaced an anisotropic
   Kuwahara filter**, which was both the single most expensive pass (2.30 ms at
   1080p) and the reason the look read as rounded blobs — an 81-tap *disc* gather
   picking the flattest *sector* is isotropic by construction. The line integral
   is O(R) instead of O(R²) and costs **~0.40 ms**. Sampling is clustered toward
   stroke spines by a smooth, C1-continuous pull (a hard lattice snap tiles into
   visible blocks), and every directional term is scaled by a **coherence**
   factor so smooth regions — where the structure tensor is pure noise — stay
   smooth instead of being confidently misoriented. The legacy filter is still
   available behind `HBE_PAINTERLY_KUWAHARA`.
2. **Stroke splatting** (`Shaders/BrushStrokes.hlsl`) — real stroke geometry
   splatted over the frame, with an early vertex-shader coverage cull
   (`HBE_STROKE_CULL`, ~26 % faster).
3. **True 3D brush strokes** (`Shaders/StrokeSurface.hlsl`) — strokes as actual
   PBR-lit surface geometry in the world, not a screen-space effect.
4. **Authored paint** — the Art Editor's painted impasto/bristle relief lit by
   PBR (see [The Art Editor](#the-art-editor-painting)).

All of it is driven by `PostSettings` and exposed in the **Post Process** panel
(strength, stroke size/flow, edge keep, light tint, warm/cool, stroke texture,
canvas weave/scale, posterize steps, stroke length/density/sharpness).

> **Gotcha:** UI, sky and preview PSOs inherit the mesh PSO, so a mesh cull-mode
> change must explicitly override cull on the screen-space passes.

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

## Scenes & levels

- **Scenes** (`.hbscene`) — JSON serialization of every component; a Scene
  manager lists them, marks the startup scene, and loads / streams / renames /
  duplicates / deletes. Serialization is a three-phase Parse → Stage →
  Instantiate, with `runtimeTags` gating runtime-only state into `.hbsave`.
- **Levels** — a **level is ONE `.hbscene`**. There is no static/dynamic file
  pair and no separate level loader: every object carries its own **Static /
  Dynamic** tag (set in the Inspector, saved per entity), which is what the
  navmesh filter and the painterly exemption read. UI lives in its own standalone
  scene (the project's `uiScene`), never inside a level. Proof:
  `--test-noleveltypes`.
- **No world streaming yet** — the old `.hbworld` distance-based cell streamer
  was removed; it was authored-but-unused (no project ever shipped a `.hbworld`).
  Streaming returns as **tag streaming** (`docs/Design-TagStreaming.md`), which
  spawns and despawns by tag rather than by cell. The measured behaviours worth
  keeping from the old streamer — load/unload hysteresis, the one-finalize-per-frame
  budget and its ~1–2 s jank note, and the "is the world settled" predicate — are
  preserved verbatim in `Scene/StreamingSalvage.h`.
- **World state** — per-area persistent state, so revisited areas remember what
  changed.
- **Entity identity** — every entity carries a stable 64-bit **guid**, minted in
  `Scene::CreateEntity` and serialized as `"guid"` in the `.hbscene`. It survives
  save → load → save unchanged, and a **duplicate mints a fresh one** (copy/paste,
  duplicate, prefab instantiate, spawner burst), so persisted per-entity state can
  never alias two objects. Entity **names are not identity** — they are neither
  unique nor stable. Scenes written before the field existed get a guid derived
  deterministically from (file, entity index) on load, so they are stable across
  reloads until the first save writes real ones. Proof: `--test-entityguid`.
- **Prefabs** (`.hbprefab`) — save a subtree as a reusable prefab; browse and
  instantiate it (reuses the copy/paste plumbing).
- **Keyframe animation** — AnimationTrack + a Timeline panel with
  scrubbing/keys/transport.

> **Note:** a save/load/save cycle **reverses** a scene file's entity array
> (`BuildSceneJson` gathers by walking EnTT pools, which iterate in reverse
> insertion order). Content is preserved exactly — identity is the guid, not the
> index — and two round trips return byte-for-byte to the original, but every
> save churns the whole file in a diff.

## Schematics (visual scripting)

Gameplay is authored as **node graphs** (`.hbschem`) in an ImGui node editor —
**this replaced the previous C# scripting subsystem.** A graph has event nodes
(Start/Update), flow (Branch/Sequence/Delay), variables, math/logic, transform
and gameplay actions. In the editor and during development the graphs run through
an **interpreter**; for shipping, **HeartbreakBaker transpiles them to C++** so
the runtime executes compiled gameplay (the interpreter is the fallback for
unbaked graphs).

Node categories include **Game** (Reach Checkpoint, Set/Complete Objective),
**Music** (Set Music State, Set Music Param, Play Stinger), **UI** (events with
payloads, panel/setter ops) and **Narrative** (Play Voiceline, Play Dialogue,
Play Cutscene). Side-effecting nodes that need engine systems route through a
small deferred command queue the engine drains each frame.

> **Adding a node is a four-site lockstep**: the enum (`Schematic.h`) → the
> catalog (`Schematic.cpp` `BuildCatalog`) → the interpreter
> (`SchematicSystem.cpp`) → the transpiler (`SchematicTranspile.cpp`). Miss one
> and the node silently misbehaves in exactly one of editor or shipped build.

## Gameplay systems

- **Player movement** — a `CharacterController` (move/jump/gravity, camera-
  relative) for kinematic player control; the runtime uses the game camera (no
  freecam in shipped play).
- **Game cameras & rigs** — a `CameraComponent` with behaviour presets (Static,
  First/Third Person, Orbit, Distance, Spline) and rotation modes (Free, Look At,
  Slow Follow, Spin, Fixed), damped follows, `CameraSpline` paths, and
  **Camera Zones** (trigger volumes that switch the active rig by priority).
- **Cinematic camera rig** — handheld noise, breathing sway, shake, collision
  return and automatic framing, layered on top of any rig.
- **Game flow** — a runtime state machine: **main menu → loading → playing**,
  with cursor lock and project slots for the menu / HUD / loading / studio-splash
  scenes; `UIElement` action buttons drive transitions.
- **Checkpoints & objectives** — a `Checkpoint` component + objective tracker
  (HUD `{objective}` token), `.hbsave` save/load, and schematic game nodes. A
  shipped-build **dev overlay** (Ctrl+`, gated by `BuildSettings.devMenu`) aids
  debugging.
- **Combat & health** — faction-aware damage/health.
- **AI** — a finite state machine with perception and behaviour states.
- **Spawning & encounters** — spawners and encounter triggers.
- **Inventory, pickups & crafting.**
- **Facial animation** — CPU lip-sync, blinks and expressions.
- **Terrain** — an editable chunked heightfield with a sculpt brush
  (Raise/Lower/Smooth/Flatten) updating chunk meshes in place.
- **IK & locomotion** — two-bone IK constraints (foot/hand), a **Rotator**
  (constant spin), and **Motion Matching** (data-driven locomotion from a rig's
  clips).

## Narrative: dialogue, cutscenes & interaction

See [`docs/NarrativeSystem.md`](docs/NarrativeSystem.md).

- **Branching dialogue** (`.hbdialogue`) — a node **graph** (Start / Line /
  Choice / Condition / SetFlag / End) with its own editor window, a runtime
  conversation player with on-screen choice buttons, and a **global story-flag
  store** persisted into `.hbsave`. This supersedes the earlier linear dialogue
  editor.
- **Voicelines & subtitles** — speaker-tagged lines (`"Name: line"`), a stacking
  and expiring caption system unified with generic subtitles, and 3D
  `DialogueActor` voices bound by name or explicit component.
- **Cutscenes** (`.hbcutscene`) — camera / animation / dialogue tracks with
  easing, roll, focus and aim targets, played by `PlayCutscene`. Authored in an
  artist-facing **NLE Timeline** panel (draggable keys, scrubbing, live viewport
  preview) that shares `cutscene::Evaluate` / `FireMarkers` with the runtime
  player, so preview and playback cannot drift.
- **Interaction** — `Interactable` objects and NPCs (`[E] Talk`) plus box
  `TriggerVolume`s that fire dialogue, cutscenes, flags or objectives, all
  flag-gated.

## Characters

See [`docs/ModularCharacters.md`](docs/ModularCharacters.md).

A **modular character rig**: swappable **parts** on one shared skeleton, with a
build-time **seam weld** that is bit-exact and gap-free. Authored as `.hbchar`
(with a `.hbcharcache` build cache) through a dedicated **Character Editor**.
Verified by `--test-seamweld`.

> The scene JSON key is `characterRig`. `character` is the unrelated
> `CharacterController`.

## Destruction

See [`docs/Design-Destruction.md`](docs/Design-Destruction.md).

- **Offline Voronoi fracture** — half-space clipping (Sutherland–Hodgman per face
  plus a stitched cap face) produces convex cells, so each chunk maps to an exact
  Jolt `ConvexHullShape`. Cooked to `.hbfrac`. Verified by `--fracturetest`.
- **Runtime destruction** — a `Destructible` component with per-chunk health,
  impulse thresholds and debris lifetimes, driven by **physics contact events**.
  **Structural integrity** is a flood fill from anchored chunks over the chunk
  adjacency graph, so unsupported structures collapse. Verified by
  `--destructiontest`.

## VFX & particles

- **Module-stack VFX** (`Source/Vfx/`) — a Niagara-style stack of modules over a
  fixed **64-byte particle attribute** superset, with read/write masks driving
  dead-stream elimination, a structure-of-arrays pool with lazy commit, and a
  separate `ParticleCollide` stage so collision can never be ordered before
  integration. The legacy fixed pipeline is re-expressed as modules and is
  **bit-exact** against the old simulation — `--test-vfxcompat` proves all
  presets identical over 360 frames plus a 66-emitter fuzz. `--test-vfxstack`
  covers the attribute model.
- **Rendering** — a GPU billboard pass in both backends, with sub-UV animation,
  alpha and additive batches. Particle GPU cost has its own `particles` profiler
  slot (it used to be hidden inside `scene`).
- **Volumetric particles** — true 3D volume-texture VFX (compute splat +
  raymarch), on its own `volparticles` slot.

**Measured:** at 200 000 particles the frame is ~2.9 ms while the *entire* GPU is
0.72 ms — the particle system is **CPU-bound** at scale, because vertex expansion
(6 verts/particle) runs on the CPU. GPU simulation and GPU vertex expansion are
therefore the highest-value next step, not overdraw governance.

## In-game UI & input

- **Canvases** — Unity-CanvasScaler-style canvases with Panel / Label / Button /
  Image / Progress elements and real TrueType text (stb_truetype atlas), drawn by
  a textured UI overlay pass in every backend (the runtime has no ImGui).
- **Widgets & layout** — sliders, toggles, selectors, text input, scroll views
  with clipping, layout groups and canvas groups, UI transforms, and keyframed
  `.hbuianim` clips.
- **Focus & navigation** — focus model with gamepad navigation.
- **Skinning** — per-state colours and sounds, part textures, 9-slice, text wrap.
- **One persistent UI scene** — `UIManager` runs over a single `Persistent`-tagged
  UI scene (the older per-screen scene slots were removed), plus a settings menu
  with persistence and closed captions.
- **World-space / physical UI** — a canvas rendered to a texture and drawn on a
  lit page quad in the world, with raycast interaction (`--uiworldtest`).
- **Input actions** — data-driven **actions** with per-user **rebinding** and a
  full per-device/per-button **glyph library** (`Core/InputActions`, `ActionMap`;
  `ProjectSettings.inputActions`; `UserSettings.inputBindings`). Supersedes the
  old hardcoded interact key.

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
  lighting.
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
- **Spatial occlusion** — a per-voice low-pass node driven by multi-ray
  occlusion tests through `PhysicsWorld::Raycast`, so sound leaks through gaps
  rather than switching on and off. Tuned in `ProjectSettings.occlusion`.
- **Adaptive music** — a graph (`.hbmusic`) of **states** (sections), each made
  of synced looping **layers** (stems) that **crossfade** on a state change, plus
  runtime **parameters** (e.g. *intensity*) that fade individual layers in/out
  over a range. Authored in a **DAW-style Music timeline** (lanes, ruler,
  playhead, live gain) with **live preview**. Transitions can be
  **beat-quantized**. **Music Zones** are world volumes that switch state on
  enter, by priority. Gameplay drives it through the **Music** schematic nodes
  (or stingers — one-shot accents).

## Cinematics & movie rendering

See [`docs/MovieRender.md`](docs/MovieRender.md).

Offline "trailer to file" capture: GPU readback (`ReadbackViewportColor`, both
backends) plus **fixed-`dt` deterministic capture**, driven by a `MovieJob` state
machine and the `--render-movie` CLI flag. PNG-sequence output is implemented and
verified (`--test-readback`); MP4 muxing (Media Foundation), audio and an editor
panel are the remaining phases.

## Assets & shipping

- **Unified Asset Format (`.uaf`)** — imported content (images, models, audio) is
  cooked into a binary container (like `.uasset`); the runtime loads `.uaf`, not
  raw files. Skeletal rigs + clips are stored in the mesh asset.
- **Format registry** — `Source/Assets/AssetFormats.{h,cpp}` is the **single
  source of truth** for every recognized extension. Its `runtimeLoaded` flag is a
  **shipping contract**: a runtime-loaded format that is missing from the
  registry is silently absent from packed builds. Loaders must read through
  `vfs::ReadFile`, never `std::ifstream`, or they cannot read out of a pack.
- **Asset packs (`.uap`)** — assets cook into chunked packs of 50 fixed slots; a
  slot is kept for an asset's life (deleting one frees its slot without shifting
  the rest, so patches stay small). Packs are **LZMS-compressed** and also carry
  the **compiled shaders** and the **project file** as packed extras.
- **Shipping** — *Project → Build Settings…* configures the game (name, version,
  backend/build profile, resolution, startup scene, dev menu). **Build**
  assembles a clean `Build/` folder that is **just the runtime exe + `.uap`
  packs + any DLLs** (no loose `shaders/`, `.hbproj` or `Assets/`). The runtime
  auto-discovers the packs next to the exe, mounts them through the VFS, and
  reads the project, shaders and every asset back out of them. Headless:
  `HeartbreakEditor.exe --project <file.hbproj> --ship`.

**Engine formats**

| Format | What |
|---|---|
| `.hbproj` | Project + settings |
| `.hbscene` | Scene (also the format of `.hbprefab` subtrees) |
| `.hbprefab` | Saved subtree |
| `.hbmat` | PBR material |
| `.hbevent` | Audio event |
| `.hbmusic` | Adaptive music graph |
| `.hbschem` | Schematic graph |
| `.hbdialogue` | Branching dialogue graph |
| `.hbcutscene` | Cutscene |
| `.hbchar` / `.hbcharcache` | Modular character rig / build cache |
| `.hbfrac` | Fracture asset |
| `.hbpaint` | Painted stroke database |
| `.hbuianim` | UI keyframe clip |
| `.hbgi` | Baked GI volume cache |
| `.hbsave` | Save game |
| `.uaf` / `.uapmanifest` | Cooked asset / pack manifest |

**Imported source formats** — models `.gltf .glb .obj .fbx .dae .ply .stl`;
images `.png .jpg .jpeg .tga .bmp .psd .gif .hdr .pic .ppm .pgm`; audio
`.wav .mp3 .flac`; fonts `.ttf .otf`.

## Editor UX

A Unity-style dockable layout: **Viewport** (scene to an offscreen target) +
Hierarchy (drag-drop reparenting), Inspector, Asset browser (icon grid + folders,
right-click context menu, universal drag-drop), Asset Viewer, Scenes, Audio
Mixer, **Music**, **Art Editor**, **Schematic Editor**, **Dialogue Editor**,
**Cutscene Timeline**, **Character Editor**, Post Process, Navigation,
Stats, Timeline, Project Settings. A **Window** menu shows/hides every panel with
Show All / Reset Layout.

- **Multi-viewport** — panels detach into real OS windows and dock across
  multiple monitors (both backends).
- **Undo/redo** (Ctrl+Z / Ctrl+Y, 64 steps) over every scene mutation — gizmo
  drags, inspector edits, create/delete/spawn, reparenting, component add/remove,
  material applies, scene loads. Snapshots are in-memory scene JSON with
  process-wide GPU caches, so restoring even a large scene is instant.
- **Gizmo grid snapping** (Ctrl override) and **prefabs** (`.hbprefab`).
- **Play / Pause / Stop** in the Game tab snapshot the scene on Play and restore
  it on Stop (including story flags); the primary `CameraComponent` drives the
  view in play mode and the runtime.
- Layout is persisted to an `.ini` next to the exe.

## Concurrency, navigation & physics

- **Fiber job system** (`Core/JobSystem`, after Naughty Dog's fiber talk) — one
  worker per core; jobs run on a pool of 128 fibers and can **wait on a counter
  mid-execution without blocking the worker** (the fiber parks and resumes later,
  possibly on another thread). `jobs::ParallelFor` / `Kick`+`Wait` /
  `RunDetached`. Skeletal animation poses every character across the workers, and
  the editor's additive scene loads run on them.
- **Navigation** — an in-house real-time **grid A\*** pathfinder (`GridNav`,
  auto-rebuilt from static geometry each frame, no bake and no external nav
  dependency); `NavigationAgent`s steer along paths with local avoidance around
  dynamic `NavigationObstacle`s. The **Navigation** panel tunes/visualises it and
  tests paths; `--navtest` runs a headless route-and-walk check.
- **Physics** — **Jolt** rigid bodies behind a plain API (fixed step, transform
  sync), an editor *Simulate* toggle, **contact events** (queued from Jolt's
  worker threads), impulses at a point, and detailed raycasts used by the camera,
  gameplay, destruction and audio occlusion.

## Design documents

`docs/` holds system guides and design documents. The design documents each
carry **appended adversarial reviews that contradict parts of the design above
them** — read the critique sections before building from them.

| Doc | Status |
|---|---|
| [`NarrativeSystem.md`](docs/NarrativeSystem.md) | Built — dialogue, cutscenes, interaction |
| [`ModularCharacters.md`](docs/ModularCharacters.md) | Built — modular rig + seam weld |
| [`MovieRender.md`](docs/MovieRender.md) | Partly built — PNG sequence done, MP4 pending |
| [`Design-Destruction.md`](docs/Design-Destruction.md) | Fracture + runtime destruction built; rest designed |
| [`Design-VfxGraph.md`](docs/Design-VfxGraph.md) | Phase 1 built; later phases design only |
| [`Design-Water.md`](docs/Design-Water.md) | **Design only** — budget premise is stale, do not build as written |
| [`Design-UnifiedLighting.md`](docs/Design-UnifiedLighting.md) | **Design only** — unresolved correctness findings + engine blockers |
| [`Design-Modeling.md`](docs/Design-Modeling.md) | **Design only** — DCC / editable mesh core |
| [`Design-CharacterCreator.md`](docs/Design-CharacterCreator.md) | **Design only** |
| [`Design-EngineRecon.md`](docs/Design-EngineRecon.md), [`Design-LightingRecon.md`](docs/Design-LightingRecon.md), [`Design-VfxRecon.md`](docs/Design-VfxRecon.md) | Fact-finding snapshots |

## Requirements

- Windows 10/11, x64
- CMake ≥ 3.21 and a C++20 toolchain (Visual Studio 2022 / MSVC recommended)
- [Vulkan SDK](https://vulkan.lunarg.com/) (sets `VULKAN_SDK`) — only when the
  Vulkan backend is enabled (also provides the SPIR-V DXC)
- Direct3D 12 and OpenGL ship with Windows; no extra install

## License

TBD.
