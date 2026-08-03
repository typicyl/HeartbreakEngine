// Scene/SceneSerializer.h - save/load scenes as .hbscene JSON files.
//
// A scene file stores entities with their components; mesh provenance comes
// from MeshRef ("prim:..." or "uaf:<relative>#<submesh>"), so loading rebuilds
// GPU meshes/textures from the project's assets. Loading is split into three
// phases so SceneStreamer can run the CPU half on a worker thread:
//   Parse (JSON -> SceneData) -> Stage (asset file IO) -> Instantiate (GPU +
//   registry, main thread only).
#pragma once

#include "Assets/MaterialAsset.h"
#include "Assets/Mesh.h"
#include "Assets/UAF.h"
#include "Scene/Components.h"

#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hbe {

class Scene;
class Renderer;
struct SceneEnvironment;

namespace scene {

enum class LoadMode {
    Replace,  // clear the registry first
    Additive, // merge into the current world
};

// CPU-side description of one serialized entity.
struct EntityData {
    // Stable identity (Components.h `struct Guid`). 0 = the file carried none, in
    // which case Instantiate mints a fresh one. ParseSceneFile pre-fills this for
    // pre-guid `.hbscene` files with a value DERIVED from the file + entity index,
    // so the same unmodified file always yields the same guids. Fragments written
    // by BuildSubtreeJson (clipboard, `.hbprefab`) deliberately never carry one.
    u64 guid = 0;
    std::string name;
    std::string prefabSource;  // PrefabInstance link (.hbprefab rel Assets/; "" = none)
    bool editorHidden = false; // editor-only visibility (runtime ignores)
    bool hasTransform = false;
    Transform transform;
    int parent = -1; // index into SceneData::entities

    // SIBLING ORDER (Components.h `struct HierarchyOrder`, key "order").
    //
    // -1 MEANS ABSENT, and absent is the migration path, not an error: a
    // `.hbscene` written before this field existed carries no "order", and
    // Instantiate then uses the entity's FILE ROW INDEX instead - which is exactly
    // the implicit order those files already had (rows are created in array order,
    // and every consumer used to read the resulting handles as "creation order").
    // So every pre-existing file loads with byte-identical hierarchy behaviour and
    // no migration flag is needed.
    //
    // The -1 default here and the `-1` fallback in ParseSceneJson's
    // `je.value("order", -1)` are the SAME default in two places; change both.
    int order = -1;

    // STREAMING GROUP (Components.h `struct Tag`). Authored data written to disk,
    // unlike the two runtime-only snapshot tags at the bottom of this struct. The
    // NAME is what serializes - Instantiate interns it to a TagId through
    // tags::Intern, MAIN THREAD ONLY (StageAssets workers must never touch the
    // table). Absent / empty = Untagged = always resident.
    //
    // `shard` is the BAKED spatial shard index. -1 = untagged or not yet baked;
    // it is carried from P4 so the shard bake needs no second format change.
    bool hasTag = false;
    std::string tag;
    int shard = -1;

    // PAINT-STROKE ZONE GROUP (Components.h `struct StrokeGroup`, key
    // "strokeGroup"). The zone's tag NAME, interned by Instantiate on the main
    // thread exactly like `tag` above. Absent = not a group node; PRESENT AND
    // "Untagged" = the untagged group, which is a different statement.
    bool hasStrokeGroup = false;
    std::string strokeGroupTag;

    bool hasMesh = false;
    std::string meshSource; // MeshRef::source
    glm::vec4 baseColor{1.0f};
    f32 metallic = 0.0f;
    f32 roughness = 0.5f;
    u32 materialFlags = 0;
    glm::vec3 subsurfaceColor{1.0f, 0.3f, 0.2f};
    f32 subsurfaceRadius = 1.0f;
    glm::vec3 emissiveColor{0.0f};
    f32 emissiveIntensity = 1.0f;
    std::string materialAsset; // MaterialRef::asset (.hbmat, optional)

