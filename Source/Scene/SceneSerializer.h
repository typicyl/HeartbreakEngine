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

namespace scene {

enum class LoadMode {
    Replace,  // clear the registry first
    Additive, // merge into the current world
};

// CPU-side description of one serialized entity.
struct EntityData {
    std::string name;
    bool editorHidden = false; // editor-only visibility (runtime ignores)
    bool hasTransform = false;
    Transform transform;
    int parent = -1; // index into SceneData::entities

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
    bool hasCameraSpline = false;
    CameraSpline cameraSpline;
    bool hasTerrain = false;
    TerrainComponent terrain;
    bool hasMotionMatching = false;
    MotionMatching motionMatching;
    bool hasRotator = false;
    Rotator rotator;
    bool hasCharacter = false;
    CharacterController character;
    bool hasIK = false;
    IKConstraint ik;
    bool hasUI = false;
    UIElement uiElement;
    bool hasUICanvas = false;
    UICanvas uiCanvas;
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

    // Runtime-only tags carried by in-memory snapshots (play mode, undo/redo) so
    // a Replace restore preserves which scene/level-layer each entity belongs to.
    // Absent in on-disk scene files (those are partitioned per SceneSource).
    bool hasSceneSourceTag = false;
    std::string sceneSourceTag;
    bool hasSceneLayerTag = false;
    SceneKind sceneLayerKind = SceneKind::Static;
};

struct SceneData {
    std::vector<EntityData> entities;
    f32 ambientIntensity = 1.0f;
    f32 exposure = 1.0f;
    std::string giSource; // cached .hbgi GI volume (rel to Assets)
    rhi::PostSettings post; // HDR post-process stack (defaults = effects on)
    // Level layer this file represents (header "kind"). Full = standalone scene.
    SceneKind kind = SceneKind::Full;
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

// --- Phases ------------------------------------------------------------------
// Parses a .hbscene file. Returns false on IO/JSON failure.
bool ParseSceneFile(const std::filesystem::path& path, SceneData& out);
// Loads every asset SceneData references from `assetsDir` (CPU only;
// thread-safe: touches no registry or GPU state).
void StageAssets(const SceneData& data, const std::filesystem::path& assetsDir,
                 StagedAssets& out);
// Creates entities/uploads GPU resources. Main thread only. When `createdOut`
// is non-null it receives every entity created (used by StreamingWorld to track
// a cell's entities so it can destroy exactly them on unload). When `sceneTag`
// is non-empty, every created entity gets a SceneSource{sceneTag} so the editor
// hierarchy can group it under its source scene (used for additive/streamed
// loads; the main Replace load leaves entities untagged = the active scene).
void Instantiate(Scene& scene, Renderer& renderer, const SceneData& data,
                 StagedAssets& staged, LoadMode mode,
                 std::vector<entt::entity>* createdOut = nullptr,
                 const std::string& sceneTag = {});

// Writes every PaintComponent's canvas in `scene` to a `.hbpaint` file under
// `assetsDir`/Paint/ (assigning the component's `source` when unset, derived
// from `sceneStem` + the entity name) so SaveScene can reference them. Editor
// save path only - the runtime never saves scenes. Call before SaveScene.
void SavePaintCanvases(Scene& scene, const std::filesystem::path& assetsDir,
                       const std::string& sceneStem);

// --- Convenience (synchronous) ------------------------------------------------
// Writes `scene` to a .hbscene. With `include` set, only entities it accepts are
// written - used to save one of several loaded scenes (active vs. streamed) back
// to its own file instead of merging the whole registry into one.
bool SaveScene(const Scene& scene, const std::filesystem::path& path,
               const std::function<bool(entt::entity)>& include = {},
               SceneKind kind = SceneKind::Full);
bool LoadScene(Scene& scene, Renderer& renderer, const std::filesystem::path& path,
               LoadMode mode = LoadMode::Replace);

// --- Levels (Naughty-Dog-style static / dynamic / UI split) -----------------
// A level is three sibling scene files sharing a base name:
//   <base>.static.hbscene   <base>.dynamic.hbscene   <base>.ui.hbscene
// Any single member identifies the whole level; a level may omit a layer.
struct LevelPaths {
    std::filesystem::path base; // <dir>/<name> with no suffix
    std::string Name() const { return base.filename().string(); }
    // Path of one layer file ("" for SceneKind::Full).
    std::filesystem::path Member(SceneKind kind) const;
};
// True when `p` is a "<name>.static|dynamic|ui.hbscene" level layer file.
bool IsLevelMember(const std::filesystem::path& p);
// Resolves the level that a member path (or a base path) belongs to.
LevelPaths ResolveLevel(const std::filesystem::path& memberOrBase);
// Loads a level: the first existing layer replaces the world (and owns the
// environment), the others load additively; every layer is tagged with its file
// path (SceneSource) + SceneLayer kind. Returns true if any layer loaded.
// When `additive`, ALL layers load additively (no clear, no environment change)
// so the level stacks onto the current world - used to compose several levels.
bool LoadLevel(Scene& scene, Renderer& renderer, const LevelPaths& level,
               std::vector<entt::entity>* createdOut = nullptr, bool additive = false);

// --- In-memory snapshots (editor undo/redo) -------------------------------------
// Serializes the scene to a JSON string / parses one back into SceneData.
std::string SaveSceneToString(const Scene& scene);
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

} // namespace scene
} // namespace hbe
