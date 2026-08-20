// Scene/Components.h - ECS components for the scene graph.
#pragma once

#include "Core/Types.h"
#include "Core/Easing.h"      // ease::Curve (per-key AnimationTrack interpolation)
#include "Scene/BodyShape.h"
#include "RHI/RHI.h"
#include "Scene/CameraRig.h"   // cam::CinematicSettings (CameraComponent cinematic rig)
#include "Scene/PaintSystem.h" // paint::Stroke (PaintComponent stroke database)
#include "Schematic/Schematic.h" // schematic::Value (SchematicComponent blackboard)
#include "Vfx/VfxLegacy.h"       // vfx::LegacyParams (ParticleEmitter compat block)
#include "Vfx/VfxStack.h"        // vfx::CompiledStack / ParticleSoA (ParticleEmitter pool)
#include "Volume/VolumeSimConfig.h" // volume::VolumeSimConfig (VolumeComponent's embedded sim recipe)

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace hbe {

// Position/rotation/scale; produces a TRS model matrix.
struct Transform {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; // identity (w,x,y,z)
    glm::vec3 scale{1.0f};

    glm::mat4 Matrix() const {
        glm::mat4 m = glm::translate(glm::mat4(1.0f), position);
        m *= glm::mat4_cast(rotation);
        m = glm::scale(m, scale);
        return m;
    }
};

// Attaches the entity under a parent: its Transform becomes local space and
// the world transform is parentWorld * local (see Scene::WorldMatrix).
struct Parent {
    entt::entity entity = entt::null;
};

// SIBLING ORDER ("order"). The one authoritative answer to "which child comes
// first" - see Scene/Hierarchy.h for the full contract.
//
// WHY A FIELD AND NOT THE ENTITY HANDLE. Every consumer used to sort by the raw
// `entt::entity` value, which is `index | (version << 20)`: the moment ANY entity
// has been deleted in the session, entt recycles its index with a bumped version,
// so a recycled handle compares ABOVE every never-recycled one. A Ctrl+D after any
// delete draws its whole subtree from the free list and the Hierarchy showed the
// clone's children scrambled even though the fragment was correct. (`.hbui` hit
// this first and fixed it with an explicit order list - UI/UIDocument.cpp:585.)
//
// And the handle was not stable across a SAVE either: entt's `swap_only` deletion
// policy moves the last live entity into a destroyed entity's slot, and
// BuildSceneJson derives its row order from exactly that pool - so deleting one
// unrelated object permanently reordered siblings in the written file.
//
// SEMANTICS. Only COMPARED, never required dense or contiguous: `index` is
// allocated from Scene's monotonic counter at CreateEntity (so a new entity always
// sorts last), overwritten by the file's value on load, and renumbered densely
// within one sibling group by an explicit drag-reorder. Comparison is scoped to a
// sibling group, so values from different groups never interact.
//
// MIGRATION. A `.hbscene` written before this field existed carries no "order";
// scene::Instantiate then falls back to the entity's FILE ROW INDEX, which is
// exactly the implicit order those files already had. Every existing file
// therefore loads identically to before.
struct HierarchyOrder {
    i32 index = 0;
};

// Editor-only visibility: when present, the entity (and everything parented under
// it) is hidden in the editor viewport but stays fully loaded. Serialized so the
// hidden setup survives reloads; the runtime ignores it (see Scene::SetEditorView).
struct EditorHidden {};

// RUNTIME visibility suppression (unlike EditorHidden, honoured in play mode and
// the shipped runtime). A bare tag: when present, Scene::CollectDrawItems skips the
// entity's mesh. Driven by the cinematic Visibility track (Source/Cinematics) and
// restored when the sequence ends. NOT serialized - it is transient run state, like
// StreamShard; authored visibility belongs to the sequence asset, not the scene.
struct Hidden {};

// Editor-only, SESSION-ONLY: force an INACTIVE UIPanel to lay out anyway, so the
// author can look at a screen the game has not shown.
//
// A `.hbui` holds several named screens (the reference menu has four) and
// UIPanel::active is RUNTIME state - it is false on load and only ever set by
// UIManager as the game flow runs, so a document opened in the editor lays out
// NOTHING at all (LayoutUI skips an inactive panel's whole subtree). Flipping
// `active` to look at a screen would work, but `active` is the field the game
// flow owns; flipping `startVisible` instead would be an AUTHORED edit that
// CaptureDocument writes to disk. So this is a separate tag, and:
//   * it is NOT one of the six document keys, so CaptureDocument cannot write it;
//   * unlike EditorHidden it deliberately gets NO scene serializer key either -
//     it is which-screen-am-I-looking-at, not project data;
//   * it is honoured only while Scene::EditorView() is on, so the runtime and a
//     shipped build cannot be affected by one even if it somehow existed.
struct EditorUIShow {};

// CPU-simulated, GPU-billboarded particle emitter, simulated by the VFX module stack
// (Source/Vfx). Drawn as camera-facing quads in one batched pass per blend mode (see
// Scene/ParticleSystem + the renderer's particle pass). Authored params are
// serialized; the pool, the compiled stack and the emitter clocks are runtime-only.
//
// The fields below are the AUTHORED surface, unchanged from before the module stack.
// They are not simulation state any more - Scene/ParticleSystem.cpp compiles them
// into a module stack whose default shape is the four Legacy.* compatibility modules,
// which reproduce the old fixed loop bit-exactly. The `use*` / `simulate*` flags at
// the bottom are the opt-ins that swap a legacy module for a real one; every one of
// them defaults to the value that keeps an already-authored emitter identical.
struct ParticleEmitter {
    // Emission
    f32 rate = 24.0f;          // particles/second (continuous)
    u32 maxParticles = 512;    // pool cap (oldest recycled when full)
    bool emitting = true;      // spawn new particles (existing keep simulating)
    // Spawn
    f32 lifetime = 2.0f;       // seconds
    f32 lifetimeVariance = 0.3f; // 0..1 fraction
    f32 emitRadius = 0.0f;     // spawn within this sphere (0 = a point)
    glm::vec3 direction{0.0f, 1.0f, 0.0f}; // base emit direction
    f32 startSpeed = 1.5f;
    f32 speedVariance = 0.4f;  // 0..1 fraction
    f32 spread = 0.35f;        // 0 = along direction, 1 = full sphere
    // Motion
    glm::vec3 gravity{0.0f, -0.6f, 0.0f};
    f32 drag = 0.0f;           // velocity damping (per second)
    // Buoyancy: upward accel proportional to remaining "heat" (1 at birth -> 0 at
    // death), so hot young particles rise fast then stall as they cool. This is what
    // makes a rising fireball pile material into a cap (mushroom clouds, big fire).
    f32 buoyancy = 0.0f;
    // Vortex ring: a toroidal roll about the emitter's vertical axis - particles push
    // outward and curl over/down as they rise, forming the classic mushroom-cap roll.
    f32 vortex = 0.0f;
    // Look (lerped over each particle's life)
    glm::vec4 startColor{1.0f, 0.85f, 0.4f, 1.0f};
    glm::vec4 endColor{1.0f, 0.2f, 0.05f, 0.0f};
    f32 startSize = 0.3f;
    f32 endSize = 0.05f;
    f32 spin = 0.0f;           // billboard rotation speed (rad/s)
    std::string texture;       // sprite .uaf (empty = soft round dot)
    bool additive = true;      // additive (fire/sparks) vs alpha blend (smoke)

    // --- Volumetric overhaul (defaults reproduce the legacy sphere emitter) ---
    // Emission shape (how spawn position + direction are sampled).
    enum class Shape : u32 { Point = 0, Sphere, Hemisphere, Box, Disc, Cone };
    Shape shape = Shape::Sphere;         // Sphere + emitRadius == legacy behaviour
    glm::vec3 boxHalfExtents{0.5f};      // Box shape half-size
    f32 coneAngle = 25.0f;               // Cone half-angle (degrees)
    // Bursts / one-shot (explosions): `burst` particles fire when emission starts;
    // when !loop, continuous emission runs for `duration` seconds then auto-stops.
    u32 burst = 0;
    bool loop = true;
    f32 duration = 1.0f;
    // Turbulence: a swirly noise force added to velocity (smoke/dust/magic).
    f32 turbulence = 0.0f;               // strength
    f32 turbulenceScale = 1.0f;          // spatial frequency
    // Alpha envelope over life (fractions of life to ramp; 0 = off).
    f32 fadeIn = 0.0f;
    f32 fadeOut = 0.0f;
    // Rendering.
    enum class Render : u32 { Billboard = 0, Stretched, Horizontal };
    Render render = Render::Billboard;   // Stretched = velocity streaks (rain/sparks)
    f32 stretch = 2.0f;                  // length/width aspect for Stretched mode
    u32 subUVCols = 1, subUVRows = 1;    // sprite-sheet grid (1x1 = single frame)
    f32 subUVFps = 0.0f;                 // >0 loops the sheet; 0 plays it once over life
    f32 softFade = 0.0f;                 // soft-particle depth-fade distance (0 = hard)

    // (Legacy per-emitter volumetric splatting was removed; real smoke/fire is now the baked
    //  VolumeComponent path - author it with the Volume Baker panel.)

    // --- Module-stack opt-ins (serialized; defaults == the pre-stack simulation) ---
    // Each flag swaps one compatibility module for a real one. They are OFF by
    // default and every absent key in an old scene therefore parses to the legacy
    // behaviour - which is the whole reason they are flags and not replacements.

    // Update.CurlNoiseForce: the curl of a sinusoidal potential, so it is EXACTLY
    // divergence-free and particles swirl instead of piling into sinks. The legacy
    // `turbulence` field is a raw sin/cos triple with non-zero divergence, which is
    // why it visibly clumps. Both can run at once (curl is applied after the legacy
    // forces and before drag).
    bool useCurlNoise = false;
    f32 curlStrength = 1.0f;
    f32 curlFrequency = 0.9f;
    // Update.Drag: exp(-drag*dt), the exact solution of dv/dt = -drag*v, replacing the
    // legacy max(0, 1 - drag*dt). Identical at 30/60/144 Hz, where the legacy form is
    // framerate-dependent and saturates to a dead stop once drag*dt >= 1 (reachable on
    // a hitch at high drag). Reuses the same `drag` value.
    bool expDrag = false;
    // Update.ColorOverLife / SizeOverLife: promote colour and size from render-time
    // curve evaluation to real per-particle ATTRIBUTES. With variance 0 the result is
    // identical to the renderer-side ramp; above 0 each particle gets its own stable
    // scale, which the render-time curve could not express at all.
    bool simulateColor = false;
    f32 colorVariance = 0.0f; // 0..1 per-particle brightness spread
    bool simulateSize = false;
    f32 sizeVariance = 0.0f;  // 0..1 per-particle size spread

    // Render.GpuExpand: build the billboard quads in the VERTEX SHADER instead of on
    // the CPU. The simulation is unchanged - only the expansion moves. The CPU then
    // uploads one 64-byte record per particle instead of six 40-byte world-space
    // vertices (240 B), which is the measured bottleneck at high counts, and the
    // 6 MB vertex ring stops truncating the batch at ~26k particles.
    //
    // OPT-IN, and it must stay opt-in for now: the GPU path packs colour as half4
    // (the CPU path keeps float4) and clamps one sub-UV expression the CPU leaves
    // undefined, so it is visually equivalent but NOT bit-identical - and
    // --test-vfxcompat pins bit-identity for the default configuration.
    bool gpuExpand = false;

    // Sim.GpuSim: run the SIMULATION itself in a compute shader (Shaders/VfxSim.hlsl),
    // not just the billboarding. Implies GPU expansion - the compute-written record
    // buffer IS what the vertex shader reads, so there is no upload and no per-particle
    // CPU work of any kind left in the frame.
    //
    // OPT-IN, and for a stronger reason than gpuExpand's. This flag CHANGES THE
    // SIMULATION: a GPU emitter compiles to the v1 module stack instead of the four
    // Legacy.* compatibility modules, because those are CPU-only by construction (they
    // read a 25-scalar parameter block and a serial emitter RNG stream - see
    // Vfx/VfxTypes.h ParticleView). Concretely that means an emitter switched to GPU
    // simulation loses the six legacy emit SHAPES (v1 spawns in a sphere of emitRadius
    // with directional spread), and `turbulence` becomes divergence-free curl noise
    // while `buoyancy`/`vortex` have no v1 equivalent and are dropped. It is a
    // different, better simulation - not the same one moved - and --test-vfxcompat
    // still pins the legacy one bit-exactly for everything that has not opted in.
    bool gpuSim = false;

    // --- runtime (NOT serialized) ---
    // Structure-of-arrays pool, sized when the stack is compiled and grown
    // geometrically at spawn time up to `maxParticles` - the old pool was a per-emitter
    // std::vector<Particle> grown by bare push_back on every single spawn.
    vfx::ParticleSoA pool;
    vfx::CompiledStack stack;   // recompiled only when the module SET or cap changes
    vfx::EmitterState state;    // clocks, spawn carry, emitter RNG, burst/emit edges
    vfx::LegacyParams legacy;   // rebuilt each frame; reaches kernels via ParticleView::user
    u64 stackSignature = 0;     // structural hash; 0 == never compiled
    u32 textureCache = 0;       // resolved bindless index
    bool textureResolved = false;

    // --- GPU simulation runtime (NOT serialized; see Scene/ParticleGpuSim.h) ---
    // The pool for a `gpuSim` emitter is a RING of fixed slots inside one shared
    // device-local buffer, and the CPU holds only these seven words for it - no
    // per-particle memory and no per-particle work at all.
    u32 gpuSlotBase = 0;      // element offset of this emitter's block in that buffer
    u32 gpuCapacity = 0;      // ring slots allocated (0 = not resident)
    u32 gpuUsed = 0;          // high-water: the update range and the draw count
    u32 gpuCursor = 0;        // next ring slot a spawn takes
    u32 gpuTotalSpawned = 0;  // monotonic; min(this, capacity) IS gpuUsed
    u32 gpuSeed = 0;          // per-emitter RNG key (assigned from the entity id)
    u32 gpuEpoch = 0;         // suballocation generation; != the context's = re-place me
};

// Renders a GPU mesh with a metallic-roughness material.
struct MeshInstance {
    rhi::MeshHandle mesh;
    // Physically-based material VALUES (OpenPBR Surface). Textures stay layer-specific as
    // bindless handles; SurfaceParams owns values only (see RHI/SurfaceMaterial.h).
    SurfaceParams surface;
    // Bindless texture indices (0 = none).
    rhi::TextureHandle albedoTexture;
    rhi::TextureHandle normalTexture;
    rhi::TextureHandle mrTexture;
    rhi::TextureHandle aoTexture;
    rhi::TextureHandle emissiveTexture;
    rhi::TextureHandle thicknessTexture; // SSS transmission thickness (0 = none)
    u32 materialFlags = rhi::MaterialFlag_None;
};

// ROOT of a placed multi-submesh model. A model imported with N submeshes spawns as
// one root + N child MeshInstances; this tags the root so the hierarchy shows the model
// as ONE collapsed row (its submesh children are filtered from the outliner) instead of
// exploding into N entries. `modular` records the authored choice: modular = the parts
// stay individually swappable; static = they are a locked group. Both collapse in the
// outliner; the children remain real, serialized entities either way.
struct ModelGroup {
    bool modular = true;
    std::string source; // the .uaf this was placed from (relative to Assets/), for info
};

// One paint layer in a PaintComponent's stack (bottom -> top). Each layer holds
// two CPU RGBA8 buffers (resolution^2, row-major):
//   color   : RGB albedo, A = colour coverage.
//   material: R metallic, G roughness, B height (0.5 neutral), A = material coverage.
// Layers composite with `opacity` + transparency into the component's flattened
// output (paint::Flatten). A layer can paint colour, material, or both.
struct PaintLayer {
    // STABLE IDENTITY, not the array index.
    //
    // paint::Stroke used to reference its layer BY INDEX, and the layer add/remove/
    // reorder controls record no stroke - so after any reorder the next
    // BakeFromStrokes replayed every stroke into the WRONG layer. That is a live
    // single-user bug, and it makes a durable stroke history impossible: an index
    // recorded today means something different tomorrow, and no migration can recover
    // it because the edit that changed its meaning was never written down.
    //
    // 0 = unassigned (a v4-or-older file, migrated on load to id == index).
    u32 id = 0;
    std::string name = "Layer";
    std::vector<u8> color;
    std::vector<u8> material;
    f32 opacity = 1.0f;
    bool visible = true;
};

// Art Editor surface-paint canvas. Lets 2D artists paint pigment, PBR material
// (metallic/roughness), and relief directly onto a mesh's surface (UV-space
// texture painting, not vertex colors), as a STACK of layers with transparency.
// The CPU `layers` are the source of truth; paint::Flatten composites them into
// `flatColor`/`flatMaterial`, which are mip-chained and uploaded to the bindless
// `colorTex`/`matTex`. The forward pass samples them with a distance-derived LOD
// bias (far strokes average into broader washes) and blends the painted albedo +
// metallic/roughness + relief over the base material by coverage. Pixels persist
// to a `.hbpaint` file (`source`); the scene stores only metadata, never pixels.
struct PaintComponent {
    u32 resolution = 1024;          // canvas is resolution x resolution
    std::vector<PaintLayer> layers; // bottom -> top
    i32 activeLayer = 0;            // runtime: layer the brush edits

    // STROKE DATABASE (the editable source of truth): the ordered history of paint
    // operations. The `layers` above are a BAKED cache of these strokes (replayed
    // in order by paint::BakeFromStrokes); the runtime loads the baked layers and
    // never needs the strokes. The editor records strokes while painting so they
    // stay editable (undo = pop a stroke + rebake). Persisted in `.hbpaint` v3.
    std::vector<paint::Stroke> strokes;

    // TRUE when `strokes` can fully reproduce `layers` by itself.
    //
    // FALSE for a canvas loaded from a pre-v3 `.hbpaint`, which stores baked PIXELS
    // and no history at all. That distinction is load-bearing because
    // paint::BakeFromStrokes ZEROES every layer before replaying: on such a canvas a
    // rebake (undo, redo, or any stroke edit) cleared the art to nothing and then
    // replayed an empty list, destroying the whole painting in one click. The loader
    // already knew this - its comment says the baked layers "are still authoritative"
    // - but nothing carried that fact to the one function that had to honour it.
    //
    // Not serialized: it is derived from the file version on load, and a canvas saved
    // at v3+ from a legacy base is still not self-describing (the baseline pixels are
    // in `layers`, not in any stroke), so the flag must stay false for its lifetime
    // rather than be re-derived as true from the new version number.
    bool strokesComplete = true;

    // Next PaintLayer::id to hand out. Monotonic and NEVER reused: recycling an id
    // would let a stroke recorded against a deleted layer silently attach itself to
    // whatever layer took that number next - the exact class of bug the id replaced.
    // Not serialized; re-derived on load as max(existing id) + 1.
    u32 nextLayerId = 1;

    // Flattened (composited) output, uploaded to the GPU (not serialized).
    std::vector<u8> flatColor;      // RGB albedo, A coverage
    std::vector<u8> flatMaterial;   // R metal, G rough, B height, A coverage

    // STAGING-ONLY, thread-built (not serialized, empty on a runtime component): the
    // fully-prepared upload buffers - flattened, edge-dilated, and mip-chained - computed
    // OFF the main thread by paint::Prepare during StageAssets. Instantiate then only
    // UPLOADS them (async), so streaming a shard full of painted meshes no longer
    // flattens+mips dozens of 1024^2 canvases in one finalize frame (the ~200ms stall).
    // preparedMips == 0 means "not prepared" -> Instantiate falls back to paint::Sync.
    std::vector<u8> preparedColor;    // sRGB, mip-chained
    std::vector<u8> preparedMaterial; // UNORM, mip-chained
    u32 preparedMips = 0;

    bool enabled = true;            // show the paint (per-object visibility toggle)
    bool locked = false;            // mask: ignore brush strokes on this object
    bool reliefEnabled = true;      // apply the painted height as a normal deformation
    f32 opacity = 1.0f;             // global blend of the paint over the material
    f32 heightScale = 0.12f;        // relief strength (0 = no relief); impasto by default
    f32 lodBias = 1.0f;             // distance -> mip averaging strength

    // Object group this canvas is bound to (the brush can be masked to the active
    // group, and a group shows/hides/locks all its members together).
    std::string layer = "Default";

    // Paint coordinate projection: 0 = Mesh UV, 1 = Box (world-scaled, no stretch).
    i32 projection = 0;

    std::string source;             // `.hbpaint` path relative to Assets/ (persisted)

    // Runtime state (managed by paint::Sync; not serialized).
    rhi::TextureHandle colorTex;    // flattened bindless paint colour canvas
    rhi::TextureHandle matTex;      // flattened bindless material+height canvas
    bool dirty = true;              // CPU layers changed -> reflatten + re-upload
    bool gpuReady = false;          // textures created (else this draw ignores paint)

    // The `.hbpaint` named by `source` did NOT load (deleted, renamed, unreadable).
    // The component still exists so the scene keeps the reference and every authored
    // setting above - but `layers` is EMPTY BECAUSE IT IS UNKNOWN, not because the
    // canvas is blank. WritePaintCanvases refuses to write such a canvas back: an
    // empty `.hbpaint` over a file that was merely locked for a moment turns a
    // recoverable broken reference into permanent loss. Runtime only, never
    // serialized.
    bool canvasMissing = false;

    // This runtime canvas was BOX-DOWNSAMPLED at stream-in (paint::Downsample, driven by
    // the project's maxStreamedPaintResolution cap). The on-disk .hbpaint is HIGHER
    // resolution, so this in-memory copy must NEVER be written back - WritePaintCanvases
    // skips it, exactly like canvasMissing, so a save can't destroy the authored original.
    // Runtime only, never serialized.
    bool resCapped = false;
};

// References a `.hbmat` material asset (path relative to Assets/). When
// present, the MeshInstance's material fields were applied from this asset;
// re-applying the asset (or editing it in the Asset Viewer) refreshes them.
struct MaterialRef {
    std::string asset;
};

// Local-space bounding box, used for click-to-select ray picking.
struct AABB {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
};

// Rigid-body participation in the physics simulation (Jolt). The PhysicsWorld
// creates the backing body lazily and stores its id here; engine code only
// touches this plain data, never the physics SDK.
struct RigidBody {
    // Box/Sphere/Capsule are primitive approximations; Mesh is an exact triangle
    // collider (static only) and ConvexHull is a tight convex wrap (dynamic-safe)
    // - both built from the entity's actual mesh, not a bounding box.
    enum class Shape : u8 { Box, Sphere, Capsule, Mesh, ConvexHull };
    enum class Motion : u8 { Static, Dynamic };

    Shape  shape = Shape::Box;
    Motion motion = Motion::Dynamic;
    glm::vec3 halfExtents{0.5f}; // Box (scaled by the entity's world scale)
    f32 radius = 0.5f;           // Sphere / Capsule radius
    f32 halfHeight = 0.5f;       // Capsule cylinder half-height (caps add radius)
    glm::vec3 centerOffset{0.0f}; // collider center in local space (mesh AABB center)
    f32 friction = 0.5f;
    f32 restitution = 0.2f;

    // Exact collider geometry for Mesh / ConvexHull, in mesh-local space. Filled
    // from the entity's mesh at instantiate (or when the shape is picked in the
    // editor) and NOT serialized - it's rebuilt from the mesh on load, so scenes
    // stay small. Empty -> the shape falls back to a box.
    std::vector<glm::vec3> collisionVertices;
    std::vector<u32> collisionIndices; // triangle list (Mesh shape)

    static constexpr u32 kInvalidBody = 0xFFFFFFFFu;
    u32 bodyId = kInvalidBody; // managed by PhysicsWorld

    // Render interpolation between fixed physics steps (see CharacterController for
    // why). The two most recent stepped world poses; PhysicsWorld lerps/slerps
    // between them by the leftover-accumulator fraction each frame for the drawn
    // Transform. Not serialized - rebuilt at runtime.
    glm::vec3 renderPrevPos{0.0f};
    glm::vec3 renderCurPos{0.0f};
    glm::quat renderPrevRot{1.0f, 0.0f, 0.0f, 0.0f};
    glm::quat renderCurRot{1.0f, 0.0f, 0.0f, 0.0f};
    bool renderPoseInit = false;
};

// Optional human-readable label (debugging / editor).
//
// NOT an identity mechanism. Names are neither unique nor stable (the shipping
// level has two roots called "Cube"; UIScene has seven "UI Label"s; every
// runtime prefab clone shares one name by construction). Anything that needs to
// say "this exact object, across a save/load" uses Guid below.
struct Name {
    std::string value;
};

// STABLE PER-ENTITY IDENTITY. Every entity gets one, minted exactly once in
// Scene::CreateEntity - the single funnel every authored, loaded and spawned
// entity passes through. Serialized into `.hbscene` as a 16-char hex string
// under the "guid" key, so it survives save -> load -> save unchanged.
//
// The two rules that make it worth having:
//   * A LOAD adopts the guid in the file (same object, restored).
//   * A DUPLICATE mints a fresh one (copy/paste, duplicate, prefab
//     instantiate, spawner burst). A duplicate is a NEW object; carrying the
//     source's guid forward would silently alias two entities' persisted state.
// See Scene/EntityGuid.h for the minting/derivation helpers and the invariant
// that guids are unique among LIVE entities.
struct Guid {
    u64 value = 0; // 0 = unset (never true for a live entity)
};

// Marks the ROOT entity of a placed prefab instance, linking it back to the
// source `.hbprefab` (path relative to Assets/). The editor uses this to Apply
// the instance's edits to the source, Revert the instance to the source, or
// Unpack (drop the link). Serialized so links survive save/load and nest (a
// prefab whose subtree contains instances keeps their links). Runtime ignores it.
struct PrefabInstance {
    std::string source; // `.hbprefab` path relative to Assets/
};

// Runtime tag: which loaded scene an entity came from (a streamed/additive scene
// or world-streaming cell). The editor hierarchy groups entities under their
// source scene. NOT serialized - it's set by the loader, and saving a combined
// world flattens everything into one scene file. Entities with no SceneSource
// (the main scene, editor-created) fall under the active-scene group.
struct SceneSource {
    std::string scene; // display name of the originating scene / cell
};

// Per-object layering WITHIN one scene. A level is ONE .hbscene file; each
// object in it is tagged Static (non-moving world geometry - the navmesh source,
// and what the painterly pass exempts) or Dynamic (actors / physics / skeletal
// meshes that move). Full means "no layer tag" and is what a scene FILE header
// carries, since a file is no longer a layer of anything.
//
// There is no UI kind: UI is authored in its own standalone scene (the project's
// uiScene), not as a layer of a level.
enum class SceneKind : u8 {
    Full = 0,  // no layer tag (the scene-file header's value)
    Static,
    Dynamic,
};

inline const char* ToString(SceneKind k) {
    switch (k) {
        case SceneKind::Static:  return "static";
        case SceneKind::Dynamic: return "dynamic";
        case SceneKind::Full:    break;
    }
    return "full";
}

inline SceneKind SceneKindFromString(const std::string& s) {
    if (s == "static")  return SceneKind::Static;
    if (s == "dynamic") return SceneKind::Dynamic;
    return SceneKind::Full;
}

// Runtime tag: which layer an entity belongs to. Serialized per entity
// ("sceneLayer"). Systems query it - the navmesh bakes only Static-layer
// geometry, so dynamic/skeletal meshes never leak into the walkable surface.
struct SceneLayer {
    SceneKind kind = SceneKind::Static;
};

// STREAMING GROUP ("tag"). The authoring unit of tag streaming: the author gives
// an object a tag ("Camp", "Interior_Mill"), the save-time sharder splits each
// tag into spatially-coherent SHARDS, and the runtime spawns/despawns whole
// shards by distance. Per-tag streaming CONFIG (load/unload radius, priority,
// alwaysLoaded, autoShard) lives on the project's tag list, never on the entity.
//
// STORAGE. The id is an interned index into the process-wide table in
// Scene/TagTable.h; the NAME is what serializes, so a `.hbscene` stays diffable,
// hand-editable and portable between projects. 2 bytes keeps the O(entities)
// scans (navmesh fingerprint, shard bucketing) cache-friendly the way SceneLayer
// above already does - a std::string per entity would not.
//
// ORTHOGONAL TO SceneLayer, and both survive: SceneLayer selects the navmesh
// filter and MaterialFlag_PainterlyExempt, Tag selects the streaming group. One
// is not a repurposing of the other.
//
// ABSENCE IS MEANINGFUL: no Tag component == kTagUntagged == always resident,
// never streamed. tags::Assign is the one mutation site and it REMOVES the
// component rather than storing id 0, so the two spellings can never disagree.
//
// A `.hbui` document's entities can NEVER carry one (tags::Taggable refuses):
// UI is asset content, outside the streaming world entirely.
using TagId = u16;
inline constexpr TagId kTagUntagged = 0; // index 0 of the tag table, always resident

struct Tag {
    TagId id = kTagUntagged;
    // BAKED spatial shard index, written by the save-time sharder (P5). -1 =
    // not yet baked. Carried here (and in the file, under "shard") from P4 so
    // the shard bake needs no second scene-format change.
    i32 shard = -1;
};

// PAINT-STROKE ZONE GROUP. Marks the empty node that collects the 3D paint
// strokes (real ribbon/quad mesh entities, not the painterly post pass) belonging
// to ONE streaming zone. See Scene/StrokeZone.h for the whole rule; the short
// version is that there is one group node per tag, the node itself carries that
// `Tag`, and every stroke painted on a surface in that zone is parented under it.
//
// WHY A COMPONENT AND NOT A NAME. The old code found its single global group by
// scanning for an entity literally named "Paint Strokes", so renaming the node in
// the Hierarchy silently forked a second group and two additively-loaded scenes
// each contributed one.
//
// WHY IT IS A BARE MARKER. It used to carry a TagId copied from the node's own Tag,
// and NOTHING kept the copy in sync: tags::RemoveTag remaps every `Tag` when a tag
// is deleted, and tags::AssignSubtree rewrites `Tag` from the Inspector. Either one
// left a group whose copy said "Mill" and whose Tag said something else - after
// which strokes painted on always-resident terrain could be parented into a
// streaming atom and despawn with it. The zone is now DERIVED
// (strokezone::GroupZone = the node's Tag, absent meaning Untagged), which removes
// the invariant instead of adding a third site that must be kept in lockstep.
//
// Still serialized as "strokeGroup": "<tag name>", the same NAME-not-id spelling
// `Tag` uses - now as a readable echo of that tag, so a `.hbscene` diff says which
// zone a group collects. Presence is what is parsed back; the id is not.
struct StrokeGroup {};

// RUNTIME-ONLY shard membership, stamped by the streamer on everything a shard
// spawn created, and inherited by anything created LATER on behalf of a member
// (spawn::DoBurst copies it onto each Spawned root). NEVER serialized - not to a
// `.hbscene`, not to a snapshot - because it describes the current residency of a
// live world, not authored content. `Tag::shard` is the authored/baked fact; this
// is the runtime one.
//
// WHY IT EXISTS SEPARATELY FROM Tag. Despawn must be a closure over LIVE state, not
// a replay of the load-time created list: Instantiate itself creates entities after
// filling that list (modular-character parts), and terrain chunks, destruction
// debris, world-UI surfaces and spawned NPCs all appear afterwards. Membership has
// to be readable from the registry at despawn time, which is what this is.
//
// `index` is an index into the STREAMER's flat shard vector (which is built from the
// scene file header, so it is stable for a given binding), not a per-tag ordinal.
struct StreamShard {
    u32 index = 0;
};

// Provenance of a MeshInstance's mesh, so scenes can be saved and reloaded:
//   "prim:cube" / "prim:sphere"          - procedural primitives
//   "uaf:<path-relative-to-Assets>#<n>"  - submesh n of a .uaf mesh asset
struct MeshRef {
    std::string source;
};

// Spatial sound emitter. `asset` is a .uaf Audio path relative to Assets/.
// The AudioSystem creates a positional voice (3D attenuation between min and
// max distance) that follows the entity's world transform each frame.
struct AudioSource {
    std::string asset;
    std::string bus = "SFX"; // mixer bus (empty/unknown = Master)
    f32 volume = 1.0f;
    f32 minDistance = 1.0f;  // full volume inside this radius
    f32 maxDistance = 30.0f; // silent beyond
    bool loop = true;
    bool autoplay = true;
    bool playing = false;      // runtime state (autoplay sets it on first update)
    bool autoStarted = false;  // runtime latch: autoplay has fired this play session

    static constexpr u32 kNoVoice = 0;
    u32 voiceId = kNoVoice; // managed by AudioSystem; reset to re-create
};

// Marks an entity as a DIALOGUE ACTOR: a speaking character whose voice lines
// emit 3D from its world position instead of flat 2D. When a dialogue Line's
// `speaker` matches this actor's `speaker`, the clip plays positionally from here
// (with `bus`/distance). Speaker-name -> entity binding also falls back to any
// entity whose Name matches the speaker (a "General" 3D voice with no explicit
// component); this component is the explicit override for precise bus/range.
struct DialogueActor {
    std::string speaker;          // dialogue speaker name this actor voices ("" = its Name)
    std::string bus = "Dialogue"; // voice mixer bus ("" = Master)
    f32 minDistance = 1.0f;       // full volume inside this radius
    f32 maxDistance = 35.0f;      // silent beyond
};

// Plays skeletal animation clips on a skinned MeshInstance entity. The
// TARGET skeleton comes from the entity's own mesh asset (MeshRef
// "uaf:<rel>#<n>"); `sourceAsset` optionally RETARGETS clips authored in
// another mesh asset onto this skeleton - joints are matched by name at
// sample time (common DCC prefixes stripped), unmatched joints keep their
// bind pose, and translations scale by the skeletons' bone-length ratio.
struct Animator {
    std::string sourceAsset; // .uaf providing the clips ("" = own mesh asset)
    i32 clip = 0;            // index into the source's clips
    f32 time = 0.0f;         // playhead, seconds
    f32 speed = 1.0f;
    bool loop = true;
    bool playing = true;
    // Root motion: the motion root's HORIZONTAL translation moves the entity's
    // Transform instead of the skeleton (the character animates in place and
    // actually travels through the world, surviving loop wraps).
    bool rootMotion = false;
    // Crossfade duration (seconds) when the clip changes. A clip switch - by hand,
    // by MotionMatching, or by a cutscene marker - eases from the pose held at the
    // switch into the new clip over this window instead of popping. 0 = hard cut.
    f32 blendTime = 0.15f;

    // Runtime state (managed by anim::UpdateSkeletal; not serialized).
    std::vector<glm::mat4> palette; // per-joint global * inverseBind
    std::vector<i32> channelToJoint; // cached source-channel -> target-joint map
    u64 mapKey = 0;                  // identity of the cached mapping
    f32 translationScale = 1.0f;     // retarget bone-length ratio
    i32 rootChannel = -1;            // clip channel driving root motion
    glm::vec3 lastRootPos{0.0f};     // previous frame's sampled root position
    f32 lastRootTime = 0.0f;
    bool rootTrackValid = false;     // lastRootPos primed (no first-frame jump)

    // Crossfade runtime state. On a clip switch the FINAL local pose of the frame
    // before the switch (already snapshotted below) is eased into the new clip's
    // pose over blendTime. The snapshot is the posed local transforms (pre-body-
    // scale), refreshed every frame so a switch mid-blend re-eases from wherever
    // the blend currently is. Not serialized. See anim::UpdateSkeletal.
    i32 activeClip = -1;             // clip currently being posed (switch detector)
    bool blending = false;           // a crossfade is in progress
    f32 blendElapsed = 0.0f;         // seconds into the current crossfade
    bool blendPoseValid = false;     // the snapshot below holds a real pose
    std::vector<glm::vec3> blendPos; // snapshot local translations
    std::vector<glm::quat> blendRot; // snapshot local rotations
    std::vector<glm::vec3> blendScale; // snapshot local scales
};

// ROOT of a MODULAR CHARACTER: a character built from swappable PARTS (head,
// torso, upper-arm, ...) that all skin to ONE shared skeleton. The root owns the
// single Animator (one pose/palette per frame) + a MeshRef to the skeleton-bearing
// .uaf (with an INVALID-mesh MeshInstance so it poses but draws nothing), and this
// component records the active loadout. Each active part is a child entity tagged
// SkinnedPartRef -> this root, borrowing the root's palette in CollectDrawItems.
// Seams stay solid because the .hbchar build canonicalizes seam vertices so all
// parts skin bit-identically at the boundary. See Assets/CharacterAsset + docs.
struct Character {
    std::string asset;                       // .hbchar path (relative to Assets/)
    // slot name -> active variant id ("" = the slot is empty/hidden this loadout).
    std::unordered_map<std::string, std::string> activeVariant;
    // Is `activeVariant` AUTHORED, or was it RESOLVED from the .hbchar's defaults?
    //
    // character::Instantiate fills activeVariant from the asset whenever it is empty,
    // and the serializer used to write the result unconditionally. One load+save later
    // the entity carried a frozen custom loadout that the author never chose - and
    // because Instantiate then sees a non-empty map, changing a slot's DEFAULT VARIANT
    // in the .hbchar could never again reach any already-saved scene. Only a real
    // equip (character::SetSlotVariant) or a file that already carried overrides sets
    // this; the serializer writes the map only when it is set. NOT serialized itself -
    // "the file carried overrides" is exactly "the map was non-empty on parse".
    bool variantAuthored = false;
    std::string loadout;                     // current named loadout ("" = custom)
    // Runtime: slot -> the child part entity currently spawned (null = none).
    // Rebuilt whenever the loadout changes; NOT serialized.
    std::unordered_map<std::string, entt::entity> liveParts;
};

// Tags a mesh entity as a PART of a modular Character: instead of owning an
// Animator, it BORROWS the character root's Animator.palette at draw time (one
// shared pose for every part). Its local Transform must stay identity - skinning
// is authored in the shared skeleton's model space.
struct SkinnedPartRef {
    entt::entity character = entt::null; // the Character root entity
    std::string slot;                    // which slot this part fills
    std::string variant;                 // which variant id it is
};

// Keyframe animation of the entity's LOCAL Transform (TRS keys, linear /
// slerp interpolation). Driven by anim::Update; edited in the Timeline panel.
struct AnimationTrack {
    struct Key {
        f32 time = 0.0f;
        glm::vec3 position{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 scale{1.0f};
        // Curve used to reach THIS key from the previous one (serialized as an int;
        // append-only). InOutCubic reads as a natural "ease into the pose".
        ease::Curve ease = ease::Curve::InOutCubic;
    };
    std::vector<Key> keys; // kept sorted by time
    f32  duration = 5.0f;  // seconds
    f32  time = 0.0f;      // playhead
    f32  speed = 1.0f;
    bool playing = false;
    bool loop = true;
};

// A game camera. The primary camera drives rendering in play mode and in the
// runtime; the editor's scene camera is used otherwise (see Scene/CameraSystem).
//
// `mode` is a behaviour preset deciding where the camera positions itself each
// frame; `rotation` decides how it aims. In the default Static/Free combination
// the camera simply uses its own world Transform (looks along local -Z), so
// legacy scenes are unchanged. Follow/look targets are referenced by entity
// NAME (stable across save/load); the system resolves them each frame.
struct CameraComponent {
    f32 fovY = 60.0f; // vertical field of view, degrees
    f32 nearZ = 0.1f;
    f32 farZ = 500.0f;
    bool primary = true;

    // Where the camera sits each frame.
    enum class Mode : u8 {
        Static = 0,    // uses the entity's own Transform (default)
        FirstPerson,   // eye at the target + offset; playerLook aims it (else faces the target)
        ThirdPerson,   // trails the target on a boom (distance behind + pitch up)
        Orbit,         // orbits the target, auto-spinning at spinSpeed
        Distance,      // holds a fixed distance from the target along a set direction
        Spline,        // travels along a named CameraSpline path
    };
    Mode mode = Mode::Static;

    // How the camera aims each frame (layered on top of the mode).
    enum class RotationMode : u8 {
        Free = 0,      // keep the orientation the mode produced (entity's for Static)
        LookAt,        // snap to face the target
        SlowFollow,    // damp toward the target (rotationDamping)
        Spin,          // continuous yaw at spinSpeed (ignores target)
        Fixed,         // hold a fixed world rotation (fixedEuler)
    };
    RotationMode rotation = RotationMode::Free;

    std::string target;                  // entity to follow / look at ("" = none)
    glm::vec3 offset{0.0f, 1.7f, 0.0f};  // eye / boom-pivot offset from the target
    f32 distance = 6.0f;                 // boom length (third person / orbit / distance)
    f32 yaw = 0.0f;                      // boom yaw offset, degrees
    f32 pitch = 15.0f;                   // boom pitch, degrees
    f32 positionDamping = 10.0f;         // follow smoothing (0 = instant snap)
    f32 rotationDamping = 8.0f;          // aim smoothing (SlowFollow)
    f32 spinSpeed = 25.0f;               // deg/s (Spin / Orbit auto-rotate)
    glm::vec3 fixedEuler{0.0f};          // Fixed rotation (pitch/yaw/roll degrees)

    std::string spline;                  // CameraSpline entity name (Spline mode)
    f32 splineSpeed = 0.1f;              // progress per second (0..1 across the path)
    bool splineLoop = true;

    // Player look (First Person AND Third Person): when on, the mouse + right
    // gamepad stick aim the camera (instead of it just trailing the target's
    // facing). The character then moves camera-relative (CharacterController)
    // for standard third/first-person controls. The two modes accumulate look
    // through the SAME code and the same fields below - they differ only in
    // where the camera sits:
    //   Third Person - the look orbits the target on a boom.
    //   First Person - the look IS the eye's aim, and its YAW is written onto
    //                  the target's Transform (the body turns; pitch stays
    //                  camera-only). While that is happening the camera owns the
    //                  body's facing, so CharacterController::faceMoveDir is
    //                  suppressed for that frame (externalFacing below).
    // With playerLook off, both modes keep their old target-derived aim exactly.
    bool playerLook = true;
    f32  lookSensitivity = 0.2f;         // degrees per mouse pixel
    f32  lookStickSpeed = 160.0f;        // degrees/second at full right-stick
    bool invertLookY = false;
    f32  lookPitchMin = -80.0f;          // clamp (degrees, looking up)
    f32  lookPitchMax = 80.0f;           // clamp (degrees, looking down)
    // Camera collision (Third Person / Orbit / Distance): when on, the boom is
    // pulled in so the camera never clips through world geometry between it and
    // the target. Needs a physics world (the runtime / play mode).
    bool collide = true;
    f32  collisionMinDistance = 0.6f;    // never pull closer than this
    f32  collisionPadding = 0.25f;       // keep the camera this far off the hit
    // How fast the boom RETURNS after a collision clears. Pulling in must be
    // instant (or the camera punches through the wall for a few frames), but
    // easing back out is what stops the shot snapping when the player rounds a
    // corner. 0 = the old instant behaviour.
    f32  collisionReturnSpeed = 6.0f;    // metres/second the boom extends back out

    // Cinematic rig (handheld / breathing / framing). Authored per camera and
    // overridable by a camera zone, like every other value here. All-zero
    // amplitudes make it an exact no-op. See Scene/CameraRig.h.
    cam::CinematicSettings cinematic;

    // Runtime state (advanced by cam::Update while the camera is live; not
    // meaningfully serialized beyond their authored start values).
    f32 orbitAngle = 0.0f;               // current orbit/spin yaw, degrees
    f32 splineT = 0.0f;                  // current progress along the spline (0..1)
    f32 lookYaw = 0.0f;                  // accumulated player look (third person)
    f32 lookPitch = 0.0f;
    bool lookInit = false;               // seed lookYaw/Pitch from the target once
};

// A trigger volume that switches the active game camera when a tracked entity
// enters it (an oriented box: the entity's world Transform places/rotates/scales
// it, halfExtents are local). Zones let one level blend between camera rigs as
// the player moves (e.g. a first-person zone inside a third-person world). When
// several zones overlap, the highest `priority` wins; with none active the
// scene's primary CameraComponent is used. Switches blend smoothly via the
// target camera's positionDamping/rotationDamping.
struct CameraZone {
    glm::vec3 halfExtents{5.0f}; // local box half-size (scaled by world scale)
    std::string camera;          // CameraComponent entity to activate ("" = none)
    std::string track;           // entity tested against the volume ("" = active
                                 // camera's target, else the camera itself)
    int priority = 0;            // higher wins when zones overlap
    bool enabled = true;

    // Inline camera override: instead of pointing `camera` at a separate
    // CameraComponent entity, the zone can carry the camera behaviour (mode,
    // speed, distance, fov, ...) to switch to directly when entered. When
    // `settings.target` is empty the zone inherits the base camera's target, so
    // it can change the camera mode while still following the player. `settings`
    // also holds the live orbit/spline runtime state while the zone is active.
    bool useSettings = false;
    CameraComponent settings;

    bool active = false;         // runtime state (set by cam::Update)
};

// A world volume that drives the ADAPTIVE MUSIC when the player enters it (FMOD-
// style "zones"): an oriented box (the entity's world Transform places/rotates/
// scales it, halfExtents are local). On the frame the player enters the highest-
// priority enabled zone, it crossfades the music to `musicState` and/or sets a
// music `parameter`. Author a large low-priority "base" zone covering the level
// plus smaller high-priority zones (Combat, Boss) so leaving one re-enters the
// base. Reference states/parameters by name from the project's music graph.
struct MusicZone {
    glm::vec3 halfExtents{8.0f};  // local box half-size (scaled by world scale)
    std::string musicState;       // crossfade to this state on enter ("" = leave state)
    std::string parameter;        // optional music parameter to set on enter ("" = none)
    f32 parameterValue = 1.0f;    // value the parameter is set to on enter
    f32 fadeSeconds = -1.0f;      // crossfade seconds (-1 = graph default)
    int priority = 0;             // higher wins when zones overlap
    bool enabled = true;
    bool active = false;          // runtime state (set by the engine's music-zone update)
};

// A MATERIAL VOLUME (the box-brush WORLD TOOL): a box placed in the scene that PROJECTS a material
// onto the geometry it encloses, with a configurable soft falloff. Per the design's core rule the
// box never edits geometry - it produces a spatial WEIGHT FIELD (mat::BoxBrush) that feeds a
// material LAYER; the editor's "Bake Volumes" action composites every enabled volume into each
// overlapping mesh's PAINT CANVAS as a non-destructive overlay (mat::BakeMeshVolumesOverlay), so the
// projected material blends over the mesh's own material at its true world size and is removable.
// The box's world placement + rotation IS this entity's Transform; `halfExtents` is its local
// half-size. Enum-typed authoring lives as ints so this scene header stays free of the Material
// module; the editor maps them onto the mat:: enums when it builds the box brush.
struct MaterialVolumeComponent {
    glm::vec3 halfExtents{2.0f}; // local box half-size (scaled + rotated by this entity's Transform)

    // Weight field (mat::Falloff on the box SDF).
    int falloffType = 2;         // mat::FalloffType (0 Constant,1 Linear,2 Smoothstep,3 Smoother,4 In,5 Out)
    f32 falloffGamma = 1.0f;     // final shaping exponent (>0; 1 = none)
    f32 falloffWidth = 0.25f;    // fraction of each half-extent the edge fades over
    f32 strength = 1.0f;         // 0..1 multiplier on the produced weight

    // Material this volume projects. `material` is a `.hbmat` ref (rel to Assets/); when empty the
    // inline OpenPBR-legacy values below define the surface.
    std::string material;
    glm::vec4 color{0.8f, 0.8f, 0.8f, 1.0f}; // inline base colour (rgb) + opacity (a)
    f32 metallic = 0.0f;
    f32 roughness = 0.6f;

    // Tiling / projection of the projected material (size-independent; mat::BoxProjection as int).
    int projection = 0;          // 0 World, 1 Local, 2 Triplanar
    glm::vec3 tileMeters{1.0f};  // metres per material tile, independent per axis

    // Layer compositing.
    int blend = 0;               // mat::BlendMode (0 Linear, 1 Height, 2 Height+Noise)
    f32 opacity = 1.0f;          // layer strength on top of the mask

    bool enabled = true;         // include in the bake + draw the wireframe
    int bakeResolution = 1024;   // paint-canvas resolution the bake writes
};

// A CSG BLOCKOUT BRUSH (the Unreal-style box brush): an editable box PRIMITIVE that becomes real
// level geometry - NOT a decal or a projection. An Additive brush generates a solid box mesh; a
// Subtractive brush boolean-CARVES doorways / windows / room interiors out of the additive brushes
// it overlaps (brush::BuildEntityMesh -> csg::CarveBox). The generated mesh, its AABB, and its
// static triangle collision are DERIVED state, rebuilt by brush::Update whenever `dirty` is set and
// never serialized (exactly like terrain chunks regenerate from the heightfield). The box's world
// placement / rotation / scale IS this entity's Transform; `halfExtents` is its local half-size.
struct BrushComponent {
    glm::vec3 halfExtents{1.0f}; // local box half-size (placed + scaled by this entity's Transform)
    int op = 0;                  // csg::Op: 0 = Additive (adds solid), 1 = Subtractive (carves)
    f32 uvScale = 2.0f;          // metres per texture tile on the generated faces
    bool dirty = true;           // NOT serialized; re-derived true on load -> geometry rebuilds
};

// Runtime-only auto-despawn timer for a ONE-SHOT `.hbvfx` effect spawned via game::SpawnEffect.
// NOT serialized (the scene serializer never writes it): when a non-looping effect has finished
// emitting and its last particles have died, `remaining` reaches 0 and the spawn system destroys
// the entity. Looping effects get no EffectLifetime and persist until explicitly removed.
struct EffectLifetime {
    f32 remaining = 0.0f; // seconds until the spawned effect entity is destroyed
};

// A ROOM / enclosed acoustic space: a box region whose dimensions + wall/floor/ceiling acoustic
// materials define the reverberation + early reflections heard while the LISTENER is inside it.
// The highest-priority enabled space containing the listener drives the spatial backend's room
// acoustics (AcousticWorld); outside all spaces the listener hears no room (dry / outdoors). Box
// uses this entity's Transform, like MusicZone. Materials are acoustic-preset names (see
// Assets/AcousticMaterial.h). Part of the physically-informed audio system (see
// docs/Design-HeartbreakAcoustics.md).
struct AcousticSpace {
    glm::vec3 halfExtents{6.0f, 3.0f, 6.0f};              // local box half-size (scaled by world scale)
    std::string wallMaterial = "Drywall / Sheetrock";    // the four side walls (-x,+x,-z,+z)
    std::string floorMaterial = "Concrete (sealed)";     // -y face
    std::string ceilingMaterial = "Acoustic Ceiling Tile"; // +y face
    f32 reverbGain = 1.0f;      // scales the late-reverb tail level
    f32 reverbTime = 1.0f;      // scales RT60 (>1 = longer tail)
    f32 reflectionGain = 1.0f;  // scales the early-reflections level
    int priority = 0;           // higher wins when spaces overlap
    bool enabled = true;
    bool active = false;        // runtime state (set by the room driver; not serialized)
};

// An ACOUSTIC PORTAL: a doorway / window / opening that lets sound pass through what is
// otherwise a wall. A box region; when a source->listener transmission ray passes through it, the
// wall it hits there is treated as this opening instead - `openness` 1 = fully open (sound passes),
// 0 = closed (uses `closedMaterial`, e.g. a shut door). Lets a dynamic door open/close its acoustic
// hole without re-authoring geometry. Box uses this entity's Transform. Part of the physically-
// informed audio system (see docs/Design-HeartbreakAcoustics.md).
struct AcousticPortal {
    glm::vec3 halfExtents{0.9f, 1.2f, 0.25f}; // a doorway-ish thin box (local, scaled by world)
    std::string closedMaterial = "Wood Panel"; // acoustic material when closed (openness 0)
    f32 openness = 1.0f;        // 0 = shut (closedMaterial), 1 = fully open (sound passes freely)
    bool enabled = true;
};

// A path of WORLD-space control points a Spline-mode camera travels along (a
// Catmull-Rom curve through the points). Reference it by this entity's Name from
// a CameraComponent's `spline` field.
struct CameraSpline {
    std::vector<glm::vec3> points;
    bool loop = true;
};

// A UI canvas root (like Unity's Canvas + CanvasScaler). Entities carrying
// this component start a UI tree: UIElement entities parented (directly or
// transitively, via the Parent component) under it lay out inside this
// canvas's reference rectangle. Canvases draw in ascending sortOrder.
// Elements with NO canvas ancestor fall back to the project-wide canvas
// configuration (legacy scenes keep working).
struct UICanvas {
    u32 scaleMode = 1;       // ui::ScaleMode: 0 stretch, 1 match height, 2 pixel
    f32 refWidth = 1920.0f;  // reference resolution the canvas is authored at
    f32 refHeight = 1080.0f;
    int sortOrder = 0;
    bool visible = true;
    // --- World-space ("physical"/diegetic) canvas --------------------------------
    // When set, this canvas renders to a texture shown on a LIT quad in the 3D
    // scene (paper-like: shaded by scene lighting) instead of the screen overlay.
    // The page lies in the canvas entity's local XZ plane facing +Y - rotate the
    // entity to mount it (on a notebook, a wall...), parent it under an object to
    // follow it. Panels, animators, tokens and the UIManager all work unchanged
    // (same layout walk). scaleMode is ignored: the canvas is exactly ref-sized.
    bool worldSpace = false;
    f32 worldWidth = 1.0f;   // page width in meters; height = worldWidth*refH/refW
    f32 emissive = 0.0f;     // 0 = pure lit paper; >0 adds self-glow (readability)
    u32 rtWidth = 0;         // render-target resolution (0 = refWidth/refHeight)
    u32 rtHeight = 0;
    // --- Interaction (world-space canvases only) ---------------------------------
    // `occlude`: the page is hidden behind world geometry, i.e. a solid collider
    // nearer than the page along the pick ray makes it un-clickable (the fix for
    // "a button behind a wall is pressable"). Clear it for a hologram / a wrist
    // screen parented to the player / anything deliberately drawn through walls.
    // NOTE: only entities with a RigidBody (and terrain) occlude - a MeshInstance
    // with no collider blocks nothing. "If it should block interaction, give it a
    // collider."
    bool occlude = true;
    // Maximum pick distance in meters; 0 = unlimited (the ray's own range).
    // Deliberately 0 by default: a non-zero default would silently make already
    // authored pages inert past that distance.
    f32 interactRange = 0.0f;
    // Runtime only (NOT serialized): the per-canvas UI render target + the hidden
    // lit quad entity (UISurface) that displays it.
    rhi::TextureHandle rtTexture;
    u32 rtTexW = 0, rtTexH = 0;      // size rtTexture was created at
    entt::entity surface = entt::null;
};

// A screen-space UI element with a Unity-style RectTransform. Layout is
// hierarchical: the element's rectangle is computed inside its PARENT's
// rectangle (the canvas for top-level elements, the parent element's rect for
// nested ones):
//   * anchorMin/anchorMax: normalized points of the parent rect the element
//     anchors to. Equal anchors = fixed size (`size` is the pixel size);
//     different anchors = the element stretches with the parent (`size` then
//     acts as Unity's sizeDelta, i.e. growth beyond the anchor region).
//   * pivot: the normalized point of the element's own rect that `offset`
//     positions relative to the anchor region's pivot point.
// Buttons track hover/click each frame (C# scripts read them via
// Entity.UIClicked / set text-fill-visibility back).
struct UIElement {
    enum class Type : u8 {
        Panel = 0,       // colored (optionally textured) rect, optional caption
        Label = 1,       // text only
        Button = 2,      // interactive rect + caption
        Image = 3,       // textured rect (color tints)
        ProgressBar = 4, // background rect + fill (linear or radial "wheel")
        Slider = 5,      // draggable track + handle, `value` in [0,1]
        Toggle = 6,      // on/off, `toggled`
        Selector = 7,    // one-of-N options (`options`/`selected`), e.g. High/Med/Low
        ScrollView = 8,  // clips + scrolls its children (mouse wheel / autoScroll)
        TextInput = 9,   // editable text box; `text` is the buffer
    };
    Type type = Type::Label;
    // Optional STABLE, author-chosen id (P5, D2). Unique within a document by
    // convention (like `action`); empty = none. A stable handle that future data-
    // binding, localization, animation-targeting and accessibility can reference an
    // element by, instead of abusing the non-unique `name`/`action`. NOT an identity
    // for persistence - a `.hbui` entity still carries no guid.
    std::string id;
    std::string text;            // Label / Button / ProgressBar caption (authoring)
    // Runtime-resolved caption: when non-empty the UI renderer shows this instead
    // of `text`. The engine fills it on screens that use {token}s ({backend}/{gpu}/
    // {audio}/{version}/{progress}). NOT serialized.
    std::string runtimeText;
    glm::vec2 anchorMin{0.5f, 0.5f};
    glm::vec2 anchorMax{0.5f, 0.5f};
    glm::vec2 pivot{0.5f, 0.5f};
    glm::vec2 offset{0.0f};       // anchored position (canvas px)
    glm::vec2 size{300.0f, 80.0f}; // pixel size / sizeDelta when stretching
    // 2D render transform about the element's pivot (drives UI animation): scale
    // THEN rotation. Identity (rotation 0, scale {1,1}) = plain axis-aligned rect.
    f32 rotation = 0.0f;          // degrees, clockwise in screen space
    glm::vec2 scale{1.0f, 1.0f};  // per-axis scale about the pivot
    glm::vec4 color{1.0f};        // text color (Label) / fill or tint otherwise
    f32 textSize = 28.0f;         // text height in canvas pixels
    std::string font;             // `.uaf` Font asset (empty = engine default)
    // Caption alignment within the element rect (default = centered, the legacy
    // behavior). Applies to Label/Button/Panel/ProgressBar text.
    enum class HAlign : u8 { Left = 0, Center = 1, Right = 2 };
    enum class VAlign : u8 { Top = 0, Center = 1, Bottom = 2 };
    HAlign hAlign = HAlign::Center;
    VAlign vAlign = VAlign::Center;
    bool visible = true;
    // Fit to parent: the element always covers its entire parent rect (the
    // whole canvas for top-level elements); the RectTransform is ignored.
    bool fullscreen = false;

    // --- Layout constraints (P6) ---------------------------------------------
    // Clamp the laid-out SIZE (post-RectTransform, resized about the pivot in place).
    // 0 on an axis = unconstrained (default = no-op, byte-identical to pre-P6).
    // `aspectRatio` (width/height; 0 = off) then fits the height to the width. Applied
    // to both free elements and layout-group-placed children. Skipped for fullscreen.
    glm::vec2 minSize{0.0f};
    glm::vec2 maxSize{0.0f};
    f32 aspectRatio = 0.0f;

    // Image / textured Panel: a texture .uaf (relative to Assets/).
    std::string texture;
    // Sprite-frame animation: optional list of texture .uaf paths; a UIAnimator's
    // SpriteFrame track indexes this list and swaps `texture` to the active frame.
    std::vector<std::string> frames;

    // Button game-flow action, handled by the engine when the button is clicked:
    // "play" (menu -> loading -> gameplay), "menu" (back to main menu), "quit".
    // Empty = no built-in action (drive it from a script instead).
    std::string action;

    // ProgressBar: fill fraction + color; radial sweeps a "wheel" instead.
    f32 fill = 0.65f;
    glm::vec4 fillColor{0.86f, 0.27f, 0.33f, 1.0f};
    bool radial = false;

    // Slider: normalized value + draggable handle. Toggle: on/off. Selector: one of
    // `options` by `selected` index. `fillColor` = the active/handle tint for all three.
    f32 value = 0.5f;                     // Slider position [0,1]
    bool toggled = false;                 // Toggle state
    std::vector<std::string> options;     // Selector choices (e.g. High/Med/Low)
    int selected = 0;                     // Selector index into `options`

    // ScrollView: children lay out inside a content rect shifted by -scrollPos
    // and the view rect clips the whole subtree. contentSize 0 = auto (measured
    // from the laid-out children each frame). autoScroll drifts the content at
    // px/s (credits roll); with autoScrollLoop the content re-enters from the
    // far side once it has fully scrolled past. `fillColor` tints the scrollbar
    // thumb (hidden while auto-scrolling); `color` is the background (a=0 none).
    glm::vec2 contentSize{0.0f};          // scrollable extent (0 = auto-measure)
    glm::vec2 scrollPos{0.0f};            // current offset into the content (px)
    f32 scrollSpeed = 40.0f;              // px per mouse-wheel notch
    bool scrollVertical = true;           // wheel + autoScroll move Y
    bool scrollHorizontal = false;        // ... or X (vertical wins when both)
    f32 autoScroll = 0.0f;                // content drift px/s (0 = manual)
    bool autoScrollLoop = false;          // wrap around (credits) vs stop at end
    glm::vec2 contentExtent{0.0f};        // measured content size (runtime)
    glm::vec2 viewExtent{0.0f};           // laid-out view size (runtime)

    // TextInput: `text` is the edit buffer. Click (or Enter/gamepad-A while
    // focused) starts editing; Enter commits (`changed` fires), Escape cancels.
    // `placeholder` shows grayed while the buffer is empty and not editing.
    std::string placeholder;
    int maxLength = 64;                   // buffer cap (characters)

    // --- Skinning (U5) -------------------------------------------------------
    // Per-state tint for interactive widgets. Alpha 0 = "unset" sentinel: the
    // legacy automatic multiply is used (hover 1.22x, pressed 0.72x, disabled
    // 0.5x gray), so pre-U5 widgets are byte- and pixel-identical.
    glm::vec4 hoverColor{0.0f};
    glm::vec4 pressedColor{0.0f};
    glm::vec4 disabledColor{0.0f};
    // Focused/selected are the two visual states the U5 skin lacked. Same alpha-0
    // "unset" sentinel, but with NO legacy auto-multiply fallback: unset = the state
    // simply does not recolor (backward-compatible - old widgets never had them).
    // `focusedColor` recolors the keyboard/gamepad-focused widget; `selectedColor`
    // an "on" widget (a toggled Toggle). NOTE (P4 scope): both are applied through the
    // interactive state-fill path, which today only the Button case uses - a Toggle/
    // Selector still paints its on-state via `fillColor`, so a selectedColor on one of
    // those is currently inert. Broadening per-widget state theming is a follow-up.
    glm::vec4 focusedColor{0.0f};
    glm::vec4 selectedColor{0.0f};
    bool enabled = true;                  // false = inert + disabled look
    std::string hoverSound;               // `.uaf` Audio, played on hover-enter
    std::string clickSound;               // `.uaf` Audio, played on click

    // Widget-part textures (empty = flat-color legacy look), resolved through
    // the same path->index UI texture cache as `texture`. `text`ure is the
    // Button/Panel/Image base; these skin the sub-parts.
    std::string trackTexture;             // Slider groove
    std::string fillTexture;              // Slider fill / ProgressBar fill
    std::string handleTexture;            // Slider handle
    f32 handleSize = 0.0f;                // Slider handle px (0 = auto from height)
    std::string onTexture;                // Toggle on
    std::string offTexture;               // Toggle off
    std::string hoverTexture;             // Button hover state
    std::string pressedTexture;           // Button pressed state
    std::string disabledTexture;          // Button/TextInput disabled state
    std::string cellTexture;              // Selector option cell

    // 9-slice border in SOURCE-texture pixels (L,T,R,B); 0 = plain stretch. The
    // corners keep their native size, edges stretch (Unity-style sprite border).
    glm::vec4 slice{0.0f};
    // Word-wrap the caption to the element width (Label/Button/Panel/TextInput).
    bool wrap = false;

    // --- Style reference (P4: reusable themes) -------------------------------
    // A `.hbtheme` asset + a named style within it. When set, ui::style::ApplyStyle
    // fills THIS element's UNSET skin fields (the alpha-0 / empty-string sentinels
    // above) from the named style at emit time, so a whole set of widgets share one
    // editable look. The element's own set fields always win (per-element override).
    // Empty theme = no styling (every field stays exactly as authored).
    std::string styleTheme;
    std::string styleName;

    // Per-element render EFFECT (P7 custom UI materials): 0 = normal; 1 = grayscale/
    // desaturate. Stamped into every vertex of the element (background, text, parts);
    // the UI shader branches on it - no blend-state or pipeline change, so the whole UI
    // is still one bindless draw. Extensible (add ids + a shader branch). 0 = identical.
    u32 effect = 0;

    // --- Focus graph (P8): authored directional-navigation overrides ---------
    // Each names a target element `id` (P5) to focus when the keyboard/gamepad moves
    // that way from this widget; empty = fall back to the geometric nearest-in-
    // direction pick. The designer wires the flow (Play -> Settings -> Quit) instead
    // of trusting geometry, and an unresolvable id also falls back to geometric.
    std::string navUp, navDown, navLeft, navRight;

    // --- Tooltip (P9): a composable ATTRIBUTE, not a new widget type ----------
    // Hover this element for `tooltipDelay` seconds and a small popup shows `tooltip`
    // (wrapped, clamped on-screen, drawn on top) just below/above the element. Empty =
    // none. Works on ANY element - it is an attribute, so it needs no new Type enum
    // value or emit-switch arm (the composition the overhaul favours over monolithic
    // widget classes).
    std::string tooltip;
    f32 tooltipDelay = 0.6f;

    // --- Tab switching (P9): composable ATTRIBUTES, not a new widget type ----------
    // A "tab" is any clickable element (usually a Button) that carries a `tabTarget`
    // (the P5 `id` of the content element to SHOW when it is clicked) and a `tabGroup`
    // (a name shared by the sibling tabs). Clicking one shows its target, hides every
    // other target in the same group, and sets `toggled` on the active tab (clearing the
    // others) so a selectedColor highlights it. Empty tabTarget = not a tab. No new Type,
    // no new component - tabs compose from existing elements + ids (see ui::ProcessTabs).
    std::string tabGroup;
    std::string tabTarget;

    // --- Collapsible / foldout (P9): another composable attribute ------------------
    // A clickable element with `collapseTarget` (a P5 `id`) TOGGLES that element's
    // visibility when clicked (accordion sections, foldable panels, tree nodes), and
    // sets its own `toggled` to the new expanded state so a caret / selectedColor can
    // reflect open vs closed. Empty = not a foldout. Independent of tabs (a tab SETS
    // visibility exclusively; a foldout TOGGLES its own). Handled by ui::ProcessTabs.
    std::string collapseTarget;

    // --- Data-binding (P9.4): model keys -> runtime fields --------------------------
    // Each names a ui::UIDataModel key whose value is pushed into the matching RUNTIME
    // field every frame by ui::ResolveBindings (never the serialized authored field, the
    // same contract as runtimeText). Empty = not bound. bindText->runtimeText (numbers
    // format), bindValue->value+fill, bindVisible->visible, bindTexture->texture. This
    // unifies the ad-hoc dynamic-content paths (tokens, settings, interact/caption pushes).
    std::string bindText;
    std::string bindValue;
    std::string bindVisible;
    std::string bindTexture;

    bool hovered = false;            // runtime state (UISystem)
    bool clicked = false;            // pressed this frame (runtime state)
    // HELD for as long as the pointer's button is DOWN over this element - the
    // press state of the hover/press/release triple. `clicked` is a one-frame
    // down-EDGE, so a visual driven by it alone flickers for a single frame and a
    // 3D button the player is holding looks unpressed. RELEASE is the falling edge
    // of this flag (the per-frame clear puts it back), so no fourth flag is needed.
    bool held = false;
    bool changed = false;            // value changed this frame (slider/toggle/selector)
    bool dragging = false;           // slider grabbed (persists across frames while held)
    bool prevHovered = false;        // runtime: hover-enter edge (drives hoverSound)
    u32 textureIndexCache = 0;       // resolved bindless index (runtime)
    bool textureResolved = false;    // reset to re-resolve `texture`
    // --- Dirty flags (P5 foundation; NOT serialized) -------------------------
    // Per-element invalidation set by ui::MarkElementDirty at mutation points. The
    // full layout/emit walk still runs today and clears layoutDirty each frame (so
    // it is observable via UIFrameStats.layoutDirty); P11's incremental layout is the
    // CONSUMER that will skip clean elements. styleDirty forces a style re-resolve.
    bool layoutDirty = true;
    bool styleDirty = true;
};

// Runtime-only tag on a dialogue Choice button the conversation player spawns while
// a Choice node is active. Maps the clicked button back to its node's output pin so
// the player can follow the right branch. Transient - never serialized, destroyed
// when the choice is resolved.
struct DialogueChoiceButton {
    u32 outPin = 0;
};

// Runtime-only tag on the interaction prompt ("[E] Talk") the Engine shows while the
// player is near an Interactable. One shared entity, toggled visible. Transient -
// never serialized (excluded from the scene snapshot like DialogueChoiceButton).
struct InteractPromptTag {};

// Plays a reusable UI animation clip (.hbuianim) on this entity's UIElement. The clip
// drives offset/scale/rotation/color/opacity/sprite-frame over time; ui::UpdateAnimations
// advances it each frame per `trigger`. See UI/UIAnimation.h.
struct UIAnimator {
    std::string clip;                 // .hbuianim path (relative to Assets/)
    enum class Trigger : u8 {
        Manual = 0, // started by gameplay/schematic (set playing + time=0)
        OnShow,     // when the element becomes visible (panel shown)
        OnHide,     // when it becomes hidden
        Hover,      // on hover-enter
        Click,      // on click
        Loop,       // always looping
    };
    Trigger trigger = Trigger::OnShow;

    // Runtime playback state (not authored).
    f32 time = 0.0f;
    bool playing = false;
    bool captured = false;           // baseOffset captured for additive offset tracks
    bool justStarted = false;        // hold the exact t=0 pose on the start frame
    glm::vec2 baseOffset{0.0f};
    bool prevVisible = false;        // OnShow/OnHide edge detection
    bool prevHovered = false;        // Hover edge detection
};

// Runtime-only tag: entities carrying it SURVIVE a Replace scene load (the loader
// destroys everything else). The engine applies it to the persistent UI scene so the
// menus / HUD stay resident across gameplay scene swaps. NOT serialized.
struct Persistent {};

// Which OPEN `.hbui` document instance this entity was created by (UI/UIDocument.h).
// Runtime-only: NEVER serialized into a .hbscene, a .hbprefab or a .hbsave - a
// document is loaded from its own file, so writing its entities into a scene
// snapshot would duplicate the whole UI layer on the next restore.
//
// `doc` is a bare u32 rather than `ui::DocHandle` ON PURPOSE: Components.h is the
// bottom of the include graph (Core/Types, RHI, CameraRig, PaintSystem, Schematic,
// Vfx only) and UI/UIDocument.h includes IT, so naming the alias here would be a
// cycle. Same precedent as UICanvas::scaleMode storing `u32` instead of
// ui::ScaleMode. 0 = no document.
//
// `screenOwned` is a copy of "this document is the resident screen UI layer",
// duplicated into the component because the two Replace-sweep predicates that
// must spare it (scene::Instantiate and Engine::FlowMainMenu) have no access to
// an Engine or a DocumentSet and must evaluate the test identically.
struct UIDocMember {
    u32 doc = 0; // u32 == ui::DocHandle
    bool screenOwned = false;
};

// Runtime-only tag: the auto-managed lit quad that displays a worldSpace UICanvas
// in the 3D scene. Created/pruned by ui::UpdateWorldSurfaces each frame; NEVER
// serialized (the scene writer skips these entities; recreated on load).
struct UISurface {
    entt::entity canvas = entt::null; // the UICanvas entity this quad displays
};

// 3D text as its OWN object (Unity TextMesh / Unreal TextRender style): glyph
// quads placed directly in the world by this entity's Transform - no canvas, no
// panels. Drawn through the world-space particle pass (unlit, alpha-blended,
// depth-tested against the scene). Text lies in the entity's local XY plane
// facing +Z (a standing sign), centered on the origin; `billboard` makes it
// face the camera instead (floating label). Supports '\n'.
struct WorldText {
    std::string text = "Text";
    f32 size = 0.25f;              // line height in METERS
    glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
    std::string font;              // font asset (rel. Assets; empty = system font)
    bool billboard = false;        // always face the camera
};

// Auto-layout container (U5): positions its NON-stretch child UIElements in a
// row / column / grid AFTER their normal RectTransform solve, so lists, menus,
// and credits don't need hand-placed offsets. Children that stretch (anchorMin
// != anchorMax on an axis) are left untouched. `fitContent` back-sizes the
// owning element to wrap its children (a ContentSizeFitter).
struct UILayoutGroup {
    enum class Kind : u8 { Vertical = 0, Horizontal = 1, Grid = 2 };
    Kind kind = Kind::Vertical;
    f32 spacing = 8.0f;             // gap between children (px)
    glm::vec2 cellSize{160.0f, 48.0f}; // Grid cell size (px)
    glm::vec4 padding{0.0f};        // inner padding L,T,R,B (px)
    int columns = 1;                // Grid columns (>=1)
    bool fitContent = false;        // grow the element to fit the children
};

// Inherited opacity + interactivity for a whole subtree (U5), like Unity's
// CanvasGroup: multiplies every descendant's alpha (menu fade-in/out from one
// value) and can make the subtree non-interactive without hiding it.
struct UICanvasGroup {
    f32 opacity = 1.0f;      // 0..1, multiplies descendant alpha
    bool interactable = true; // false = subtree ignores the pointer
};

// Marks a UI subtree root as a named "screen" the UIManager shows/hides as a unit.
// While inactive the entire subtree is skipped by layout (not drawn, not interactive).
struct UIPanel {
    std::string name;           // screen id: "MainMenu" / "Settings" / "HUD" / "Pause" / ...
    bool startVisible = false;  // active when the UI scene first loads
    // P8 focus graph: element `id` that takes focus when keyboard/gamepad navigation
    // FIRST begins on this screen (empty = geometric reading-order first). NOTE: it is
    // applied on the first nav press while focus is null, not eagerly on Show - eager
    // focus-on-activate would need UIManager<->UIContext wiring (a follow-up). `modal`:
    // while active AND shown, pointer hit-testing AND keyboard/gamepad focus are TRAPPED
    // to this panel's subtree - widgets behind it become inert, the dialog barrier. No
    // active modal = no change.
    std::string firstFocus;
    bool modal = false;
    bool active = false;        // runtime: manager-controlled visibility (NOT serialized)
};

// Attaches a visual-scripting graph (a `.hbschem` "Schematic"). Runs On Start
// once then On Update each frame while the simulation plays.
// Only `asset` serializes; the rest is per-instance runtime state.
struct SchematicComponent {
    std::string asset;                 // .hbschem rel path (under Assets/)
    bool started = false;              // On Start fired (reset to re-fire)
    std::unordered_map<std::string, schematic::Value> vars; // blackboard
    std::unordered_map<u32, f32> timers;                    // Delay-node cooldowns
};

// Procedural heightfield terrain that builds itself from a grid of mesh CHUNKS
// (one MeshInstance child entity per chunk, tagged TerrainChunk). The terrain
// system (re)generates the chunks whenever `dirty` is set. Only the parameters
// here are serialized - the chunk meshes are rebuilt from them on load.
struct TerrainComponent {
    u32 chunks = 4;          // chunks per side (chunks x chunks grid)
    u32 resolution = 24;     // quads per chunk side
    f32 chunkSize = 16.0f;   // world units per chunk side
    f32 height = 0.0f;       // procedural noise amplitude; 0 = spawn FLAT (sculpt or
                             // raise this for hills). Was 8 (random hills on spawn).
    f32 frequency = 0.04f;   // noise frequency (smaller = broader hills)
    u32 octaves = 4;         // fractal noise octaves
    i32 seed = 1337;
    glm::vec4 color{0.36f, 0.5f, 0.28f, 1.0f};
    f32 roughness = 0.95f;
    bool dirty = true;       // runtime: (re)generate chunks on the next tick

    // Editable heightmap: (chunks*resolution + 1)^2 samples, row-major (z outer,
    // x inner), terrain-local Y. Empty -> filled procedurally from the params on
    // the next build; the terrain editor's brushes sculpt these values. Resized
    // (and re-seeded procedurally) whenever the grid resolution changes.
    std::vector<f32> heights;

    // Hole mask: one byte per heightfield sample (GridN^2, same layout as `heights`).
    // POLARITY: 255 = HOLE, 0 = SOLID. (This comment used to claim the opposite; the
    // brush at terrain::PaintHole and the clip in MeshPBR.hlsl have always agreed on
    // 255 = hole, so the comment was the thing that was wrong.) Terrain pixels at a
    // hole are clipped so cliff/cave models show through, AND the physics heightfield
    // collider punches the same hole - you can now fall through one.
    // The hole brush paints this; uploaded to `holeMaskTex` and sampled (terrain-wide
    // UV) by the forward pass. Empty = no holes. A mask whose size does not match
    // GridN^2 is STALE (nothing resizes it when the resolution changes) and must be
    // ignored wholesale rather than partially applied - see terrain::HoleMaskUsable.
    std::vector<u8> holeMask;
    rhi::TextureHandle holeMaskTex; // runtime: GPU upload of holeMask (not serialized)
    bool holeDirty = false;         // runtime: holeMask changed -> re-upload

    // Splat material painting: up to 4 tiling MATERIALS blended by a painted weight
    // mask. `splatLayerSrc` = .hbmat material paths (serialized); the runtime resolves
    // each into albedo/normal/MR handles. `splatWeight` is RGBA8 (GridN^2), one
    // channel per layer (default = layer 0 everywhere); painted by the terrain brush,
    // uploaded to `splatWeightTex`. `splatTile` = world units per texture repeat.
    bool splatEnabled = false;
    std::string splatLayerSrc[4];        // .hbmat material paths (serialized)
    rhi::TextureHandle splatAlbedoTex[4]; // runtime, resolved from each layer material
    rhi::TextureHandle splatNormalTex[4];
    rhi::TextureHandle splatMRTex[4];
    f32 splatRoughFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // each material's roughness factor
    f32 splatTile = 8.0f;
    std::vector<u8> splatWeight;         // RGBA8, GridN^2 (R..A = layer 0..3 weight)
    rhi::TextureHandle splatWeightTex;   // runtime upload of splatWeight
    bool splatDirty = false;             // runtime: splatWeight changed -> re-upload
    i32 activeSplatLayer = 0;            // runtime: layer the terrain brush paints

    // --- Physics collider handshake (runtime-only, never serialized) ----------
    // ONE static Jolt HeightFieldShape body for the WHOLE terrain, owned by
    // PhysicsWorld and rebuilt only when the grid layout changes. It replaces the
    // old per-chunk triangle-mesh colliders (256 static bodies / 819k collision
    // triangles / ~15 MB of retained vertices on the reference 16x16 terrain) and,
    // unlike them, it TRACKS SCULPTING: `heights` has exactly the row-major layout
    // Jolt wants, so a brush stroke pushes only the sample blocks it touched
    // through HeightFieldShape::SetHeights instead of rebuilding anything.
    //
    // `colliderDirty*` is the inclusive SAMPLE rect edited since the last push
    // (max < min = nothing pending). terrain::Sculpt / terrain::PaintHole grow it;
    // PhysicsWorld drains it. Holes go down the same path - a hole is just a sample
    // set to Jolt's no-collision value.
    static constexpr u32 kInvalidCollider = 0xFFFFFFFFu;
    u32 colliderBodyId = kInvalidCollider; // Jolt body key (managed by PhysicsWorld)
    i32 colliderDirtyMinX = 0, colliderDirtyMinZ = 0;
    i32 colliderDirtyMaxX = -1, colliderDirtyMaxZ = -1;

    u32 GridN() const { return chunks * resolution + 1; } // samples per side
};

// Runtime marker on a generated terrain chunk (NOT serialized; the terrain
// system rebuilds chunks from the parent's TerrainComponent).
struct TerrainChunk {
    u32 cx = 0;
    u32 cz = 0;
};

// Scene-level VEGETATION authoring config (one per world, typically on the terrain or a
// world root). It is the species PALETTE + biome the distribution draws from, a global
// density, and the deterministic world seed. Asset references are literal strings (so
// the pack closure can walk them); the runtime resolves the names into the
// SpeciesRegistry / BiomeRegistry (Source/Vegetation). Serialized in P3/P9.
struct VegetationLayer {
    std::vector<std::string> speciesAssets; // .hbspecies paths (serialized)
    std::string biomeAsset;                 // .hbbiome path (serialized)
    f32 globalDensity = 1.0f;               // multiplies the biome's base density
    u64 worldSeed = 1337;                   // master seed for deterministic generation
    bool enabled = true;

    // Optional painted density / exclusion mask (same lifecycle as the terrain splat
    // mask; 0 = no plants .. 255 = full density). Empty = uniform. P2/P9.
    std::vector<u8> densityMask;            // GridN^2 (matches the terrain grid)
    rhi::TextureHandle densityMaskTex;      // runtime upload (not serialized)
    bool maskDirty = false;                 // runtime: densityMask changed -> re-upload
};

// ONE lightweight entt entity per streamed REGION whose residency triggers async
// vegetation generation for that region (P8). The bulk plant data (trees/branches/
// foliage clusters) lives in the side VegetationStore keyed by `shardKey`, NEVER as
// entt entities - which is what keeps 10k trees / 1M branches / 10M+ foliage feasible.
struct VegetationField {
    u64 seed = 0;                 // folded with VegetationLayer::worldSeed for this region
    glm::vec3 boundsMin{0.0f};    // world-space region bounds (XZ used for scatter)
    glm::vec3 boundsMax{0.0f};
    std::string biomeOverride;    // "" = inherit the layer's biome
    f32 density = 1.0f;           // per-region density multiplier

    // Runtime-only (never serialized): generation state + the store key.
    u64 shardKey = 0;             // key into VegetationWorld's per-shard stores
    bool generated = false;       // this region's vegetation has been built
    bool generating = false;      // an async generation job is in flight
};

// Tags a single PAINTED / scattered plant entity (the woody body or its foliage), spawned by
// veg::PopulateForest / the editor vegetation paint brush. Two jobs: (1) it lets the erase
// brush find plant entities to remove within a radius (a plain MeshInstance query can't tell a
// tree from a rock), and (2) it carries the data needed to REHYDRATE the runtime mesh handle on
// scene load - `species` is the stable species NAME (not the runtime-interned SpeciesId, which is
// not stable across sessions), `seed` reproduces the per-instance transform, `foliage` picks the
// woody vs. leaf submesh. The MeshInstance.mesh handle itself is runtime-only and is rebuilt from
// these fields by the vegetation rehydrate pass.
struct VegetationInstance {
    std::string species;    // stable species name (looked up in the VegetationWorld registry)
    u64 seed = 0;           // per-instance deterministic seed (yaw + scale)
    bool foliage = false;   // false = woody body, true = the leaf submesh
    f32 age01 = 1.0f;       // maturity the mesh was generated at
};

// Data-driven locomotion via motion matching: a feature database is built from
// the source asset's animation clips (root speed / turn rate sampled over time)
// and, each search interval, the entity's Animator is switched to the clip+time
// that best matches the desired velocity - so idle / walk / run / turn clips are
// selected automatically from movement intent. Requires an Animator on the same
// entity (motion matching drives that Animator's clip + time).
struct MotionMatching {
    std::string sourceAsset;         // .uaf with locomotion clips ("" = Animator's source)
    f32 searchInterval = 0.12f;      // seconds between database queries
    f32 speedScale = 1.0f;           // clip speed -> world speed calibration
    glm::vec3 desiredVelocity{0.0f}; // requested world velocity (gameplay sets this)
    bool useNavVelocity = true;      // drive from a NavigationAgent if present
    bool enabled = true;

    // Runtime state (managed by anim::UpdateMotionMatching; not serialized).
    f32 sinceSearch = 1e9f;          // forces a search on the first frame
    u64 dbKey = 0;                   // identity of the cached feature database
};

// One two-bone inverse-kinematics chain: the end effector joint (matched by
// name, like animation channels) plus its parent (mid) and grandparent (root)
// are rotated so the end reaches `target`. Used for foot/hand placement and
// look-at-with-the-arm style constraints.
struct IKChain {
    std::string endJoint;            // end effector joint name (e.g. "foot_l")
    glm::vec3 target{0.0f};          // world-space goal for the end effector
    glm::vec3 pole{0.0f};            // world-space bend hint (knee/elbow direction)
    bool hasPole = false;            // use `pole` (else keep the current bend plane)
    f32 weight = 1.0f;               // 0..1 blend toward the solved pose
    bool enabled = true;
    std::string targetEntity;        // optional: follow this entity's world position

    // RUNTIME, never serialized. `target` is not written to the `.hbscene` while
    // `targetEntity` is set (it is derived from that entity every frame, so writing
    // it would make the file a function of how long the editor had been open). The
    // consequence is that an UNRESOLVED binding - the entity renamed, deleted, or
    // sitting in a shard that is not resident - leaves `target` at its default
    // {0,0,0}, and the limb would reach for the WORLD ORIGIN. anim::UpdateSkeletal
    // clears this flag when the lookup fails, and the solver then skips the chain
    // and warns once, so a broken binding is a limb that keeps its animated pose and
    // says so, instead of one that silently snaps across the level.
    bool targetResolved = true;
    bool warnedUnresolved = false;
};

// Inverse kinematics applied AFTER skeletal animation poses the entity: each
// chain solves a two-bone IK so its end joint reaches its target (analytic,
// blended by weight). Requires an Animator + a skinned mesh on the entity.
// Slider-driven body proportions. Lives on the CHARACTER ROOT - the entity that owns the
// Animator and therefore the one skinning palette every part borrows. Putting it on a part
// would shape that part alone and tear the seams open.
//
// `values` is authored and serialized BY NAME so a scene saved today still loads after more
// sliders exist; `joints` is the resolved form the pose pass reads. The pose runs inside a
// parallel job, so resolving happens once, before the job, whenever `dirty` is set - never
// inside it.
struct BodyShape {
    std::unordered_map<std::string, f32> values; // slider name -> [-1, 1], 0 = as imported

    // Runtime, not serialized.
    std::vector<bodyshape::JointShape> joints;
    bool dirty = true;
};

struct IKConstraint {
    std::vector<IKChain> chains;
};

// Constant rotation applied to the entity's Transform while the simulation runs
// (local axis, degrees per second). A simple gameplay helper component.
struct Rotator {
    glm::vec3 axis{0.0f, 1.0f, 0.0f};
    f32 speed = 45.0f; // degrees per second
    bool enabled = true;
};

// Painterly "censor": attach to any entity (a person, a prop) to re-paint it with
// the painterly brush-stroke look inside a soft world-space sphere that follows
// the entity - a moving censor patch. Only does anything while the painterly post
// stack + "Real brush strokes" are on; the renderer projects this sphere to screen
// and feathers it into the painterly passes via a 3D sphere test (see
// rhi::CensorData / CollectCensors). `offset` raises the center off the origin
// (e.g. onto a character's torso). `strength` 1 = fully painted, 0 = no effect.
struct CensorComponent {
    f32 radius = 2.0f;          // sphere radius in world units
    f32 feather = 0.5f;         // soft-edge fraction of the radius (0 = hard cut)
    f32 strength = 1.0f;        // 0..1 how fully the object is painted over
    glm::vec3 offset{0.0f, 1.0f, 0.0f}; // local-space center offset from the entity
    bool enabled = true;
};

// Player / character movement. While the simulation runs (play mode / runtime),
// character::Update reads movement input (WASD + left stick) and produces a move
// intent; PhysicsWorld drives a capsule CharacterVirtual with it - real gravity,
// ground detection and collision against the world's colliders (slopes, steps,
// walls). Pair it with a CameraComponent whose `target` is this entity
// (First/Third Person) for a player-controlled view.
struct CharacterController {
    f32 radius = 0.4f;            // capsule radius
    f32 height = 1.8f;            // total capsule height (>= 2*radius)
    f32 moveSpeed = 5.0f;         // m/s on the ground
    f32 sprintMultiplier = 1.8f;  // Shift / left shoulder
    f32 jumpHeight = 1.2f;        // peak jump height in metres (0 = no jump)
    f32 gravity = 20.0f;          // downward acceleration, m/s^2
    f32 turnSpeed = 12.0f;        // rad/s to face the move direction (0 = no turn)
    bool cameraRelative = true;   // move relative to the active camera's facing
    bool faceMoveDir = true;      // rotate the body toward its movement
    bool useKeyboard = true;      // WASD + Space
    bool useGamepad = true;       // left stick + A (jump) + LB (sprint)
    bool enabled = true;

    // Runtime state. character::Update writes the intent (horizontal world-space
    // velocity + a one-shot jump request); PhysicsWorld owns velocityY/grounded
    // and the CharacterVirtual body id. Not serialized.
    glm::vec3 desiredVelocity{0.0f}; // horizontal move velocity this frame
    bool jumpRequested = false;      // latched on press, consumed by physics
    // Set by cam::Update while a FIRST-PERSON player-look camera targets this
    // entity: the camera owns the body's yaw, so faceMoveDir must not also turn
    // it (the two would use different facing conventions and fight). Latched by
    // the camera and CONSUMED by character::Update - the same one-shot shape as
    // jumpRequested - so the suppression lapses by itself the moment the camera
    // stops driving this body. Not serialized.
    bool externalFacing = false;
    f32 velocityY = 0.0f;            // vertical velocity (gravity + jump)
    bool grounded = false;           // touching ground after the last step
    static constexpr u32 kInvalidBody = 0xFFFFFFFFu;
    u32 bodyId = kInvalidBody;       // CharacterVirtual id (PhysicsWorld)

    // Render interpolation between fixed physics steps. The simulation runs at a
    // fixed 60 Hz (0..4 steps per rendered frame), so writing the raw last-step
    // capsule position into the Transform makes the mesh lurch against the variable
    // frame rate - while a damped follow-camera stays smooth (the "player jitters,
    // camera doesn't" symptom). PhysicsWorld keeps the two most recent stepped
    // world positions here and writes Transform = mix(prev, cur, accumulator/step)
    // once per frame, so the drawn pose advances evenly. Not serialized.
    glm::vec3 renderPrevPos{0.0f};
    glm::vec3 renderCurPos{0.0f};
    bool renderPoseInit = false;     // false until the first step seeds prev == cur
};

// A directional light source (one is used as the scene's primary light).
struct DirectionalLightComponent {
    glm::vec3 direction{-0.5f, -1.0f, -0.4f};
    glm::vec3 color{1.0f};
    f32 intensity = 4.0f;
};

// An omnidirectional light at the entity's world position with a windowed
// inverse-square falloff reaching zero at `range`.
struct PointLightComponent {
    glm::vec3 color{1.0f};
    f32 intensity = 10.0f; // radiance at 1m
    f32 range = 10.0f;
};

// A volumetric effect (smoke/fire/explosion/...) placed at the entity's world position. It has TWO
// sides: (1) AUTHORING - an embedded volume::VolumeSimConfig recipe the editor runs as a live low-res
// CPU preview so you see it in the real scene before baking; (2) RUNTIME - a baked `.hbvol` (`source`)
// the shipped game streams + plays. "Bake in place" turns the config into a `.hbvol` and sets source.
// The loaded asset + resolved grid are runtime-only (owned by volume::VolumeCache); the source path,
// playback/look knobs, and the sim config serialize. The runtime renders ONE volume at a time (the RHI
// feed is single-grid), so with several VolumeComponents only the first eligible one renders - the rest
// still advance their playhead. Placement is translation only (no rotation/scale).
struct VolumeComponent {
    std::string             source;              // ".hbvol" asset path (serialized; empty until baked)
    bool                    playing = true;      // serialized
    bool                    loop = true;         // serialized
    f32                     time = 0.0f;         // playhead seconds (serialized; resumes)
    f32                     speed = 1.0f;        // serialized
    rhi::VolumeRenderParams render{};            // density/emission/extinction/albedo/steps (serialized;
                                                 // boundsMin/Max + worldOffset are drive-owned each frame)
    // Authoring (serialized): the embedded sim recipe + live-preview state. When livePreview is on and
    // the editor is in edit mode, the editor runs a low-res CPU sim of `sim` and feeds it live at this
    // entity's transform. previewRes overrides sim.bounds.dim for the LIVE preview only (bake uses the
    // config's own resolution). effectName is the preset it was seeded from (UI label only).
    volume::VolumeSimConfig sim{};
    bool                    livePreview = true;
    i32                     previewRes = 40;
    std::string             effectName;
    // Runtime-only (never serialized):
    u32                     cacheHandle = 0xFFFFFFFFu; // volume::VolumeCache handle (invalid = unassigned)
    i32                     resolvedFrame = -1;        // last frame the drive selected (inspector/debug)
};

// A cone light at the entity's world position. The cone axis is the entity's
// local -Y rotated into world space (points "down" by default); inner/outer
// angles (degrees, half-angle) shape the falloff between full and zero.
struct SpotLightComponent {
    glm::vec3 color{1.0f};
    f32 intensity = 20.0f;
    f32 range = 15.0f;
    f32 innerAngle = 25.0f;
    f32 outerAngle = 35.0f;
};

// A rectangular AREA light: a glowing panel of width×height (world units) whose
// emitting normal is the entity's local -Z. Shaded with the representative-point
// approximation (soft, area-ish near-field falloff) and packed into the punctual
// light array as a third kind alongside point + spot. `twoSided` emits from both
// faces. Also the model for "emissive surface = light" placed by hand.
struct RectLightComponent {
    glm::vec3 color{1.0f};
    f32 intensity = 20.0f;
    f32 width = 2.0f;
    f32 height = 1.0f;
    f32 range = 20.0f;
    bool twoSided = false;
};

// Moves the entity toward `target`: nav::UpdateAgents finds a Detour path on the
// scene's baked navmesh (streamed by nav::NavWorld), steers the Transform along it
// (seek + arrival), snaps the agent onto the navmesh, faces movement, and softly avoids
// NavigationObstacles and other agents. Runs while the simulation is playing (like
// physics/scripts). If required navmesh tiles are still streaming it keeps the current
// path and retries - it never blocks.
struct NavigationAgent {
    glm::vec3 target{0.0f}; // desired destination (world space)
    bool hasTarget = false; // set true to start moving (gameplay or inspector)

    f32 speed = 3.5f;            // max move speed (m/s)
    f32 acceleration = 8.0f;     // m/s^2 toward the desired velocity
    f32 radius = 0.5f;           // avoidance + corner-arrival radius
    f32 stoppingDistance = 0.3f; // arrival threshold at the goal
    f32 turnSpeed = 10.0f;       // rad/s to face movement (0 = no rotation)
    bool autoRepath = true;      // re-query when the target moves

    // Runtime state (managed by nav::UpdateAgents; not serialized).
    std::vector<glm::vec3> path; // current path corners (world space)
    u32 corner = 0;              // index of the next corner to steer toward
    glm::vec3 velocity{0.0f};
    glm::vec3 lastTarget{0.0f};  // detects target changes for autoRepath
    bool reached = false;        // within stoppingDistance of the goal
    f32 repathCooldown = 0.0f;   // re-plans periodically so it reroutes around movers
};

// A dynamic obstacle that NavigationAgents route + steer around. It is fed to the
// navmesh as a DetourTileCache obstacle, which rebuilds the affected navmesh tiles so
// agents re-plan around it in real time (no re-bake), plus a soft local-avoidance push
// between re-plans. Move it freely - crates, vehicles, barricades, doors.
struct NavigationObstacle {
    f32 radius = 1.0f;
    f32 height = 2.0f; // reserved (vertical extent)
    bool enabled = true;
};

// A box region that OVERRIDES the post-process LOOK while the camera is inside
// it - bloom / DoF / color grade / fog / SSGI / SSR / exposure. The highest-
// priority enabled volume containing the camera wins; outside all volumes the
// scene's default post (SceneEnvironment::post) applies. Anti-aliasing (TAA/
// FXAA) and GTAO are NOT part of a volume - they are project-global quality, so
// only the "look" fields of `settings` are read (see Scene's OverlayPostLook).
struct PostVolume {
    glm::vec3 halfExtents{10.0f, 5.0f, 10.0f}; // local-space box half-size
    int priority = 0;                          // higher wins when volumes overlap
    bool enabled = true;
    rhi::PostSettings settings;                // look fields only
};

// A local light & reflection probe. Bakes the LOCAL environment (the room's own
// surfaces + lights, ray-cast on the CPU) into diffuse irradiance + a prefiltered
// specular map, so surfaces inside its influence box sample the room's lighting
// instead of the global sky. This is what makes a sealed interior go dark (lit by
// its own lamps) with correct local reflections, instead of being filled by the
// unoccluded sky IBL. Position comes from the Transform; bake via the editor.
struct ReflectionProbe {
    glm::vec3 halfExtents{8.0f, 4.0f, 8.0f}; // local-space influence box half-size
    f32 intensity = 1.0f;                    // scales the baked ambient
    f32 skyMix = 0.0f;                       // 0 = fully local, 1 = blend the sky back in
    f32 range = 60.0f;                       // ray-cast max distance (a miss = sky)
    int priority = 0;                        // higher wins where boxes overlap
    std::string source;                      // cached .hbprobe (rel to Assets; "" = unbaked)
    // Runtime bake products (rebuilt by Bake / loaded from `source`).
    rhi::TextureHandle irradiance;
    rhi::TextureHandle prefiltered;
    f32 prefilteredMaxLod = 0.0f;
    bool baked = false;
};

// A projected decal. The box (the entity's Transform, sized by halfExtents) projects
// its albedo/normal/metal-rough onto whatever surfaces it overlaps, blended into the
// material in the forward pass BEFORE lighting - so decals are correctly lit and conform
// to any geometry (blood, cracks, posters, puddle stains, ...). Projects along the box
// local +Z: orient the decal so +Z points INTO the surface. Reuses `.uaf` textures
// (bindless), resolved by decal::Update. Runs on up to rhi::kMaxDecals decals per frame.
struct DecalComponent {
    glm::vec3 halfExtents{0.5f, 0.5f, 0.15f}; // local box half-size (thin Z = projection depth)
    f32 opacity = 1.0f;
    f32 angleFade = 2.0f;      // pow() falloff as the surface turns away from the projector
    f32 normalStrength = 1.0f; // decal normal-map blend (when a normal map is set)
    f32 roughness = 0.8f;      // used only when no metal-rough map is set
    f32 metallic = 0.0f;       // used only when no metal-rough map is set
    // Channel influence: which surface channels this decal writes. Lets an artist author a
    // normal-only detail decal, a colour-only stain, or a roughness-only wet patch instead of always
    // overriding everything. All true = a full material decal (today's behaviour).
    bool affectBaseColor = true;
    bool affectNormal = true;
    bool affectMR = true;      // roughness + metallic together
    // Emissive: a self-illumination tint the decal adds (shaped by its coverage/alpha), for glowing
    // signs, runes, embers, markings. Off by default so existing decals are unchanged.
    bool affectEmissive = false;
    glm::vec3 emissiveColor{1.0f, 0.6f, 0.2f};
    f32 emissiveIntensity = 0.0f; // 0 = no glow even if affectEmissive is on
    bool twoSided = false;     // also project onto surfaces facing AWAY from the projector
    // Hard projection cone: reject surfaces turned more than this from the projector axis (degrees).
    // 90 = disabled (the legacy behaviour: any front-facing surface). Lower values kill the smear a
    // box decal leaves as it wraps around a corner / onto near-perpendicular faces.
    f32 maxAngle = 90.0f;
    std::string albedoTex;     // .uaf (rel to Assets); "" = tint only (no colour change)
    std::string normalTex;     // .uaf; "" = keep the surface normal
    std::string mrTex;         // .uaf (glTF b=metal g=rough); "" = use roughness/metallic above
    // Runtime: resolved bindless indices (NOT serialized; decal::Update fills them).
    rhi::TextureHandle albedo;
    rhi::TextureHandle normal;
    rhi::TextureHandle mr;
    bool resolved = false;
};

// A Gerstner-wave water surface. water::Update gives the entity a flat grid MeshInstance
// (flagged Water|Transparent|NoShadow so it draws in the dedicated water pass) that the
// water VS displaces by the summed waves. The wave/colour/ripple params are GLOBAL for the
// scene (Water.hlsl reads them from the frame constants) and the FIRST water entity drives
// them; interactive ripples (rain splashes + water::AddRipple) ride a scene-global ring
// buffer (Scene::WaterRipples). Reuses the terrain-style build-on-dirty pattern.
struct WaterComponent {
    f32 size = 120.0f;    // world extent of the (square) grid, metres
    u32 resolution = 128; // grid cells per side (verts = (res+1)^2); clamped 16..256
    // Gerstner waves (4): direction (compass degrees), amplitude (m), wavelength (m),
    // speed (m/s), steepness (0 rolling .. 1 sharp crests).
    f32 waveAngle[4]     = {20.0f, 70.0f, 115.0f, 160.0f};
    f32 waveAmplitude[4] = {0.55f, 0.32f, 0.16f, 0.08f};
    f32 waveLength[4]    = {26.0f, 15.0f, 8.5f, 5.0f};
    f32 waveSpeed[4]     = {5.0f, 4.2f, 3.3f, 2.6f};
    f32 waveSteepness[4] = {0.50f, 0.50f, 0.45f, 0.40f};
    // Look.
    glm::vec3 shallowColor{0.10f, 0.30f, 0.42f}; // grazing / sky-blended tint
    f32 fresnelPower = 5.0f;
    glm::vec3 deepColor{0.02f, 0.08f, 0.13f};    // body colour looking straight down
    f32 reflectionRoughness = 0.10f;             // IBL reflection blur (0 = mirror)
    f32 foam = 0.5f;
    f32 rippleStrength = 1.0f; // rain micro-ripple + interactive ring intensity
    f32 rippleScale = 1.0f;
    f32 buoyancy = 1.2f;       // floating strength for rigid bodies (1 = neutral, >1 floats up)
    // FFT ocean (Tessendorf). When on, the surface is displaced by the GPU spectral FFT (a
    // real ocean) instead of the Gerstner sum above; Gerstner then only feeds CPU buoyancy.
    // The FFT tile is a fixed 256x256 grid repeating every fftPatchSize metres. See OceanFFT.h.
    bool fftOcean = false;
    f32 fftWindSpeed = 18.0f;  // wind speed (m/s) - sets the dominant wavelength
    f32 fftWindDir = 35.0f;    // wind direction (compass degrees)
    f32 fftPatchSize = 128.0f; // world metres per FFT tile (smaller = finer/choppier detail)
    f32 fftChoppiness = 1.0f;  // horizontal-displacement strength (sharper crests)
    f32 fftAmplitude = 4.0f;   // overall wave-height scale
    f32 fftHeightScale = 1.0f; // extra runtime vertical multiplier applied in the VS
    // Depth-based water: the PS reads scene depth to grade absorption/foam/soft edges by the
    // water-column depth (deeper = deep colour + opaque; shallow = see-through + shoreline foam).
    f32 absorptionDepth = 6.0f;  // metres of water column to full deep-colour absorption
    f32 shorelineWidth = 1.5f;   // metres of the shoreline foam band
    f32 edgeFade = 0.5f;         // metres of soft depth-fade where water meets geometry
    // Runtime (NOT serialized).
    rhi::MeshHandle mesh;
    bool dirty = true; // rebuild/UpdateMesh the grid on the next water::Update
};

// Opts an entity's mesh OUT of the baked navmesh when `enabled` is false. All meshes
// bake into the navmesh by default (except paint strokes, explicit NavigationObstacles,
// and Dynamic-layer movers), and the baker decides walkable vs. blocking from slope - so
// this is only needed to EXCLUDE a mesh the baker would otherwise treat as walkable
// floor or blocking geometry. An enabled NavmeshInput is a no-op (the mesh already bakes).
struct NavmeshInput {
    bool enabled = true;
};

// A checkpoint / task-goal marker (TLOU-style). When REACHED it can set or
// complete an objective and write a save. It is reached either by the player
// walking into its box trigger (`triggerOnEnter`) or programmatically -
// game::ReachCheckpoint(id) from a Schematic/script (e.g. an enemy-death graph).
// The reached set lives in game:: (run state), so `reached` here is just a cache.
// What an Interactable / TriggerVolume does when activated. All reuse the deferred
// game:: queues, so they behave exactly like a schematic firing the same action.
enum class InteractAction : u8 {
    Dialogue = 0,       // run `asset` (.hbdialogue) as a branching conversation
    Cutscene,           // run `asset` (.hbcutscene) cinematic
    SetFlag,            // game::SetFlag(flag, flagValue) - story state
    SetObjective,       // game::SetObjective(flag, text)   (flag = objective id)
    CompleteObjective,  // game::CompleteObjective(flag)
    None,               // no-op
    // APPEND-ONLY: the action is serialized as a numeric index, so new actions go
    // AFTER None or old scenes silently reinterpret their action id.
    GrantItem,          // game::AddItem(itemId, itemCount) + mark the pickup picked
};

// A world object / NPC the player can interact with. While the player is within
// `range` a prompt ("[E] <prompt>") shows; pressing the interact key (E / gamepad Y)
// fires `action`. `requiredFlag` gates availability (non-empty => only while that
// story flag != 0); `once` disables it after the first activation. This is how the
// player TOUCHES the narrative - e.g. talk to an NPC (Dialogue), examine/pick up
// (SetFlag/Objective). See [[heartbreak-dialogue-graph]].
struct Interactable {
    InteractAction action = InteractAction::Dialogue;
    std::string prompt = "Interact"; // shown as "[E] <prompt>"
    std::string asset;               // Dialogue/Cutscene asset (relative to Assets/)
    std::string flag;                // SetFlag/Objective: flag name or objective id
    f32 flagValue = 1.0f;            // SetFlag value written
    std::string text;                // SetObjective: the objective text to show
    f32 range = 2.5f;                // interact distance (world units)
    bool once = false;               // fire only the first time
    std::string requiredFlag;        // gate: interactable only while this flag != 0 (empty = always)
    // GrantItem action: grant `itemCount` of `itemId`, then permanently remove the
    // pickup via a "picked.<pickupId>" flag (survives save/load AND level reload).
    std::string itemId;
    u32         itemCount = 1;
    std::string pickupId;
    bool fired = false;              // runtime: already activated (honours `once`)
};

// A box trigger: when the player enters the Transform-centred box, `action` fires
// (same set as Interactable). Great for "enter room -> start cutscene/dialogue".
// Fires on the ENTER edge; `once` (default) prevents re-firing; `requiredFlag` gates.
struct TriggerVolume {
    InteractAction action = InteractAction::Dialogue;
    std::string asset;
    std::string flag;
    f32 flagValue = 1.0f;
    std::string text;
    glm::vec3 halfExtents{2.0f, 2.0f, 2.0f}; // box around the Transform
    bool once = true;
    std::string requiredFlag;
    std::string itemId;               // GrantItem action (see Interactable)
    u32         itemCount = 1;
    std::string pickupId;
    bool fired = false;               // runtime: already fired (honours `once`)
    bool inside = false;              // runtime: player was inside last frame (enter-edge detect)
};

struct Checkpoint {
    std::string id;               // unique key (save/restore + ReachCheckpoint)
    std::string setObjective;     // objective text to SHOW when reached ("" = none)
    std::string completesObjective; // objective id to mark DONE when reached ("" = none)
    glm::vec3 halfExtents{2.0f, 2.0f, 2.0f}; // trigger box around the Transform
    bool triggerOnEnter = true;   // auto-fire when the player enters the box
    bool saveOnReach = true;      // write a checkpoint save when reached
    bool once = true;             // fire only the first time
    bool reached = false;         // runtime cache (authoritative set is in game::)
};

// --- Combat -----------------------------------------------------------------
// A team id. Damage rules are DERIVED, not stored: same faction = friendly
// (blocked unless the target allows friendlyFire), different = hostile. Neutral
// never initiates and is never a valid AI target, but can still be hurt by
// scripted/environmental damage. Flat enum (like SceneKind/InteractAction) so the
// editor shows a combo and it round-trips as a u32 cast.
enum class Faction : u8 { Neutral = 0, Player, Enemy, Ally, Faction4, Faction5 };

// Are `a` and `b` hostile? Used by BOTH AI target selection and damage filtering.
// Neutral is never hostile; the Player and their Ally companions are on one side;
// any other cross-faction pair is hostile.
inline bool Hostile(Faction a, Faction b) {
    if (a == Faction::Neutral || b == Faction::Neutral || a == b) return false;
    const bool aFriendly = a == Faction::Player || a == Faction::Ally;
    const bool bFriendly = b == Faction::Player || b == Faction::Ally;
    if (aFriendly && bFriendly) return false; // Player + Ally fight together
    return true;
}

// Makes an entity DAMAGEABLE. Shared by the player and every enemy/NPC - combat is
// faction-based, not type-based. combat::ApplyDamage mutates this; combat::Update
// ticks regen + the invuln timer and fires the one-shot death dispatch on the
// alive->dead edge (mirrors how Checkpoint fires once). Runtime fields are not
// written to the .hbscene source (they ARE snapshotted into the .hbsave).
struct Health {
    f32     max     = 100.0f;
    f32     current = 100.0f;
    Faction faction = Faction::Enemy;

    // Regen: after `regenDelay` s without taking damage, heal `regenRate` hp/s up
    // to max. 0 rate = no regen (typical enemy; the player often regens).
    f32 regenRate  = 0.0f;
    f32 regenDelay = 5.0f;

    // Invulnerability. `hitInvuln` grants i-frames per successful hit (debounces
    // multi-ray/overlap doubles); `invincible` is a hard flag (god-mode / scripted
    // cutscene immunity) that never expires.
    f32  hitInvuln  = 0.0f;
    bool invincible = false;
    bool friendlyFire = false; // allow same-faction damage TO this entity

    // Zero-script death reactions (fire once on death, like Checkpoint):
    std::string onDeathFlag;        // game::SetFlag(onDeathFlag, onDeathFlagValue)
    f32         onDeathFlagValue = 1.0f;
    std::string onDeathObjective;   // game::CompleteObjective(onDeathObjective)
    std::string deathTag;           // matched by the OnDeath schematic event ("" = Name)
    i32         deathClip = -1;      // Animator clip to switch to on death (-1 = none)

    // Runtime (not serialized to source; snapshotted for saves).
    bool alive           = true;
    bool deathDispatched = false;  // guards the one-shot death dispatch
    f32  invulnTimer     = 0.0f;   // seconds of i-frames remaining
    f32  sinceDamage     = 1e9f;   // seconds since last damage (regen delay gate)
    entt::entity lastAttacker = entt::null; // most recent instigator (OnDeath output)
};

// An attack capability carried by an actor (player or AI), like CharacterController
// is a movement capability. combat::TryFire consults it: Hitscan does a two-step
// trace (wall occlusion via PhysicsWorld::Raycast + ray-vs-sphere over Health
// entities, since characters are bodyless capsules); Melee sweeps a forward arc.
// All hits go through combat::ApplyDamage so faction/invuln rules always hold.
struct Weapon {
    enum class Kind : u8 { Hitscan, Melee, Projectile /*reserved*/ };

    Kind kind     = Kind::Hitscan;
    f32  damage   = 25.0f;
    f32  range    = 50.0f;  // hitscan trace length / melee reach (m)
    f32  fireRate = 3.0f;   // shots per second (cooldown = 1/rate)
    f32  radius   = 0.0f;   // >0 = radial/AoE at the hit point
    f32  impulse  = 4.0f;   // knockback impulse along the hit direction
    f32  meleeArc = 45.0f;  // melee half-angle (deg) of the forward sweep
    f32  hitRadius = 0.5f;  // ray-vs-sphere radius used when testing targets

    // Ammo (maxAmmo < 0 = infinite, e.g. fists / AI).
    i32 maxAmmo    = 12;
    i32 ammo       = 12;
    i32 reserve    = 60;
    f32 reloadTime = 1.5f;

    // Runtime (not serialized): cooldown + reload countdowns.
    f32 cooldown  = 0.0f;
    f32 reloading = 0.0f;
};

// --- AI ---------------------------------------------------------------------
// The senses. ai::Update fills the runtime block each tick from a sight cone
// (range + FOV + eye-height, occluded by a PhysicsWorld::Raycast) plus a hearing
// radius fed by the game:: noise bus, integrating an awareness meter that decays
// when the target is lost. Targets are the nearest HOSTILE Health entity (faction).
struct AIPerception {
    // Sight (authored).
    f32 sightRange    = 18.0f;   // max view distance (m)
    f32 sightFovDeg   = 100.0f;  // full horizontal cone angle
    f32 eyeHeight     = 1.6f;    // eye offset above the Transform origin
    f32 loseSightGrace = 0.5f;   // s of continuous no-LoS before "lost" (anti-flicker)

    // Hearing (authored).
    f32 hearingRadius = 12.0f;   // base radius; a noise's loudness scales it

    // Awareness (authored tuning).
    f32 gainRate  = 1.5f;        // awareness/s while target sensed (scaled by proximity)
    f32 decayRate = 0.4f;        // awareness/s while target unsensed
    f32 detectThreshold = 1.0f;  // awareness at which the target is "spotted"

    // Runtime (not serialized).
    f32 awareness      = 0.0f;             // 0..1 detection meter
    bool canSeeTarget  = false;            // LoS + in cone THIS tick
    f32 timeSinceSeen  = 999.0f;           // s since last positive sight
    entt::entity knownTarget = entt::null; // nearest sensed hostile
    glm::vec3 lastKnownPos{0.0f};          // where the target was last sensed
    bool hasLastKnownPos = false;
    glm::vec3 heardPos{0.0f};              // most recent qualifying noise
    bool heardSomething = false;
};

// AI behavior states. Idle/Patrol are calm; Investigate/Search are alerted-but-
// unsure; Chase/Attack are engaged; Flee is retreating; Dead is terminal.
enum class AIState : u8 { Idle = 0, Patrol, Investigate, Chase, Attack, Search, Flee, Dead };

// Parses an AIState from its name (schematic SetAIState node); unknown -> Idle.
inline AIState AIStateFromName(const std::string& s) {
    if (s == "Patrol") return AIState::Patrol;
    if (s == "Investigate") return AIState::Investigate;
    if (s == "Chase") return AIState::Chase;
    if (s == "Attack") return AIState::Attack;
    if (s == "Search") return AIState::Search;
    if (s == "Flee") return AIState::Flee;
    if (s == "Dead") return AIState::Dead;
    return AIState::Idle;
}

// The brain. A compact hand-rolled FSM. ai::Update reads AIPerception + Health,
// picks the next state, and writes the NavigationAgent target + fires the Weapon /
// melee via combat::. Locomotion animates automatically from the agent's velocity
// (motion matching), so the AI does no animation itself.
struct AIBehavior {
    AIState state = AIState::Idle;

    // Authored tuning.
    f32  attackRange    = 2.0f;   // enter Attack within this of the target (melee)
    f32  attackDamage   = 20.0f;  // per melee hit (weaponless fallback)
    f32  attackInterval = 1.2f;   // s between melee hits (weaponless fallback)
    f32  investigateTime = 6.0f;  // s to inspect a noise before giving up
    f32  searchTime     = 8.0f;   // s to sweep last-known-pos before de-escalating
    f32  fleeHealthFrac = 0.0f;   // flee below this HP fraction (0 = never)
    bool startAlerted   = false;  // spawn already Chasing (scripted encounters)
    bool useWeapon      = true;   // if the entity has a Weapon, fire it instead of melee

    // Patrol: inline world-space waypoints (empty = stand at Idle). Mode 0=loop,
    // 1=ping-pong, 2=once-then-idle. Kept inline (not an entity ref) so it
    // serializes trivially as an array of points.
    std::vector<glm::vec3> patrolPoints;
    u8  patrolMode  = 0;
    f32 waitAtPoint = 1.5f;

    // Runtime (not serialized).
    f32 stateTime      = 0.0f;   // s in the current state
    f32 attackCooldown = 0.0f;   // s until the next melee hit is allowed
    u32 patrolIndex    = 0;
    bool patrolForward = true;
    f32 waitTimer      = 0.0f;   // pause countdown at a waypoint
    AIState prevState  = AIState::Idle; // edge-detect for OnSpotPlayer
    bool spawnApplied  = false;  // startAlerted handled once
};

// --- Spawning / encounters --------------------------------------------------
// A spawn point/group. On its trigger it instantiates `prefab` (a .hbprefab, rel
// Assets/) `count` times on a disc of `radius` about the Transform, tags each root
// Spawned{encounterId, spawnerId}, throttles to `maxAlive`, and refills per
// `respawn`. Runtime fields persist ONLY under runtimeTags (like TriggerVolume) so
// a save keeps the fired/spawned state but an authored .hbscene starts un-triggered.
struct Spawner {
    std::string prefab;      // .hbprefab path rel Assets/ (the authored NPC)
    std::string encounterId; // Encounter.id this feeds ("" = standalone)
    std::string spawnerId;   // unique name for schematic SpawnGroup targeting

    u32 count  = 3;          // instances per burst
    f32 radius = 4.0f;       // disc radius the burst scatters over

    enum class Trigger : u8 { Volume = 0, Flag, Manual };
    Trigger trigger = Trigger::Volume;
    glm::vec3   halfExtents{6.0f}; // Volume-mode box (player enter-edge)
    std::string requiredFlag;      // Flag-mode gate + universal availability gate

    u32 maxAlive = 0;        // 0 = uncapped; else never exceed this many alive
    enum class Respawn : u8 { Once = 0, Continuous };
    Respawn respawn = Respawn::Once;
    f32 respawnDelay = 5.0f; // seconds between Continuous refill checks

    // Runtime (serialized only under runtimeTags).
    bool activated       = false; // trigger fired at least once
    bool inside          = false; // Volume enter-edge (mirrors TriggerVolume.inside)
    bool spawnRequested  = false; // schematic SpawnGroup / manual burst
    bool despawnRequested = false; // schematic DespawnAll
    u32  spawnedTotal    = 0;     // lifetime instances emitted
    f32  respawnCooldown = 0.0f;
};

// Orchestrates a staged encounter: groups Spawners by matching encounterId, tallies
// live members by scanning Spawned tags (stable across the .hbsave Replace, unlike
// entt handles), and fires a completion InteractAction when cleared (spawned then
// all dead). Completion reuses the InteractAction dispatch so "cleared -> SetFlag /
// Objective / Dialogue / Cutscene" gates progression with no new plumbing.
struct Encounter {
    std::string id;              // unique; Spawners + Spawned tags reference it
    bool startActive = false;    // armed at load (else armed by flag/schematic)

    InteractAction clearedAction = InteractAction::SetFlag;
    std::string clearedAsset;    // Dialogue/Cutscene
    std::string clearedFlag;     // SetFlag name / Objective id
    f32         clearedFlagValue = 1.0f;
    std::string clearedText;     // SetObjective text
    std::string requiredFlag;    // availability gate

    // Runtime (serialized only under runtimeTags).
    enum class State : u8 { Idle = 0, Active, Cleared };
    State state = State::Idle;
    u32  aliveCount = 0;        // recomputed each tick from Spawned tags
    bool everHadAlive = false;  // saw >=1 alive member (so alive==0 => cleared)
    bool clearedEdge = false;   // set the frame it clears
    bool activateRequested = false; // schematic/flag arm request
    // "my members are absent because the world unloaded them, not because they died."
    // aliveCount is recomputed every tick from live Spawned tags, and stream::
    // DespawnShard destroys them - so without this an encounter the player WALKED AWAY
    // from reads alive==0 with everHadAlive==true on the very next gameplay tick (the
    // same frame) and fires its cleared cutscene / objective / flag. The player is
    // awarded a fight they abandoned. Set by stream::DespawnShard for every encounter
    // whose population it took; cleared as soon as live members exist again.
    bool membersStreamedOut = false;
};

// Runtime membership tag stamped on the ROOT of every spawned instance. Ties it to
// its spawner + encounter by STRING id. Serialized under runtimeTags; absent from
// authored .hbscene files.
struct Spawned {
    std::string encounterId;
    std::string spawnerId;
};

// --- Facial / blendshapes ---------------------------------------------------
// Low-level blendshape channel weights on the entity that owns the morph mesh (the
// head part in a modular rig, or a whole-face single mesh). facial::Update and the
// SetMorphWeight schematic node write `weights`; Scene::CollectDrawItems resolves
// channel names -> morph-atlas rows and fills the top-8 non-zero into the DrawItem
// so the vertex shader accumulates them before skinning.
struct MorphState {
    std::unordered_map<std::string, f32> weights; // channel name -> 0..1

    // Resolve cache (not serialized): filled once from the mesh's morph target list.
    rhi::TextureHandle morphTexture;              // bindless delta atlas
    u32 vertexCount = 0;
    std::vector<std::string> targetNames;         // atlas row order
    bool resolved = false;
};

// Higher-level face driver: amplitude-envelope lip-sync + timed eye-blink + an
// expression preset, all writing into the target MorphState each frame. On a
// modular rig it sits on the Character root and drives the live head part's
// MorphState; on a single-mesh character it sits on the same entity.
struct FacialAnimator {
    bool lipSync = true;
    std::string jawTarget = "jawOpen";
    f32 jawStrength = 1.0f;
    f32 jawAttack   = 25.0f; // per-second rise toward the envelope
    f32 jawRelease  = 12.0f; // per-second fall

    bool autoBlink = true;
    std::string blinkL = "blink_L";
    std::string blinkR = "blink_R";
    f32 blinkMin = 2.5f, blinkMax = 6.0f, blinkDuration = 0.12f;

    std::string expression;      // active preset name ("" = neutral)
    f32 expressionWeight = 1.0f;

    // Runtime (not serialized).
    std::vector<f32> env;    // lip-sync amplitude envelope of the current line
    f32 envRate = 60.0f;     // envelope samples/sec
    f32 envTime = 1e9f;      // seconds since the line started (>= env end = idle)
    f32 jawCur = 0.0f;
    f32 blinkTimer = 0.0f;
    f32 blinkPhase = -1.0f;  // >=0 while a blink pulse plays
    u32 rng = 0;             // per-entity PRNG (seeded from entity id)
    bool seeded = false;
    std::vector<std::string> driven; // channels written last frame (cleared when idle)
};

// A pre-fractured breakable object (a `.hbfrac`, see Assets/Fracture.h).
//
// COST MODEL - the reason this is worth having at all: while INTACT the object is
// exactly one draw and one static body, identical to any other prop. The chunks are
// DATA (indices into the fracture asset), not entities. Only when something actually
// breaks it do chunk entities get created, and only for the chunks that come loose.
// A level can therefore be full of breakables at no cost until the player touches
// them.
struct Destructible {
    std::string asset;               // `.hbfrac` path relative to Assets/
    f32 impulseThreshold = 12.0f;    // contact impulse (kg*m/s) that starts a break
    f32 chunkHealth = 12.0f;         // hp per chunk before it comes loose
    f32 damageRadius = 0.6f;         // world radius one damage hit spreads over
    f32 breakImpulseScale = 1.0f;    // multiplier on the impulse handed to freed chunks
    f32 debrisLifetime = 12.0f;      // seconds a freed chunk lives (0 = forever)
    f32 density = 1000.0f;           // kg/m^3; chunk mass = volume * density
    bool structural = true;          // re-run the support flood fill after each break
    std::string breakEvent;          // audio/particle tag fired per break ("" = none)

    // --- runtime -------------------------------------------------------------
    // Loose = still in place but no longer welded; Detached = a live physics body.
    enum class ChunkState : u8 { Intact = 0, Loose = 1, Detached = 2 };
    bool activated = false;          // chunk entities exist; the root mesh is hidden
    bool structureDirty = false;     // a break happened; the support pass is owed
    // "activated is TRUE but the chunk entities do not exist" - the state a persisted
    // break lands in after a shard respawn or a save load. It cannot be represented by
    // `activated` alone: destruction::Activate refuses to run while activated is true,
    // and it is also what removes the root's MeshInstance/RigidBody - so a restored
    // half-broken wall would come back rendering and colliding as PRISTINE while being
    // internally 100% broken and permanently unbreakable. Set by
    // world::ApplyEntityState; consumed by destruction::Update (and by any damage entry
    // point), which rebuilds the chunk entities and re-detaches the restored ones.
    // Runtime only, never serialized (the persisted `activated` implies it).
    bool reactivate = false;
    std::vector<u8> chunkState;      // ChunkState per chunk
    std::vector<f32> chunkHp;
    std::vector<entt::entity> chunkEntity; // entt::null until that chunk detaches
    std::vector<u8> supportScratch;  // flood-fill visited buffer, reused (no per-break alloc)
};

// One live chunk. Runtime-generated, never authored and never serialized - the
// serializer skips these the way it already skips TerrainChunk / SkinnedPartRef,
// because they are rebuilt from the Destructible's state.
struct DebrisChunk {
    entt::entity owner = entt::null;
    u32 index = 0;
    f32 age = 0.0f;
    // Impulse owed to this chunk. PhysicsWorld creates bodies LAZILY during its own
    // Update, so at the moment a chunk detaches it has no body yet and an impulse
    // applied then is silently dropped - the chunk would just sag instead of being
    // thrown. The push is parked here and applied on a later tick, once the body
    // exists. hasImpulse guards against re-applying it every frame.
    glm::vec3 pendingImpulse{0.0f};
    glm::vec3 pendingPoint{0.0f};
    bool hasImpulse = false;
};

} // namespace hbe