    // Art Editor surface paint (pixels live in the referenced .hbpaint file).
    bool hasPaint = false;
    std::string paintSource;        // .hbpaint path relative to Assets/
    u32 paintResolution = 1024;
    bool paintEnabled = true;
    bool paintLocked = false;
    bool paintReliefEnabled = true;
    f32 paintOpacity = 1.0f;
    f32 paintHeightScale = 0.05f;
    f32 paintLodBias = 1.0f;
    std::string paintLayer = "Default";
    i32 paintProjection = 0;

    bool hasAABB = false;
    AABB aabb;
    bool hasRigidBody = false;
    RigidBody rigidBody;
    bool hasLight = false;
    DirectionalLightComponent light;
    bool hasPointLight = false;
    PointLightComponent pointLight;
    bool hasSpotLight = false;
    SpotLightComponent spotLight;
    bool hasRectLight = false;
    RectLightComponent rectLight;
    bool hasCamera = false;
    CameraComponent camera;
    bool hasCameraZone = false;
    CameraZone cameraZone;
    bool hasMusicZone = false;
    MusicZone musicZone;
    bool hasDialogueActor = false;
    DialogueActor dialogueActor;
    bool hasCameraSpline = false;
    CameraSpline cameraSpline;
    bool hasTerrain = false;
    TerrainComponent terrain;
    bool hasMotionMatching = false;
    MotionMatching motionMatching;
    bool hasRotator = false;
    Rotator rotator;
    bool hasCensor = false;
    CensorComponent censor;
    bool hasCharacter = false;
    CharacterController character;
    bool hasIK = false;
    IKConstraint ik;
    bool hasUI = false;
    UIElement uiElement;
    bool hasUICanvas = false;
    UICanvas uiCanvas;
    bool hasUIAnimator = false;
    UIAnimator uiAnimator;
    bool hasUIPanel = false;
    UIPanel uiPanel;
    bool hasUILayoutGroup = false;
    UILayoutGroup uiLayoutGroup;
    bool hasUICanvasGroup = false;
    UICanvasGroup uiCanvasGroup;
    bool hasWorldText = false;
    WorldText worldText;
    bool hasAnim = false;
    AnimationTrack anim;
    bool hasAnimator = false;
    Animator animator;
    bool hasAudio = false;
    AudioSource audio;
    bool hasParticles = false;
    ParticleEmitter particles;
    bool hasSchematic = false;
    std::string schematicAsset;
    bool hasDestructible = false;
    Destructible destructible;
    bool hasNavAgent = false;
    NavigationAgent navAgent;
    bool hasNavObstacle = false;
    NavigationObstacle navObstacle;
    bool hasNavmeshInput = false;
    NavmeshInput navmeshInput;
    bool hasPostVolume = false;
    PostVolume postVolume;
    bool hasProbe = false;
    ReflectionProbe probe;
    bool hasCheckpoint = false;
    Checkpoint checkpoint;
    bool hasHealth = false;
    Health health;
    bool hasWeapon = false;
    Weapon weapon;
    bool hasAIPerception = false;
    AIPerception aiPerception;
    bool hasAIBehavior = false;
    AIBehavior aiBehavior;
    bool hasSpawner = false;
    Spawner spawner;
    bool hasEncounter = false;
    Encounter encounter;
    bool hasSpawned = false;
    Spawned spawned;
    bool hasMorphState = false;
    MorphState morphState;
    bool hasFacialAnimator = false;
    FacialAnimator facialAnimator;
    bool hasInteractable = false;
    Interactable interactable;
    bool hasTrigger = false;
    TriggerVolume trigger;
    bool hasCharacterRig = false; // modular-rig Character (distinct from CharacterController)
    Character characterRig;

