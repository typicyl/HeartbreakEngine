// Scene/Components.h - ECS components for the scene graph.
#pragma once

#include "Core/Types.h"
#include "RHI/RHI.h"
#include "Scene/PaintSystem.h" // paint::Stroke (PaintComponent stroke database)
#include "Schematic/Schematic.h" // schematic::Value (SchematicComponent blackboard)

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

// Editor-only visibility: when present, the entity (and everything parented under
// it) is hidden in the editor viewport but stays fully loaded. Serialized so the
// hidden setup survives reloads; the runtime ignores it (see Scene::SetEditorView).
struct EditorHidden {};

// CPU-simulated, GPU-billboarded particle emitter. Particles are pooled (no
// per-frame allocation) and drawn as camera-facing quads in one batched pass per
// blend mode (see Scene/ParticleSystem + the renderer's particle pass). Authored
// params are serialized; the live pool is runtime-only.
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

    // --- True volumetric VFX (raymarched 3D density; see heartbreak-volumetric-vfx) ---
    // When set, this emitter's particles ALSO feed a 3D density/temperature volume
    // that a compute pass splats and a raymarch pass lights + composites (real
    // volumetric smoke/fire, not billboards). The billboard pass still runs too;
    // set emitting/rate low + additive off for pure volumetric.
    bool volumetric = false;
    f32 volDensity = 1.0f;       // per-particle density contribution
    f32 volRadiusScale = 2.0f;   // blob radius = particle size * this
    f32 volTemperature = 0.0f;   // 0 = smoke, 1 = fire (drives blackbody emission)
    f32 volEmission = 2.5f;      // fire glow strength
    f32 volExtinction = 1.5f;    // absorption (higher = denser/darker smoke)
    i32 volSteps = 48;           // raymarch step count (perf vs quality)
    i32 volResolution = 96;      // volume voxel dim (quality knob; 64 = low-end)
    f32 volDetail = 0.6f;        // procedural noise detail: 0 = raw blobs, 1 = heavy wisps

    // --- runtime (NOT serialized) ---
    struct Particle {
        glm::vec3 pos{0.0f};
        glm::vec3 vel{0.0f};
        f32 age = 0.0f;
        f32 life = 1.0f;
        f32 rot = 0.0f;
        f32 seed = 0.0f;
    };
    std::vector<Particle> pool;     // live particles
    f32 spawnAccum = 0.0f;          // fractional spawn carry
    u32 rngState = 0x1234567u;      // per-emitter PRNG
    u32 textureCache = 0;           // resolved bindless index
    bool textureResolved = false;
    f32 simTime = 0.0f;             // ever-accumulating time (turbulence phase)
    f32 activeAge = 0.0f;           // time since emission (re)started (duration gate)
    bool bursted = false;           // one-shot burst already fired this activation
    bool wasEmitting = false;       // edge-detect emission restart
};

// Renders a GPU mesh with a metallic-roughness material.
struct MeshInstance {
    rhi::MeshHandle mesh;
    glm::vec4 baseColor{1.0f};
    f32 metallic  = 0.0f;
    f32 roughness = 0.5f;
    // Bindless texture indices (0 = none).
    rhi::TextureHandle albedoTexture;
    rhi::TextureHandle normalTexture;
    rhi::TextureHandle mrTexture;
    rhi::TextureHandle aoTexture;
    rhi::TextureHandle emissiveTexture;
    rhi::TextureHandle thicknessTexture; // SSS transmission thickness (0 = none)
    glm::vec3 emissiveColor{0.0f};
    f32 emissiveIntensity = 1.0f;
    glm::vec3 subsurfaceColor{1.0f, 0.3f, 0.2f};
    f32 subsurfaceRadius = 1.0f;          // SSS scatter scale
    u32 materialFlags = rhi::MaterialFlag_None;
};

// One paint layer in a PaintComponent's stack (bottom -> top). Each layer holds
// two CPU RGBA8 buffers (resolution^2, row-major):
//   color   : RGB albedo, A = colour coverage.
//   material: R metallic, G roughness, B height (0.5 neutral), A = material coverage.
// Layers composite with `opacity` + transparency into the component's flattened
// output (paint::Flatten). A layer can paint colour, material, or both.
struct PaintLayer {
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

    // Flattened (composited) output, uploaded to the GPU (not serialized).
    std::vector<u8> flatColor;      // RGB albedo, A coverage
    std::vector<u8> flatMaterial;   // R metal, G rough, B height, A coverage

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
};

