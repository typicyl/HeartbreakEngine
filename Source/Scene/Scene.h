// Scene/Scene.h - ECS-backed scene container plus draw/view extraction.
#pragma once

#include "Scene/Components.h"
#include "Renderer/Camera.h"
#include "Renderer/IBL.h" // GiStatus (SceneEnvironment reports why its .hbgi is/isn't bound)
#include "RHI/RHI.h"

#include <entt/entt.hpp>

#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace hbe {

class Renderer;

// Global per-scene lighting/exposure environment, including IBL handles.
struct SceneEnvironment {
    DirectionalLightComponent sun;
    f32 ambientIntensity = 0.04f;
    f32 exposure = 1.0f;
    // Cascaded-shadow draw distance (world units). The cascades are fit to the camera
    // out to this range; beyond it geometry casts/receives no sun shadow. Bigger =
    // shadows reach further (large terrains) but the near cascades get coarser. Capped
    // by sceneRadius*2.5 and the camera far plane in Scene::MakeView.
    f32 shadowDistance = 150.0f;

    // Day/night cycle. The sky is rendered analytically from the sun direction
    // (real-time atmosphere), so moving the sun moves the whole sky + lighting.
    // When `dynamicSky` is on, the engine advances `timeOfDay` (hours, [0,24)) at
    // `dayLengthSeconds` real seconds per full cycle and recomputes the sun
    // direction/colour/intensity + ambient each frame. 0 dayLength = time is held
    // (scrub it from the editor). Off = the authored `sun` is used as-is.
    f32  timeOfDay = 10.0f;       // hours [0,24)
    f32  dayLengthSeconds = 0.0f; // real seconds for one 24h cycle (0 = paused)
    u32  dynamicSky = 0;          // 1 = drive the sun from timeOfDay
    // 1 = the three fields above came from the SCENE FILE's header, not from the
    // project. They are a per-level OVERRIDE (see scene::ApplyEnvironment), and this
    // flag is what makes them round-trip: a scene that never authored an override
    // omits the keys entirely and keeps inheriting the project's cycle.
    //
    // WHY A SCENE MAY OWN THE CLOCK AT ALL. The clock free-runs (Engine::UpdateDayNight
    // advances it every frame from whenever the process booted), so with the project's
    // cycle on, the editor and the shipped game are at different hours the moment they
    // have been open for different lengths of time - a 60-second day length puts them
    // 12 in-game hours apart after 30 seconds. Nothing about the level file could fix
    // that, and a sealed interior could not opt out of a cycle it does not want.
    // An authored override pins the hour (and can switch the cycle off) per level,
    // which is both the parity fix and the interior-authoring fix.
    u32  dayNightAuthored = 0;

    // Weather (analytic sky cloud layer). cloudCoverage 0 = clear; overcast grays
    // the whole sky toward a flat ceiling. Fed to the sky shader each frame.
    f32  cloudCoverage = 0.0f;
    f32  cloudDensity = 0.6f;
    f32  overcast = 0.0f;
    f32  windAngle = 45.0f;   // compass heading clouds drift toward (degrees)
    f32  windSpeed = 0.01f;   // cloud drift speed (UV units/sec)

    // Image-based lighting (bindless); zero handles -> flat ambient fallback.
    rhi::TextureHandle irradiance;
    rhi::TextureHandle prefiltered;
    rhi::TextureHandle brdfLUT;
    rhi::TextureHandle skinLUT; // pre-integrated subsurface-scattering LUT
    rhi::TextureHandle sky; // background environment (drawn behind the scene)
    f32 prefilteredMaxLod = 0.0f;