    // Runtime-only tags carried by in-memory snapshots (play mode, undo/redo) so
    // a Replace restore preserves which scene/level-layer each entity belongs to.
    // Absent in on-disk scene files (those are partitioned per SceneSource).
    bool hasSceneSourceTag = false;
    std::string sceneSourceTag;
    bool hasSceneLayerTag = false;
    SceneKind sceneLayerKind = SceneKind::Static;
};

// One BAKED streaming shard, as it appears in the scene file header ("tagShards").
//
// WHERE THE SHARD'S WORLD AABB LIVES, and why here: this is its ONE home. It is
// produced by the save-time bake (Scene/TagShard.h) from the authored world bounds
// of the shard's members, written into the file the members live in, and read back
// at parse into SceneData::tagShards. Nothing recomputes it at runtime - clustering
// twice (once in the editor, once in the game) is a whole class of divergence bug -
// and nothing mutates it, so it cannot drift within a session.
//
// It is kept correct by being re-derived from scratch on EVERY save, and by
// `count`: the runtime buckets the file's entities by their per-entity `shard`
// field and compares the tally. A mismatch (a hand-edited file, a header from an
// older save) makes the whole set UNTRUSTED, and an untrusted set degrades to
// "everything always loaded" - correct-but-unstreamed, never missing content.
struct ShardDesc {
    std::string tag;  // tag NAME (names serialize, never interned ids)
    u32 index = 0;    // ordinal WITHIN the tag; the shard key is "<tag>#<index>"
    glm::vec3 min{0.0f}, max{0.0f}; // padded world AABB (see tagshard::kBoundsPad)
    u32 count = 0;    // members the bake put in this shard (the cross-check)
};

struct SceneData {
    std::vector<EntityData> entities;
    f32 ambientIntensity = 1.0f;
    f32 exposure = 1.0f;
    f32 shadowDistance = 150.0f;
    std::string giSource; // cached .hbgi GI volume (rel to Assets)
    // The scene's PERMANENT pack slot (top-level "packSlot"; see Assets/SlotIds.h).
    // kNoPackSlot = the file carries none (slot 0 is a VALID slot - Tree=0 in the
    // owner-spec test - so absence needs a sentinel, not zero). Parsed and re-emitted
    // so a parse -> save round trip through ANY writer keeps the asset's identity;
    // dropping it renumbers the asset at the next cook.
    static constexpr u32 kNoPackSlot = 0xFFFFFFFFu;
    u32 packSlot = kNoPackSlot;