// Optional human-readable label (debugging / editor).
struct Name {
    std::string value;
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

// Naughty-Dog-style level layering: a level is authored as three scene files,
// one per kind. Static = non-moving world geometry (the navmesh + baked-GI
// source, stays resident); Dynamic = actors / physics / skeletal meshes that
// move or reload; UI = HUD / menu canvases. A scene file records its kind in
// the header; the loader stamps every entity from it with a SceneLayer.
enum class SceneKind : u8 {
    Full = 0,  // a standalone, single-file scene (legacy / not part of a level)
    Static,
    Dynamic,
    UI,
};

inline const char* ToString(SceneKind k) {
    switch (k) {
        case SceneKind::Static:  return "static";
        case SceneKind::Dynamic: return "dynamic";
        case SceneKind::UI:      return "ui";
        case SceneKind::Full:    break;
    }
    return "full";
}

inline SceneKind SceneKindFromString(const std::string& s) {
    if (s == "static")  return SceneKind::Static;
    if (s == "dynamic") return SceneKind::Dynamic;
    if (s == "ui")      return SceneKind::UI;
    return SceneKind::Full;
}

// Runtime tag: which level layer an entity belongs to. Set by the loader from
// its scene file's kind (NOT per-entity serialized, like SceneSource). Systems
// query it - the navmesh bakes only Static-layer geometry, so dynamic/skeletal
// meshes never leak into the walkable surface.
struct SceneLayer {
    SceneKind kind = SceneKind::Static;
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
    bool playing = false; // runtime state (autoplay sets it on first update)

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

    // Runtime state (managed by anim::UpdateSkeletal; not serialized).
    std::vector<glm::mat4> palette; // per-joint global * inverseBind
    std::vector<i32> channelToJoint; // cached source-channel -> target-joint map
    u64 mapKey = 0;                  // identity of the cached mapping
    f32 translationScale = 1.0f;     // retarget bone-length ratio
    i32 rootChannel = -1;            // clip channel driving root motion
    glm::vec3 lastRootPos{0.0f};     // previous frame's sampled root position
    f32 lastRootTime = 0.0f;
    bool rootTrackValid = false;     // lastRootPos primed (no first-frame jump)
};

// Keyframe animation of the entity's LOCAL Transform (TRS keys, linear /
// slerp interpolation). Driven by anim::Update; edited in the Timeline panel.
struct AnimationTrack {
    struct Key {
        f32 time = 0.0f;
        glm::vec3 position{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 scale{1.0f};
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
        FirstPerson,   // sits at the target's position + eye offset, faces its forward
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

    // Third-person player look: when on, the mouse + right gamepad stick orbit
    // the camera around the target (instead of just trailing its facing). The
    // character then moves camera-relative (CharacterController) for standard
    // third-person controls.
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

    bool hovered = false;            // runtime state (UISystem)
    bool clicked = false;            // pressed this frame (runtime state)
    bool changed = false;            // value changed this frame (slider/toggle/selector)
    bool dragging = false;           // slider grabbed (persists across frames while held)
    bool prevHovered = false;        // runtime: hover-enter edge (drives hoverSound)
    u32 textureIndexCache = 0;       // resolved bindless index (runtime)
    bool textureResolved = false;    // reset to re-resolve `texture`
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
    // 255 = solid, 0 = hole (terrain pixels there are clipped so cliff/cave models show
    // through; collision stays). The hole brush paints this; uploaded to `holeMaskTex`
    // and sampled (terrain-wide UV) by the forward pass. Empty = no holes.
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

    u32 GridN() const { return chunks * resolution + 1; } // samples per side
};

// Runtime marker on a generated terrain chunk (NOT serialized; the terrain
// system rebuilds chunks from the parent's TerrainComponent).
struct TerrainChunk {
    u32 cx = 0;
    u32 cz = 0;
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
};

// Inverse kinematics applied AFTER skeletal animation poses the entity: each
// chain solves a two-bone IK so its end joint reaches its target (analytic,
// blended by weight). Requires an Animator + a skinned mesh on the entity.
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
    f32 velocityY = 0.0f;            // vertical velocity (gravity + jump)
    bool grounded = false;           // touching ground after the last step
    static constexpr u32 kInvalidBody = 0xFFFFFFFFu;
    u32 bodyId = kInvalidBody;       // CharacterVirtual id (PhysicsWorld)
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

// Moves the entity toward `target`: nav::UpdateAgents finds a grid A* path
// (nav::GridNav), steers the Transform along it (seek + arrival), snaps the
// agent to the ground, faces movement, and softly avoids NavigationObstacles and
// other agents. Runs while the simulation is playing (like physics/scripts).
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

// A dynamic obstacle that NavigationAgents route + steer around. nav::GridNav
// blocks the grid cells it covers each query, so agents re-plan around it in real
// time, plus a soft local-avoidance push between re-plans.
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

// Opts an entity's mesh INTO the pathfinding geometry. When any entity in the
// scene carries this, nav::GridNav samples only the tagged meshes (the walkable
// floors plus the walls/props that should block) instead of every mesh - so
// decorative geometry, the player, etc. stay out of pathfinding. With no
// NavmeshInput anywhere it falls back to using every mesh, so existing scenes
// keep working. GridNav decides walkable vs. blocking from slope, so tag both
// the ground and the obstacles you want considered.
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

} // namespace hbe