    // Baked SH-L1 irradiance volume (the diffuse-GI upgrade over box probes).
    // giSh is the SH atlas (0 = no volume); the grid spans dims cells from origin.
    rhi::TextureHandle giSh;
    rhi::TextureHandle giDepth; // octahedral depth atlas (DDGI visibility)
    glm::vec3  giOrigin{0.0f};
    glm::vec3  giSpacing{1.0f};
    glm::ivec3 giDims{0};
    std::string giSource;       // cached .hbgi (rel to Assets; "" = no baked volume)
    // The scene FILE's permanent pack slot, carried through load so a live save can
    // re-emit it (0xFFFFFFFF = none; slot 0 is valid). Packaging identity, not
    // rendering state - it rides here because the environment is the one thing that
    // survives from the file into the live scene. See SceneData::packSlot.
    u32 packSlot = 0xFFFFFFFFu;
    // Collaboration identity, carried file -> live scene -> file so a save never
    // strips it. See SceneData::docId / SceneData::guidEpoch for what they are and
    // why guidEpoch specifically must exist.
    u64 docId = 0;
    u64 guidEpoch = 0;
    // Why the volume above is (or is not) bound. Written ONLY by
    // scene::ApplyEnvironment, so "the scene says it has GI but it did not load"
    // is a state the editor can show and a test can assert instead of a look
    // nobody can explain. See GiStatus in Renderer/IBL.h.
    GiStatus giStatus = GiStatus::None;

    // HDR post-process stack (bloom/SSAO/FXAA/grade); defaults are sensible.
    rhi::PostSettings post;

    // THE PLAYER'S QUALITY PRESET, DECLARED (0 High = as authored, 1 Medium, 2 Low).
    // NOT serialized and NOT authored content: the shipped Settings menu owns it, and
    // the editor sets it only to PREVIEW what ships. It is applied to a COPY of the
    // post stack at read time (Scene::MakeView, after Post Volumes) instead of being
    // stamped over `post` - see scene::ApplyGraphicsPreset. Stamping it meant the
    // runtime overwrote six authored flags every frame, baked the degraded stack into
    // `.hbsave` files, and mutated one of the five fields --test-lightingparity
    // compares.
    int postQualityPreset = 0;
    // TEMP perf A/B (`--shadow-cascades`): non-zero wins over the preset's count.
    u32 forceShadowCascades = 0;
};

// THE VALUE a `.hbscene` header determines - everything, and nothing else.
//
// This is the object --test-lightingparity compares, and the reason it exists is
// that "the editor and the runtime agree about lighting" was previously asserted
// by nobody and drifted invisibly. It deliberately does NOT include the IBL/sky
// handles or the weather: no scene field determines them (they come from the
// project via scene::SetupSky), so including them would make the test assert
// something a scene load cannot control. The day/night clock IS included, because
// a scene header can now own it (see SceneEnvironment::dayNightAuthored) - and an
// unauthored clock is the one field a scene load legitimately leaves alone, which
// the stamp still catches because both paths then carry the same inherited value.
//
// GI is compared by handle VALIDITY, never by handle INDEX: bindless indices are
// allocation-ordered, so two worlds built in one process get different indices for
// identical content and an index compare would fail for the wrong reason. Validity
// plus giSource/giStatus/dims/origin/spacing is the whole DECISION, which is what
// parity means.
//
// HONEST LIMIT OF THE VALIDITY COMPARE. --test-lightingparity runs headless, and
// LoadGIVolume parses without uploading when there is no device, so in that harness
// giShValid/giDepthValid are false on BOTH sides in every case it compares -
// including the good-volume case. They therefore prove "neither path bound an
// atlas", not "both did". They are live coverage only for a caller that stamps two
// worlds in a process that HAS a device; the headless test's real GI coverage is
// giStatus + giSource + dims/origin/spacing.
struct EnvironmentStamp {
    f32 ambientIntensity = 0.0f;
    f32 exposure = 0.0f;
    f32 shadowDistance = 0.0f;
    rhi::PostSettings post;   // compared WHOLE, so a new member is covered the day it lands
    std::string giSource;
    GiStatus    giStatus = GiStatus::None;
    glm::vec3   giOrigin{0.0f};
    glm::vec3   giSpacing{0.0f};
    glm::ivec3  giDims{0};
    bool giShValid = false;
    bool giDepthValid = false;
    f32 timeOfDay = 0.0f;
    f32 dayLengthSeconds = 0.0f;
    u32 dynamicSky = 0;
    u32 dayNightAuthored = 0;
    bool operator==(const EnvironmentStamp&) const = default;
};