    // --- COLLABORATION IDENTITY (top-level "docId" / "guidEpoch") ---------------
    //
    // docId names THIS DOCUMENT across machines and across renames. Minted once and
    // stored IN the file, never derived from its path - this engine already wrote the
    // post-mortem of path-keying into Assets/SlotIds.h, where a path-derived pack slot
    // made shipped pack stability exactly zero because a rename is indistinguishable
    // from delete + create. Written as 16-char hex for the same reason `guid` is: a
    // 64-bit JSON number is the one thing tooling reliably mangles.
    //
    // guidEpoch is the guard against the nastiest failure this format has.
    // scene::MigrateSceneGuids derives every entity guid as Derive(SeedFromPath(path),
    // rowIndex) - a PURE FUNCTION of the path and the row, with no randomness and no
    // machine identity. That determinism is exactly right for a single project (anyone
    // migrating the same file lands on the same ids), and it becomes a data-corruption
    // hazard the moment two DIVERGENT copies of a pre-guid scene exist: a 100-entity
    // copy and an 80-entity copy both compute Derive(seed, 5) and get the SAME guid for
    // DIFFERENT objects. Nothing detects it - each file is internally unique, so the
    // duplicate-claim guard sees nothing - and a merge would land one person's tent
    // transform onto another's rock and report it clean.
    //
    // guidEpoch is a RANDOM u64 minted once per document at the same moment as docId.
    // Same docId with a DIFFERENT guidEpoch means the two sides migrated independently,
    // so their guids are not comparable and any merge between them must be refused BY
    // NAME rather than silently aliasing entities. 0 = a file that predates the field.
    u64 docId = 0;
    u64 guidEpoch = 0;
    rhi::PostSettings post; // HDR post-process stack (defaults = effects on)
    // PER-SCENE DAY/NIGHT OVERRIDE (header keys "timeOfDay"/"dayLengthSeconds"/
    // "dynamicSky"). Optional, and `hasDayNight` is the whole point of it being
    // optional: a file that omits them keeps inheriting the PROJECT's cycle
    // (scene::SetupSky), which is what every scene authored before this did. See
    // SceneEnvironment::dayNightAuthored for why a level may own the clock.
    bool hasDayNight = false;
    f32 timeOfDay = 10.0f;
    f32 dayLengthSeconds = 0.0f;
    u32 dynamicSky = 0;
    // Level layer this file represents (header "kind"). Full = standalone scene.
    SceneKind kind = SceneKind::Full;
    // Baked streaming shards (header "tagShards"). Empty = this scene was never
    // baked (no tags, or saved by a pre-shard editor), which means every entity is
    // resident: absence is always the safe answer.
    std::vector<ShardDesc> tagShards;
};

// CPU asset payloads referenced by a SceneData (loaded during staging).
struct StagedAssets {
    // "uaf:<rel>" mesh path -> loaded model (all submeshes).
    std::unordered_map<std::string, Model> models;
    // texture file name (relative to Assets) -> decoded texture.
    std::unordered_map<std::string, uaf::Texture> textures;
    // .hbmat path (relative to Assets) -> parsed material.
    std::unordered_map<std::string, MaterialAsset> materials;
    // .hbpaint path (relative to Assets) -> loaded canvas (resolution + pixels;
    // metadata such as enabled/opacity comes from the owning EntityData).
    std::unordered_map<std::string, PaintComponent> paints;
};

// --- Slices (partial instantiation) ------------------------------------------
// StageAssets and Instantiate both take an optional INDEX LIST selecting a SUBSET
// of `data.entities` - one streaming shard out of a level - so several disjoint
// slices of ONE parsed SceneData compose into the same world a single full load
// would have built.
//
//   * `indices == nullptr` (the default) means THE WHOLE FILE. That is the
//     shipping full-file path and it behaves exactly as it did before slices
//     existed; every rule below is reached only when a slice is active.
//   * A non-null `indices` means a slice is active EVEN IF `count == 0` (an empty
//     slice legitimately creates nothing).
//   * The values are indices into `data.entities`, NOT positions within the
//     slice. Nothing is renumbered, so `EntityData::parent` keeps its meaning
//     across slices. Out-of-range or repeated indices are skipped with a warning.
//
// CROSS-SLICE PARENT - the deliberate decision, not an accident. A child inside
// the slice whose `parent` row lies OUTSIDE it is instantiated as a ROOT (no
// Parent component) and warned about. This is the same documented fallback the
// deleted SplitSceneFile used for a cross-layer parent (StreamingSalvage.h,
// SALVAGE 1: "any cross-layer parent gracefully becomes a root"), and it is
// deliberately a warning rather than an error because the authoring tools tag
// whole subtrees (tags::AssignSubtree) - so the case means the shard bake was
// bypassed, not that the file is corrupt. It is a silent teleport of the child to
// its LOCAL transform in world space, which is why it is loud in the log. The
// alternative - skipping the child - would delete authored content, and the
// pre-slice code's alternative was worse still: it emplaced Parent on
// entt::null (plan blocker B1).
//
// LoadMode::Replace is ILLEGAL with a slice and is refused with an HBE_ERROR,
// changing nothing: a shard neither owns the level's environment nor may destroy
// the world its siblings live in. Call BindWorld once, then Additive slices.

// The scene writer's own SKIP LIST as a predicate: false for an entity that is
// never written to a scene file at all (runtime-generated terrain chunks and world-UI
// surfaces, transient dialogue/interaction UI, modular-character parts, destruction
// debris, the Persistent UI layer, and a `.hbui` document's entities). Exposed so
// that code which must agree with "what actually lands in the file" reads the one
// predicate instead of copying the list - the shard bake's per-shard member counts
// are cross-checked against the file at load, so a disagreement there would report
// every shard as corrupt.
bool IsSerializedEntity(const entt::registry& reg, entt::entity e);

// THE EXCLUSION LIST, readable as data. Returns the COMPONENT name that excludes
// `e` (nullptr = it is written), and through `regeneratorOut` the reason - which is
// always the thing that puts the entity back, because that is the only justification
// for leaving anything out of the one complete copy of an authored level.
//
// Exposed so a reviewer, a self-test and the save path all read the same rows
// instead of a comment: --test-scenesave walks every excluded entity in the real
// level and re-checks that the row's regenerator still applies.
const char* SceneWriteExclusion(const entt::registry& reg, entt::entity e,
                                const char** regeneratorOut);
// Iterate the whole table (component name + regenerator), for reporting.
usize SceneWriteExclusionCount();
const char* SceneWriteExclusionRow(usize i, const char** regeneratorOut);

// "May this world be written back to an authored .hbscene?" - "" = yes, otherwise
// the author-facing reason it must be REFUSED.
//
// The one case it exists for: tag streaming despawns entities, and the save-time
// shard bake re-derives itself from whatever is live, so a scene written while
// shards are missing is INTERNALLY CONSISTENT and nothing downstream ever notices.
// The loss is silent, total and permanent. `expectedWorldToken` is the token the
// caller believes it loaded (Scene::WorldToken); pass 0 to skip that half.
std::string SaveRefusal(const Scene& scene, u64 expectedWorldToken = 0);

// --- Phases ------------------------------------------------------------------
// Parses a .hbscene file. Returns false on IO/JSON failure.
bool ParseSceneFile(const std::filesystem::path& path, SceneData& out);
// Loads every asset SceneData references from `assetsDir` (CPU only;
// thread-safe: touches no registry or GPU state). With `indices` set, only the
// assets that slice's entities reference are staged (see "Slices" above) - which
// is what keeps a streamed shard from loading the whole level up front.
void StageAssets(const SceneData& data, const std::filesystem::path& assetsDir,
                 StagedAssets& out, const u32* indices = nullptr, u32 count = 0);
// Creates entities/uploads GPU resources. Main thread only. When `createdOut`
// is non-null it receives every entity created (so a caller can destroy exactly
// the entities one load produced). When `sceneTag`
// is non-empty, every created entity gets a SceneSource{sceneTag} so the editor
// hierarchy can group it under its source scene (used for additive/streamed
// loads; the main Replace load leaves entities untagged = the active scene).
// `indices`/`count` instantiate only a slice of `data` - see "Slices" above.
void Instantiate(Scene& scene, Renderer& renderer, const SceneData& data,
                 StagedAssets& staged, LoadMode mode,
                 std::vector<entt::entity>* createdOut = nullptr,
                 const std::string& sceneTag = {},
                 const u32* indices = nullptr, u32 count = 0);

// Clears the world and applies `data`'s environment WITHOUT creating a single
// entity. This is the slice-load prologue, and it exists because LoadMode::Replace
// is the ONLY place ambientIntensity / exposure / shadowDistance / post / giSource
// are applied and the only place the previous world is destroyed - and Replace is
// illegal for a slice. A purely additive shard-by-shard load would otherwise
// render with whatever look the previous scene left behind, and would stack a
// second world on top of the first on the second load.
//
// It runs exactly the code LoadMode::Replace runs - the same two spared sets
// (Persistent, and UIDocMember{screenOwned} = an open `.hbui` document), the same
// five environment fields, the same `.hbgi` volume load - so "BindWorld + N
// Additive slices" and "one full Replace" leave the same environment behind.
//
// Call it ONCE per level bind. Calling it again re-binds (destroy + re-apply),
// which is what makes a second Play not stack a second world.
void BindWorld(Scene& scene, Renderer& renderer, const SceneData& data);

// THE ONE WRITER of a scene's LOOK: ambientIntensity, exposure, shadowDistance,
// post, and the baked GI volume named by giSource. Both world-load paths call
// exactly this - LoadMode::Replace (which is what Editor::LoadSceneInEditor's
// scene::LoadScene does) and BindWorld (which is what the runtime's
// stream::Streamer::BindLevel does) - and that is the whole reason the editor and
// the shipped game agree about lighting.
//
// STRUCTURAL RULE: SceneEnvironment's five header fields have exactly one writer
// reachable from a scene file, and it is this function. Any future path that needs
// a scene's look CALLS IT or does without. Adding a second stamping site is how
// editor/runtime lighting drifts apart invisibly, which is what --test-lightingparity
// exists to prevent - it compares the environment this produces via each path.
//
// Creates no entities and destroys nothing; safe to call on a live world.
// A missing or corrupt `.hbgi` CLEARS the GI handles and records the reason in
// SceneEnvironment::giStatus (it does not inherit the previous scene's volume).
void ApplyEnvironment(Scene& scene, Renderer& renderer, const SceneData& data);

// Writes every PaintComponent's canvas in `scene` to a `.hbpaint` file under
// `assetsDir`/Paint/ (assigning the component's `source` when unset, derived
// from `sceneStem` + the entity name) so SaveScene can reference them. Editor
// save path only - the runtime never saves scenes. Call before SaveScene.
void SavePaintCanvases(Scene& scene, const std::filesystem::path& assetsDir,
                       const std::string& sceneStem);

// Assigns a `source` to every PaintComponent that does not have one and writes JUST
// those canvases. The SNAPSHOT-path counterpart of SavePaintCanvases: BuildSceneJson
// skips a sourceless canvas, so without this a canvas painted before the scene was
// ever saved is missing from the play/undo snapshot and is destroyed by the Replace
// that Stop-Play and Undo perform. Cheap to call repeatedly - a canvas that already
// has a file is left alone.
void EnsurePaintSources(Scene& scene, const std::filesystem::path& assetsDir,
                        const std::string& sceneStem);

// --- Convenience (synchronous) ------------------------------------------------
// Writes `scene` to a .hbscene. With `include` set, only entities it accepts are
// written - used to save one of several loaded scenes (active vs. streamed) back
// to its own file instead of merging the whole registry into one.
// `shards` (optional) is the save-time shard bake's output for exactly the entities
// `include` accepts; it is written to the header as "tagShards". Pass nullptr and
// the key is omitted entirely, which is what every non-editor caller does - a
// snapshot, a test scene or a `.hbprefab` has no streaming geometry to describe.
// Run tagshard::BakeScene FIRST (it is what stamps each entity's shard index).
// `headerFrom` (optional) supplies the LOOK half of the header - ambientIntensity,
// exposure, shadowDistance, post, giSource and the day/night override - instead of
// reading it off the live scene environment. It exists for the streamed-scene
// save-back: an additively loaded file's look was never applied to the live
// environment (scene::ApplyEnvironment does nothing on an additive load), so
// writing the live values into it would silently overwrite that file's authored
// look with the ACTIVE document's - giSource included, which lights a level with
// another level's baked volume. Pass the destination file's own parsed header.
// nullptr = the live environment, which is right for every other caller.
bool SaveScene(const Scene& scene, const std::filesystem::path& path,
               const std::function<bool(entt::entity)>& include = {},
               SceneKind kind = SceneKind::Full,
               const std::vector<ShardDesc>* shards = nullptr,
               const SceneData* headerFrom = nullptr);

// The live environment's header fields as a SceneData (entities empty). The
// inverse of scene::ApplyEnvironment, and what SaveScene writes when `headerFrom`
// is null.
SceneData HeaderOf(const SceneEnvironment& env);
bool LoadScene(Scene& scene, Renderer& renderer, const std::filesystem::path& path,
               LoadMode mode = LoadMode::Replace);

// --- In-memory snapshots (editor undo/redo, .hbsave) ----------------------------
// Serializes the scene to a JSON string / parses one back into SceneData.
//
// With `include` set, only entities it accepts are written - the same predicate
// SaveScene takes. The `.hbsave` writer uses it to EXCLUDE StreamShard holders: a
// streamed shard's entities are restored by respawning the shard from the level file
// and replaying its persisted state, not by baking them into the save. That is only
// correct because StreamShard is transitive (it reaches spawned NPCs, character parts
// and debris); otherwise a spawned NPC would be restored as an authored entity AND
// its freshly-restored Spawner would burst again (Spawner::maxAlive defaults to 0 =
// uncapped).
std::string SaveSceneToString(const Scene& scene,
                              const std::function<bool(entt::entity)>& include = {});
bool ParseSceneString(const std::string& text, SceneData& out);

// Serializes a single entity and its descendants to a JSON string (editor
// clipboard copy/paste / duplicate). The captured root has no parent; paste it
// with ParseSceneString + StageAssets + Instantiate(LoadMode::Additive).
std::string SaveSubtreeToString(const Scene& scene, entt::entity root);

// Instantiate keeps process-wide GPU caches (mesh source -> handle, texture
// name -> handle) so repeated loads — undo/redo restores, scene switches —
// never re-upload identical data, and StageAssets skips disk IO for anything
// already resident. Call this when assets change on disk (import, delete) or
// a different project opens.
void ClearInstantiateCaches();

// Publishes a mesh the CALLER already uploaded into that cache, under the same
// "uaf:<rel>#<n>" key Instantiate would resolve it by.
//
// For a tool that GENERATES geometry, uploads it, and writes the same geometry to
// disk in one step - the 3D paint tool's ribbon strokes are the case. Without this
// the next scene load misses the cache, re-reads the `.uaf` and uploads a SECOND
// copy of bytes already resident; the RHI has no mesh destroy, so the first copy is
// leaked for the process lifetime, once per stroke. `bounds` must be the bounds of
// the same data (Instantiate hands them straight to the AABB component on a hit).
void CacheUploadedMesh(const std::string& key, rhi::MeshHandle mesh, const AABB& bounds);

// How many blendshape delta atlases have been built AND uploaded since process
// start (or the last ClearInstantiateCaches). Diagnostic, and the observable pin on
// blocker B3's leak half: respawning an entity whose mesh already resolved must
// leave this UNCHANGED. There is no texture release in the RHI, so every increment
// is permanent VRAM.
u32 MorphAtlasBuildCount();

// Headless proof of the blendshape/morph cache (--test-morphcache): the SECOND
// spawn of a mesh keeps its resolved blendshapes even though StageAssets skipped
// the CPU model, no second atlas is built or uploaded on a respawn (the leak half),
// a morph entity whose mesh was already cached by a morph-LESS entity still
// resolves, a mesh with no blendshapes caches its negative instead of retrying
// forever, and `.uaf` v8 round-trips morph targets at all (before v8 the importer
// dropped them, so nothing downstream could ever have worked). No GPU, no window.
bool MorphCacheSelfTest();

// Headless proof that a level is ONE scene file (--test-noleveltypes). Asserts
// there is no UI scene kind left, that a file merely NAMED "<base>.static.hbscene"
// is loaded as an ordinary standalone scene (no sibling ".dynamic" layer is
// composed in), that the per-object Static/Dynamic tag the navmesh reads survives
// a round trip, and that SaveScene -> ParseSceneFile -> SaveScene of a merged
// scene is byte-identical. No GPU, no window.
bool LevelTypesSelfTest();

// One-time migration: stamp the DERIVED guid of every guid-less entity into every
// `.hbscene` under `assetsDir` (recursively), so the index-based derivation in
// ParseSceneJson never has to run on those files again.
//
// WHY IT IS TIME-SENSITIVE. A pre-guid scene's guids are derived from
// hash(file identity, entity INDEX). Stable across launches - but inserting or
// reordering an entity shifts every derived guid after that point. Harmless while
// nothing is keyed to a guid; silent state corruption the moment per-entity state
// is persisted BY guid. Run this before that happens.
//
// It is a SURGICAL JSON EDIT, not a load/save round-trip - one key added per
// entity, nothing else touched, no GPU needed - and it calls the very same
// guid::SeedFromPath / guid::Derive that ParseSceneJson calls, so the values
// written are by construction the ones already in use. Semantically a no-op,
// which is exactly what makes it safe to run on authored content.
//
// `.hbprefab` is deliberately EXCLUDED: a prefab is a template that mints fresh
// guids per instantiation, so freezing them would alias every instance.
struct GuidMigrationStats { u32 files = 0, stamped = 0, already = 0, failed = 0; };
GuidMigrationStats MigrateSceneGuids(const std::filesystem::path& assetsDir,
                                     bool dryRun);

// Headless proof that PARTIAL instantiation is correct (--test-sceneslice), which
// is what the whole of tag streaming stands on. Loads one parsed scene twice: once
// whole, once as two disjoint slices, and asserts the two worlds are the same
// world - same entities by guid, same components, same hierarchy - EXCEPT that a
// parent link crossing the slice boundary becomes a root. Also pins: no Parent is
// ever emplaced on a null/invalid handle (blocker B1), the environment is applied
// exactly once by BindWorld and never by a slice, a second bind does not stack a
// second world, guids stay unique across slices, Replace-with-a-slice is refused,
// and StageAssets stages strictly the slice's assets. No GPU (a device-less
// Renderer uploads nothing), no window.
bool SceneSliceSelfTest();

// Headless proof that THE EDITOR AND THE SHIPPED GAME LIGHT A SCENE THE SAME WAY
// (--test-lightingparity). The user-visible bug this exists for: an interior
// surrounded by cubes rendered correctly dark in the game and wrongly bright in
// the editor, which makes lighting unauthorable.
//
// It loads the SAME file through both real paths -
//   editor:  scene::LoadScene(...)                    (Editor::LoadSceneInEditor)
//   runtime: scene::BindWorld + every shard Additive  (stream::Streamer::BindLevel)
// - and asserts the resulting EnvironmentStamp is IDENTICAL: all five header
// fields plus giSource/giStatus/giOrigin/giSpacing/giDims and the VALIDITY of the
// giSh/giDepth handles. Covers a scene WITH a baked `.hbgi` and one without, plus
// a missing and a corrupt `.hbgi` (which must be reported and must NOT inherit the
// previously loaded scene's volume), additive loads contributing no environment,
// re-bind idempotence, and shard-order independence. No GPU, no window; it creates
// its own scratch project so `giSource` resolves.
bool LightingParitySelfTest();

// Headless proof of the paint-canvas save path (--test-paintcanvas): every
// PaintComponent gets a `source` (BuildSceneJson silently skips any that does
// not, which loses the painting), name collisions get distinct files, an
// already-assigned source is never reassigned, and the result reloads. No GPU.
bool PaintCanvasSelfTest();

// ---------------------------------------------------------------------------
// PER-COMPONENT DELTAS - the seam collaborative editing needs.
// ---------------------------------------------------------------------------
//
// Saving is monolithic: EntityToJson writes an entity's whole component set, and
// Instantiate applies one inline while CREATING the entity. Neither shape works for
// a collaboration client, which has to say "component C of an entity that already
// exists becomes this" and nothing else - sending a whole entity would make every
// nudge of one object overwrite every other field a second artist was editing.
//
// These three functions are that seam. The WRITE side reuses EntityToJson verbatim
// and extracts one key, so a delta can never disagree with what a save would have
// produced. The APPLY side is an explicit per-key table.
//
// COVERAGE IS DELIBERATELY PARTIAL, AND AN UNSUPPORTED KEY IS REFUSED, NEVER
// IGNORED. A silent no-op would let a client believe an edit landed when the other
// machines never saw it - a divergence with no symptom until someone saves. Adding a
// component is one entry in kAppliers plus one test row.
enum class DeltaApply : u8 {
    Applied,     // the component was written onto the entity
    Removed,     // an empty payload removed it
    UnknownKey,  // not in the delta table - the caller MUST surface this
    BadJson,     // the payload did not parse or had the wrong shape
    NoEntity,    // that entity does not exist in this registry
};
const char* DeltaApplyName(DeltaApply r);

// Component keys this build can send and apply, in a stable order.
const std::vector<std::string>& DeltaComponentKeys();
bool IsDeltaComponent(const std::string& key);

// Serializes ONE component to JSON. False when the entity does not have it (which is
// how a caller distinguishes "unchanged" from "removed"). The text is exactly the
// sub-object a full save would write for that key.
bool ComponentToJson(const Scene& scene, entt::entity e, const std::string& key,
                     std::string& outJson);

// Applies one component to an EXISTING entity. An empty `json` removes it.
DeltaApply ApplyComponentJson(Scene& scene, entt::entity e, const std::string& key,
                              const std::string& json);

// --test-componentdelta: proves every registered key round-trips through
// ComponentToJson -> ApplyComponentJson onto a DIFFERENT entity and lands identical,
// that removal works, and that an unknown key is refused rather than ignored.
bool ComponentDeltaSelfTest();

} // namespace scene
} // namespace hbe