EnvironmentStamp StampOf(const SceneEnvironment& env);
// Human-readable diff of two stamps ("" when equal) - the test's failure message.
std::string DescribeStampDiff(const EnvironmentStamp& a, const EnvironmentStamp& b);

// --- Day/night curve ----------------------------------------------------------
// The ONE evaluation of "what does time-of-day do to the lighting", shared by the
// engine's per-frame driver (which owns the sun DIRECTION) and Scene::MakeView
// (which MODULATES the authored colour/intensity/ambient/exposure by it). It used
// to be two copies in two translation units - Engine.cpp computed `day` from the
// hour and Scene.cpp recomputed it from the resulting light direction - which is
// exactly the "a default lives in two places" shape.
struct DayNight {
    glm::vec3 toSun{0.0f, 1.0f, 0.0f}; // unit vector pointing AT the sun
    f32 day = 1.0f;                    // 0 = night, 1 = full day
    glm::vec3 tint{1.0f};              // sunset warmth, MULTIPLIED onto the authored colour
};
DayNight EvalDayNight(f32 timeOfDayHours);

// What tag streaming has done to THIS world, in the form the save path needs.
//
// WHY IT LIVES ON THE SCENE. A `.hbscene` written while shards are despawned is
// missing objects, and the save-time shard bake re-derives itself from whatever is
// live - so the resulting file is INTERNALLY CONSISTENT and nothing downstream ever
// complains. The loss is total and silent. The save path therefore has to be able to
// ask "is this world complete?" without knowing that streaming exists, so the
// streamer PUSHES this summary in (stream::Streamer::PublishResidency) rather than
// the serializer pulling on a stream:: type it must not depend on.
//
// Pushed, not observed, on purpose: a callback or a back-pointer to the Streamer
// would put a lifetime question on the save path, and the answer "the streamer died
// without telling us" must never be "write the file anyway".
struct StreamingResidency {
    bool bound = false;      // a level is bound to a stream::Streamer
    u32  shardCount = 0;
    u32  nonResident = 0;    // shards that are NOT currently spawned
    std::string missing;     // "Props#2, Foliage#0, ..." (first few, for the message)
    // An AUTHORING bind (stream::BindMode::AuthorWorld): the EDITOR bound its own
    // streamer to the world it already has, non-destructively, so the author can watch
    // zones spawn and despawn. Defaults FALSE, which is what makes this safe: a plain
    // runtime Streamer never sets it, so the blanket "a streamer owns this world"
    // refusal below is unchanged for every existing caller.
    //
    // It weakens EXACTLY ONE clause - "the editor may not author a streamer-owned
    // world", which is the statement live editor zones reverse. The clause that
    // actually protects the file (nonResident > 0 => refuse, by name) applies
    // identically either way, because a partial write is silent, total and permanent
    // whoever caused it.
    bool authoring = false;
};

// Owns an entt::registry. Entities with Transform + MeshInstance are drawn.
class Scene {
public:
    Scene(); // wires the UI structure-version signals (see BumpUIVersion)

    // WORLD IDENTITY. Bumped by every wholesale world replacement (the one
    // scene::DestroyWorld call site behind LoadMode::Replace and scene::BindWorld).
    //
    // The editor records the token it last loaded or saved and refuses to write the
    // registry back to that path once they disagree. That is the guard against a
    // world being swapped out from under the editor without the editor being told -
    // the shape of the checkpoint/dev-menu load, and of anything like it added later.
    // It costs one comparison and it fails CLOSED, which is the only acceptable
    // direction when the alternative is overwriting a level with a different world.
    u64  WorldToken() const { return worldToken_; }
    void BumpWorldToken() { ++worldToken_; streaming_ = {}; }

    // See StreamingResidency. Cleared by BumpWorldToken (a replaced world is no
    // longer the world the old summary described).
    const StreamingResidency& Streaming() const { return streaming_; }
    void SetStreaming(StreamingResidency r) { streaming_ = std::move(r); }

    entt::registry&       Registry()       { return registry_; }
    const entt::registry& Registry() const { return registry_; }

    // UI structure version: bumped whenever the UI HIERARCHY may have changed
    // (Parent/UIElement/UICanvas/UIPanel construct/destroy via EnTT signals, plus
    // explicit bumps at field-write sites like reparenting or panel activation).
    // The UI layout caches its parent->children map against this counter.
    u64  UIStructureVersion() const { return uiStructureVersion_; }
    void BumpUIVersion() { ++uiStructureVersion_; }

    SceneEnvironment&       Environment()       { return env_; }
    const SceneEnvironment& Environment() const { return env_; }

    // Creates an entity, optionally tagging it with a Name component.
    entt::entity CreateEntity(const std::string& name = {});

    // SIBLING-ORDER ALLOCATOR (Components.h `struct HierarchyOrder`). Monotonic and
    // never reused, which is the whole point: entt RECYCLES entity handles, so the
    // handle cannot be a creation-sequence number and every consumer that sorted by
    // it was wrong after the first delete. CreateEntity stamps every entity with the
    // next value, so a freshly made object always sorts LAST among its siblings.
    //
    // NOT reset by a Replace load. It is a watermark, not a count - re-basing it
    // would let a new entity collide with a value already loaded from a file.
    i32 NextHierarchyOrder() { return nextOrder_++; }
    // Raises the watermark past a value that came from a FILE (scene::Instantiate
    // overwrites the mint above with the serialized order). Without this a load
    // followed by a create would hand the new entity an order that already exists.
    void NoteHierarchyOrder(i32 v) {
        if (v >= nextOrder_) nextOrder_ = (v == std::numeric_limits<i32>::max()) ? v : v + 1;
    }

    // O(1) lookup of an entity by its Name. Cutscenes, camera targets, animation
    // tracks and dialogue actors all address entities by name, and they did it
    // with a full linear scan + string compare EVERY FRAME - cam::Update alone
    // ran several (follow target, zone track, zone camera, spline), so the cost
    // scaled with total scene size on a path that has nothing to do with it.
    // The index is rebuilt lazily only after a Name is added/changed/removed
    // (EnTT signals mark it dirty), so a steady-state frame pays one hash lookup.
    // Duplicate names resolve to one arbitrary match, exactly as the linear scan
    // did; entt::null when there is no match.
    entt::entity FindByName(const std::string& name) const;

    // World-space matrix of an entity: composes Transform up the Parent chain.
    glm::mat4 WorldMatrix(entt::entity e) const;

    // World matrix of an entity's PARENT (identity when it has none). This is the
    // frame `Transform` is expressed in, so it is what converts between the two.
    glm::mat4 ParentWorldMatrix(entt::entity e) const;

    // Write a WORLD-space position/rotation into an entity's LOCAL Transform.
    //
    // These exist because gameplay code kept computing a world-space answer (a nav
    // step, a look-at direction, a spawn point) and assigning it straight to
    // `Transform::position` / `Transform::rotation`, which are PARENT-relative.
    // For a root entity the two coincide, which is why it went unnoticed - and why
    // it broke silently the moment an agent was parented to anything. No-ops on an
    // entity with no Transform.
    void SetWorldPosition(entt::entity e, const glm::vec3& worldPos);
    void SetWorldRotation(entt::entity e, const glm::quat& worldRot);

    // Editor visibility mode: when on, entities carrying EditorHidden (or parented
    // under one) are skipped by CollectDrawItems / the UI build, so they stay
    // loaded but invisible. The runtime leaves this off (EditorHidden has no
    // effect there). The editor turns it on at init.
    void SetEditorView(bool on) { editorView_ = on; }
    bool EditorView() const { return editorView_; }
    // True when `e` or any ancestor carries EditorHidden (only meaningful while
    // EditorView() is on). Cheap no-op when nothing is hidden.
    bool IsEditorHidden(entt::entity e) const;

    // UI AUTHORING OVERRIDES: when on, the UI layout honours the session-only
    // EditorUIShow tag, which forces an INACTIVE UIPanel (a named screen) visible
    // so the dedicated `.hbui` editor can author a screen the game flow has not
    // switched to.
    //
    // WHY THIS IS SEPARATE FROM EditorView(). EditorView() is true for the WHOLE
    // editor process, Play mode included - it drives EditorHidden culling, which
    // is meant to persist while playing. Gating EditorUIShow on it alone meant that
    // picking a screen to author left that screen force-visible during Play and in
    // the Game view, stacked on top of whatever the game flow actually shows. Play
    // then stopped matching the shipped build, which is the one thing the authoring
    // canvas exists to guarantee. The editor drives this with `!playMode_`, so Play
    // sees exactly the runtime's behaviour; the runtime and a shipped build never
    // turn it on at all.
    void SetUIAuthoringView(bool on) { uiAuthoringView_ = on; }
    bool UIAuthoringView() const { return uiAuthoringView_; }

    // Drops `e` from the per-object motion-vector history. Call it for every entity
    // a despawn destroys.
    //
    // WHY: the history maps are keyed by entt::entity, and EnTT RECYCLES ids. The
    // comment on those maps says they "self-clean as entities disappear" - true only
    // because the CURRENT maps are rebuilt each collect; the PREVIOUS ones still hold
    // the dead entity's matrix. A respawn that lands on a recycled id inherits the
    // despawned object's previous world matrix and emits one frame of bogus velocity -
    // a TAA smear / motion-blur streak between the old and new positions. Latent until
    // something actually despawned, which is what tag streaming introduces.
    void ForgetMotionHistory(entt::entity e);

    usize EntityCount() const {
        // EnTT's const storage<T>() returns a pointer (null if absent).
        const auto* s = registry_.storage<entt::entity>();
        return s ? s->size() : 0;
    }

    // Appends a DrawItem for every Transform + MeshInstance entity.
    void CollectDrawItems(std::vector<rhi::DrawItem>& out) const;

    // Builds a SceneView from the camera (view/projection plus the frustum
    // parameters needed to fit the shadow cascades).
    rhi::SceneView MakeView(const Camera& camera) const;

private:
    // EnTT signal target: any construct/destroy/update of a hierarchy-shaping
    // component bumps the UI structure version.
    void OnUIStructural(entt::registry&, entt::entity) { ++uiStructureVersion_; }
    // EnTT signal target: a Name was added / changed / removed, so the name index
    // is stale. Rebuilt lazily on the next FindByName.
    void OnNameChanged(entt::registry&, entt::entity) { nameIndexDirty_ = true; }

    entt::registry   registry_;
    SceneEnvironment env_;
    u64              worldToken_ = 1; // see WorldToken()
    StreamingResidency streaming_;
    bool             editorView_ = false; // editor-only EditorHidden culling
    bool             uiAuthoringView_ = false; // editor-only EditorUIShow override
    u64              uiStructureVersion_ = 1; // see BumpUIVersion()

    // Per-object motion-vector history (for TAA + motion blur). Double-buffered:
    // CollectDrawItems hands out pointers/values from the "previous" maps (kept
    // alive through DrawScene) and records this frame into the "current" maps;
    // the two are swapped at the start of the next collect. Keyed by entity, so
    // they self-clean as entities disappear. mutable: CollectDrawItems is const.
    mutable std::unordered_map<entt::entity, glm::mat4> prevWorld_, curWorld_;
    mutable std::unordered_map<entt::entity, std::vector<glm::mat4>> prevPalette_, curPalette_;

    // Name -> entity index behind FindByName. mutable: FindByName is const and
    // rebuilds lazily. Starts dirty so the first lookup builds it.
    mutable std::unordered_map<std::string, entt::entity> nameIndex_;
    mutable bool nameIndexDirty_ = true;
    i32 nextOrder_ = 0; // HierarchyOrder allocator (see NextHierarchyOrder)
};

namespace scene {

// The project's authored sky, as the bake/IBL parameter block. Exposed so the GI
// bake stops passing `{}` (the BUILT-IN default sky) and bakes against the sky the
// game actually renders.
ProceduralSkyParams ProjectSkyParams();

// Applies a graphics-quality preset (0 High, 1 Medium, 2 Low) by DEGRADING a post
// stack: it only ever disables passes, never enables anything the author turned off,
// and never touches look values (bloom/intensities/exposure). High = exactly as
// authored. Painterly is the art style, not a perf knob - untouched.
//
// PURE, AND APPLIED TO A COPY. It lived in Engine.cpp and was called on the LIVE
// scene environment, which is why the editor could not show what ships and why a
// shipped `.hbsave` recorded the degraded stack as authored data. Scene::MakeView
// calls it on the SceneView's copy of `post`, at the SAME point in the frame the
// engine loop used to reach it (before the Post Volume overlay, which could
// therefore re-enable a dropped pass - preserved deliberately), so the rendered
// result is unchanged.
void ApplyGraphicsPreset(rhi::PostSettings& p, int preset);

// Generates the procedural-sky IBL environment (irradiance/prefiltered/BRDF LUT +
// background sky) from the PROJECT settings and installs it on `scene`, along with
// the fallback sun and the day/night + weather settings - none of which any
// `.hbscene` field can express.
//
// SPLIT FROM ApplyProjectLookDefaults ON PURPOSE. These two used to be one
// function, and its second caller (the Project Settings "Rebuild Sky + Lighting"
// button) only ever wanted this half: pressing it mid-session silently overwrote
// the LOADED SCENE's ambientIntensity / exposure / post with the PROJECT's, and
// nothing restored them until the next scene load. That is an editor-only path
// that makes the viewport brighter than the file it opened. `SetupSky` is
// idempotent and look-preserving; it is safe to press.
void SetupSky(Scene& scene, Renderer& renderer);

// Stamps the PROJECT's look defaults (ambientIntensity / exposure / post) onto the
// scene, destroying whatever the loaded scene authored. Legitimate at BOOT, before
// the startup scene is loaded, and from an explicit author gesture ("Apply to
// Scene"). Nowhere else - see SetupSky.
void ApplyProjectLookDefaults(Scene& scene);

// Boot composition: SetupSky then ApplyProjectLookDefaults, in that order, so the
// startup scene's own header (applied by scene::ApplyEnvironment, right after)
// overrides the project defaults rather than the other way round.
void SetupEnvironment(Scene& scene, Renderer& renderer);

// Builds the minimal default world (an editable sun entity in an otherwise
// empty scene) when no startup scene or model is provided.
void BuildDefaultScene(Scene& scene);

// Loads a model file (glTF/GLB/OBJ/FBX) as entities. Returns false on failure.
bool LoadModel(Scene& scene, Renderer& renderer, const std::string& path);

// Spawns `count` mesh instances (draw-call stress test). `sharedMesh` spawns N
// instances of ONE mesh (draw-sort/instancing measurement rig) instead of a
// unique mesh per instance (vertex-bandwidth stress).
void SpawnStress(Scene& scene, Renderer& renderer, u32 count, bool sharedMesh = false);

// Spawns emitters totalling roughly `count` LIVE particles (particle stress rig).
// Exists because neither the reference project nor MyProject authors a single
// ParticleEmitter, so the GpuMark("particles") slot could only ever be observed
// reading 0.00 - which proves the mark exists, not that it accumulates. Particle
// cost is overdraw-bound, so the emitters are deliberately spawned near the origin
// with large, overlapping, additive sprites.
// `gpuExpand` flips every spawned emitter onto the GPU vertex-expansion path
// (--gpu-particles), which is how the two expansion paths are A/B measured against
// each other at an identical particle count.
void SpawnParticleStress(Scene& scene, u32 count, bool gpuExpand = false, bool gpuSim = false);

// --test-worldlocal: pins Scene::SetWorldPosition / SetWorldRotation - the
// conversion gameplay code needs when it computes a WORLD answer for an entity
// whose Transform is PARENT-RELATIVE. Covers a rotated + non-uniformly scaled
// parent, a two-level chain, the root identity case, and a Transform-less entity;
// every case also asserts that the old raw-assignment behaviour FAILS it, so the
// test measures the fix rather than the status quo. Headless: no GPU, no window,
// no project. See Scene/WorldLocalTest.cpp.
bool WorldLocalSelfTest();

} // namespace scene
} // namespace hbe
