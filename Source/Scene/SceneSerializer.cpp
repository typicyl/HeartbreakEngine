// Scene/SceneSerializer.cpp
#include "Scene/SceneSerializer.h"

#include "Assets/AssetLoader.h"
#include "Assets/MeshGenerator.h"
#include "Assets/VFS.h"
#include "Core/Log.h"
#include "Renderer/IBL.h"
#include "Renderer/Renderer.h"
#include "Project/Project.h"
#include "Scene/AnimationSystem.h"
#include "Scene/CharacterSystem.h"
#include "Scene/EntityGuid.h"
#include "Scene/Hierarchy.h" // BuildChildrenMap (the one parent->children walk)
#include "Scene/PaintSystem.h"
#include "Scene/PostSettingsSerialization.h"
#include "Scene/Scene.h"
#include "Scene/TagTable.h" // tags::Name / Intern / Assign (streaming groups)
#include "Volume/VolumeSimConfigIO.h" // ConfigToJson/ConfigFromJson (VolumeComponent embedded sim)
#include "UI/UIDocumentJson.h" // the shared per-component UI JSON blocks (.hbscene + .hbui)
#include "UI/UISystem.h" // PreloadUIAssets (eager UI font/texture load)

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hbe::scene {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

json ToJson(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }
json ToJson(const glm::vec4& v) { return json::array({v.x, v.y, v.z, v.w}); }
json ToJson(const glm::quat& q) { return json::array({q.w, q.x, q.y, q.z}); }

glm::vec3 Vec3(const json& j, glm::vec3 def = glm::vec3(0.0f)) {
    if (!j.is_array() || j.size() < 3) return def;
    return {j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>()};
}
glm::vec4 Vec4(const json& j, glm::vec4 def = glm::vec4(1.0f)) {
    if (!j.is_array() || j.size() < 4) return def;
    return {j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>(), j[3].get<f32>()};
}
glm::quat Quat(const json& j) {
    if (!j.is_array() || j.size() < 4) return glm::quat(1, 0, 0, 0);
    return glm::quat(j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>(), j[3].get<f32>());
}

// CameraComponent <-> JSON (shared by an entity's Camera and a CameraZone's
// inline override settings).
json CameraToJson(const CameraComponent& c) {
    return json{{"fov", c.fovY},
                {"near", c.nearZ},
                {"far", c.farZ},
                {"primary", c.primary},
                {"mode", static_cast<int>(c.mode)},
                {"rotation", static_cast<int>(c.rotation)},
                {"target", c.target},
                {"offset", ToJson(c.offset)},
                {"distance", c.distance},
                {"yaw", c.yaw},
                {"pitch", c.pitch},
                {"positionDamping", c.positionDamping},
                {"rotationDamping", c.rotationDamping},
                {"spinSpeed", c.spinSpeed},
                {"fixedEuler", ToJson(c.fixedEuler)},
                {"spline", c.spline},
                {"splineSpeed", c.splineSpeed},
                {"splineLoop", c.splineLoop},
                {"playerLook", c.playerLook},
                {"lookSensitivity", c.lookSensitivity},
                {"lookStickSpeed", c.lookStickSpeed},
                {"invertLookY", c.invertLookY},
                {"lookPitchMin", c.lookPitchMin},
                {"lookPitchMax", c.lookPitchMax},
                {"collide", c.collide},
                {"collisionMinDistance", c.collisionMinDistance},
                {"collisionPadding", c.collisionPadding},
                {"collisionReturnSpeed", c.collisionReturnSpeed},
                // Cinematic rig (nested so the camera block stays readable).
                {"cinematic",
                 {{"handheld", c.cinematic.handheld},
                  {"handheldPosAmount", c.cinematic.handheldPosAmount},
                  {"handheldRotAmount", c.cinematic.handheldRotAmount},
                  {"handheldFrequency", c.cinematic.handheldFrequency},
                  {"handheldRoll", c.cinematic.handheldRoll},
                  {"handheldSharpness", c.cinematic.handheldSharpness},
                  {"breathing", c.cinematic.breathing},
                  {"breathAmount", c.cinematic.breathAmount},
                  {"breathRate", c.cinematic.breathRate},
                  {"framing", c.cinematic.framing},
                  {"framingX", c.cinematic.framingX},
                  {"framingY", c.cinematic.framingY},
                  {"leadAmount", c.cinematic.leadAmount},
                  {"leadSpeed", c.cinematic.leadSpeed},
                  {"framingDamping", c.cinematic.framingDamping}}}};
}

void CameraFromJson(const json& j, CameraComponent& c) {
    c.fovY = j.value("fov", 60.0f);
    c.nearZ = j.value("near", 0.1f);
    c.farZ = j.value("far", 500.0f);
    c.primary = j.value("primary", true);
    c.mode = static_cast<CameraComponent::Mode>(glm::clamp(j.value("mode", 0), 0, 5));
    c.rotation =
        static_cast<CameraComponent::RotationMode>(glm::clamp(j.value("rotation", 0), 0, 4));
    c.target = j.value("target", "");
    c.offset = Vec3(j.value("offset", json()), c.offset);
    c.distance = j.value("distance", c.distance);
    c.yaw = j.value("yaw", c.yaw);
    c.pitch = j.value("pitch", c.pitch);
    c.positionDamping = j.value("positionDamping", c.positionDamping);
    c.rotationDamping = j.value("rotationDamping", c.rotationDamping);
    c.spinSpeed = j.value("spinSpeed", c.spinSpeed);
    c.fixedEuler = Vec3(j.value("fixedEuler", json()), c.fixedEuler);
    c.spline = j.value("spline", "");
    c.splineSpeed = j.value("splineSpeed", c.splineSpeed);
    c.splineLoop = j.value("splineLoop", c.splineLoop);
    c.playerLook = j.value("playerLook", c.playerLook);
    c.lookSensitivity = j.value("lookSensitivity", c.lookSensitivity);
    c.lookStickSpeed = j.value("lookStickSpeed", c.lookStickSpeed);
    c.invertLookY = j.value("invertLookY", c.invertLookY);
    c.lookPitchMin = j.value("lookPitchMin", c.lookPitchMin);
    c.lookPitchMax = j.value("lookPitchMax", c.lookPitchMax);
    c.collide = j.value("collide", c.collide);
    c.collisionMinDistance = j.value("collisionMinDistance", c.collisionMinDistance);
    c.collisionPadding = j.value("collisionPadding", c.collisionPadding);
    c.collisionReturnSpeed = j.value("collisionReturnSpeed", c.collisionReturnSpeed);
    if (const auto cit = j.find("cinematic"); cit != j.end() && cit->is_object()) {
        cam::CinematicSettings& cs = c.cinematic;
        cs.handheld = cit->value("handheld", cs.handheld);
        cs.handheldPosAmount = cit->value("handheldPosAmount", cs.handheldPosAmount);
        cs.handheldRotAmount = cit->value("handheldRotAmount", cs.handheldRotAmount);
        cs.handheldFrequency = cit->value("handheldFrequency", cs.handheldFrequency);
        cs.handheldRoll = cit->value("handheldRoll", cs.handheldRoll);
        cs.handheldSharpness = cit->value("handheldSharpness", cs.handheldSharpness);
        cs.breathing = cit->value("breathing", cs.breathing);
        cs.breathAmount = cit->value("breathAmount", cs.breathAmount);
        cs.breathRate = cit->value("breathRate", cs.breathRate);
        cs.framing = cit->value("framing", cs.framing);
        cs.framingX = glm::clamp(cit->value("framingX", cs.framingX), -1.0f, 1.0f);
        cs.framingY = glm::clamp(cit->value("framingY", cs.framingY), -1.0f, 1.0f);
        cs.leadAmount = glm::max(cit->value("leadAmount", cs.leadAmount), 0.0f);
        cs.leadSpeed = glm::max(cit->value("leadSpeed", cs.leadSpeed), 0.01f);
        cs.framingDamping = glm::max(cit->value("framingDamping", cs.framingDamping), 0.0f);
    }
    c.orbitAngle = c.yaw; // start orbit/spin at the authored yaw
}

// Splits "uaf:path#3" into path + submesh index. Returns false for non-uaf.
bool SplitUafSource(const std::string& source, std::string& outPath, u32& outIndex) {
    if (source.rfind("uaf:", 0) != 0) return false;
    const std::string rest = source.substr(4);
    const auto hash = rest.find_last_of('#');
    outPath = rest.substr(0, hash);
    outIndex = 0;
    if (hash != std::string::npos) {
        outIndex = static_cast<u32>(std::strtoul(rest.c_str() + hash + 1, nullptr, 10));
    }
    return !outPath.empty();
}

// Rebuilds a Mesh/ConvexHull collider's geometry from the entity's mesh (it is
// not serialized). prim:* meshes regenerate; uaf:* meshes come from the staged
// models (StageAssets force-loads them for mesh-collider entities).
void FillColliderGeometry(RigidBody& rb, const std::string& meshSource, StagedAssets& staged) {
    if (rb.shape != RigidBody::Shape::Mesh && rb.shape != RigidBody::Shape::ConvexHull) return;
    if (!rb.collisionVertices.empty()) return;
    MeshData prim;
    const MeshData* md = nullptr;
    if (meshSource.rfind("prim:", 0) == 0) {
        prim = mesh::GeneratePrimitive(meshSource.substr(5));
        md = &prim;
    } else if (meshSource.rfind("uaf:", 0) == 0) {
        const std::string rest = meshSource.substr(4);
        const auto hash = rest.find_last_of('#');
        const std::string rel = rest.substr(0, hash);
        const u32 submesh =
            hash == std::string::npos ? 0u
                                      : static_cast<u32>(std::strtoul(rest.c_str() + hash + 1,
                                                                      nullptr, 10));
        if (auto it = staged.models.find(rel);
            it != staged.models.end() && submesh < it->second.size()) {
            md = &it->second[submesh];
        }
    }
    if (!md || md->vertices.empty()) return;
    rb.collisionVertices.reserve(md->vertices.size());
    for (const Vertex& v : md->vertices) rb.collisionVertices.push_back(v.position);
    rb.collisionIndices = md->indices;
}

// A RESOLVED blendshape atlas for one mesh source ("uaf:<rel>#<n>"). This is a
// cache VALUE, and `hasMorphs == false` is a real answer ("that mesh has no
// blendshapes"), not a failure - which is what lets a negative be cached and stop
// the lookup from being retried for every instance of every prop.
//
// It exists as a cache entry because deriving it per spawn is plan blocker B3, and
// both halves of B3 come from that: StageAssets skips loading the CPU model when
// the GPU mesh is already cache-resident, so the SECOND spawn of a mesh had no
// MeshData to read morphs out of and silently lost facial animation for the rest
// of the session; and on the one path that does keep staging (a Mesh/ConvexHull
// collider needs CPU geometry) the atlas was re-uploaded on every respawn with no
// release, leaking one atlas per cycle - there is no texture destroy in the RHI.
struct MorphAtlas {
    rhi::TextureHandle texture; // bindless delta atlas (invalid if hasMorphs == false)
    u32 vertexCount = 0;
    std::vector<std::string> names; // atlas row order
    bool hasMorphs = false;
};

// How many atlases have actually been BUILT+uploaded since process start (reset by
// ClearInstantiateCaches). A respawn must not increment it - see MorphAtlasBuildCount.
std::atomic<u32>& MorphBuilds() {
    static std::atomic<u32> n{0};
    return n;
}

// Builds + uploads a blendshape delta atlas from CPU mesh data. Layout: RGBA32F,
// width = vertex count, one ROW per target (xyz = position delta). Returns an
// entry with hasMorphs == false when the mesh carries no blendshapes.
//
// Callers go through ResolveMorphAtlas, never here: this function is the cache
// MISS path and knows nothing about caching or about which mesh it was handed.
MorphAtlas BuildMorphAtlasFromMesh(Renderer& renderer, const MeshData& md) {
    MorphAtlas out;
    const u32 w = md.VertexCount();
    const u32 h = static_cast<u32>(md.morphTargets.size());
    if (w == 0 || h == 0) return out; // no blendshapes: a definitive answer
    std::vector<glm::vec4> pix(static_cast<usize>(w) * h, glm::vec4(0.0f));
    for (u32 t = 0; t < h; ++t) {
        const MorphTarget& mt = md.morphTargets[t];
        out.names.push_back(mt.name);
        const usize n = std::min<usize>(mt.posDelta.size(), w);
        for (usize v = 0; v < n; ++v)
            pix[static_cast<usize>(t) * w + v] = glm::vec4(mt.posDelta[v], 0.0f);
    }
    rhi::TextureDesc desc;
    desc.width = w;
    desc.height = h;
    desc.format = rhi::Format::R32G32B32A32_FLOAT;
    desc.pixels = pix.data();
    desc.debugName = "morph_atlas";
    out.vertexCount = w;
    out.hasMorphs = true;
    out.texture = renderer.UploadTexture(desc); // invalid without a device (headless)
    MorphBuilds().fetch_add(1, std::memory_order_relaxed);
    return out;
}

rhi::TextureHandle UploadStagedTexture(Renderer& renderer, uaf::Texture& tex,
                                       const char* name) {
    assets::GenerateMips(tex);
    rhi::TextureDesc d;
    d.width = tex.width;
    d.height = tex.height;
    d.format = static_cast<rhi::Format>(tex.format);
    d.mipCount = tex.mipCount;
    d.pixels = tex.pixels.data();
    d.debugName = name;
    return renderer.UploadTexture(d);
}

} // namespace

// The scene writer's SKIP LIST, lifted out of BuildSceneJson so that anything which
// has to agree with "what actually lands in the file" reads the same predicate
// instead of copying the list. The shard bake is the first such caller: its
// per-shard member `count` is cross-checked at load against the entities the FILE
// contains, so a bake that counted an entity the writer drops would report every
// shard as corrupt.
// THE EXCLUSION LIST. One table, one reason per row, and the reason is the
// REGENERATOR: an entity may be left out of the file if and only if something
// puts it back. Nothing else may be omitted, ever - a `.hbscene` is the only
// complete copy of an authored level.
//
// Kept as data rather than a chain of `if`s so that the writer, the shard bake,
// BuildSubtreeJson and --test-scenesave all read the SAME rows, and so that the
// reason travels with the rule instead of living in a comment somewhere upstream.
// Adding a row here is a claim that must be true; --test-scenesave checks the
// claim for every excluded entity it can reach.
namespace {
struct Exclusion {
    // Does `e` match this row?
    bool (*match)(const entt::registry&, entt::entity);
    const char* component;
    const char* regenerator;
};

constexpr Exclusion kExclusions[] = {
    {[](const entt::registry& r, entt::entity e) { return r.all_of<TerrainChunk>(e); },
     "TerrainChunk",
     "terrain::Update destroys and rebuilds every chunk from TerrainComponent::heights, "
     "which IS serialized (with holeMask + splat); parse sets the dirty flag"},
    {[](const entt::registry& r, entt::entity e) { return r.all_of<UISurface>(e); },
     "UISurface",
     "ui::UpdateWorldSurfaces re-creates the page quad + render target from its "
     "UICanvas every frame"},
    // Transient UI the conversation player spawns; a checkpoint landing mid-Choice
    // must NOT bake them into the .hbsave (they would reload as permanent, tag-less,
    // un-clearable UI).
    {[](const entt::registry& r, entt::entity e) { return r.all_of<DialogueChoiceButton>(e); },
     "DialogueChoiceButton",
     "Engine::ClearDialogueChoices creates and destroys these per conversation"},
    {[](const entt::registry& r, entt::entity e) { return r.all_of<InteractPromptTag>(e); },
     "InteractPromptTag",
     "Engine::UpdateInteractions stamps the prompt/icon fresh each frame"},
    // Modular-character parts: serializing them would double them up as static
    // bind-pose meshes alongside the freshly-spawned ones.
    {[](const entt::registry& r, entt::entity e) { return r.all_of<SkinnedPartRef>(e); },
     "SkinnedPartRef",
     "character::Instantiate respawns every slot from the .hbchar on load; the root's "
     "Character{asset,loadout,activeVariant} is serialized and is what it respawns from"},
    {[](const entt::registry& r, entt::entity e) { return r.all_of<DebrisChunk>(e); },
     "DebrisChunk",
     "rebuilt from the owning Destructible::chunkState on reactivate, so a save landing "
     "mid-collapse does not bake the loose pieces in as authored props"},
    // See EntityGuid.h rule 4: writing it means a restore re-creates all of it
    // alongside the copies the Replace sweep spared - duplicate UI, with freshly
    // minted per-launch identities on the duplicates.
    {[](const entt::registry& r, entt::entity e) { return r.all_of<Persistent>(e); },
     "Persistent",
     "the resident UI layer is loaded ONCE from the project's uiScene at boot and is "
     "the exact set LoadMode::Replace refuses to destroy"},
    {[](const entt::registry& r, entt::entity e) { return r.all_of<UIDocMember>(e); },
     "UIDocMember",
     "a .hbui DOCUMENT's entities are ASSET content with their own file; the editor "
     "writes them through SaveUIDocument and rebuilds them from it on the next open"},
};
} // namespace

const char* SceneWriteExclusion(const entt::registry& reg, entt::entity e,
                                const char** regeneratorOut) {
    for (const Exclusion& x : kExclusions) {
        if (!x.match(reg, e)) continue;
        if (regeneratorOut) *regeneratorOut = x.regenerator;
        return x.component;
    }
    if (regeneratorOut) *regeneratorOut = nullptr;
    return nullptr;
}

usize SceneWriteExclusionCount() { return std::size(kExclusions); }

const char* SceneWriteExclusionRow(usize i, const char** regeneratorOut) {
    if (i >= std::size(kExclusions)) return nullptr;
    if (regeneratorOut) *regeneratorOut = kExclusions[i].regenerator;
    return kExclusions[i].component;
}

bool IsSerializedEntity(const entt::registry& reg, entt::entity e) {
    return SceneWriteExclusion(reg, e, nullptr) == nullptr;
}

std::string SaveRefusal(const Scene& scene, u64 expectedWorldToken) {
    // 1) IDENTITY. The registry is no longer the world the caller loaded, so the path
    //    it is holding names a different level. Refusing is the whole point: the
    //    alternative is writing world B over level A's file, which is unrecoverable.
    if (expectedWorldToken != 0 && scene.WorldToken() != expectedWorldToken) {
        return "this world was REPLACED since it was loaded (a checkpoint load, a "
               "scene load, or a level bind), so it is not the level this file "
               "describes. Reload the level, or use Save As.";
    }
    // 2) COMPLETENESS. A streamed world only contains what is spawned right now.
    const StreamingResidency& s = scene.Streaming();
    if (!s.bound) return {};
    if (s.nonResident > 0) {
        return std::to_string(s.nonResident) + " of " + std::to_string(s.shardCount) +
               " streaming shard(s) are NOT loaded (" + s.missing +
               "), so this world is missing objects that are in the file.";
    }
    // Every shard happens to be resident - but a world owned by the RUNTIME streamer
    // is a world whose contents change with the camera, and a save is not atomic with
    // respect to that. Authoring belongs to the editor's own load path.
    //
    // AN AUTHORING BIND IS THE EXCEPTION, and only this clause bends for it. The
    // editor's own streamer binds the world the editor already loaded (no BindWorld,
    // no DestroyWorld) precisely so that world can still be authored, and it settles
    // every shard resident BEFORE it reaches this function. The dangerous clause is
    // the one above - a world with holes - and that one fires for an authoring bind
    // exactly as it does for a runtime one.
    if (!s.authoring)
        return "this world is owned by the tag STREAMER (a bound level), whose contents "
               "change with the camera. Load the level in the editor to author it.";
    return {};
}

namespace {
json EntityToJson(const entt::registry& reg, entt::entity e,
                  const std::unordered_map<u32, int>& indexOf, bool runtimeTags = false);

SceneData HeaderOf(const SceneEnvironment& env) {
    SceneData h;
    h.ambientIntensity = env.ambientIntensity;
    h.exposure = env.exposure;
    h.shadowDistance = env.shadowDistance;
    h.giSource = env.giSource;
    h.navSource = env.navSource;
    // Slot identity survives file -> live scene -> save. Without this hop a save to
    // a NEW path (Save As, a migration, a headless tool) silently strips the asset's
    // permanent pack slot, renumbering it at the next cook. SaveScene also carries
    // the key off the file it overwrites, but that only covers saves over an
    // existing file - this is the path that covers the rest.
    h.packSlot = env.packSlot;
    h.docId = env.docId;
    h.guidEpoch = env.guidEpoch;
    h.post = env.post;
    h.hasDayNight = env.dayNightAuthored != 0;
    h.timeOfDay = env.timeOfDay;
    h.dayLengthSeconds = env.dayLengthSeconds;
    h.dynamicSky = env.dynamicSky;
    return h;
}

// Builds the .hbscene JSON document (shared by file saves and snapshots). When
// `include` is set, only entities it accepts are written (used to save each
// loaded scene back to its own file - the active scene vs. streamed-in ones).
json BuildSceneJson(const Scene& scene,
                    const std::function<bool(entt::entity)>& include = {},
                    SceneKind kind = SceneKind::Full, bool runtimeTags = false,
                    const std::vector<ShardDesc>* shards = nullptr,
                    const SceneData* headerFrom = nullptr) {
    const auto& reg = scene.Registry();

    // WHICH entities are written. Gathered from every component type the serializer
    // writes (anything with none of them would round-trip to nothing anyway). This
    // decides membership only - the ORDER is settled separately, below.
    std::vector<entt::entity> order;
    std::unordered_map<u32, int> indexOf;
    std::unordered_set<u32> member;
    const auto add = [&](entt::entity e) {
        if (!IsSerializedEntity(reg, e)) return;
        if (include && !include(e)) return; // not part of the scene being saved
        member.insert(static_cast<u32>(e));
    };
    for (const entt::entity e : reg.view<const Transform>()) add(e);
    for (const entt::entity e : reg.view<const Name>()) add(e);
    for (const entt::entity e : reg.view<const MeshInstance>()) add(e);
    for (const entt::entity e : reg.view<const DirectionalLightComponent>()) add(e);
    for (const entt::entity e : reg.view<const PointLightComponent>()) add(e);
    for (const entt::entity e : reg.view<const SpotLightComponent>()) add(e);
    for (const entt::entity e : reg.view<const RectLightComponent>()) add(e);
    for (const entt::entity e : reg.view<const VolumeComponent>()) add(e);
    for (const entt::entity e : reg.view<const RigidBody>()) add(e);
    for (const entt::entity e : reg.view<const AnimationTrack>()) add(e);
    for (const entt::entity e : reg.view<const AudioSource>()) add(e);
    for (const entt::entity e : reg.view<const DialogueActor>()) add(e);
    for (const entt::entity e : reg.view<const CameraComponent>()) add(e);
    for (const entt::entity e : reg.view<const CameraZone>()) add(e);
    for (const entt::entity e : reg.view<const MusicZone>()) add(e);
    for (const entt::entity e : reg.view<const CameraSpline>()) add(e);
    for (const entt::entity e : reg.view<const TerrainComponent>()) add(e);
    for (const entt::entity e : reg.view<const MotionMatching>()) add(e);
    for (const entt::entity e : reg.view<const Rotator>()) add(e);
    for (const entt::entity e : reg.view<const ModelGroup>()) add(e);
    for (const entt::entity e : reg.view<const CensorComponent>()) add(e);
    for (const entt::entity e : reg.view<const IKConstraint>()) add(e);
    for (const entt::entity e : reg.view<const UIElement>()) add(e);
    for (const entt::entity e : reg.view<const UICanvas>()) add(e);
    for (const entt::entity e : reg.view<const NavigationAgent>()) add(e);
    for (const entt::entity e : reg.view<const NavigationObstacle>()) add(e);
    for (const entt::entity e : reg.view<const NavmeshInput>()) add(e);
    for (const entt::entity e : reg.view<const PostVolume>()) add(e);
    for (const entt::entity e : reg.view<const ReflectionProbe>()) add(e);
    for (const entt::entity e : reg.view<const DecalComponent>()) add(e);
    for (const entt::entity e : reg.view<const WaterComponent>()) add(e);
    for (const entt::entity e : reg.view<const ParticleEmitter>()) add(e);
    for (const entt::entity e : reg.view<const SchematicComponent>()) add(e);
    for (const entt::entity e : reg.view<const Checkpoint>()) add(e);
    for (const entt::entity e : reg.view<const Health>()) add(e);
    for (const entt::entity e : reg.view<const Weapon>()) add(e);
    for (const entt::entity e : reg.view<const AIPerception>()) add(e);
    for (const entt::entity e : reg.view<const AIBehavior>()) add(e);
    for (const entt::entity e : reg.view<const Spawner>()) add(e);
    for (const entt::entity e : reg.view<const Encounter>()) add(e);
    for (const entt::entity e : reg.view<const Spawned>()) add(e);
    for (const entt::entity e : reg.view<const MorphState>()) add(e);
    for (const entt::entity e : reg.view<const FacialAnimator>()) add(e);
    for (const entt::entity e : reg.view<const Interactable>()) add(e);
    for (const entt::entity e : reg.view<const TriggerVolume>()) add(e);
    // Streaming tag. Every tagged entity is gathered by one of the sweeps above in
    // practice (they all carry Name/Transform), but an entity whose ONLY component
    // were a Tag would otherwise be dropped silently - and dropping a member of a
    // streaming group is exactly the failure that is impossible to notice.
    for (const entt::entity e : reg.view<const Tag>()) add(e);

    // THE WRITE ORDER, and why it is not just "whatever the gather produced".
    //
    // The gather above is a sequence of sweeps over DIFFERENT component pools, and an
    // entt view walks its pool in REVERSE insertion order - so the file's entity array
    // came out reversed relative to the world, and reversed AGAIN on the next save.
    // Every save of a level therefore rewrote the entire file with its array flipped
    // (37 MB of churn on the reference level, and a diff that says everything changed),
    // and save -> load -> save was not a fixed point: it took TWO round trips to come
    // back to the same bytes. That is a save whose output depends on how many times it
    // has been saved, which is not a property a save may have.
    //
    // So the order comes from ONE pool - the entity storage - walked once, reversed
    // into creation order. Instantiate creates entities in file order, so file order
    // becomes creation order becomes file order: a fixed point after ONE round trip,
    // for every scene, regardless of which components each entity happens to hold.
    // (Membership is still the multi-sweep set above; this only sequences it.)
    if (const auto* es = reg.storage<entt::entity>()) {
        order.reserve(member.size());
        for (const entt::entity e : *es)
            if (reg.valid(e) && member.count(static_cast<u32>(e))) order.push_back(e);
        std::reverse(order.begin(), order.end());
    }
    // Parent links are indices into the array we are about to write, so this map is
    // built from the FINAL order - never from the gather.
    for (usize i = 0; i < order.size(); ++i)
        indexOf[static_cast<u32>(order[i])] = static_cast<int>(i);

    json root;
    root["version"] = 1;
    if (kind != SceneKind::Full) root["kind"] = ToString(kind);
    // THE HEADER'S SOURCE, and why it is a parameter.
    //
    // A scene file's header describes THAT FILE's look. The live environment
    // describes the look of whatever was loaded with LoadMode::Replace - which for
    // a STREAMED-IN scene is a different file entirely (scene::ApplyEnvironment
    // deliberately applies nothing on an additive load). Writing the live
    // environment into every file the editor happens to have open therefore
    // overwrote each streamed file's ambient/exposure/shadowDistance/post AND its
    // giSource with the active document's, on a plain Ctrl+S - so a streamed level
    // silently adopted another level's baked `.hbgi`, which resolves, loads, and
    // lights the room with an irradiance grid baked for different geometry. That is
    // worse than no GI and completely silent.
    //
    // So the caller may hand in the header to emit (Editor::SaveStreamedScenes
    // re-parses the DESTINATION file and passes its own header straight back).
    // nullptr = "this scene IS the live environment", which is every other caller.
    const SceneData live = HeaderOf(scene.Environment());
    const SceneData& hdr = headerFrom ? *headerFrom : live;
    root["ambientIntensity"] = hdr.ambientIntensity;
    root["exposure"] = hdr.exposure;
    root["shadowDistance"] = hdr.shadowDistance;
    root["post"] = PostToJson(hdr.post);
    if (!hdr.giSource.empty()) root["giSource"] = hdr.giSource;
    if (!hdr.navSource.empty()) root["navSource"] = hdr.navSource;
    // Slot identity survives the round trip (see SceneData::packSlot). Written only
    // when the source carried one, so a never-stamped scene stays byte-identical.
    if (hdr.packSlot != SceneData::kNoPackSlot) root["packSlot"] = hdr.packSlot;
    // COLLABORATION IDENTITY - written ONLY when the document already has one.
    //
    // Minting here was wrong and --test-tagtable caught it immediately: saving is
    // supposed to be DETERMINISTIC (two round trips of one scene are byte-identical,
    // which is what makes a save diffable and a round-trip testable), and minting a
    // random id per write made every save differ from the last.
    //
    // Identity is assigned ONCE, by an explicit act - enabling collaboration on a
    // document, or the central guid migration - not as a side effect of pressing
    // Ctrl+S. Until then these stay 0, which the collaboration layer already reads as
    // "not identified, cannot be merged" rather than as a wildcard. A scene that never
    // collaborates therefore never grows the keys, and its bytes never change.
    if (hdr.docId != 0) root["docId"] = guid::ToHex(hdr.docId);
    if (hdr.guidEpoch != 0) root["guidEpoch"] = guid::ToHex(hdr.guidEpoch);
    // Written only when the scene AUTHORED an override, so a level that inherits the
    // project's cycle stays byte-for-byte what it was before the keys existed.
    if (hdr.hasDayNight) {
        root["timeOfDay"] = hdr.timeOfDay;
        root["dayLengthSeconds"] = hdr.dayLengthSeconds;
        root["dynamicSky"] = hdr.dynamicSky;
    }
    // Baked streaming shards (see ShardDesc). Written only when there are any, so a
    // scene with no tags is byte-for-byte what it was before sharding existed -
    // which is also what keeps the two-round-trip byte-identity assertions in
    // LevelTypesSelfTest / tags::SelfTest honest.
    if (shards && !shards->empty()) {
        json& sa = root["tagShards"] = json::array();
        for (const ShardDesc& s : *shards) {
            sa.push_back(json{{"tag", s.tag},
                              {"index", s.index},
                              {"min", ToJson(s.min)},
                              {"max", ToJson(s.max)},
                              {"count", s.count}});
        }
    }

    json& arr = root["entities"] = json::array();
    for (const entt::entity e : order)
        arr.push_back(EntityToJson(reg, e, indexOf, runtimeTags));
    return root;
}

// Serializes one entity's components to a JSON object. `indexOf` maps entity ->
// array index for parent links; an entity whose parent is outside the map (e.g.
// the root of a copied subtree) serializes no parent and pastes top-level.
json EntityToJson(const entt::registry& reg, entt::entity e,
                  const std::unordered_map<u32, int>& indexOf, bool runtimeTags) {
        json je;
        // Stable per-entity identity, written as a 16-char hex STRING (a 64-bit
        // JSON number is the one thing tooling reliably mangles). Always written,
        // exactly like "sceneLayer" below - saved state keys on this, so an entity
        // that round-trips without one rebinds to a different object on the next
        // load. BuildSubtreeJson deliberately REMOVES it again: a copy/paste,
        // duplicate or prefab instance is a NEW object and must mint a fresh guid.
        if (const Guid* g = reg.try_get<Guid>(e); g && g->value != 0)
            je["guid"] = guid::ToHex(g->value);
        if (const Name* n = reg.try_get<Name>(e)) je["name"] = n->value;
        // Prefab-instance link (root entity of a placed .hbprefab).
        if (const PrefabInstance* pi = reg.try_get<PrefabInstance>(e); pi && !pi->source.empty())
            je["prefab"] = pi->source;
        // Editor-only visibility (hidden but loaded). Runtime ignores it.
        if (reg.all_of<EditorHidden>(e)) je["editorHidden"] = true;
        if (const Transform* t = reg.try_get<Transform>(e)) {
            Transform xf = *t;
            // AN AUTO-PLAYING KEYFRAME TRACK OWNS THIS TRANSFORM, so the live value is
            // a PLAYHEAD POSITION, not authored data - anim::Update samples into it
            // every frame, in the editor as well as in Play. Writing it made a save
            // depend on how long the scene had been open (open the level, walk away,
            // Ctrl+S, and every animated prop is frozen wherever its clip happened to
            // be), which is both authored-value loss and a save that is not a function
            // of the scene.
            //
            // t = 0 is not an arbitrary choice: `time` is deliberately not serialized,
            // so t = 0 is exactly where the entity WILL be on the frame after this file
            // is loaded. Writing it makes the file agree with the world it produces,
            // and makes save -> load -> save a fixed point (--test-scenesave step 4).
            // A paused/stopped track (playing == false) is never sampled on load, so
            // its live transform IS the authored one and is written unchanged.
            if (const AnimationTrack* at = reg.try_get<AnimationTrack>(e);
                at && at->playing && at->duration > 0.0f && !at->keys.empty()) {
                anim::SampleAt(*at, 0.0f, xf);
            }
            je["transform"] = {{"p", ToJson(xf.position)},
                               {"r", ToJson(xf.rotation)},
                               {"s", ToJson(xf.scale)}};
        }
        if (const Parent* p = reg.try_get<Parent>(e)) {
            if (auto it = indexOf.find(static_cast<u32>(p->entity)); it != indexOf.end()) {
                je["parent"] = it->second;
            }
        }
        // SIBLING ORDER. Authored data, written unconditionally like "guid" and
        // "sceneLayer" - the whole point of the field is that hierarchy order stops
        // depending on entt pool order, which a single unrelated delete permutes
        // (swap_and_pop / swap_only) and which this file's ROW order still derives
        // from. See Scene/Hierarchy.h. Absent on read = the file's row index, so
        // older scenes are unaffected.
        if (const HierarchyOrder* ho = reg.try_get<HierarchyOrder>(e))
            je["order"] = ho->index;
        if (const MeshInstance* mi = reg.try_get<MeshInstance>(e)) {
            const MeshRef* ref = reg.try_get<MeshRef>(e);
            je["mesh"] = {{"source", ref ? ref->source : std::string()},
                          {"baseColor", ToJson(mi->baseColor)},
                          {"metallic", mi->metallic},
                          {"roughness", mi->roughness},
                          {"flags", mi->materialFlags},
                          {"subsurfaceColor", ToJson(mi->subsurfaceColor)},
                          {"subsurfaceRadius", mi->subsurfaceRadius},
                          {"clearcoat", mi->clearcoat},
                          {"clearcoatRoughness", mi->clearcoatRoughness},
                          {"emissiveColor", ToJson(mi->emissiveColor)},
                          {"emissiveIntensity", mi->emissiveIntensity}};
            if (const MaterialRef* mat = reg.try_get<MaterialRef>(e)) {
                je["mesh"]["material"] = mat->asset;
            }
        }
        // Art Editor surface paint: store the canvas reference + metadata (pixels
        // live in the .hbpaint file; SavePaintCanvases writes it before saving).
        if (const PaintComponent* pc = reg.try_get<PaintComponent>(e);
            pc && !pc->source.empty()) {
            je["paint"] = {{"source", pc->source},
                           {"resolution", pc->resolution},
                           {"enabled", pc->enabled},
                           {"locked", pc->locked},
                           {"relief", pc->reliefEnabled},
                           {"opacity", pc->opacity},
                           {"heightScale", pc->heightScale},
                           {"lodBias", pc->lodBias},
                           {"layer", pc->layer},
                           {"projection", pc->projection}};
        }
        if (const AABB* box = reg.try_get<AABB>(e)) {
            je["aabb"] = {{"min", ToJson(box->min)}, {"max", ToJson(box->max)}};
        }
        if (const RigidBody* rb = reg.try_get<RigidBody>(e)) {
            const char* shapeStr =
                rb->shape == RigidBody::Shape::Sphere       ? "sphere"
                : rb->shape == RigidBody::Shape::Capsule    ? "capsule"
                : rb->shape == RigidBody::Shape::Mesh        ? "mesh"
                : rb->shape == RigidBody::Shape::ConvexHull  ? "convexHull"
                                                             : "box";
            // Mesh/ConvexHull collider geometry is NOT written - it is rebuilt
            // from the entity's mesh on load (keeps scenes small).
            je["rigidBody"] = {
                {"shape", shapeStr},
                {"motion", rb->motion == RigidBody::Motion::Dynamic ? "dynamic" : "static"},
                {"halfExtents", ToJson(rb->halfExtents)},
                {"radius", rb->radius},
                {"halfHeight", rb->halfHeight},
                {"centerOffset", ToJson(rb->centerOffset)},
                {"friction", rb->friction},
                {"restitution", rb->restitution}};
        }
        if (const DirectionalLightComponent* l = reg.try_get<DirectionalLightComponent>(e)) {
            je["light"] = {{"direction", ToJson(l->direction)},
                           {"color", ToJson(l->color)},
                           {"intensity", l->intensity}};
        }
        if (const PointLightComponent* l = reg.try_get<PointLightComponent>(e)) {
            je["pointLight"] = {{"color", ToJson(l->color)},
                                {"intensity", l->intensity},
                                {"range", l->range}};
        }
        if (const VolumeComponent* v = reg.try_get<VolumeComponent>(e)) {
            je["volume"] = {{"source", v->source},
                            {"playing", v->playing},
                            {"loop", v->loop},
                            {"time", v->time},
                            {"speed", v->speed},
                            {"densityScale", v->render.densityScale},
                            {"emission", v->render.emission},
                            {"extinction", v->render.extinction},
                            {"stepCount", v->render.stepCount},
                            {"shadowSteps", v->render.shadowSteps},
                            {"albedo", ToJson(v->render.albedo)},
                            {"emissionColor", ToJson(v->render.emissionColor)},
                            {"emissionMode", v->render.emissionMode},
                            {"livePreview", v->livePreview},
                            {"previewRes", v->previewRes},
                            {"effectName", v->effectName},
                            {"sim", volume::ConfigToJson(v->sim)}};
        }
        if (const SpotLightComponent* l = reg.try_get<SpotLightComponent>(e)) {
            je["spotLight"] = {{"color", ToJson(l->color)},
                               {"intensity", l->intensity},
                               {"range", l->range},
                               {"innerAngle", l->innerAngle},
                               {"outerAngle", l->outerAngle}};
        }
        if (const RectLightComponent* l = reg.try_get<RectLightComponent>(e)) {
            je["rectLight"] = {{"color", ToJson(l->color)}, {"intensity", l->intensity},
                               {"width", l->width},         {"height", l->height},
                               {"range", l->range},         {"twoSided", l->twoSided}};
        }
        if (const SchematicComponent* sg = reg.try_get<SchematicComponent>(e)) {
            je["schematic"] = {{"asset", sg->asset}};
        }
        if (const Destructible* ds = reg.try_get<Destructible>(e)) {
            je["destructible"] = {{"asset", ds->asset},
                                  {"impulseThreshold", ds->impulseThreshold},
                                  {"chunkHealth", ds->chunkHealth},
                                  {"damageRadius", ds->damageRadius},
                                  {"breakImpulseScale", ds->breakImpulseScale},
                                  {"debrisLifetime", ds->debrisLifetime},
                                  {"density", ds->density},
                                  {"structural", ds->structural},
                                  {"breakEvent", ds->breakEvent}};
            // Runtime break progress rides the .hbsave only (runtimeTags), never the
            // authored scene - saving it there would ship a pre-broken level.
            if (runtimeTags && ds->activated) {
                je["destructible"]["activated"] = true;
                je["destructible"]["chunkState"] = ds->chunkState;
                je["destructible"]["chunkHp"] = ds->chunkHp;
            }
        }
        if (const Checkpoint* cp = reg.try_get<Checkpoint>(e)) {
            je["checkpoint"] = {{"id", cp->id},
                                {"setObjective", cp->setObjective},
                                {"completesObjective", cp->completesObjective},
                                {"halfExtents", ToJson(cp->halfExtents)},
                                {"triggerOnEnter", cp->triggerOnEnter},
                                {"saveOnReach", cp->saveOnReach},
                                {"once", cp->once}};
        }
        if (const Health* h = reg.try_get<Health>(e)) {
            je["health"] = {{"max", h->max},
                            {"current", h->current},
                            {"faction", static_cast<u32>(h->faction)},
                            {"regenRate", h->regenRate},
                            {"regenDelay", h->regenDelay},
                            {"hitInvuln", h->hitInvuln},
                            {"invincible", h->invincible},
                            {"friendlyFire", h->friendlyFire},
                            {"onDeathFlag", h->onDeathFlag},
                            {"onDeathFlagValue", h->onDeathFlagValue},
                            {"onDeathObjective", h->onDeathObjective},
                            {"deathTag", h->deathTag},
                            {"deathClip", h->deathClip}};
            // alive/deathDispatched are RUNTIME state: snapshots + .hbsave only, never
            // the authored .hbscene (a play-test kill must not bake a dead enemy).
            if (runtimeTags) {
                je["health"]["alive"] = h->alive;
                je["health"]["deathDispatched"] = h->deathDispatched;
            }
        }
        if (const Weapon* w = reg.try_get<Weapon>(e)) {
            je["weapon"] = {{"kind", static_cast<u32>(w->kind)},
                            {"damage", w->damage},
                            {"range", w->range},
                            {"fireRate", w->fireRate},
                            {"radius", w->radius},
                            {"impulse", w->impulse},
                            {"meleeArc", w->meleeArc},
                            {"hitRadius", w->hitRadius},
                            {"maxAmmo", w->maxAmmo},
                            {"ammo", w->ammo},
                            {"reserve", w->reserve},
                            {"reloadTime", w->reloadTime}};
        }
        if (const AIPerception* p = reg.try_get<AIPerception>(e)) {
            je["aiPerception"] = {{"sightRange", p->sightRange},
                                  {"sightFovDeg", p->sightFovDeg},
                                  {"eyeHeight", p->eyeHeight},
                                  {"loseSightGrace", p->loseSightGrace},
                                  {"hearingRadius", p->hearingRadius},
                                  {"gainRate", p->gainRate},
                                  {"decayRate", p->decayRate},
                                  {"detectThreshold", p->detectThreshold}};
            if (runtimeTags) { // live awareness restored by snapshots + .hbsave
                je["aiPerception"]["awareness"] = p->awareness;
                je["aiPerception"]["timeSinceSeen"] = p->timeSinceSeen;
            }
        }
        if (const AIBehavior* b = reg.try_get<AIBehavior>(e)) {
            nlohmann::json pts = nlohmann::json::array();
            for (const glm::vec3& p : b->patrolPoints) pts.push_back(ToJson(p));
            je["aiBehavior"] = {{"attackRange", b->attackRange},
                                {"attackDamage", b->attackDamage},
                                {"attackInterval", b->attackInterval},
                                {"investigateTime", b->investigateTime},
                                {"searchTime", b->searchTime},
                                {"fleeHealthFrac", b->fleeHealthFrac},
                                {"startAlerted", b->startAlerted},
                                {"useWeapon", b->useWeapon},
                                {"patrolMode", b->patrolMode},
                                {"waitAtPoint", b->waitAtPoint},
                                {"patrolPoints", pts}};
            if (runtimeTags) { // FSM state restored mid-fight (also stops startAlerted re-firing)
                je["aiBehavior"]["state"] = static_cast<u32>(b->state);
                je["aiBehavior"]["patrolIndex"] = b->patrolIndex;
                je["aiBehavior"]["patrolForward"] = b->patrolForward;
                je["aiBehavior"]["spawnApplied"] = b->spawnApplied;
            }
        }
        if (const Spawner* sp = reg.try_get<Spawner>(e)) {
            je["spawner"] = {{"prefab", sp->prefab},
                             {"encounterId", sp->encounterId},
                             {"spawnerId", sp->spawnerId},
                             {"count", sp->count},
                             {"radius", sp->radius},
                             {"trigger", static_cast<u32>(sp->trigger)},
                             {"halfExtents", ToJson(sp->halfExtents)},
                             {"requiredFlag", sp->requiredFlag},
                             {"maxAlive", sp->maxAlive},
                             {"respawn", static_cast<u32>(sp->respawn)},
                             {"respawnDelay", sp->respawnDelay}};
            if (runtimeTags) { // spawn progress: snapshots + .hbsave only
                je["spawner"]["activated"] = sp->activated;
                je["spawner"]["inside"] = sp->inside;
                je["spawner"]["spawnedTotal"] = sp->spawnedTotal;
                je["spawner"]["respawnCooldown"] = sp->respawnCooldown;
            }
        }
        if (const Encounter* en = reg.try_get<Encounter>(e)) {
            je["encounter"] = {{"id", en->id},
                               {"startActive", en->startActive},
                               {"clearedAction", static_cast<u32>(en->clearedAction)},
                               {"clearedAsset", en->clearedAsset},
                               {"clearedFlag", en->clearedFlag},
                               {"clearedFlagValue", en->clearedFlagValue},
                               {"clearedText", en->clearedText},
                               {"requiredFlag", en->requiredFlag}};
            if (runtimeTags) {
                je["encounter"]["state"] = static_cast<u32>(en->state);
                je["encounter"]["everHadAlive"] = en->everHadAlive;
            }
        }
        if (const Spawned* sd = reg.try_get<Spawned>(e)) { // runtime-only membership tag
            if (runtimeTags)
                je["spawned"] = {{"encounterId", sd->encounterId}, {"spawnerId", sd->spawnerId}};
        }
        if (const MorphState* mo = reg.try_get<MorphState>(e)) {
            nlohmann::json w = nlohmann::json::object();
            for (const auto& [name, val] : mo->weights) w[name] = val; // authored/rest pose
            je["morphState"] = {{"weights", w}};
        }
        if (const FacialAnimator* fa = reg.try_get<FacialAnimator>(e)) {
            je["facialAnimator"] = {{"lipSync", fa->lipSync},
                                    {"jawTarget", fa->jawTarget},
                                    {"jawStrength", fa->jawStrength},
                                    {"jawAttack", fa->jawAttack},
                                    {"jawRelease", fa->jawRelease},
                                    {"autoBlink", fa->autoBlink},
                                    {"blinkL", fa->blinkL},
                                    {"blinkR", fa->blinkR},
                                    {"blinkMin", fa->blinkMin},
                                    {"blinkMax", fa->blinkMax},
                                    {"blinkDuration", fa->blinkDuration},
                                    {"expression", fa->expression},
                                    {"expressionWeight", fa->expressionWeight}};
        }
        if (const Interactable* ia = reg.try_get<Interactable>(e)) {
            je["interactable"] = {{"action", static_cast<u32>(ia->action)},
                                  {"prompt", ia->prompt},
                                  {"asset", ia->asset},
                                  {"flag", ia->flag},
                                  {"flagValue", ia->flagValue},
                                  {"text", ia->text},
                                  {"range", ia->range},
                                  {"once", ia->once},
                                  {"requiredFlag", ia->requiredFlag},
                                  {"itemId", ia->itemId},
                                  {"itemCount", ia->itemCount},
                                  {"pickupId", ia->pickupId}};
            // `fired` is RUNTIME state: persist it only in in-memory snapshots + the
            // .hbsave (runtimeTags), never the authored .hbscene - else play-testing
            // then Ctrl+S would bake a consumed-once state into the source scene.
            if (runtimeTags) je["interactable"]["fired"] = ia->fired;
        }
        if (const TriggerVolume* tv = reg.try_get<TriggerVolume>(e)) {
            je["trigger"] = {{"action", static_cast<u32>(tv->action)},
                             {"asset", tv->asset},
                             {"flag", tv->flag},
                             {"flagValue", tv->flagValue},
                             {"text", tv->text},
                             {"halfExtents", ToJson(tv->halfExtents)},
                             {"once", tv->once},
                             {"requiredFlag", tv->requiredFlag},
                             {"itemId", tv->itemId},
                             {"itemCount", tv->itemCount},
                             {"pickupId", tv->pickupId}};
            // Runtime state (see Interactable above): `fired` persists "once"; `inside`
            // the enter-edge so a load-while-inside a repeating trigger doesn't re-fire.
            if (runtimeTags) {
                je["trigger"]["fired"] = tv->fired;
                je["trigger"]["inside"] = tv->inside;
            }
        }
        if (const CameraComponent* cam = reg.try_get<CameraComponent>(e)) {
            je["camera"] = CameraToJson(*cam);
        }
        if (const CameraZone* z = reg.try_get<CameraZone>(e)) {
            je["cameraZone"] = {{"halfExtents", ToJson(z->halfExtents)},
                                {"camera", z->camera},
                                {"track", z->track},
                                {"priority", z->priority},
                                {"enabled", z->enabled},
                                {"useSettings", z->useSettings},
                                {"settings", CameraToJson(z->settings)}};
        }
        if (const MusicZone* mz = reg.try_get<MusicZone>(e)) {
            je["musicZone"] = {{"halfExtents", ToJson(mz->halfExtents)},
                               {"musicState", mz->musicState},
                               {"parameter", mz->parameter},
                               {"parameterValue", mz->parameterValue},
                               {"fadeSeconds", mz->fadeSeconds},
                               {"priority", mz->priority},
                               {"enabled", mz->enabled}};
        }
        if (const CameraSpline* sp = reg.try_get<CameraSpline>(e)) {
            json pts = json::array();
            for (const glm::vec3& p : sp->points) pts.push_back(ToJson(p));
            je["cameraSpline"] = {{"points", std::move(pts)}, {"loop", sp->loop}};
        }
        if (const TerrainComponent* tr = reg.try_get<TerrainComponent>(e)) {
            je["terrain"] = {{"chunks", tr->chunks},
                             {"resolution", tr->resolution},
                             {"chunkSize", tr->chunkSize},
                             {"height", tr->height},
                             {"frequency", tr->frequency},
                             {"octaves", tr->octaves},
                             {"seed", tr->seed},
                             {"color", ToJson(tr->color)},
                             {"roughness", tr->roughness}};
            // Persist sculpted heights so edits survive save/load (omitted for
            // purely procedural terrain, which regenerates from the params).
            if (!tr->heights.empty()) je["terrain"]["heights"] = tr->heights;
            // Persist the painted hole mask (cliff/cave cutouts) when present.
            if (!tr->holeMask.empty()) je["terrain"]["holeMask"] = tr->holeMask;
            // Persist splat material layers (texture paths) + tile + painted weights.
            if (tr->splatEnabled || !tr->splatWeight.empty()) {
                json sp;
                sp["enabled"] = tr->splatEnabled;
                sp["tile"] = tr->splatTile;
                sp["layers"] = {tr->splatLayerSrc[0], tr->splatLayerSrc[1],
                                tr->splatLayerSrc[2], tr->splatLayerSrc[3]};
                if (!tr->splatWeight.empty()) sp["weight"] = tr->splatWeight;
                je["terrain"]["splat"] = sp;
            }
        }
        if (const MotionMatching* mm = reg.try_get<MotionMatching>(e)) {
            je["motionMatching"] = {{"sourceAsset", mm->sourceAsset},
                                    {"searchInterval", mm->searchInterval},
                                    {"speedScale", mm->speedScale},
                                    {"useNavVelocity", mm->useNavVelocity},
                                    {"enabled", mm->enabled}};
        }
        if (const Rotator* ro = reg.try_get<Rotator>(e)) {
            je["rotator"] = {{"axis", ToJson(ro->axis)},
                             {"speed", ro->speed},
                             {"enabled", ro->enabled}};
        }
        if (const ModelGroup* mg = reg.try_get<ModelGroup>(e)) {
            je["modelGroup"] = {{"modular", mg->modular}, {"source", mg->source}};
        }
        if (const CensorComponent* ce = reg.try_get<CensorComponent>(e)) {
            je["censor"] = {{"radius", ce->radius},
                            {"feather", ce->feather},
                            {"strength", ce->strength},
                            {"offset", ToJson(ce->offset)},
                            {"enabled", ce->enabled}};
        }
        if (const CharacterController* cc = reg.try_get<CharacterController>(e)) {
            je["character"] = {{"radius", cc->radius},
                               {"height", cc->height},
                               {"moveSpeed", cc->moveSpeed},
                               {"sprintMultiplier", cc->sprintMultiplier},
                               {"jumpHeight", cc->jumpHeight},
                               {"gravity", cc->gravity},
                               {"turnSpeed", cc->turnSpeed},
                               {"cameraRelative", cc->cameraRelative},
                               {"faceMoveDir", cc->faceMoveDir},
                               {"useKeyboard", cc->useKeyboard},
                               {"useGamepad", cc->useGamepad},
                               {"enabled", cc->enabled}};
        }
        if (const IKConstraint* ik = reg.try_get<IKConstraint>(e)) {
            json chains = json::array();
            for (const IKChain& ch : ik->chains) {
                json jc = {{"endJoint", ch.endJoint},
                           {"pole", ToJson(ch.pole)},
                           {"hasPole", ch.hasPole},
                           {"weight", ch.weight},
                           {"enabled", ch.enabled},
                           {"targetEntity", ch.targetEntity}};
                // `target` is AUTHORED only while the chain has no targetEntity. With
                // one set, anim::UpdateSkeletal OVERWRITES target from that entity's
                // world position every frame - editor included - so writing it stored
                // a derived value and destroyed whatever vec3 the author had typed
                // before they bound the entity. Omitting it also makes the file a
                // function of the scene rather than of how long it had been open:
                // the parser leaves the default and the first UpdateSkeletal
                // recomputes it, which is what would have happened anyway.
                if (ch.targetEntity.empty()) jc["target"] = ToJson(ch.target);
                chains.push_back(std::move(jc));
            }
            je["ik"] = {{"chains", std::move(chains)}};
        }
        // The seven UI-adjacent component blocks live in UI/UIDocumentJson.h so
        // that `.hbscene` and `.hbui` cannot drift: ONE writer, two callers. The
        // bodies moved verbatim (--test-uidoc diffs them against a frozen copy of
        // what used to be inlined right here, byte for byte). `worldText` is in
        // that set by adjacency only - it is world-space 3D text, NOT screen UI,
        // and is deliberately not a `.hbui` key.
        if (const UIElement* el = reg.try_get<UIElement>(e)) je["ui"] = ui::WriteElement(*el);
        if (const UICanvas* canvas = reg.try_get<UICanvas>(e))
            je["uiCanvas"] = ui::WriteCanvas(*canvas);
        if (const UIAnimator* an = reg.try_get<UIAnimator>(e))
            je["uiAnimator"] = ui::WriteAnimator(*an);
        if (const UIPanel* p = reg.try_get<UIPanel>(e)) je["uiPanel"] = ui::WritePanel(*p);
        if (const UILayoutGroup* lg = reg.try_get<UILayoutGroup>(e))
            je["uiLayoutGroup"] = ui::WriteLayout(*lg);
        if (const UICanvasGroup* cg = reg.try_get<UICanvasGroup>(e))
            je["uiCanvasGroup"] = ui::WriteGroup(*cg);
        if (const WorldText* wt = reg.try_get<WorldText>(e))
            je["worldText"] = ui::WriteWorldText(*wt);
        if (const ParticleEmitter* pe = reg.try_get<ParticleEmitter>(e)) {
            je["particles"] = {
                {"rate", pe->rate}, {"maxParticles", pe->maxParticles},
                {"emitting", pe->emitting}, {"lifetime", pe->lifetime},
                {"lifetimeVariance", pe->lifetimeVariance}, {"emitRadius", pe->emitRadius},
                {"direction", ToJson(pe->direction)}, {"startSpeed", pe->startSpeed},
                {"speedVariance", pe->speedVariance}, {"spread", pe->spread},
                {"gravity", ToJson(pe->gravity)}, {"drag", pe->drag},
                {"buoyancy", pe->buoyancy}, {"vortex", pe->vortex},
                {"startColor", ToJson(pe->startColor)}, {"endColor", ToJson(pe->endColor)},
                {"startSize", pe->startSize}, {"endSize", pe->endSize},
                {"spin", pe->spin}, {"texture", pe->texture}, {"additive", pe->additive},
                // Volumetric overhaul (all optional; omit == legacy defaults).
                {"shape", static_cast<u32>(pe->shape)},
                {"boxHalfExtents", ToJson(pe->boxHalfExtents)}, {"coneAngle", pe->coneAngle},
                {"burst", pe->burst}, {"loop", pe->loop}, {"duration", pe->duration},
                {"turbulence", pe->turbulence}, {"turbulenceScale", pe->turbulenceScale},
                {"fadeIn", pe->fadeIn}, {"fadeOut", pe->fadeOut},
                {"render", static_cast<u32>(pe->render)}, {"stretch", pe->stretch},
                {"subUVCols", pe->subUVCols}, {"subUVRows", pe->subUVRows},
                {"subUVFps", pe->subUVFps}, {"softFade", pe->softFade},
                // Module-stack opt-ins. Every one is written, but every one also
                // parses back to the pre-stack behaviour when ABSENT, which is what
                // lets a scene saved before the module stack load unchanged.
                {"useCurlNoise", pe->useCurlNoise}, {"curlStrength", pe->curlStrength},
                {"curlFrequency", pe->curlFrequency}, {"expDrag", pe->expDrag},
                {"simulateColor", pe->simulateColor}, {"colorVariance", pe->colorVariance},
                {"simulateSize", pe->simulateSize}, {"sizeVariance", pe->sizeVariance},
                {"gpuExpand", pe->gpuExpand}, {"gpuSim", pe->gpuSim}};
        }
        if (const AudioSource* src = reg.try_get<AudioSource>(e)) {
            je["audio"] = {{"asset", src->asset},
                           {"bus", src->bus},
                           {"volume", src->volume},
                           {"minDistance", src->minDistance},
                           {"maxDistance", src->maxDistance},
                           {"loop", src->loop},
                           {"autoplay", src->autoplay}};
        }
        if (const DialogueActor* da = reg.try_get<DialogueActor>(e)) {
            je["dialogueActor"] = {{"speaker", da->speaker},
                                   {"bus", da->bus},
                                   {"minDistance", da->minDistance},
                                   {"maxDistance", da->maxDistance}};
        }
        if (const NavigationAgent* na = reg.try_get<NavigationAgent>(e)) {
            je["navAgent"] = {{"target", ToJson(na->target)},
                              {"hasTarget", na->hasTarget},
                              {"speed", na->speed},
                              {"acceleration", na->acceleration},
                              {"radius", na->radius},
                              {"stoppingDistance", na->stoppingDistance},
                              {"turnSpeed", na->turnSpeed},
                              {"autoRepath", na->autoRepath}};
        }
        if (const NavigationObstacle* no = reg.try_get<NavigationObstacle>(e)) {
            je["navObstacle"] = {
                {"radius", no->radius}, {"height", no->height}, {"enabled", no->enabled}};
        }
        if (const NavmeshInput* nin = reg.try_get<NavmeshInput>(e)) {
            je["navmeshInput"] = {{"enabled", nin->enabled}};
        }
        if (const PostVolume* pv = reg.try_get<PostVolume>(e)) {
            je["postVolume"] = {{"halfExtents", ToJson(pv->halfExtents)},
                                {"priority", pv->priority},
                                {"enabled", pv->enabled},
                                {"settings", PostToJson(pv->settings)}};
        }
        if (const ReflectionProbe* rp = reg.try_get<ReflectionProbe>(e)) {
            // Box + bake params only; the baked maps are rebuilt on demand.
            je["reflectionProbe"] = {{"halfExtents", ToJson(rp->halfExtents)},
                                     {"intensity", rp->intensity},
                                     {"skyMix", rp->skyMix},
                                     {"range", rp->range},
                                     {"priority", rp->priority},
                                     {"source", rp->source}};
        }
        if (const DecalComponent* dc = reg.try_get<DecalComponent>(e)) {
            je["decal"] = {{"halfExtents", ToJson(dc->halfExtents)},
                           {"opacity", dc->opacity},
                           {"angleFade", dc->angleFade},
                           {"normalStrength", dc->normalStrength},
                           {"roughness", dc->roughness},
                           {"metallic", dc->metallic},
                           {"albedo", dc->albedoTex},
                           {"normal", dc->normalTex},
                           {"mr", dc->mrTex}};
        }
        if (const WaterComponent* wc = reg.try_get<WaterComponent>(e)) {
            je["water"] = {
                {"size", wc->size},
                {"resolution", wc->resolution},
                {"waveAngle", {wc->waveAngle[0], wc->waveAngle[1], wc->waveAngle[2], wc->waveAngle[3]}},
                {"waveAmplitude",
                 {wc->waveAmplitude[0], wc->waveAmplitude[1], wc->waveAmplitude[2], wc->waveAmplitude[3]}},
                {"waveLength", {wc->waveLength[0], wc->waveLength[1], wc->waveLength[2], wc->waveLength[3]}},
                {"waveSpeed", {wc->waveSpeed[0], wc->waveSpeed[1], wc->waveSpeed[2], wc->waveSpeed[3]}},
                {"waveSteepness",
                 {wc->waveSteepness[0], wc->waveSteepness[1], wc->waveSteepness[2], wc->waveSteepness[3]}},
                {"shallowColor", ToJson(wc->shallowColor)},
                {"fresnelPower", wc->fresnelPower},
                {"deepColor", ToJson(wc->deepColor)},
                {"reflectionRoughness", wc->reflectionRoughness},
                {"foam", wc->foam},
                {"rippleStrength", wc->rippleStrength},
                {"rippleScale", wc->rippleScale},
                {"buoyancy", wc->buoyancy},
                {"fftOcean", wc->fftOcean},
                {"fftWindSpeed", wc->fftWindSpeed},
                {"fftWindDir", wc->fftWindDir},
                {"fftPatchSize", wc->fftPatchSize},
                {"fftChoppiness", wc->fftChoppiness},
                {"fftAmplitude", wc->fftAmplitude},
                {"fftHeightScale", wc->fftHeightScale},
                {"absorptionDepth", wc->absorptionDepth},
                {"shorelineWidth", wc->shorelineWidth},
                {"edgeFade", wc->edgeFade}};
        }
        if (const Animator* an = reg.try_get<Animator>(e)) {
            je["animator"] = {{"source", an->sourceAsset},
                              {"clip", an->clip},
                              {"speed", an->speed},
                              {"loop", an->loop},
                              {"playing", an->playing},
                              {"rootMotion", an->rootMotion},
                              {"blendTime", an->blendTime}};
        }
        // Modular-character root: the .hbchar + active loadout. Parts are NOT
        // serialized (regenerated on load). Key is "characterRig" to avoid the
        // CharacterController's "character" key.
        if (const Character* ch = reg.try_get<Character>(e)) {
            json av = json::object();
            // ONLY when the map is authored (an equip, or overrides the file already
            // carried). A map that character::Instantiate merely resolved from the
            // .hbchar's defaults is derived state; writing it froze a custom loadout
            // the author never chose and permanently cut the entity off from later
            // changes to the asset's defaults. See Character::variantAuthored.
            if (ch->variantAuthored)
                for (const auto& [slot, variant] : ch->activeVariant) av[slot] = variant;
            je["characterRig"] = {{"asset", ch->asset}, {"loadout", ch->loadout},
                                  {"activeVariant", std::move(av)}};
        }
        if (const AnimationTrack* a = reg.try_get<AnimationTrack>(e)) {
            json keys = json::array();
            for (const auto& k : a->keys) {
                keys.push_back({{"t", k.time},
                                {"p", ToJson(k.position)},
                                {"r", ToJson(k.rotation)},
                                {"s", ToJson(k.scale)},
                                {"e", static_cast<int>(k.ease)}});
            }
            je["animation"] = {{"duration", a->duration},
                               {"speed", a->speed},
                               {"loop", a->loop},
                               {"playing", a->playing},
                               {"keys", std::move(keys)}};
        }
        // Per-object Static/Dynamic layer, authored INTO the one scene file. A
        // level is ONE .hbscene, so this tag is the ONLY carrier of the layer -
        // the navmesh baker and MaterialFlag_PainterlyExempt read it.
        // Always written on disk, not just in snapshots.
        if (const SceneLayer* sl = reg.try_get<SceneLayer>(e))
            je["sceneLayer"] = ToString(sl->kind);
        // Streaming group + baked shard. Authored, so on disk - but written
        // CONDITIONALLY, unlike "sceneLayer" above. LevelTypesSelfTest asserts two
        // save/parse/save round trips are BYTE-IDENTICAL and its `rebuild` lambda
        // reconstructs only Guid/Transform/SceneLayer/Parent; an always-present
        // key (even an empty one) would survive the first save and vanish from the
        // second. Absence already means Untagged, so there is nothing to write.
        if (const Tag* tg = reg.try_get<Tag>(e); tg && tg->id != kTagUntagged) {
            const std::string& tn = tags::Name(tg->id);
            // BOTH KEYS OR NEITHER. "shard" without "tag" is a row the loader drops
            // from its streaming group (parse leaves hasTag false, Instantiate gives
            // it no Tag, tagshard::FromParsed skips it) while the header still counts
            // it - so the whole shard table reads as stale and the level stops
            // streaming. A live Tag holding an id with no table entry is what
            // tags::ReconcileWithTable exists to prevent, but the writer must not
            // depend on that having run.
            if (tn.empty()) {
                HBE_WARN("Scene: entity holds tag id {} which the live tag table does not "
                         "name; its streaming group is not written.",
                         tg->id);
            } else {
                je["tag"] = tn; // the NAME serializes, never the id
                // Only when actually baked: "shard": -1 on every entity would double
                // the churn in a diff and mean nothing.
                if (tg->shard >= 0) je["shard"] = tg->shard;
            }
        }
        // PAINT-STROKE ZONE GROUP (Scene/StrokeZone.h). The NAME serializes, exactly
        // like "tag" above and for the same reason - a `.hbscene` stays portable
        // between projects, where a raw interned id would not. Written whenever the
        // component is present, INCLUDING the untagged group ("Untagged"): the
        // component is the group's identity, and the untagged group is precisely the
        // one that has no Tag to infer it from.
        //
        // The zone is read from the node's OWN Tag (strokezone::GroupZone), not from a
        // second copy inside the component: the marker carries no id any more, because
        // nothing kept a copy in sync with tags::RemoveTag / tags::AssignSubtree. The
        // written value is therefore always the same string "tag" carries above (or
        // "Untagged" when the node has no Tag, which is the untagged group).
        if (reg.all_of<StrokeGroup>(e)) {
            const Tag* gt = reg.try_get<Tag>(e);
            const std::string gn = gt ? tags::Name(gt->id) : std::string();
            je["strokeGroup"] = gn.empty() ? std::string(tags::kUntaggedName) : gn;
        }
        // SceneSource (which FILE an entity belongs to) stays snapshot-only: it's a
        // load-time partition tag, meaningless inside a single authored scene.
        if (runtimeTags) {
            if (const SceneSource* ss = reg.try_get<SceneSource>(e); ss && !ss->scene.empty())
                je["sceneSrc"] = ss->scene;
        }
    return je;
}

// Gathers `root` and its descendants (depth-first) and serializes them as a
// standalone scene document - root first, with no parent - for clipboard
// copy/paste. Paste via ParseSceneString + StageAssets + Instantiate(Additive).
json BuildSubtreeJson(const Scene& scene, entt::entity root) {
    const auto& reg = scene.Registry();
    json doc;
    doc["version"] = 1;
    json& arr = doc["entities"] = json::array();
    if (!reg.valid(root)) return doc;

    std::vector<entt::entity> order;
    std::unordered_map<u32, int> indexOf;
    std::vector<entt::entity> stack{root};
    // B11 is a CROSS-BOUNDARY rule, not a "documents are uncopyable" rule. The
    // reference frame is the ROOT's document: 0 for a world root, `doc` for a
    // subtree rooted inside an open `.hbui`. Skipping the root itself (which is
    // what testing `all_of<UIDocMember>` unconditionally did) made
    // BuildSubtreeJson return `{"version":1,"entities":[]}` for every document
    // entity - non-empty as a STRING, so every caller's `frag.empty()` guard
    // sailed past it: Ctrl+C / Ctrl+D on a menu button became a silent no-op,
    // Ctrl+X deleted the widget and copied nothing, and "Save as Prefab" wrote a
    // zero-entity `.hbprefab`. Intra-document copy is legitimate and is how a
    // widget gets duplicated; PasteSubtree re-stamps UIDocMember on the clone and
    // refuses the fragment outright when no document is the edit target, and
    // Editor::CreatePrefabFromSelection refuses a UI-carrying fragment, so the
    // "document content must never reach a .hbscene / .hbprefab" half still holds.
    const UIDocMember* rootDoc = reg.try_get<UIDocMember>(root);
    const u32 rootDocId = rootDoc ? rootDoc->doc : 0u;
    // ONE pass over the Parent pool, bucketed and SORTED BY SIBLING ORDER, instead
    // of a full `view<Parent>` scan per visited node. Two separate fixes in one:
    //
    //   ORDER. The old inner scan queued children in Parent-POOL order, which is
    //   authored order only until the pool is perturbed - and `swap_and_pop` (one
    //   component erase, one unparent) plus `swap_only` (one destroy) both permute
    //   it. A copy taken after any delete therefore emitted a different child order
    //   from the one the author sees, and the paste reproduced the wrong one. The
    //   map sorts by HierarchyOrder, which is authored data (Scene/Hierarchy.h).
    //
    //   COST. The scan made this O(N x P) against the WORLD-WIDE Parent pool, not
    //   the subtree: measured 5 ms at 1000 nodes, 79 ms at 4000, 1.26 s at 16000 -
    //   a hard stall on Ctrl+C. This is linear.
    const ChildrenMap kids = BuildChildrenMap(reg);
    while (!stack.empty()) {
        const entt::entity e = stack.back();
        stack.pop_back();
        // THE SAME EXCLUSION TABLE the file writer uses, minus its UIDocMember row -
        // which is replaced, not dropped, by the cross-boundary rule just below (a
        // subtree rooted INSIDE a document legitimately copies its own members).
        //
        // It used to be a hand-written subset: TerrainChunk, UISurface, DebrisChunk
        // and nothing else. SkinnedPartRef was the expensive omission - Ctrl+D or
        // "Save as Prefab" on a modular character walked its spawned parts, which
        // carry no MeshRef, so they serialized with an empty mesh source and pasted
        // back as junk entities that the next Ctrl+S then wrote into the .hbscene as
        // real authored content, six more per duplicate. Persistent,
        // DialogueChoiceButton and InteractPromptTag were the same shape.
        if (const char* excluded = SceneWriteExclusion(reg, e, nullptr);
            excluded && std::strcmp(excluded, "UIDocMember") != 0)
            continue;
        // The cross-boundary skip: a world child parented under a document
        // element (or vice versa) is NOT carried along.
        {
            const UIDocMember* m = reg.try_get<UIDocMember>(e);
            if ((m ? m->doc : 0u) != rootDocId) continue;
        }
        const u32 key = static_cast<u32>(e);
        if (indexOf.count(key)) continue;
        indexOf[key] = static_cast<int>(order.size());
        order.push_back(e);
        // Queue children, pushed in REVERSE so the LIFO pops them in sibling order.
        if (const auto it = kids.find(key); it != kids.end()) {
            for (auto r = it->second.rbegin(); r != it->second.rend(); ++r)
                stack.push_back(*r);
        }
    }
    for (const entt::entity e : order) {
        json je = EntityToJson(reg, e, indexOf);
        // IDENTITY IS NOT COPIED. Every duplication path in the editor funnels
        // through here - CopySelection/PasteSubtree, DuplicateSelection,
        // CreatePrefabFromSelection (so a .hbprefab on disk is guid-free too),
        // InstantiatePrefab and RevertPrefabInstance - and a duplicate is a NEW
        // object, not the same one restored. Dropping the key here means the
        // paste has nothing to adopt, so Scene::CreateEntity's fresh mint stands.
        // Carrying it forward instead would alias two entities' persisted state,
        // which is the one failure this whole mechanism exists to prevent.
        // (Undo/redo and .hbsave restores go through BuildSceneJson, NOT here,
        // and do keep their guids - those are the same objects coming back.)
        je.erase("guid");
        // Preserve each entity's level layer (Static/Dynamic) so copy/paste, duplicate
        // and prefab instancing keep the same layer instead of being re-auto-classified
        // on paste. (sceneSrc is deliberately NOT copied - a clone belongs to whatever
        // scene it is pasted into, but it should stay on the layer it was authored on.)
        if (const SceneLayer* sl = reg.try_get<SceneLayer>(e))
            je["sceneLayer"] = ToString(sl->kind);
        // THE STREAMING TAG IS COPIED, THE BAKED SHARD IS NOT. A clone belongs to
        // the same streaming GROUP as its source (that is what the author asked
        // for, and it is how a `.hbprefab` of a camp tent stays part of "Camp"),
        // but the shard index is a spatial fact about a position the clone does
        // not have yet - it is re-derived by the sharder on the next save. Keeping
        // it would file the clone under whatever shard the original sat in.
        // (EntityToJson already wrote "tag"; only "shard" needs removing.)
        je.erase("shard");
        arr.push_back(std::move(je));
    }
    return doc;
}

// Fills SceneData from a parsed .hbscene JSON document.
void ParseSceneJson(const json& root, SceneData& out) {
    // FILLS `out` - it does not append to it. Every caller assumes that, and the
    // guid migration in ParseSceneFile depends on it hard: it derives each
    // pre-guid entity's identity from its INDEX in `out.entities`, which is only
    // the index in the FILE when the vector starts empty. Re-parsing into a
    // reused SceneData without this would both double the entities and shift
    // every derived guid.
    out.entities.clear();
    out.kind = SceneKindFromString(root.value("kind", std::string("full")));
    out.ambientIntensity = root.value("ambientIntensity", 1.0f);
    out.giSource = root.value("giSource", std::string());
    out.navSource = root.value("navSource", std::string());
    if (const auto ps = root.find("packSlot"); ps != root.end() && ps->is_number_unsigned())
        out.packSlot = ps->get<u32>();
    // Absent = a file written before these existed; 0 means "unidentified", which the
    // collaboration layer treats as "cannot be merged yet", never as a wildcard match.
    if (const auto it = root.find("docId"); it != root.end() && it->is_string())
        out.docId = guid::FromHex(it->get<std::string>());
    if (const auto it = root.find("guidEpoch"); it != root.end() && it->is_string())
        out.guidEpoch = guid::FromHex(it->get<std::string>());
    out.exposure = root.value("exposure", 1.0f);
    out.shadowDistance = root.value("shadowDistance", 150.0f);
    if (const auto it = root.find("post"); it != root.end() && it->is_object()) {
        PostFromJson(*it, out.post); // out.post starts at defaults (effects on)
    }
    // The optional per-scene day/night override. ANY of the three keys claims the
    // clock for this level; the other two then fall back to the STRUCT's defaults
    // (`out.<field>`, never a literal - the "a default lives in two places" rule).
    // Absent entirely = inherit the project, exactly as before this key existed.
    out.hasDayNight = root.contains("timeOfDay") || root.contains("dayLengthSeconds") ||
                      root.contains("dynamicSky");
    if (out.hasDayNight) {
        out.timeOfDay = root.value("timeOfDay", out.timeOfDay);
        out.dayLengthSeconds = root.value("dayLengthSeconds", out.dayLengthSeconds);
        out.dynamicSky = root.value("dynamicSky", out.dynamicSky);
    }
    // Baked streaming shards (see ShardDesc). Rows missing a tag name are dropped:
    // a shard nothing can be matched to is worse than no shard at all, because the
    // count cross-check would then fail for a reason no author could act on.
    out.tagShards.clear();
    if (const auto it = root.find("tagShards"); it != root.end() && it->is_array()) {
        for (const json& js : *it) {
            if (!js.is_object()) continue;
            ShardDesc s;
            s.tag = js.value("tag", std::string());
            if (s.tag.empty()) continue;
            s.index = js.value("index", 0u);
            s.min = Vec3(js.value("min", json()));
            s.max = Vec3(js.value("max", json()));
            s.count = js.value("count", 0u);
            out.tagShards.push_back(std::move(s));
        }
    }

    for (const json& je : root.value("entities", json::array())) {
        EntityData d;
        // Stable identity. Absent (pre-guid file, or a clipboard/prefab fragment
        // that had it stripped) leaves 0, which Instantiate reads as "mint one".
        if (auto it = je.find("guid"); it != je.end() && it->is_string())
            d.guid = guid::FromHex(it->get<std::string>());
        d.name = je.value("name", "");
        d.prefabSource = je.value("prefab", "");
        d.editorHidden = je.value("editorHidden", false);
        if (auto it = je.find("transform"); it != je.end()) {
            d.hasTransform = true;
            d.transform.position = Vec3(it->value("p", json()));
            d.transform.rotation = Quat(it->value("r", json()));
            d.transform.scale = Vec3(it->value("s", json()), glm::vec3(1.0f));
        }
        d.parent = je.value("parent", -1);
        // -1 = the file predates the field; Instantiate substitutes the row index.
        // The same default is spelled out on EntityData::order - change both.
        d.order = je.value("order", -1);
        if (auto it = je.find("mesh"); it != je.end()) {
            d.hasMesh = true;
            d.meshSource = it->value("source", "");
            d.baseColor = Vec4(it->value("baseColor", json()));
            d.metallic = it->value("metallic", 0.0f);
            d.roughness = it->value("roughness", 0.5f);
            d.materialFlags = it->value("flags", 0u);
            d.subsurfaceColor = Vec3(it->value("subsurfaceColor", json()),
                                     glm::vec3(1.0f, 0.3f, 0.2f));
            d.subsurfaceRadius = it->value("subsurfaceRadius", 1.0f);
            d.clearcoat = it->value("clearcoat", 0.0f);
            d.clearcoatRoughness = it->value("clearcoatRoughness", 0.08f);
            d.emissiveColor = Vec3(it->value("emissiveColor", json()), glm::vec3(0.0f));
            d.emissiveIntensity = it->value("emissiveIntensity", 1.0f);
            d.materialAsset = it->value("material", "");
        }
        if (auto it = je.find("aabb"); it != je.end()) {
            d.hasAABB = true;
            d.aabb.min = Vec3(it->value("min", json()));
            d.aabb.max = Vec3(it->value("max", json()));
        }
        if (auto it = je.find("rigidBody"); it != je.end()) {
            d.hasRigidBody = true;
            const std::string shape = it->value("shape", "box");
            d.rigidBody.shape = shape == "sphere"       ? RigidBody::Shape::Sphere
                                : shape == "capsule"    ? RigidBody::Shape::Capsule
                                : shape == "mesh"        ? RigidBody::Shape::Mesh
                                : shape == "convexHull"  ? RigidBody::Shape::ConvexHull
                                                         : RigidBody::Shape::Box;
            d.rigidBody.motion = it->value("motion", "static") == "dynamic"
                                     ? RigidBody::Motion::Dynamic
                                     : RigidBody::Motion::Static;
            d.rigidBody.halfExtents = Vec3(it->value("halfExtents", json()), glm::vec3(0.5f));
            d.rigidBody.radius = it->value("radius", 0.5f);
            d.rigidBody.halfHeight = it->value("halfHeight", 0.5f);
            d.rigidBody.centerOffset = Vec3(it->value("centerOffset", json()));
            d.rigidBody.friction = it->value("friction", 0.5f);
            d.rigidBody.restitution = it->value("restitution", 0.2f);
        }
        if (auto it = je.find("light"); it != je.end()) {
            d.hasLight = true;
            d.light.direction = Vec3(it->value("direction", json()), {-0.5f, -1.0f, -0.4f});
            d.light.color = Vec3(it->value("color", json()), glm::vec3(1.0f));
            d.light.intensity = it->value("intensity", 4.0f);
        }
        if (auto it = je.find("volume"); it != je.end()) {
            d.hasVolume = true;
            VolumeComponent& v = d.volume;
            v.source = it->value("source", std::string());
            v.playing = it->value("playing", v.playing);
            v.loop = it->value("loop", v.loop);
            v.time = it->value("time", v.time);
            v.speed = it->value("speed", v.speed);
            v.render.densityScale = it->value("densityScale", v.render.densityScale);
            v.render.emission = it->value("emission", v.render.emission);
            v.render.extinction = it->value("extinction", v.render.extinction);
            v.render.stepCount = glm::clamp(it->value("stepCount", v.render.stepCount), 4, 512);
            v.render.shadowSteps = glm::clamp(it->value("shadowSteps", v.render.shadowSteps), 0, 32);
            v.render.albedo = Vec3(it->value("albedo", json()), v.render.albedo);
            v.render.emissionColor = Vec3(it->value("emissionColor", json()), v.render.emissionColor);
            v.render.emissionMode = glm::clamp(it->value("emissionMode", v.render.emissionMode), 0, 2);
            v.livePreview = it->value("livePreview", v.livePreview);
            v.previewRes = glm::clamp(it->value("previewRes", v.previewRes), 8, 192);
            v.effectName = it->value("effectName", std::string());
            if (auto sit = it->find("sim"); sit != it->end())
                volume::ConfigFromJson(*sit, v.sim); // embedded authoring recipe (empty -> defaults)
        }
        if (auto it = je.find("pointLight"); it != je.end()) {
            d.hasPointLight = true;
            d.pointLight.color = Vec3(it->value("color", json()), glm::vec3(1.0f));
            d.pointLight.intensity = it->value("intensity", 10.0f);
            d.pointLight.range = it->value("range", 10.0f);
        }
        if (auto it = je.find("spotLight"); it != je.end()) {
            d.hasSpotLight = true;
            d.spotLight.color = Vec3(it->value("color", json()), glm::vec3(1.0f));
            d.spotLight.intensity = it->value("intensity", 20.0f);
            d.spotLight.range = it->value("range", 15.0f);
            d.spotLight.innerAngle = it->value("innerAngle", 25.0f);
            d.spotLight.outerAngle = it->value("outerAngle", 35.0f);
        }
        if (auto it = je.find("rectLight"); it != je.end()) {
            d.hasRectLight = true;
            d.rectLight.color = Vec3(it->value("color", json()), glm::vec3(1.0f));
            d.rectLight.intensity = it->value("intensity", 20.0f);
            d.rectLight.width = it->value("width", 2.0f);
            d.rectLight.height = it->value("height", 1.0f);
            d.rectLight.range = it->value("range", 20.0f);
            d.rectLight.twoSided = it->value("twoSided", false);
        }
        if (auto it = je.find("schematic"); it != je.end()) {
            d.hasSchematic = true;
            d.schematicAsset = it->value("asset", "");
        }
        if (auto it = je.find("destructible"); it != je.end()) {
            d.hasDestructible = true;
            Destructible& ds = d.destructible;
            ds.asset = it->value("asset", "");
            ds.impulseThreshold = it->value("impulseThreshold", ds.impulseThreshold);
            ds.chunkHealth = it->value("chunkHealth", ds.chunkHealth);
            ds.damageRadius = it->value("damageRadius", ds.damageRadius);
            ds.breakImpulseScale = it->value("breakImpulseScale", ds.breakImpulseScale);
            ds.debrisLifetime = it->value("debrisLifetime", ds.debrisLifetime);
            ds.density = it->value("density", ds.density);
            ds.structural = it->value("structural", ds.structural);
            ds.breakEvent = it->value("breakEvent", "");
            // Break progress (.hbsave only). chunkEntity/supportScratch are NOT
            // restored - they are live handles rebuilt when the object re-activates.
            ds.activated = it->value("activated", false);
            if (const auto cs = it->find("chunkState"); cs != it->end() && cs->is_array())
                ds.chunkState = cs->get<std::vector<u8>>();
            if (const auto hp = it->find("chunkHp"); hp != it->end() && hp->is_array())
                ds.chunkHp = hp->get<std::vector<f32>>();
        }
        if (auto it = je.find("checkpoint"); it != je.end()) {
            d.hasCheckpoint = true;
            Checkpoint& cp = d.checkpoint;
            cp.id = it->value("id", "");
            cp.setObjective = it->value("setObjective", "");
            cp.completesObjective = it->value("completesObjective", "");
            cp.halfExtents = Vec3(it->value("halfExtents", json{}), glm::vec3(2.0f));
            cp.triggerOnEnter = it->value("triggerOnEnter", true);
            cp.saveOnReach = it->value("saveOnReach", true);
            cp.once = it->value("once", true);
        }
        if (auto it = je.find("health"); it != je.end()) {
            d.hasHealth = true;
            Health& h = d.health;
            h.max = it->value("max", 100.0f);
            h.current = it->value("current", h.max);
            h.faction = static_cast<Faction>(glm::clamp(
                it->value("faction", 0u), 0u, static_cast<u32>(Faction::Faction5)));
            h.regenRate = it->value("regenRate", 0.0f);
            h.regenDelay = it->value("regenDelay", 5.0f);
            h.hitInvuln = it->value("hitInvuln", 0.0f);
            h.invincible = it->value("invincible", false);
            h.friendlyFire = it->value("friendlyFire", false);
            h.onDeathFlag = it->value("onDeathFlag", "");
            h.onDeathFlagValue = it->value("onDeathFlagValue", 1.0f);
            h.onDeathObjective = it->value("onDeathObjective", "");
            h.deathTag = it->value("deathTag", "");
            h.deathClip = it->value("deathClip", -1);
            h.alive = it->value("alive", true);                    // runtime (.hbsave)
            h.deathDispatched = it->value("deathDispatched", false);
        }
        if (auto it = je.find("weapon"); it != je.end()) {
            d.hasWeapon = true;
            Weapon& w = d.weapon;
            w.kind = static_cast<Weapon::Kind>(glm::clamp(
                it->value("kind", 0u), 0u, static_cast<u32>(Weapon::Kind::Projectile)));
            w.damage = it->value("damage", 25.0f);
            w.range = it->value("range", 50.0f);
            w.fireRate = it->value("fireRate", 3.0f);
            w.radius = it->value("radius", 0.0f);
            w.impulse = it->value("impulse", 4.0f);
            w.meleeArc = it->value("meleeArc", 45.0f);
            w.hitRadius = it->value("hitRadius", 0.5f);
            w.maxAmmo = it->value("maxAmmo", 12);
            w.ammo = it->value("ammo", w.maxAmmo);
            w.reserve = it->value("reserve", 60);
            w.reloadTime = it->value("reloadTime", 1.5f);
        }
        if (auto it = je.find("aiPerception"); it != je.end()) {
            d.hasAIPerception = true;
            AIPerception& p = d.aiPerception;
            p.sightRange = it->value("sightRange", 18.0f);
            p.sightFovDeg = it->value("sightFovDeg", 100.0f);
            p.eyeHeight = it->value("eyeHeight", 1.6f);
            p.loseSightGrace = it->value("loseSightGrace", 0.5f);
            p.hearingRadius = it->value("hearingRadius", 12.0f);
            p.gainRate = it->value("gainRate", 1.5f);
            p.decayRate = it->value("decayRate", 0.4f);
            p.detectThreshold = it->value("detectThreshold", 1.0f);
            p.awareness = it->value("awareness", 0.0f);           // runtime (.hbsave)
            p.timeSinceSeen = it->value("timeSinceSeen", 999.0f);
        }
        if (auto it = je.find("aiBehavior"); it != je.end()) {
            d.hasAIBehavior = true;
            AIBehavior& b = d.aiBehavior;
            b.attackRange = it->value("attackRange", 2.0f);
            b.attackDamage = it->value("attackDamage", 20.0f);
            b.attackInterval = it->value("attackInterval", 1.2f);
            b.investigateTime = it->value("investigateTime", 6.0f);
            b.searchTime = it->value("searchTime", 8.0f);
            b.fleeHealthFrac = it->value("fleeHealthFrac", 0.0f);
            b.startAlerted = it->value("startAlerted", false);
            b.useWeapon = it->value("useWeapon", true);
            b.patrolMode = static_cast<u8>(it->value("patrolMode", 0u));
            b.waitAtPoint = it->value("waitAtPoint", 1.5f);
            if (auto pit = it->find("patrolPoints"); pit != it->end() && pit->is_array())
                for (const auto& pj : *pit) b.patrolPoints.push_back(Vec3(pj, glm::vec3(0.0f)));
            b.state = static_cast<AIState>(                        // runtime (.hbsave)
                glm::clamp(it->value("state", 0u), 0u, static_cast<u32>(AIState::Dead)));
            b.patrolIndex = it->value("patrolIndex", 0u);
            b.patrolForward = it->value("patrolForward", true);
            b.spawnApplied = it->value("spawnApplied", false);
        }
        if (auto it = je.find("spawner"); it != je.end()) {
            d.hasSpawner = true;
            Spawner& sp = d.spawner;
            sp.prefab = it->value("prefab", "");
            sp.encounterId = it->value("encounterId", "");
            sp.spawnerId = it->value("spawnerId", "");
            sp.count = it->value("count", 3u);
            sp.radius = it->value("radius", 4.0f);
            sp.trigger = static_cast<Spawner::Trigger>(glm::clamp(
                it->value("trigger", 0u), 0u, static_cast<u32>(Spawner::Trigger::Manual)));
            sp.halfExtents = Vec3(it->value("halfExtents", json{}), glm::vec3(6.0f));
            sp.requiredFlag = it->value("requiredFlag", "");
            sp.maxAlive = it->value("maxAlive", 0u);
            sp.respawn = static_cast<Spawner::Respawn>(glm::clamp(
                it->value("respawn", 0u), 0u, static_cast<u32>(Spawner::Respawn::Continuous)));
            sp.respawnDelay = it->value("respawnDelay", 5.0f);
            sp.activated = it->value("activated", false);       // runtime (.hbsave)
            sp.inside = it->value("inside", false);
            sp.spawnedTotal = it->value("spawnedTotal", 0u);
            sp.respawnCooldown = it->value("respawnCooldown", 0.0f);
        }
        if (auto it = je.find("encounter"); it != je.end()) {
            d.hasEncounter = true;
            Encounter& en = d.encounter;
            en.id = it->value("id", "");
            en.startActive = it->value("startActive", false);
            en.clearedAction = static_cast<InteractAction>(glm::clamp(
                it->value("clearedAction", 2u), 0u, static_cast<u32>(InteractAction::None)));
            en.clearedAsset = it->value("clearedAsset", "");
            en.clearedFlag = it->value("clearedFlag", "");
            en.clearedFlagValue = it->value("clearedFlagValue", 1.0f);
            en.clearedText = it->value("clearedText", "");
            en.requiredFlag = it->value("requiredFlag", "");
            en.state = static_cast<Encounter::State>(glm::clamp(
                it->value("state", 0u), 0u, static_cast<u32>(Encounter::State::Cleared)));
            en.everHadAlive = it->value("everHadAlive", false);
        }
        if (auto it = je.find("spawned"); it != je.end()) {
            d.hasSpawned = true;
            d.spawned.encounterId = it->value("encounterId", "");
            d.spawned.spawnerId = it->value("spawnerId", "");
        }
        if (auto it = je.find("morphState"); it != je.end()) {
            d.hasMorphState = true;
            if (auto wit = it->find("weights"); wit != it->end() && wit->is_object())
                for (auto w = wit->begin(); w != wit->end(); ++w)
                    if (w->is_number()) d.morphState.weights[w.key()] = w->get<f32>();
        }
        if (auto it = je.find("facialAnimator"); it != je.end()) {
            d.hasFacialAnimator = true;
            FacialAnimator& fa = d.facialAnimator;
            fa.lipSync = it->value("lipSync", true);
            fa.jawTarget = it->value("jawTarget", std::string("jawOpen"));
            fa.jawStrength = it->value("jawStrength", 1.0f);
            fa.jawAttack = it->value("jawAttack", 25.0f);
            fa.jawRelease = it->value("jawRelease", 12.0f);
            fa.autoBlink = it->value("autoBlink", true);
            fa.blinkL = it->value("blinkL", std::string("blink_L"));
            fa.blinkR = it->value("blinkR", std::string("blink_R"));
            fa.blinkMin = it->value("blinkMin", 2.5f);
            fa.blinkMax = it->value("blinkMax", 6.0f);
            fa.blinkDuration = it->value("blinkDuration", 0.12f);
            fa.expression = it->value("expression", "");
            fa.expressionWeight = it->value("expressionWeight", 1.0f);
        }
        if (auto it = je.find("interactable"); it != je.end()) {
            d.hasInteractable = true;
            Interactable& ia = d.interactable;
            ia.action = static_cast<InteractAction>(glm::clamp(
                it->value("action", 0u), 0u, static_cast<u32>(InteractAction::GrantItem)));
            ia.prompt = it->value("prompt", "Interact");
            ia.asset = it->value("asset", "");
            ia.flag = it->value("flag", "");
            ia.flagValue = it->value("flagValue", 1.0f);
            ia.text = it->value("text", "");
            ia.range = it->value("range", 2.5f);
            ia.once = it->value("once", false);
            ia.requiredFlag = it->value("requiredFlag", "");
            ia.itemId = it->value("itemId", "");
            ia.itemCount = it->value("itemCount", 1u);
            ia.pickupId = it->value("pickupId", "");
            ia.fired = it->value("fired", false); // persists "once" state across saves
        }
        if (auto it = je.find("trigger"); it != je.end()) {
            d.hasTrigger = true;
            TriggerVolume& tv = d.trigger;
            tv.action = static_cast<InteractAction>(glm::clamp(
                it->value("action", 0u), 0u, static_cast<u32>(InteractAction::GrantItem)));
            tv.asset = it->value("asset", "");
            tv.flag = it->value("flag", "");
            tv.flagValue = it->value("flagValue", 1.0f);
            tv.text = it->value("text", "");
            tv.halfExtents = Vec3(it->value("halfExtents", json{}), glm::vec3(2.0f));
            tv.once = it->value("once", true);
            tv.requiredFlag = it->value("requiredFlag", "");
            tv.itemId = it->value("itemId", "");
            tv.itemCount = it->value("itemCount", 1u);
            tv.pickupId = it->value("pickupId", "");
            tv.fired = it->value("fired", false);   // runtime "once" state (present in .hbsave only)
            tv.inside = it->value("inside", false); // enter-edge state (avoids re-fire on load-inside)
        }
        if (auto it = je.find("camera"); it != je.end()) {
            d.hasCamera = true;
            CameraFromJson(*it, d.camera);
        }
        if (auto it = je.find("cameraZone"); it != je.end()) {
            d.hasCameraZone = true;
            CameraZone& z = d.cameraZone;
            z.halfExtents = Vec3(it->value("halfExtents", json()), z.halfExtents);
            z.camera = it->value("camera", "");
            z.track = it->value("track", "");
            z.priority = it->value("priority", 0);
            z.enabled = it->value("enabled", true);
            z.useSettings = it->value("useSettings", false);
            if (const auto sit = it->find("settings"); sit != it->end() && sit->is_object())
                CameraFromJson(*sit, z.settings);
        }
        if (auto it = je.find("musicZone"); it != je.end()) {
            d.hasMusicZone = true;
            MusicZone& mz = d.musicZone;
            mz.halfExtents = Vec3(it->value("halfExtents", json()), mz.halfExtents);
            mz.musicState = it->value("musicState", "");
            mz.parameter = it->value("parameter", "");
            mz.parameterValue = it->value("parameterValue", 1.0f);
            mz.fadeSeconds = it->value("fadeSeconds", -1.0f);
            mz.priority = it->value("priority", 0);
            mz.enabled = it->value("enabled", true);
        }
        if (auto it = je.find("cameraSpline"); it != je.end()) {
            d.hasCameraSpline = true;
            CameraSpline& sp = d.cameraSpline;
            sp.loop = it->value("loop", true);
            if (const auto pit = it->find("points"); pit != it->end() && pit->is_array()) {
                for (const json& jp : *pit) sp.points.push_back(Vec3(jp));
            }
        }
        if (auto it = je.find("terrain"); it != je.end()) {
            d.hasTerrain = true;
            TerrainComponent& tr = d.terrain;
            tr.chunks = it->value("chunks", tr.chunks);
            tr.resolution = it->value("resolution", tr.resolution);
            tr.chunkSize = it->value("chunkSize", tr.chunkSize);
            tr.height = it->value("height", tr.height);
            tr.frequency = it->value("frequency", tr.frequency);
            tr.octaves = it->value("octaves", tr.octaves);
            tr.seed = it->value("seed", tr.seed);
            tr.color = Vec4(it->value("color", json()), tr.color);
            tr.roughness = it->value("roughness", tr.roughness);
            if (const auto hit = it->find("heights"); hit != it->end() && hit->is_array()) {
                tr.heights = hit->get<std::vector<f32>>(); // sculpted heightmap
            }
            if (const auto mit = it->find("holeMask"); mit != it->end() && mit->is_array()) {
                tr.holeMask = mit->get<std::vector<u8>>(); // painted cliff/cave holes
                tr.holeDirty = !tr.holeMask.empty();       // re-upload on load
            }
            if (const auto sit = it->find("splat"); sit != it->end()) {
                tr.splatEnabled = sit->value("enabled", false);
                tr.splatTile = sit->value("tile", tr.splatTile);
                if (const auto lit = sit->find("layers"); lit != sit->end() && lit->is_array())
                    for (int i = 0; i < 4 && i < static_cast<int>(lit->size()); ++i)
                        tr.splatLayerSrc[i] = (*lit)[i].get<std::string>();
                if (const auto wit = sit->find("weight"); wit != sit->end() && wit->is_array()) {
                    tr.splatWeight = wit->get<std::vector<u8>>();
                    tr.splatDirty = !tr.splatWeight.empty(); // re-upload on load
                }
            }
            tr.dirty = true; // rebuild chunks from the loaded params/heights
        }
        if (auto it = je.find("paint"); it != je.end() && it->is_object()) {
            d.hasPaint = true;
            d.paintSource = it->value("source", std::string());
            d.paintResolution = it->value("resolution", d.paintResolution);
            d.paintEnabled = it->value("enabled", d.paintEnabled);
            d.paintLocked = it->value("locked", d.paintLocked);
            d.paintReliefEnabled = it->value("relief", d.paintReliefEnabled);
            d.paintOpacity = it->value("opacity", d.paintOpacity);
            d.paintHeightScale = it->value("heightScale", d.paintHeightScale);
            d.paintLodBias = it->value("lodBias", d.paintLodBias);
            d.paintLayer = it->value("layer", d.paintLayer);
            d.paintProjection = it->value("projection", d.paintProjection);
        }
        if (auto it = je.find("motionMatching"); it != je.end()) {
            d.hasMotionMatching = true;
            MotionMatching& mm = d.motionMatching;
            mm.sourceAsset = it->value("sourceAsset", "");
            mm.searchInterval = it->value("searchInterval", mm.searchInterval);
            mm.speedScale = it->value("speedScale", mm.speedScale);
            mm.useNavVelocity = it->value("useNavVelocity", mm.useNavVelocity);
            mm.enabled = it->value("enabled", mm.enabled);
        }
        if (auto it = je.find("rotator"); it != je.end()) {
            d.hasRotator = true;
            Rotator& ro = d.rotator;
            ro.axis = Vec3(it->value("axis", json()), ro.axis);
            ro.speed = it->value("speed", ro.speed);
            ro.enabled = it->value("enabled", ro.enabled);
        }
        if (auto it = je.find("modelGroup"); it != je.end()) {
            d.hasModelGroup = true;
            d.modelGroup.modular = it->value("modular", true);
            d.modelGroup.source = it->value("source", std::string());
        }
        if (auto it = je.find("censor"); it != je.end()) {
            d.hasCensor = true;
            CensorComponent& ce = d.censor;
            ce.radius = it->value("radius", ce.radius);
            ce.feather = it->value("feather", ce.feather);
            ce.strength = it->value("strength", ce.strength);
            ce.offset = Vec3(it->value("offset", json()), ce.offset);
            ce.enabled = it->value("enabled", ce.enabled);
        }
        if (auto it = je.find("characterRig"); it != je.end()) {
            d.hasCharacterRig = true;
            d.characterRig.asset = it->value("asset", "");
            d.characterRig.loadout = it->value("loadout", "");
            if (const auto av = it->find("activeVariant"); av != it->end() && av->is_object())
                for (const auto& [slot, variant] : av->items())
                    d.characterRig.activeVariant[slot] = variant.get<std::string>();
            // "The file carried overrides" IS what makes the map authored - the flag
            // itself is never written (see Character::variantAuthored).
            d.characterRig.variantAuthored = !d.characterRig.activeVariant.empty();
        }
        if (auto it = je.find("character"); it != je.end()) {
            d.hasCharacter = true;
            CharacterController& cc = d.character;
            cc.radius = it->value("radius", cc.radius);
            cc.height = it->value("height", cc.height);
            cc.moveSpeed = it->value("moveSpeed", cc.moveSpeed);
            cc.sprintMultiplier = it->value("sprintMultiplier", cc.sprintMultiplier);
            cc.jumpHeight = it->value("jumpHeight", cc.jumpHeight);
            cc.gravity = it->value("gravity", cc.gravity);
            cc.turnSpeed = it->value("turnSpeed", cc.turnSpeed);
            cc.cameraRelative = it->value("cameraRelative", cc.cameraRelative);
            cc.faceMoveDir = it->value("faceMoveDir", cc.faceMoveDir);
            cc.useKeyboard = it->value("useKeyboard", cc.useKeyboard);
            cc.useGamepad = it->value("useGamepad", cc.useGamepad);
            cc.enabled = it->value("enabled", cc.enabled);
        }
        if (auto it = je.find("ik"); it != je.end()) {
            d.hasIK = true;
            if (const auto cit = it->find("chains"); cit != it->end() && cit->is_array()) {
                for (const json& jc : *cit) {
                    IKChain ch;
                    ch.endJoint = jc.value("endJoint", "");
                    ch.target = Vec3(jc.value("target", json()), ch.target);
                    ch.pole = Vec3(jc.value("pole", json()), ch.pole);
                    ch.hasPole = jc.value("hasPole", false);
                    ch.weight = jc.value("weight", 1.0f);
                    ch.enabled = jc.value("enabled", true);
                    ch.targetEntity = jc.value("targetEntity", "");
                    d.ik.chains.push_back(std::move(ch));
                }
            }
        }
        // Readers for the same seven blocks, from UI/UIDocumentJson.h. They carry
        // the back-compat rules (the v2 collapsed "anchor", the v1 "textScale" x
        // 28) and every clamp - which is exactly the part a hand-written .hbui
        // reader would have silently lost. These stay HERE PERMANENTLY: an
        // unmigrated .hbscene with UI in it must keep parsing forever, and the
        // migrator reads through this very path.
        if (auto it = je.find("ui"); it != je.end()) {
            d.hasUI = true;
            ui::ReadElement(*it, d.uiElement);
        }
        if (auto it = je.find("uiAnimator"); it != je.end()) {
            d.hasUIAnimator = true;
            ui::ReadAnimator(*it, d.uiAnimator);
        }
        if (auto it = je.find("uiPanel"); it != je.end()) {
            d.hasUIPanel = true;
            ui::ReadPanel(*it, d.uiPanel);
        }
        if (auto it = je.find("uiLayoutGroup"); it != je.end()) {
            d.hasUILayoutGroup = true;
            ui::ReadLayout(*it, d.uiLayoutGroup);
        }
        if (auto it = je.find("uiCanvasGroup"); it != je.end()) {
            d.hasUICanvasGroup = true;
            ui::ReadGroup(*it, d.uiCanvasGroup);
        }
        if (auto it = je.find("worldText"); it != je.end()) {
            d.hasWorldText = true;
            ui::ReadWorldText(*it, d.worldText);
        }
        if (auto it = je.find("uiCanvas"); it != je.end()) {
            d.hasUICanvas = true;
            ui::ReadCanvas(*it, d.uiCanvas);
        }
        if (auto it = je.find("audio"); it != je.end()) {
            d.hasAudio = true;
            d.audio.asset = it->value("asset", "");
            d.audio.bus = it->value("bus", "SFX");
            d.audio.volume = it->value("volume", 1.0f);
            d.audio.minDistance = it->value("minDistance", 1.0f);
            d.audio.maxDistance = it->value("maxDistance", 30.0f);
            d.audio.loop = it->value("loop", true);
            d.audio.autoplay = it->value("autoplay", true);
        }
        if (auto it = je.find("dialogueActor"); it != je.end()) {
            d.hasDialogueActor = true;
            d.dialogueActor.speaker = it->value("speaker", "");
            d.dialogueActor.bus = it->value("bus", "Dialogue");
            d.dialogueActor.minDistance = it->value("minDistance", 1.0f);
            d.dialogueActor.maxDistance = it->value("maxDistance", 35.0f);
        }
        if (auto it = je.find("particles"); it != je.end()) {
            d.hasParticles = true;
            ParticleEmitter& p = d.particles;
            p.rate = it->value("rate", p.rate);
            p.maxParticles = it->value("maxParticles", p.maxParticles);
            p.emitting = it->value("emitting", p.emitting);
            p.lifetime = it->value("lifetime", p.lifetime);
            p.lifetimeVariance = it->value("lifetimeVariance", p.lifetimeVariance);
            p.emitRadius = it->value("emitRadius", p.emitRadius);
            p.direction = Vec3(it->value("direction", json()), p.direction);
            p.startSpeed = it->value("startSpeed", p.startSpeed);
            p.speedVariance = it->value("speedVariance", p.speedVariance);
            p.spread = it->value("spread", p.spread);
            p.gravity = Vec3(it->value("gravity", json()), p.gravity);
            p.drag = it->value("drag", p.drag);
            p.buoyancy = it->value("buoyancy", p.buoyancy);
            p.vortex = it->value("vortex", p.vortex);
            p.startColor = Vec4(it->value("startColor", json()), p.startColor);
            p.endColor = Vec4(it->value("endColor", json()), p.endColor);
            p.startSize = it->value("startSize", p.startSize);
            p.endSize = it->value("endSize", p.endSize);
            p.spin = it->value("spin", p.spin);
            p.texture = it->value("texture", "");
            p.additive = it->value("additive", p.additive);
            // Volumetric overhaul (older scenes omit these -> struct defaults).
            p.shape = static_cast<ParticleEmitter::Shape>(
                it->value("shape", static_cast<u32>(p.shape)));
            p.boxHalfExtents = Vec3(it->value("boxHalfExtents", json()), p.boxHalfExtents);
            p.coneAngle = it->value("coneAngle", p.coneAngle);
            p.burst = it->value("burst", p.burst);
            p.loop = it->value("loop", p.loop);
            p.duration = it->value("duration", p.duration);
            p.turbulence = it->value("turbulence", p.turbulence);
            p.turbulenceScale = it->value("turbulenceScale", p.turbulenceScale);
            p.fadeIn = it->value("fadeIn", p.fadeIn);
            p.fadeOut = it->value("fadeOut", p.fadeOut);
            p.render = static_cast<ParticleEmitter::Render>(
                it->value("render", static_cast<u32>(p.render)));
            p.stretch = it->value("stretch", p.stretch);
            p.subUVCols = it->value("subUVCols", p.subUVCols);
            p.subUVRows = it->value("subUVRows", p.subUVRows);
            p.subUVFps = it->value("subUVFps", p.subUVFps);
            p.softFade = it->value("softFade", p.softFade);
            // Module-stack opt-ins. The struct defaults are all "off", so a scene
            // authored before the module stack omits every key here and compiles to
            // the pure legacy stack - which is the compatibility guarantee, expressed
            // in the one place it can actually be broken.
            p.useCurlNoise = it->value("useCurlNoise", p.useCurlNoise);
            p.curlStrength = it->value("curlStrength", p.curlStrength);
            p.curlFrequency = it->value("curlFrequency", p.curlFrequency);
            p.expDrag = it->value("expDrag", p.expDrag);
            p.simulateColor = it->value("simulateColor", p.simulateColor);
            p.colorVariance = glm::clamp(it->value("colorVariance", p.colorVariance), 0.0f, 1.0f);
            p.simulateSize = it->value("simulateSize", p.simulateSize);
            p.sizeVariance = glm::clamp(it->value("sizeVariance", p.sizeVariance), 0.0f, 1.0f);
            p.gpuExpand = it->value("gpuExpand", p.gpuExpand);
            // Absent in every scene saved before GPU simulation existed -> false ->
            // the legacy CPU stack, unchanged. Same contract as every flag above it.
            p.gpuSim = it->value("gpuSim", p.gpuSim);
        }
        if (auto it = je.find("navAgent"); it != je.end()) {
            d.hasNavAgent = true;
            d.navAgent.target = Vec3(it->value("target", json()));
            d.navAgent.hasTarget = it->value("hasTarget", false);
            d.navAgent.speed = it->value("speed", 3.5f);
            d.navAgent.acceleration = it->value("acceleration", 8.0f);
            d.navAgent.radius = it->value("radius", 0.5f);
            d.navAgent.stoppingDistance = it->value("stoppingDistance", 0.3f);
            d.navAgent.turnSpeed = it->value("turnSpeed", 10.0f);
            d.navAgent.autoRepath = it->value("autoRepath", true);
        }
        if (auto it = je.find("navObstacle"); it != je.end()) {
            d.hasNavObstacle = true;
            d.navObstacle.radius = it->value("radius", 1.0f);
            d.navObstacle.height = it->value("height", 2.0f);
            d.navObstacle.enabled = it->value("enabled", true);
        }
        if (auto it = je.find("navmeshInput"); it != je.end()) {
            d.hasNavmeshInput = true;
            d.navmeshInput.enabled = it->value("enabled", true);
        }
        if (auto it = je.find("postVolume"); it != je.end()) {
            d.hasPostVolume = true;
            d.postVolume.halfExtents =
                Vec3(it->value("halfExtents", json()), glm::vec3(10.0f, 5.0f, 10.0f));
            d.postVolume.priority = it->value("priority", 0);
            d.postVolume.enabled = it->value("enabled", true);
            if (auto s = it->find("settings"); s != it->end() && s->is_object())
                PostFromJson(*s, d.postVolume.settings);
        }
        if (auto it = je.find("reflectionProbe"); it != je.end()) {
            d.hasProbe = true;
            d.probe.halfExtents =
                Vec3(it->value("halfExtents", json()), glm::vec3(8.0f, 4.0f, 8.0f));
            d.probe.intensity = it->value("intensity", 1.0f);
            d.probe.skyMix = it->value("skyMix", 0.0f);
            d.probe.range = it->value("range", 60.0f);
            d.probe.priority = it->value("priority", 0);
            d.probe.source = it->value("source", std::string());
        }
        if (auto it = je.find("decal"); it != je.end()) {
            d.hasDecal = true;
            d.decal.halfExtents =
                Vec3(it->value("halfExtents", json()), glm::vec3(0.5f, 0.5f, 0.15f));
            d.decal.opacity = it->value("opacity", 1.0f);
            d.decal.angleFade = it->value("angleFade", 2.0f);
            d.decal.normalStrength = it->value("normalStrength", 1.0f);
            d.decal.roughness = it->value("roughness", 0.8f);
            d.decal.metallic = it->value("metallic", 0.0f);
            d.decal.albedoTex = it->value("albedo", std::string());
            d.decal.normalTex = it->value("normal", std::string());
            d.decal.mrTex = it->value("mr", std::string());
        }
        if (auto it = je.find("water"); it != je.end()) {
            d.hasWater = true;
            WaterComponent& wc = d.water;
            wc.size = it->value("size", wc.size);
            wc.resolution = it->value("resolution", wc.resolution);
            const auto rd4 = [&](const char* k, f32* out) {
                if (auto a = it->find(k); a != it->end() && a->is_array() && a->size() >= 4)
                    for (int i = 0; i < 4; ++i) out[i] = (*a)[i].get<f32>();
            };
            rd4("waveAngle", wc.waveAngle);
            rd4("waveAmplitude", wc.waveAmplitude);
            rd4("waveLength", wc.waveLength);
            rd4("waveSpeed", wc.waveSpeed);
            rd4("waveSteepness", wc.waveSteepness);
            wc.shallowColor = Vec3(it->value("shallowColor", json()), wc.shallowColor);
            wc.fresnelPower = it->value("fresnelPower", wc.fresnelPower);
            wc.deepColor = Vec3(it->value("deepColor", json()), wc.deepColor);
            wc.reflectionRoughness = it->value("reflectionRoughness", wc.reflectionRoughness);
            wc.foam = it->value("foam", wc.foam);
            wc.rippleStrength = it->value("rippleStrength", wc.rippleStrength);
            wc.rippleScale = it->value("rippleScale", wc.rippleScale);
            wc.buoyancy = it->value("buoyancy", wc.buoyancy);
            wc.fftOcean = it->value("fftOcean", wc.fftOcean);
            wc.fftWindSpeed = it->value("fftWindSpeed", wc.fftWindSpeed);
            wc.fftWindDir = it->value("fftWindDir", wc.fftWindDir);
            wc.fftPatchSize = it->value("fftPatchSize", wc.fftPatchSize);
            wc.fftChoppiness = it->value("fftChoppiness", wc.fftChoppiness);
            wc.fftAmplitude = it->value("fftAmplitude", wc.fftAmplitude);
            wc.fftHeightScale = it->value("fftHeightScale", wc.fftHeightScale);
            wc.absorptionDepth = it->value("absorptionDepth", wc.absorptionDepth);
            wc.shorelineWidth = it->value("shorelineWidth", wc.shorelineWidth);
            wc.edgeFade = it->value("edgeFade", wc.edgeFade);
        }
        // Runtime tags written only into in-memory snapshots (see SaveSceneToString).
        if (auto it = je.find("sceneSrc"); it != je.end() && it->is_string()) {
            d.hasSceneSourceTag = true;
            d.sceneSourceTag = it->get<std::string>();
        }
        if (auto it = je.find("sceneLayer"); it != je.end() && it->is_string()) {
            d.hasSceneLayerTag = true;
            d.sceneLayerKind = SceneKindFromString(it->get<std::string>());
        }
        // Streaming group + baked shard (authored, on disk - see EntityData::tag).
        // The NAME is kept as a string here: interning is a main-thread-only table
        // mutation and this parse runs on job workers.
        if (auto it = je.find("tag"); it != je.end() && it->is_string()) {
            std::string tn = it->get<std::string>();
            if (!tn.empty()) { // "" is Untagged, not a tag called ""
                d.hasTag = true;
                d.tag = std::move(tn);
            }
        }
        if (auto it = je.find("shard"); it != je.end() && it->is_number_integer())
            d.shard = it->get<int>();
        // Paint-stroke zone group (Scene/StrokeZone.h). Same NAME-as-string handling
        // as "tag": interning is a main-thread-only table mutation and this parse
        // runs on job workers, so the name is carried through to Instantiate. Unlike
        // "tag", an EMPTY/"Untagged" value is meaningful here - it is the untagged
        // group, not the absence of a group.
        if (auto it = je.find("strokeGroup"); it != je.end() && it->is_string()) {
            d.hasStrokeGroup = true;
            d.strokeGroupTag = it->get<std::string>();
        }
        if (auto it = je.find("animator"); it != je.end()) {
            d.hasAnimator = true;
            d.animator.sourceAsset = it->value("source", "");
            d.animator.clip = it->value("clip", 0);
            d.animator.speed = it->value("speed", 1.0f);
            d.animator.loop = it->value("loop", true);
            d.animator.playing = it->value("playing", true);
            d.animator.rootMotion = it->value("rootMotion", false);
            d.animator.blendTime = it->value("blendTime", d.animator.blendTime);
        }
        if (auto it = je.find("animation"); it != je.end()) {
            d.hasAnim = true;
            d.anim.duration = it->value("duration", 5.0f);
            d.anim.speed = it->value("speed", 1.0f);
            d.anim.loop = it->value("loop", true);
            d.anim.playing = it->value("playing", false);
            for (const json& jk : it->value("keys", json::array())) {
                AnimationTrack::Key k;
                k.time = jk.value("t", 0.0f);
                k.position = Vec3(jk.value("p", json()));
                k.rotation = Quat(jk.value("r", json()));
                k.scale = Vec3(jk.value("s", json()), glm::vec3(1.0f));
                k.ease = static_cast<ease::Curve>(jk.value("e", 0)); // default Linear for old files
                d.anim.keys.push_back(k);
            }
        }
        out.entities.push_back(std::move(d));
    }
}
} // namespace

bool SaveScene(const Scene& scene, const fs::path& path,
               const std::function<bool(entt::entity)>& include, SceneKind kind,
               const std::vector<ShardDesc>* shards, const SceneData* headerFrom) {
    json root =
        BuildSceneJson(scene, include, kind, /*runtimeTags*/ false, shards, headerFrom);

    // CARRY THE PACK SLOT. A scene is a packable asset and owns its slot for life
    // (--test-slotids), but BuildSceneJson rebuilds the document from scratch and
    // knows nothing about slots. The EDITOR papers over that with a post-save
    // re-stamp sweep (Editor::StampSavedAssets), but every NON-editor writer -
    // migrations, tools, headless tests - reached this function directly and
    // silently stripped the id, renumbering the asset at the next cook. The writer
    // itself now carries the key from the file it is replacing, so no caller can
    // lose it. (--test-scenesave's "no source header key may be dropped" rule is
    // what caught this.)
    if (fs::exists(path)) {
        std::ifstream prev(path, std::ios::binary);
        if (prev) {
            try {
                json old;
                prev >> old;
                for (const char* k : {"docId", "guidEpoch"}) {
                    if (const auto it2 = old.find(k); it2 != old.end() && !root.contains(k))
                        root[k] = *it2;
                }
                if (const auto it = old.find("packSlot");
                    it != old.end() && it->is_number_unsigned())
                    root["packSlot"] = *it;
            } catch (const std::exception&) {
                // Unreadable previous file: nothing to carry; the editor sweep or
                // the next cook re-stamps. Never block the save over it.
            }
        }
    }

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    // WRITE ASIDE, THEN RENAME. `std::ofstream out(path)` TRUNCATES the target the
    // instant it is constructed, and nothing after that used to be checked - no
    // flush, no good(), no close() test - so a disk-full, a quota, a stalled network
    // share or a scanner grabbing the file mid-write left a TRUNCATED `.hbscene`
    // while this function returned true, SaveSceneToDisk adopted the world and the
    // status line read "Saved scene 'X' (18 objects)". A destroyed level reported as
    // a success is the worst outcome in this file, and it was one `if` away.
    //
    // The serialised text is also built BEFORE the target is touched: `root.dump(2)`
    // on a 37 MB level is one large allocation, and a bad_alloc there used to unwind
    // through a call chain with no handler, leaving the file already truncated.
    std::string text;
    try {
        text = root.dump(2);
    } catch (const std::exception& e) {
        HBE_ERROR("Scene: failed to serialise '{}': {}. Nothing was written.",
                  path.string(), e.what());
        return false;
    }

    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            HBE_ERROR("Scene: cannot write '{}' (temp file '{}'). '{}' is untouched.",
                      path.string(), tmp.string(), path.filename().string());
            return false;
        }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        const bool streamOk = out.good();
        out.close();
        if (!streamOk || out.fail()) {
            HBE_ERROR("Scene: FAILED to write '{}' ({} bytes) - the target '{}' is "
                      "untouched. Free space or permissions?",
                      tmp.string(), text.size(), path.string());
            fs::remove(tmp, ec);
            return false;
        }
    }
    // The temp file is complete on disk; swapping it in is the only step that can
    // change the target, and it is atomic as far as any reader is concerned.
    fs::rename(tmp, path, ec);
    if (ec) {
        HBE_ERROR("Scene: cannot replace '{}' with the completed temp file '{}': {}. "
                  "The previous file is intact; the new one is at the temp path.",
                  path.string(), tmp.string(), ec.message());
        return false;
    }
    HBE_INFO("Scene: saved {} entities to '{}'.", root["entities"].size(), path.string());
    return true;
}

// Shared by SavePaintCanvases (writes every canvas) and EnsurePaintSources (writes
// only the ones that had no file yet). `onlyNew` is the difference between them.
static void WritePaintCanvases(Scene& scene, const fs::path& assetsDir,
                               const std::string& sceneStem, bool onlyNew) {
    auto& reg = scene.Registry();
    std::unordered_set<std::string> used; // sources claimed in THIS pass
    for (const entt::entity e : reg.view<PaintComponent>()) {
        PaintComponent& pc = reg.get<PaintComponent>(e);
        const bool isNew = pc.source.empty();
        if (isNew) {
            // Derived from the scene stem + the entity name, then deduplicated:
            // entity names are not unique (imported meshes share them by
            // construction, and a duplicated object clears its inherited source),
            // and two canvases sharing one file would overwrite each other.
            const Name* nm = reg.try_get<Name>(e);
            const std::string base =
                sceneStem + "_" +
                ((nm && !nm->value.empty()) ? nm->value : std::string("Object"));
            std::string candidate = "Paint/" + base + ".hbpaint";
            for (int n = 1; used.contains(candidate); ++n)
                candidate = "Paint/" + base + "_" + std::to_string(n) + ".hbpaint";
            pc.source = candidate;
        }
        // Claimed BEFORE the skip below, so a brand-new canvas can never be handed
        // the path of one that merely failed to load.
        used.insert(pc.source);
        // NEVER WRITE BACK A CANVAS WE COULD NOT READ. Instantiate keeps the
        // PaintComponent when its `.hbpaint` fails to load, so the scene does not
        // lose the reference or any of its authored settings - but the `layers` are
        // UNKNOWN, not empty, and writing them would replace a real canvas with a
        // blank one. That would turn a temporarily unreadable file (renamed, on a
        // stalled share, held by a scanner) into permanent destruction.
        if (pc.canvasMissing) {
            HBE_WARN("Scene: paint canvas '{}' never loaded - the reference is kept, "
                     "the file is NOT rewritten.",
                     pc.source);
            continue;
        }
        if (onlyNew && !isNew) continue;
        if (!paint::Save(assetsDir / pc.source, pc))
            HBE_WARN("Scene: failed to write paint canvas '{}'.", pc.source);
    }
}

void SavePaintCanvases(Scene& scene, const fs::path& assetsDir, const std::string& sceneStem) {
    // Pixels never live in the .hbscene: each PaintComponent writes its own
    // .hbpaint and the scene stores only the reference + metadata. Run this
    // BEFORE SaveScene, because it is what fills in a canvas's `source` - the
    // scene writer skips any PaintComponent whose source is still empty.
    WritePaintCanvases(scene, assetsDir, sceneStem, /*onlyNew*/ false);
}

void EnsurePaintSources(Scene& scene, const fs::path& assetsDir, const std::string& sceneStem) {
    // The SNAPSHOT half of the same problem. BuildSceneJson skips a PaintComponent
    // whose `source` is still empty, and SavePaintCanvases - the only thing that
    // assigns one - runs exclusively on the file-save path. So a canvas painted in a
    // session that had not yet saved the scene was absent from the play/undo snapshot
    // and was DESTROYED by the Replace that Stop-Play and Undo perform: the painting
    // simply ceased to exist, with no message.
    //
    // Only the canvases that had no file are written (the pixels of one that already
    // has a `.hbpaint` are the stroke history's business, not the scene undo's), so
    // this costs one write the first time a canvas appears and nothing after that -
    // which is what makes it affordable on a path that runs ~60 times a session.
    WritePaintCanvases(scene, assetsDir, sceneStem, /*onlyNew*/ true);
}

std::string SaveSceneToString(const Scene& scene,
                              const std::function<bool(entt::entity)>& include) {
    // Snapshots (play mode, undo/redo) keep the per-entity SceneSource tag too, so
    // a Replace restore puts each entity back in the scene file it came from
    // instead of collapsing everything into the active scene.
    return BuildSceneJson(scene, include, SceneKind::Full, /*runtimeTags*/ true).dump();
}

std::string SaveSubtreeToString(const Scene& scene, entt::entity root) {
    return BuildSubtreeJson(scene, root).dump();
}

bool ParseSceneFile(const fs::path& path, SceneData& out) {
    // VFS read: a shipped build serves the scene from its mounted .uap packs.
    const auto bytes = vfs::ReadFile(path);
    if (!bytes) {
        HBE_ERROR("Scene: cannot open '{}'.", path.string());
        return false;
    }
    // ParseSceneJson IS INSIDE THE TRY. Every field read goes through
    // `it->value(key, default)`, which throws json::type_error when the stored value
    // has the wrong type (a `paint.layer` written as a number, a hand-edited file, a
    // half-migrated one). Outside the try that propagated out of a `noexcept`-ish
    // call chain with no handler anywhere and TERMINATED THE PROCESS with no message
    // at all - a hard crash of an unsaved editor session, and a crash on boot in the
    // shipped runtime. Refusing the file loudly is the whole contract here.
    json root;
    try {
        root = json::parse(bytes->begin(), bytes->end());
        ParseSceneJson(root, out);
    } catch (const std::exception& e) {
        HBE_ERROR("Scene: failed to parse '{}': {}", path.string(), e.what());
        out.entities.clear(); // a half-parsed world must not look like a real one
        return false;
    }

    // MIGRATION: every `.hbscene` authored before entity guids existed has none.
    // Minting randomly on load would rebind every row of saved state to a
    // different object on the next launch, so the fill has to be DETERMINISTIC:
    // derive from (file identity, index in the file's entity array). Same file,
    // same bytes, same order -> same guids, every load, on every machine. The
    // first editor save writes them out as real guids and this never runs on that
    // file again. Caveat, and it is inherent: inserting or reordering entities in
    // a file that has never been saved by a guid-aware editor shifts the indices
    // after the insertion point, and their derived guids with them.
    //
    // `.hbscene` ONLY. A `.hbprefab` is a TEMPLATE that gets instantiated over
    // and over (Editor::InstantiatePrefab, spawn::DoBurst); its entities must
    // mint fresh every time, so it is left guid-less on purpose. See EntityGuid.h.
    if (path.extension() == ".hbscene") {
        const u64 seed = guid::SeedFromPath(path);
        for (u32 i = 0; i < static_cast<u32>(out.entities.size()); ++i)
            if (out.entities[i].guid == 0) out.entities[i].guid = guid::Derive(seed, i);
    }
    return true;
}

bool ParseSceneString(const std::string& text, SceneData& out) {
    json root;
    try { // ParseSceneJson inside the try - see ParseSceneFile for why
        root = json::parse(text);
        ParseSceneJson(root, out);
    } catch (const std::exception& e) {
        HBE_ERROR("Scene: failed to parse snapshot: {}", e.what());
        out.entities.clear();
        return false;
    }
    return true;
}

// --- Persistent GPU caches -------------------------------------------------------
namespace {
struct InstantiateCaches {
    std::unordered_map<std::string, rhi::MeshHandle> mesh; // full "uaf:rel#n"/"prim:*"
    std::unordered_map<std::string, rhi::TextureHandle> textures; // Assets-rel name
    std::unordered_map<std::string, AABB> bounds;
    std::unordered_map<std::string, std::string> submeshMat;
    // Resolved blendshape atlas per mesh source, cached BESIDE the GPU mesh - see
    // MorphAtlas. Keyed on exactly the same string as `mesh`, so a mesh that is
    // GPU-resident has its atlas resident too and the second spawn of a shard
    // resolves without a CPU model (plan blocker B3).
    std::unordered_map<std::string, MorphAtlas> morph;
    // Surface-paint canvas textures (colour + material), keyed on
    // "<paintSource>#<guid>". Same reason as `morph`: without it, EVERY respawn of a
    // painted mesh calls paint::Sync on a PaintComponent with invalid handles, which
    // takes UploadTexture's branch and mints two new mip'd RGBA textures - ~21 MB each
    // at 2048 - that nothing ever frees (there is no texture destroy in the RHI). Twenty
    // crossings of one shard boundary is ~1.7 GB. On a hit the handles are re-adopted
    // and Sync takes its UpdateTexture branch instead, so a respawn costs zero VRAM.
    //
    // Keyed on the ENTITY's stable guid as well as the source, deliberately: two
    // different entities sharing one `.hbpaint` keep their own canvases (they can be
    // painted independently in the Art Editor), while the SAME entity respawning
    // re-adopts its own. A guid of 0 (no stable identity) simply does not cache.
    std::unordered_map<std::string, std::pair<rhi::TextureHandle, rhi::TextureHandle>> paint;
};
InstantiateCaches& Caches() {
    static InstantiateCaches c;
    return c;
}

// The caches are read by StageAssets (which runs on worker threads, e.g. the
// editor's SceneStreamer) and written by Instantiate (main thread),
// so every access goes through this lock. The heavy work - file IO in staging,
// GPU upload in Instantiate - happens OUTSIDE these short critical sections.
std::mutex& CachesMutex() {
    static std::mutex m;
    return m;
}

bool CacheHasTexture(const std::string& key) {
    std::lock_guard<std::mutex> lk(CachesMutex());
    return Caches().textures.contains(key);
}
rhi::TextureHandle CacheGetTexture(const std::string& key) {
    std::lock_guard<std::mutex> lk(CachesMutex());
    const auto it = Caches().textures.find(key);
    return it != Caches().textures.end() ? it->second : rhi::TextureHandle{};
}
void CachePutTexture(const std::string& key, rhi::TextureHandle h) {
    std::lock_guard<std::mutex> lk(CachesMutex());
    Caches().textures[key] = h;
}

// A mesh cache hit returns the handle plus the bounds/material recorded with it.
struct MeshCacheHit {
    bool found = false;
    rhi::MeshHandle mesh;
    AABB bounds{glm::vec3(-0.5f), glm::vec3(0.5f)};
    std::string submeshMat;
};
MeshCacheHit CacheGetMesh(const std::string& key) {
    std::lock_guard<std::mutex> lk(CachesMutex());
    const auto it = Caches().mesh.find(key);
    if (it == Caches().mesh.end()) return {};
    MeshCacheHit hit;
    hit.found = true;
    hit.mesh = it->second;
    if (const auto b = Caches().bounds.find(key); b != Caches().bounds.end())
        hit.bounds = b->second;
    if (const auto m = Caches().submeshMat.find(key); m != Caches().submeshMat.end())
        hit.submeshMat = m->second;
    return hit;
}
std::string CacheGetSubmeshMat(const std::string& key) {
    std::lock_guard<std::mutex> lk(CachesMutex());
    const auto it = Caches().submeshMat.find(key);
    return it != Caches().submeshMat.end() ? it->second : std::string{};
}
void CachePutMesh(const std::string& key, rhi::MeshHandle mesh, const AABB& bounds,
                  const std::string& submeshMat) {
    std::lock_guard<std::mutex> lk(CachesMutex());
    Caches().mesh[key] = mesh;
    Caches().bounds[key] = bounds;
    Caches().submeshMat[key] = submeshMat;
}

// Read by StageAssets on WORKER threads (to decide whether the CPU model still has
// to be loaded) and written by Instantiate on the main thread, exactly like the
// mesh cache above - hence the same lock.
bool CacheHasMorph(const std::string& key) {
    std::lock_guard<std::mutex> lk(CachesMutex());
    return Caches().morph.contains(key);
}
bool CacheGetMorph(const std::string& key, MorphAtlas& out) {
    std::lock_guard<std::mutex> lk(CachesMutex());
    const auto it = Caches().morph.find(key);
    if (it == Caches().morph.end()) return false;
    out = it->second; // copy: `names` is a handful of short strings
    return true;
}
void CachePutMorph(const std::string& key, const MorphAtlas& atlas) {
    std::lock_guard<std::mutex> lk(CachesMutex());
    Caches().morph[key] = atlas;
}

// Paint canvases. Main thread only (Instantiate), but the same lock: one mutex for one
// cache struct is simpler to keep correct than a second one that could be forgotten.
bool CacheGetPaint(const std::string& key, rhi::TextureHandle& color, rhi::TextureHandle& mat) {
    std::lock_guard<std::mutex> lk(CachesMutex());
    const auto it = Caches().paint.find(key);
    if (it == Caches().paint.end()) return false;
    color = it->second.first;
    mat = it->second.second;
    return true;
}
void CachePutPaint(const std::string& key, rhi::TextureHandle color, rhi::TextureHandle mat) {
    std::lock_guard<std::mutex> lk(CachesMutex());
    Caches().paint[key] = {color, mat};
}
} // namespace

void ClearInstantiateCaches() {
    {
        std::lock_guard<std::mutex> lk(CachesMutex());
        Caches() = InstantiateCaches{};
    }
    MorphBuilds().store(0, std::memory_order_relaxed);
    anim::ClearRigCache(); // rigs are loaded from the same assets
}

u32 MorphAtlasBuildCount() { return MorphBuilds().load(std::memory_order_relaxed); }

void CacheUploadedMesh(const std::string& key, rhi::MeshHandle mesh, const AABB& bounds) {
    if (key.empty() || !mesh.IsValid()) return;
    // Empty submesh material: a generated mesh has no material of its own, which is
    // exactly what the miss path would have recorded after loading it back.
    CachePutMesh(key, mesh, bounds, std::string());
}

namespace {
// The slice's row list. `indices == nullptr` = the whole file, which is the
// shipping full-file path; a non-null pointer means a slice is active even at
// count 0. Values are indices into SceneData::entities and are NOT renumbered -
// see the "Slices" block in SceneSerializer.h.
struct SliceView {
    const u32* indices = nullptr;
    u32 count = 0;
    usize total = 0; // data.entities.size()

    bool sliced() const { return indices != nullptr; }
    usize visits() const { return sliced() ? static_cast<usize>(count) : total; }
    // Row for visit `k`, or `total` (= out of range) for a bad index. One warning
    // per bad row is the caller's job; this just refuses to index out of bounds.
    usize row(usize k) const {
        const usize i = sliced() ? static_cast<usize>(indices[k]) : k;
        return i < total ? i : total;
    }
};

// THE blendshape resolve. Answers "what is `meshSource`'s morph atlas?" from the
// process-wide cache first and from `staged`'s CPU model only on a miss, so the
// atlas outlives the StagedAssets it was born from - which is the whole of the
// blocker-B3 fix (a respawned character resolves from the cache; nothing is
// rebuilt, nothing is re-uploaded, nothing leaks).
//
// Returns FALSE only for "cannot tell yet": nothing cached AND the CPU model is
// not staged. That case must NOT be cached - a negative recorded there would
// permanently mark a real blendshape mesh as morph-less. StageAssets is what makes
// it rare (it force-loads the model for a morph entity whose atlas is not cached);
// the false return is the honest fallback for the rest.
bool ResolveMorphAtlas(Renderer& renderer, StagedAssets& staged,
                       const std::string& meshSource, MorphAtlas& out) {
    if (meshSource.empty()) return false;
    if (CacheGetMorph(meshSource, out)) return true; // includes cached negatives
    std::string rel;
    u32 submesh = 0;
    if (!SplitUafSource(meshSource, rel, submesh)) {
        // "prim:*" (and anything else that is not a mesh asset) can never carry
        // blendshapes. That is a definitive negative, so cache it.
        out = MorphAtlas{};
        CachePutMorph(meshSource, out);
        return true;
    }
    const auto it = staged.models.find(rel);
    if (it == staged.models.end() || submesh >= it->second.size()) return false; // unknown
    out = BuildMorphAtlasFromMesh(renderer, it->second[submesh]);
    CachePutMorph(meshSource, out);
    return true;
}
} // namespace

void StageAssets(const SceneData& data, const fs::path& assetsDir, StagedAssets& out,
                 const u32* indices, u32 count) {
    const SliceView slice{indices, count, data.entities.size()};
    // Anything already resident in the persistent Instantiate caches skips its
    // disk IO entirely (snapshots restore instantly on undo/redo).
    const auto stageTexture = [&](const std::string& tex) {
        if (tex.empty() || out.textures.contains(tex)) return;
        if (CacheHasTexture(tex)) return; // GPU-resident
        if (std::optional<uaf::Texture> t = uaf::ReadTexture(assetsDir / tex)) {
            out.textures.emplace(tex, std::move(*t));
        }
    };
    // Material JSONs are tiny; always (re)load so factor edits apply, but skip
    // their textures when resident.
    const auto stageMaterial = [&](const std::string& matRef) {
        if (matRef.empty() || out.materials.contains(matRef)) return;
        std::optional<MaterialAsset> mat = assets::LoadMaterial(assetsDir / matRef);
        if (!mat) {
            HBE_WARN("Scene: missing material asset '{}'.", matRef);
            return;
        }
        for (const std::string& tex : {mat->albedoTex, mat->normalTex, mat->mrTex,
                                       mat->aoTex, mat->emissiveTex}) {
            stageTexture(tex);
        }
        out.materials.emplace(matRef, std::move(*mat));
    };

    for (usize k = 0; k < slice.visits(); ++k) {
        const usize i = slice.row(k);
        if (i >= data.entities.size()) continue; // bad slice index (warned in Instantiate)
        const EntityData& d = data.entities[i];
        stageMaterial(d.materialAsset);
        // Surface-paint canvases (CPU file IO only; uploaded in Instantiate).
        if (d.hasPaint && !d.paintSource.empty() && !out.paints.contains(d.paintSource)) {
            PaintComponent canvas;
            if (paint::Load(assetsDir / d.paintSource, canvas))
                out.paints.emplace(d.paintSource, std::move(canvas));
            else
                HBE_WARN("Scene: missing paint canvas '{}'.", d.paintSource);
        }
    }

    for (usize k = 0; k < slice.visits(); ++k) {
        const usize i = slice.row(k);
        if (i >= data.entities.size()) continue;
        const EntityData& d = data.entities[i];
        if (!d.hasMesh) continue;
        std::string rel;
        u32 submesh = 0;
        if (!SplitUafSource(d.meshSource, rel, submesh)) continue;
        if (out.models.contains(rel)) continue;
        // A Mesh/ConvexHull collider needs the CPU geometry at instantiate even
        // when the GPU mesh is cache-resident, so don't skip the load for those.
        const bool needsCollisionMesh =
            d.hasRigidBody && (d.rigidBody.shape == RigidBody::Shape::Mesh ||
                               d.rigidBody.shape == RigidBody::Shape::ConvexHull);
        // Same exemption for BLENDSHAPES, and for the same reason: the morph atlas
        // is derived from the CPU model, so an entity that needs one must have the
        // model staged at least ONCE even if the GPU mesh is already resident. The
        // atlas is then cached beside the mesh, so this fires only until the first
        // resolve - after that a respawn skips the load exactly as before (plan
        // blocker B3). Without it the first spawn of a morph entity whose mesh was
        // already cached by some OTHER, morph-less entity would silently have no
        // face, which is the same failure one instance later.
        const bool needsMorphMesh = d.hasMorphState && !CacheHasMorph(d.meshSource);
        if (!needsCollisionMesh && !needsMorphMesh) {
            if (const MeshCacheHit hit = CacheGetMesh(d.meshSource); hit.found) {
                // GPU-resident submesh: only its material asset is still needed.
                if (!hit.submeshMat.empty()) stageMaterial(hit.submeshMat);
                continue;
            }
        }

        std::optional<Model> model = assets::LoadMesh(assetsDir / rel);
        if (!model) {
            HBE_WARN("Scene: missing mesh asset '{}'.", rel);
            continue;
        }
        // Pull in every texture the model's materials reference, plus any
        // generated .hbmat material assets (and THEIR textures).
        for (const MeshData& md : *model) {
            for (const std::string& tex :
                 {md.material.baseColorTex, md.material.normalTex, md.material.mrTex,
                  md.material.aoTex, md.material.emissiveTex}) {
                stageTexture(tex);
            }
            stageMaterial(md.material.materialAsset);
        }
        out.models.emplace(rel, std::move(*model));
    }

    // Terrain splat layers are MATERIALS: stage each (loads it + its textures) so
    // Instantiate can resolve the layer's albedo on load.
    for (usize k = 0; k < slice.visits(); ++k) {
        const usize i = slice.row(k);
        if (i >= data.entities.size()) continue;
        const EntityData& d = data.entities[i];
        if (!d.hasTerrain) continue;
        for (const std::string& mat : d.terrain.splatLayerSrc) stageMaterial(mat);
    }
}

namespace {
// LoadMode::Replace's prologue has two halves, factored out so the SLICE path
// (scene::BindWorld) runs the very same code instead of a second copy of it that
// could drift - that is the whole point of the split. This half (world
// destruction) is called only from Instantiate's Replace branch and from
// BindWorld. The other half is scene::ApplyEnvironment, which is PUBLIC (see the
// header) so --test-lightingparity can reach the environment apply without also
// destroying a world.

// Destroy everything EXCEPT the resident layers, so the UI survives gameplay
// scene swaps. Two spares, and they mean different things:
//   Persistent            - the runtime decoration (world-UI page quads, and
//                           a legacy `.hbscene` UI layer).
//   UIDocMember.screenOwned - an OPEN `.hbui` document. A document is an
//                           asset with its own file; a scene load has no
//                           business destroying it. The flag is duplicated
//                           into the component precisely so this site and
//                           Engine::FlowMainMenu evaluate it identically -
//                           the serializer has no Engine and no DocumentSet.
// With neither present this is equivalent to reg.clear() (the common case).
// Collect first, then destroy - can't mutate the registry mid-iteration.
//
// Takes the SCENE, not the registry, so that bumping the world token cannot be
// forgotten at a call site: this is the one and only place a world is wholesale
// replaced, and Scene::WorldToken is what the editor's save path compares against
// to notice that the registry it is about to write is no longer the level it loaded.
void DestroyWorld(Scene& scene) {
    entt::registry& reg = scene.Registry();
    scene.BumpWorldToken();
    std::vector<entt::entity> kill;
    for (const entt::entity e : reg.storage<entt::entity>()) {
        const UIDocMember* m = reg.try_get<UIDocMember>(e);
        if (!reg.all_of<Persistent>(e) && !(m && m->screenOwned)) kill.push_back(e);
    }
    for (const entt::entity e : kill)
        if (reg.valid(e)) reg.destroy(e);
}

} // namespace

// See the header. THE ONE WRITER of the five header fields + the GI volume.
void ApplyEnvironment(Scene& scene, Renderer& renderer, const SceneData& data) {
    SceneEnvironment& env = scene.Environment();
    env.ambientIntensity = data.ambientIntensity;
    env.exposure = data.exposure;
    env.shadowDistance = data.shadowDistance;
    // Packaging identity, not rendering state - it rides the environment because that
    // is the one thing that survives from the file into the live scene, so a later
    // save can re-emit it (HeaderOf). Set HERE, above the GI fast-path `return`
    // below: putting it after that would skip it whenever the same volume is already
    // bound, which is the common case on undo/redo and Stop-Play.
    env.packSlot = data.packSlot;
    env.docId = data.docId;
    env.guidEpoch = data.guidEpoch;
    env.post = data.post;
    // The optional per-scene day/night override. Applied here so it lands through
    // the SAME one writer as the rest of the header - the clock is lighting, and a
    // second stamping site is exactly what this function exists to prevent.
    //
    // A file that authored NO override is left alone rather than reset to a
    // default: the project's cycle (scene::SetupSky) is then the answer, which is
    // what every scene did before the keys existed. `dayNightAuthored` is cleared
    // in that case so the scene does not start CLAIMING an override it never made -
    // otherwise the first save would freeze whatever hour the process happened to
    // be at into the file.
    env.dayNightAuthored = data.hasDayNight ? 1u : 0u;
    if (data.hasDayNight) {
        env.timeOfDay = data.timeOfDay;
        env.dayLengthSeconds = data.dayLengthSeconds;
        env.dynamicSky = data.dynamicSky;
    }
    // Navigation asset for this scene. The Engine's NavWorld watches env.navSource and
    // (re)loads the .hbnav when it changes; set it here (before the GI early-return
    // below) so it is always applied on load.
    env.navSource = data.navSource;
    // Load the cached GI volume (.hbgi) so baked GI lights the scene without a
    // re-bake.
    //
    // THE SAME VOLUME IS NOT RE-UPLOADED. Nothing in the RHI destroys a texture
    // (there is DestroyGpuBuffer and no counterpart), so every re-bind of an
    // identical volume leaked an SH + depth atlas pair. This path is not once per
    // level: undo, redo and Stop-Play all restore through LoadMode::Replace, so a
    // hundred Ctrl+Z on one level uploaded a hundred pairs and freed none. When the
    // scene already has exactly this volume bound, keep it - byte-identical result,
    // no allocation. (A re-BAKE writes the handles directly and is unaffected.)
    if (!data.giSource.empty() && data.giSource == env.giSource &&
        env.giStatus == GiStatus::Loaded && env.giSh.IsValid() && env.giDepth.IsValid()) {
        return;
    }
    env.giSource = data.giSource;
    // ALWAYS CLEAR FIRST. This used to be `if (vol.valid)` with no else, so a scene
    // whose `.hbgi` was missing or corrupt kept the PREVIOUS scene's volume bound
    // while advertising its own giSource - lighting inherited across a level load,
    // silently, and dependent on which scene happened to be open before. Clearing
    // makes a failed load a visible absence instead of someone else's GI.
    env.giSh = {};
    env.giDepth = {};
    env.giOrigin = glm::vec3(0.0f);
    env.giSpacing = glm::vec3(1.0f);
    env.giDims = glm::ivec3(0);
    env.giStatus = GiStatus::None;
    if (data.giSource.empty()) return;
    const GiVolume vol = LoadGIVolume(renderer, Project::Active().AssetsDir() / data.giSource);
    env.giStatus = vol.status;
    if (vol.status != GiStatus::Loaded) return; // LoadGIVolume already warned, loudly
    env.giSh = vol.sh;
    env.giDepth = vol.depth;
    env.giOrigin = vol.origin;
    env.giSpacing = vol.spacing;
    env.giDims = vol.dims;
}

void BindWorld(Scene& scene, Renderer& renderer, const SceneData& data) {
    DestroyWorld(scene);
    ApplyEnvironment(scene, renderer, data);
    HBE_INFO("Scene: world bound (environment applied, 0 entities created).");
}

void Instantiate(Scene& scene, Renderer& renderer, const SceneData& data,
                 StagedAssets& staged, LoadMode mode,
                 std::vector<entt::entity>* createdOut, const std::string& sceneTag,
                 const u32* indices, u32 count) {
    auto& reg = scene.Registry();
    const SliceView slice{indices, count, data.entities.size()};
    // A slice owns neither the world nor its environment: Replace would destroy the
    // sibling shards it is being loaded alongside, and it carries the header fields
    // of a file it is only a fragment of. Refuse, change nothing, and name the
    // replacement in the message.
    if (slice.sliced() && mode == LoadMode::Replace) {
        HBE_ERROR("Scene: LoadMode::Replace is illegal with a slice ({} row(s) ignored). "
                  "Call scene::BindWorld once, then load each slice Additive.",
                  count);
        if (createdOut) createdOut->clear();
        return;
    }
    if (mode == LoadMode::Replace) {
        DestroyWorld(scene);
        ApplyEnvironment(scene, renderer, data);
    }

    // Process-wide caches (shared meshes/textures upload once EVER) are reached
    // through locked accessors: StageAssets reads them from worker threads.
    auto loadTexture = [&](const std::string& name) -> rhi::TextureHandle {
        if (name.empty()) return {};
        if (rhi::TextureHandle c = CacheGetTexture(name); c.IsValid()) return c;
        rhi::TextureHandle h;
        if (auto it = staged.textures.find(name); it != staged.textures.end()) {
            h = UploadStagedTexture(renderer, it->second, name.c_str());
        }
        // Only cache valid handles: the cache is process-wide, and a miss may
        // be fixed by a later import.
        if (h.IsValid()) CachePutTexture(name, h);
        return h;
    };

    // Identity claim. Seeded from the guids ALREADY live in the registry (after
    // Replace's sweep above, so a Replace starts from an empty claim and adopts
    // everything). A parsed guid that is already taken is not adopted - the
    // entity keeps the fresh mint from CreateEntity instead. That is what makes
    // the same fragment instantiated N times (a spawner burst, the same scene
    // loaded additively twice) produce N distinct objects rather than N aliases.
    //
    // SLICES: this is per-CALL and seeded from what is LIVE, so slice 2 sees slice
    // 1's already-adopted guids and cannot re-adopt one. Disjoint slices of one
    // file therefore adopt exactly the file's guids, once each - and a slice
    // loaded TWICE gets fresh mints for the second copy, same as any other
    // repeated additive load.
    guid::Claim guids(reg);

    // `created` stays indexed by FILE ROW even under a slice (parent links are file
    // indices), so a non-slice row simply stays entt::null.
    std::vector<entt::entity> created(data.entities.size(), entt::entity{entt::null});
    for (usize k = 0; k < slice.visits(); ++k) {
        const usize i = slice.row(k);
        if (i >= data.entities.size()) {
            HBE_WARN("Scene: slice index {} is out of range ({} entities); skipped.",
                     indices[k], data.entities.size());
            continue;
        }
        if (created[i] != entt::null) {
            HBE_WARN("Scene: slice index {} appears twice; the repeat is skipped.", i);
            continue;
        }
        const EntityData& d = data.entities[i];
        const entt::entity e = scene.CreateEntity(d.name);
        guid::Apply(reg, e, d.guid, guids); // adopt the file's guid, or keep a fresh one
        // SIBLING ORDER, overwriting CreateEntity's fresh mint exactly as the guid
        // above does. `d.order < 0` = the file predates the field, so its implicit
        // order - the ROW INDEX - is used, which reproduces the old behaviour
        // byte-for-byte. The watermark is raised so the next CreateEntity cannot
        // hand out a value this load already used. See Scene/Hierarchy.h.
        {
            const i32 ord = d.order >= 0 ? static_cast<i32>(d.order) : static_cast<i32>(i);
            reg.emplace_or_replace<HierarchyOrder>(e, HierarchyOrder{ord});
            scene.NoteHierarchyOrder(ord);
        }
        created[i] = e;
        if (!sceneTag.empty()) reg.emplace<SceneSource>(e, SceneSource{sceneTag});
        // NOTE: the file's header `kind` does NOT stamp a layer on its entities any
        // more. A level is ONE scene file, so a file is not a layer of anything -
        // the Static/Dynamic tag is per ENTITY (read below from `sceneLayerKind`).
        // Consumers treat an untagged entity as Static (see the navmesh baker's filter).
        // Per-entity runtime tags from an in-memory snapshot put every entity back
        // in its own scene file instead of collapsing the grouping.
        if (d.hasSceneSourceTag) reg.emplace_or_replace<SceneSource>(e, SceneSource{d.sceneSourceTag});
        if (d.hasSceneLayerTag) reg.emplace_or_replace<SceneLayer>(e, SceneLayer{d.sceneLayerKind});
        // Streaming group. tags::Intern registers a name the project does not list
        // rather than folding it into Untagged (which would move content into the
        // always-resident set without telling anyone), and tags::Assign refuses a
        // `.hbui` document entity. This is main-thread code by Instantiate's own
        // contract, which is what makes touching the tag table here legal.
        if (d.hasTag) tags::Assign(reg, e, tags::Intern(d.tag), d.shard);
        // Paint-stroke zone group. The component is a bare MARKER: the group's zone is
        // the node's own Tag, emplaced by the line above. The file's "strokeGroup"
        // string is therefore a HUMAN-READABLE ECHO of that tag rather than a second
        // source of truth - it is still written (so a `.hbscene` diff says which zone a
        // group collects) and still parsed, but nothing downstream can disagree with
        // `tag` any more. Interning it here would resurrect exactly that possibility.
        if (d.hasStrokeGroup) reg.emplace_or_replace<StrokeGroup>(e, StrokeGroup{});
        if (d.editorHidden) reg.emplace<EditorHidden>(e); // editor-only visibility
        if (!d.prefabSource.empty())
            reg.emplace<PrefabInstance>(e, PrefabInstance{d.prefabSource}); // linked prefab root

        if (d.hasTransform) reg.emplace<Transform>(e, d.transform);

        // A modular-character ROOT never draws (its MeshRef only provides the
        // skeleton for posing); character::Instantiate sets that up + spawns parts.
        //
        // Tested on the RIG ASSET, not on the presence of the component. An entity can
        // carry an EMPTY Character (one Inspector "Add Component" away, and the
        // reference level's Terrain already has one) - character::Instantiate
        // early-returns on an empty asset, so nothing would ever spawn parts, but the
        // old `!d.hasCharacterRig` test still suppressed the MeshInstance. The entity
        // loaded invisible and the next save wrote no "mesh" key at all: the mesh
        // reference and every inline material value, gone, permanently, from one
        // component the author added by accident.
        if (d.hasMesh && !d.meshSource.empty() &&
            !(d.hasCharacterRig && !d.characterRig.asset.empty())) {
            MeshInstance mi;
            mi.baseColor = d.baseColor;
            mi.metallic = d.metallic;
            mi.roughness = d.roughness;
            mi.materialFlags = d.materialFlags;
            mi.subsurfaceColor = d.subsurfaceColor;
            mi.subsurfaceRadius = d.subsurfaceRadius;
            mi.clearcoat = d.clearcoat;
            mi.clearcoatRoughness = d.clearcoatRoughness;
            mi.emissiveColor = d.emissiveColor;
            mi.emissiveIntensity = d.emissiveIntensity;

            AABB bounds{glm::vec3(-0.5f), glm::vec3(0.5f)};
            std::string submeshMaterial; // mesh-baked .hbmat ref (import-time)
            if (const MeshCacheHit hit = CacheGetMesh(d.meshSource); hit.found) {
                mi.mesh = hit.mesh;
                bounds = hit.bounds;
                submeshMaterial = hit.submeshMat;
            } else if (d.meshSource.rfind("prim:", 0) == 0) {
                MeshData md = mesh::GeneratePrimitive(d.meshSource.substr(5));
                if (!md.vertices.empty()) {
                    mi.mesh = renderer.UploadMesh(md);
                    ComputeBounds(md, bounds.min, bounds.max);
                }
            } else {
                std::string rel;
                u32 submesh = 0;
                if (SplitUafSource(d.meshSource, rel, submesh)) {
                    if (auto mit = staged.models.find(rel);
                        mit != staged.models.end() && submesh < mit->second.size()) {
                        const MeshData& md = mit->second[submesh];
                        mi.mesh = renderer.UploadMesh(md);
                        ComputeBounds(md, bounds.min, bounds.max);
                        mi.albedoTexture = loadTexture(md.material.baseColorTex);
                        mi.normalTexture = loadTexture(md.material.normalTex);
                        mi.mrTexture = loadTexture(md.material.mrTex);
                        mi.aoTexture = loadTexture(md.material.aoTex);
                        mi.emissiveTexture = loadTexture(md.material.emissiveTex);
                        submeshMaterial = md.material.materialAsset;
                    }
                }
            }
            // A material asset overrides the inline values (it is the source
            // of truth): the entity's own link wins, else the .hbmat the mesh
            // was imported with.
            const std::string& materialRef =
                !d.materialAsset.empty() ? d.materialAsset : submeshMaterial;
            if (!materialRef.empty()) {
                if (auto mit = staged.materials.find(materialRef);
                    mit != staged.materials.end()) {
                    const MaterialAsset& mat = mit->second;
                    mi.baseColor = mat.baseColor;
                    mi.metallic = mat.metallic;
                    mi.roughness = mat.roughness;
                    mi.emissiveColor = mat.emissiveColor;
                    mi.emissiveIntensity = mat.emissiveIntensity;
                    mi.subsurfaceColor = mat.subsurfaceColor;
                    mi.materialFlags = mat.flags;
                    mi.albedoTexture = loadTexture(mat.albedoTex);
                    mi.normalTexture = loadTexture(mat.normalTex);
                    mi.mrTexture = loadTexture(mat.mrTex);
                    mi.aoTexture = loadTexture(mat.aoTex);
                    mi.emissiveTexture = loadTexture(mat.emissiveTex);
                }
            }

            // A MISSING ASSET MUST NOT DELETE THE REFERENCE TO IT. All four of these
            // used to be gated on `mi.mesh.IsValid()`, i.e. on the GPU upload having
            // succeeded - so an entity whose `.uaf` had been renamed, moved, or had
            // failed to import loaded with NO MeshInstance, NO MeshRef, NO MaterialRef
            // and NO AABB, and the next save therefore wrote no "mesh" key, no
            // "aabb", and none of the inline material values. Fix the asset afterwards
            // and there was nothing left pointing at it: the object was a bare named
            // transform, permanently, and nothing had said a word.
            //
            // The handle is allowed to be invalid. Scene::CollectDrawItems skips an
            // invalid mesh (Scene.cpp), so the object simply does not draw until the
            // asset comes back - which is the correct failure for a broken reference,
            // and is also what makes every serializer self-test able to run headless
            // where UploadMesh returns nothing by design.
            if (mi.mesh.IsValid())
                CachePutMesh(d.meshSource, mi.mesh, bounds, submeshMaterial);
            reg.emplace<MeshInstance>(e, mi);
            reg.emplace<MeshRef>(e, MeshRef{d.meshSource});
            // ...and the SAME RULE for the material link, which the fix above
            // missed by one screen. `stageMaterial` warns and returns on a missing
            // or unparseable `.hbmat`, so gating the emplace on `staged.materials`
            // meant: rename Materials/Rust.hbmat -> open the level -> Ctrl+S, and
            // `mesh.material` is gone from the file. Restoring the asset afterwards
            // leaves nothing pointing at it, and the entity is permanently
            // downgraded to whatever inline colours it happened to carry (the
            // comment at the top of this block calls the link the source of truth
            // that OVERRIDES those, so this is a real demotion, not a wash).
            //
            // The AUTHORED link (`d.materialAsset`, the entity's own "material"
            // key) is never gated: it is what the file said, and the file is the
            // thing being preserved. The DERIVED one (the `.hbmat` the mesh was
            // imported with) stays gated exactly as before - it is not in the file,
            // so emplacing it unconditionally would start WRITING a "material" key
            // to entities that never had one.
            if (!d.materialAsset.empty())
                reg.emplace<MaterialRef>(e, MaterialRef{d.materialAsset});
            else if (!submeshMaterial.empty() && staged.materials.contains(submeshMaterial))
                reg.emplace<MaterialRef>(e, MaterialRef{submeshMaterial});
            reg.emplace<AABB>(e, d.hasAABB ? d.aabb : bounds);
        } else if (d.hasAABB) {
            reg.emplace<AABB>(e, d.aabb);
        }

        if (d.hasRigidBody) {
            RigidBody rb = d.rigidBody;
            rb.bodyId = RigidBody::kInvalidBody;
            FillColliderGeometry(rb, d.meshSource, staged); // rebuild mesh colliders
            reg.emplace<RigidBody>(e, rb);
        }
        if (d.hasLight) reg.emplace<DirectionalLightComponent>(e, d.light);
        if (d.hasPointLight) reg.emplace<PointLightComponent>(e, d.pointLight);
        if (d.hasVolume) reg.emplace<VolumeComponent>(e, d.volume);
        if (d.hasSpotLight) reg.emplace<SpotLightComponent>(e, d.spotLight);
        if (d.hasRectLight) reg.emplace<RectLightComponent>(e, d.rectLight);
        if (d.hasSchematic) {
            SchematicComponent sg;
            sg.asset = d.schematicAsset;
            reg.emplace<SchematicComponent>(e, std::move(sg));
        }
        if (d.hasDestructible) {
            Destructible& ds = reg.emplace<Destructible>(e, d.destructible);
            // A `.hbsave` snapshot carries break progress but never the chunk ENTITIES
            // (entt handles do not survive), so a restored break arrives in the
            // "activated but empty" state. destruction::Update rebuilds it; without the
            // flag the object comes back rendering and colliding as PRISTINE while
            // being internally fully broken and permanently unbreakable. Same reason
            // world::ApplyEntityState sets it - see Destructible::reactivate.
            ds.reactivate = ds.activated;
            ds.structureDirty = false;
            ds.chunkEntity.assign(ds.chunkState.size(), entt::entity{entt::null});
        }
        if (d.hasCheckpoint) reg.emplace<Checkpoint>(e, d.checkpoint);
        if (d.hasHealth) reg.emplace<Health>(e, d.health);
        if (d.hasWeapon) reg.emplace<Weapon>(e, d.weapon);
        if (d.hasAIPerception) reg.emplace<AIPerception>(e, d.aiPerception);
        if (d.hasAIBehavior) reg.emplace<AIBehavior>(e, d.aiBehavior);
        if (d.hasSpawner) reg.emplace<Spawner>(e, d.spawner);
        if (d.hasEncounter) reg.emplace<Encounter>(e, d.encounter);
        if (d.hasSpawned) reg.emplace<Spawned>(e, d.spawned);
        if (d.hasMorphState) {
            MorphState ms = d.morphState;
            // Resolve the blendshape delta atlas for this mesh. Cache-first, so the
            // SECOND and every later spawn of the same mesh resolves without a CPU
            // model and without a second upload (plan blocker B3, both halves).
            if (d.hasMesh) {
                MorphAtlas atlas;
                if (ResolveMorphAtlas(renderer, staged, d.meshSource, atlas) && atlas.hasMorphs) {
                    // The channel LIST comes over whenever the mesh has blendshapes -
                    // it is what the Inspector shows and what CollectDrawItems maps
                    // names to atlas rows with. `resolved` still means "there is a
                    // real atlas to sample", so it waits for a valid handle (there is
                    // none without a device).
                    ms.vertexCount = atlas.vertexCount;
                    ms.targetNames = atlas.names;
                    if (atlas.texture.IsValid()) {
                        ms.morphTexture = atlas.texture;
                        ms.resolved = true;
                    }
                }
            }
            reg.emplace<MorphState>(e, std::move(ms));
        }
        if (d.hasFacialAnimator) reg.emplace<FacialAnimator>(e, d.facialAnimator);
        if (d.hasInteractable) reg.emplace<Interactable>(e, d.interactable);
        if (d.hasTrigger) reg.emplace<TriggerVolume>(e, d.trigger);
        if (d.hasCharacterRig) reg.emplace<Character>(e, d.characterRig); // parts spawned post-load
        if (d.hasCamera) reg.emplace<CameraComponent>(e, d.camera);
        if (d.hasCameraZone) reg.emplace<CameraZone>(e, d.cameraZone);
        if (d.hasMusicZone) reg.emplace<MusicZone>(e, d.musicZone);
        if (d.hasCameraSpline) reg.emplace<CameraSpline>(e, d.cameraSpline);
        if (d.hasPaint && !d.paintSource.empty()) {
            // A MISSING CANVAS MUST NOT DELETE THE PAINT COMPONENT. Same rule, same
            // reason as the mesh/material block above: `paint::Load` failing (the
            // `.hbpaint` deleted, renamed, not yet copied to this machine, locked by
            // a virus scanner) used to mean NO PaintComponent at all - and the
            // writer emits "paint" only when the component exists, so the next
            // Ctrl+S dropped the whole key: source, resolution, relief, opacity,
            // heightScale, lodBias, layer and projection, permanently, off one
            // warning line. Every one of those lives in the SCENE, not in the
            // canvas, so all of it is recoverable from `d` alone.
            const auto pit = staged.paints.find(d.paintSource);
            const bool haveCanvas = pit != staged.paints.end();
            PaintComponent pc;          // canvas holds the layer stack;
            pc.resolution = haveCanvas ? pit->second.resolution : d.paintResolution;
            if (haveCanvas) {           // metadata always comes from the scene
                pc.layers = pit->second.layers;
                pc.activeLayer = pit->second.activeLayer;
            }
            // The pixels are UNKNOWN, not empty. WritePaintCanvases skips a canvas
            // carrying this flag, so a save cannot write an EMPTY `.hbpaint` over a
            // file that was merely unreadable for a moment - which would turn a
            // recoverable broken reference into real destruction.
            pc.canvasMissing = !haveCanvas;
            pc.source = d.paintSource;
            pc.enabled = d.paintEnabled;
            pc.locked = d.paintLocked;
            pc.reliefEnabled = d.paintReliefEnabled;
            pc.opacity = d.paintOpacity;
            pc.heightScale = d.paintHeightScale;
            pc.lodBias = d.paintLodBias;
            pc.layer = d.paintLayer;
            pc.projection = d.paintProjection;
            pc.dirty = true;
            pc.gpuReady = false;
            // RE-ADOPT this entity's canvas textures if it has had them before (a
            // shard respawn, an undo/redo). paint::Sync uploads on an invalid handle
            // and UPDATES an existing one, and there is no texture destroy in the
            // RHI - so without this every respawn of a painted mesh permanently
            // adds two mip'd RGBA textures. See InstantiateCaches::paint.
            const u64 pguid = reg.all_of<Guid>(e) ? reg.get<Guid>(e).value : 0ull;
            const std::string pkey =
                pguid != 0 ? d.paintSource + "#" + std::to_string(pguid) : std::string();
            if (haveCanvas && !pkey.empty()) CacheGetPaint(pkey, pc.colorTex, pc.matTex);
            PaintComponent& placed = reg.emplace<PaintComponent>(e, std::move(pc));
            if (haveCanvas) {
                paint::Sync(renderer, placed); // upload (first time) or update (respawn)
                if (!pkey.empty() && placed.colorTex.IsValid() && placed.matTex.IsValid())
                    CachePutPaint(pkey, placed.colorTex, placed.matTex);
            }
        }
        if (d.hasTerrain) {
            reg.emplace<TerrainComponent>(e, d.terrain);
            // Resolve each splat layer's albedo/normal/MR from its (staged) material.
            TerrainComponent& tc = reg.get<TerrainComponent>(e);
            for (int li = 0; li < 4; ++li) {
                if (tc.splatLayerSrc[li].empty()) continue;
                if (auto mit = staged.materials.find(tc.splatLayerSrc[li]);
                    mit != staged.materials.end()) {
                    tc.splatAlbedoTex[li] = loadTexture(mit->second.albedoTex);
                    tc.splatNormalTex[li] = loadTexture(mit->second.normalTex);
                    tc.splatMRTex[li] = loadTexture(mit->second.mrTex);
                    tc.splatRoughFactor[li] = mit->second.roughness;
                }
            }
        }
        if (d.hasMotionMatching) reg.emplace<MotionMatching>(e, d.motionMatching);
        if (d.hasRotator) reg.emplace<Rotator>(e, d.rotator);
        if (d.hasModelGroup) reg.emplace<ModelGroup>(e, d.modelGroup);
        if (d.hasCensor) reg.emplace<CensorComponent>(e, d.censor);
        if (d.hasCharacter) reg.emplace<CharacterController>(e, d.character);
        if (d.hasIK) reg.emplace<IKConstraint>(e, d.ik);
        if (d.hasUI) {
            UIElement el = d.uiElement;
            el.hovered = false;
            el.clicked = false;
            reg.emplace<UIElement>(e, el);
        }
        if (d.hasUICanvas) reg.emplace<UICanvas>(e, d.uiCanvas);
        if (d.hasUIAnimator) reg.emplace<UIAnimator>(e, d.uiAnimator);
        if (d.hasUIPanel) reg.emplace<UIPanel>(e, d.uiPanel);
        if (d.hasUILayoutGroup) reg.emplace<UILayoutGroup>(e, d.uiLayoutGroup);
        if (d.hasUICanvasGroup) reg.emplace<UICanvasGroup>(e, d.uiCanvasGroup);
        if (d.hasWorldText) reg.emplace<WorldText>(e, d.worldText);
        if (d.hasAnim) reg.emplace<AnimationTrack>(e, d.anim);
        if (d.hasAnimator) {
            Animator an = d.animator;
            an.palette.clear(); // runtime state rebuilds on the next tick
            an.mapKey = 0;
            reg.emplace<Animator>(e, std::move(an));
        }
        if (d.hasAudio) {
            AudioSource src = d.audio;
            src.playing = false;
            src.voiceId = AudioSource::kNoVoice;
            reg.emplace<AudioSource>(e, src);
        }
        if (d.hasDialogueActor) reg.emplace<DialogueActor>(e, d.dialogueActor);
        if (d.hasParticles) reg.emplace<ParticleEmitter>(e, d.particles);
        if (d.hasDecal) reg.emplace<DecalComponent>(e, d.decal);
        if (d.hasWater) reg.emplace<WaterComponent>(e, d.water);
        if (d.hasNavAgent) reg.emplace<NavigationAgent>(e, d.navAgent);
        if (d.hasNavObstacle) reg.emplace<NavigationObstacle>(e, d.navObstacle);
        if (d.hasNavmeshInput) reg.emplace<NavmeshInput>(e, d.navmeshInput);
        if (d.hasPostVolume) reg.emplace<PostVolume>(e, d.postVolume);
        if (d.hasProbe) {
            ReflectionProbe& rp = reg.emplace<ReflectionProbe>(e, d.probe);
            // Load the cached bake (.hbprobe) so the probe lights the scene without
            // a re-bake; falls back to unbaked if the cache is missing/stale.
            if (!rp.source.empty()) {
                const IBLMaps m =
                    LoadProbeMaps(renderer, Project::Active().AssetsDir() / rp.source);
                if (m.valid) {
                    rp.irradiance = m.irradiance;
                    rp.prefiltered = m.prefiltered;
                    rp.prefilteredMaxLod = m.prefilteredMaxLod;
                    rp.baked = true;
                }
            }
        }
    }

    // Second pass: parent links (indices are within this file's entity list).
    //
    // BLOCKER B1 - THE REGRESSION PIN. This loop used to iterate every row of the
    // file and guard only the PARENT handle, so under any slice a child row outside
    // the slice whose parent row was inside it called
    // reg.emplace<Parent>(entt::null, ...) - an ENTT_ASSERT in Debug and
    // out-of-bounds sparse-set writes in Release. It now iterates the SLICE and
    // guards BOTH handles. On the full-file path this is behaviour-identical:
    // every row has a handle, so neither new guard can fire.
    for (usize k = 0; k < slice.visits(); ++k) {
        const usize i = slice.row(k);
        if (i >= data.entities.size()) continue;      // bad index, already warned
        if (created[i] == entt::null) continue;       // the CHILD - a repeat index
        const int p = data.entities[i].parent;
        if (p < 0) continue;                          // authored root
        if (p < static_cast<int>(created.size()) && created[p] != entt::null) {
            reg.emplace<Parent>(created[i], Parent{created[p]});
            continue;
        }
        // The parent row exists in the file but not in this slice (or the index is
        // out of range): the child becomes a ROOT. Deliberate, and the same
        // fallback SplitSceneFile documented for a cross-layer parent
        // (StreamingSalvage.h SALVAGE 1). It means the child now renders at its
        // LOCAL transform in world space - a silent teleport - so it is never
        // silent. tags::AssignSubtree exists to keep whole subtrees in one shard
        // precisely so this cannot happen from ordinary authoring.
        HBE_WARN("Scene: '{}' (row {}) has parent row {} outside this slice; "
                 "loaded as a ROOT at its local transform.",
                 data.entities[i].name, i, p);
    }

    // Already slice-correct: `created` only holds handles for rows this call made,
    // so a slice's createdOut is exactly that slice (in file-row order).
    if (createdOut) {
        createdOut->clear();
        createdOut->reserve(slice.visits()); // the slice, not the whole file
        for (const entt::entity e : created)
            if (e != entt::null) createdOut->push_back(e);
    }

    // Eager UI asset preload: bake fonts + load every UI texture NOW (scene
    // load) instead of lazily on first draw - kills the blank-text/white-quad
    // first frame and the disk-I/O hitch inside the frame loop.
    // Scoped to the SLICE: a shard with no UI/WorldText in it must not re-run the
    // whole level's preload every time it spawns.
    bool anyUI = false;
    for (usize k = 0; k < slice.visits() && !anyUI; ++k) {
        const usize i = slice.row(k);
        if (i >= data.entities.size()) continue;
        const EntityData& d = data.entities[i];
        if (d.hasUI || d.hasWorldText) anyUI = true;
    }
    if (anyUI && Project::HasActive()) {
        ui::PreloadUIAssets(scene, renderer, Project::Active().AssetsDir());
    }

    // Modular characters: now that every entity + parent link exists, spawn each
    // root's loadout parts (welded from its .hbchar). Snapshot the roots first -
    // character::Instantiate creates child entities, which would invalidate a live
    // view. Only freshly-loaded roots (no live parts yet) are built, so an additive
    // load over an already-assembled character leaves it untouched.
    //
    // The candidate set is the whole registry on the full-file path (unchanged, so a
    // root left unassembled by an earlier load still gets picked up) but only THIS
    // SLICE's entities under a slice: at N shards streaming in and out, a
    // per-registry scan per spawn is O(registry) work for a fixed answer.
    const auto forEachUnassembledCharacter = [&](auto&& fn) {
        if (slice.sliced()) {
            for (const entt::entity e : created)
                if (e != entt::null && reg.valid(e))
                    if (const Character* c = reg.try_get<Character>(e); c && c->liveParts.empty())
                        fn(e);
            return;
        }
        for (const entt::entity e : reg.view<Character>())
            if (reg.get<Character>(e).liveParts.empty()) fn(e);
    };
    if (Project::HasActive()) {
        const fs::path assetsDir = Project::Active().AssetsDir();
        std::vector<entt::entity> charRoots;
        forEachUnassembledCharacter([&](entt::entity e) { charRoots.push_back(e); });
        for (const entt::entity e : charRoots)
            character::Instantiate(scene, renderer, e, assetsDir);
    } else {
        // No active project -> assets can't resolve (same assumption as UI-preload /
        // probe-load above). Don't fail silently: a Character root would otherwise
        // load with no parts + no skeleton binding.
        forEachUnassembledCharacter([&](entt::entity e) {
            HBE_WARN("Character '{}' not assembled: no active project to resolve its assets.",
                     reg.get<Character>(e).asset);
        });
    }

    // A slice lands potentially every few frames, and RecentLog backs the boot
    // screen's {log} token - so the per-load line stays INFO for a whole-file load
    // (one per scene switch, and load-bearing in bug reports) and drops to TRACE for
    // a slice.
    if (slice.sliced()) {
        u32 made = 0;
        for (const entt::entity e : created)
            if (e != entt::null) ++made;
        HBE_TRACE("Scene: instantiated {} entities (slice of {}).", made,
                  data.entities.size());
    } else {
        HBE_INFO("Scene: instantiated {} entities ({}).", data.entities.size(),
                 mode == LoadMode::Replace ? "replace" : "additive");
    }
}

// Assets are referenced relative to the project's Assets dir; the scene file
// itself lives somewhere inside it. Walk up until "Assets" is found.
fs::path FindAssetsDir(const fs::path& scenePath) {
    fs::path dir = scenePath.parent_path();
    while (!dir.empty() && dir.filename() != "Assets") {
        const fs::path up = dir.parent_path();
        if (up == dir) return scenePath.parent_path(); // not under Assets/
        dir = up;
    }
    return dir.empty() ? scenePath.parent_path() : dir;
}

bool LoadScene(Scene& scene, Renderer& renderer, const fs::path& path, LoadMode mode) {
    SceneData data;
    if (!ParseSceneFile(path, data)) return false;
    StagedAssets staged;
    StageAssets(data, FindAssetsDir(path), staged);
    // Additive loads tag their entities with the scene PATH so the hierarchy can
    // group them and saving can write each back to its own file; a Replace load
    // is the active scene (left untagged).
    const std::string tag =
        mode == LoadMode::Additive ? path.string() : std::string();
    Instantiate(scene, renderer, data, staged, mode, nullptr, tag);
    return true;
}


// --- --test-noleveltypes ------------------------------------------------------

GuidMigrationStats MigrateSceneGuids(const std::filesystem::path& assetsDir, bool dryRun) {
    namespace fs = std::filesystem;
    GuidMigrationStats st;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(assetsDir, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file(ec)) continue;
        const fs::path p = it->path();
        if (p.extension() != ".hbscene") continue; // NOT .hbprefab - see the header.
        ++st.files;

        json j;
        {
            std::ifstream in(p, std::ios::binary);
            if (!in) { HBE_WARN("MigrateSceneGuids: cannot read '{}'.", p.string()); ++st.failed; continue; }
            try { in >> j; }
            catch (const std::exception& e) {
                HBE_WARN("MigrateSceneGuids: '{}' is not valid JSON ({}).", p.string(), e.what());
                ++st.failed; continue;
            }
        }
        const auto ents = j.find("entities");
        if (ents == j.end() || !ents->is_array()) continue;

        const u64 seed = guid::SeedFromPath(p);
        u32 addedHere = 0;
        for (usize i = 0; i < ents->size(); ++i) {
            json& je = (*ents)[i];
            if (!je.is_object()) continue;
            if (const auto g = je.find("guid");
                g != je.end() && g->is_string() && guid::FromHex(g->get<std::string>()) != 0) {
                ++st.already; continue;
            }
            // Index-keyed to match ParseSceneJson exactly: parse pushes entities in
            // array order (out.entities.push_back is the single push site), and the
            // derivation loop indexes that same vector.
            je["guid"] = guid::ToHex(guid::Derive(seed, static_cast<u32>(i)));
            ++addedHere;
        }
        st.stamped += addedHere;
        if (addedHere > 0 && !dryRun) {
            std::ofstream out(p, std::ios::binary | std::ios::trunc);
            if (!out) { HBE_WARN("MigrateSceneGuids: cannot write '{}'.", p.string()); ++st.failed; continue; }
            out << j.dump(2);
        }
        if (addedHere > 0)
            HBE_INFO("MigrateSceneGuids: {} - {} stamped.", p.filename().string(), addedHere);
    }
    return st;
}

bool LevelTypesSelfTest() {
    namespace fs = std::filesystem;
    bool ok = true;
    const auto expect = [&ok](bool cond, const char* what) {
        if (!cond) {
            ok = false;
            HBE_ERROR("noleveltypes: FAILED - {}", what);
        }
    };

    // 1) There is no UI scene kind. It existed only so a level could own a third
    //    "<base>.ui.hbscene" layer; UI is now a standalone scene of its own.
    expect(SceneKindFromString("ui") == SceneKind::Full,
           "\"ui\" must no longer parse as a scene kind");
    expect(std::string(ToString(SceneKind::Full)) == "full" &&
               std::string(ToString(SceneKind::Static)) == "static" &&
               std::string(ToString(SceneKind::Dynamic)) == "dynamic",
           "the only scene kinds are full/static/dynamic");

    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "hbe_noleveltypes";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    // 2) A scene whose FILENAME looks like an old level layer is an ORDINARY
    //    scene. The deleted loader would have seen "Zone.static.hbscene", resolved
    //    the level "Zone", and composed "Zone.dynamic.hbscene" in alongside it.
    //    Nothing may do that any more: the .dynamic file next to it is a separate,
    //    unrelated scene.
    const fs::path layerNamed = dir / "Zone.static.hbscene";
    const fs::path sibling = dir / "Zone.dynamic.hbscene";
    {
        Scene a;
        for (const char* n : {"Ground", "Wall"}) {
            const entt::entity e = a.CreateEntity(n);
            a.Registry().emplace<Transform>(e);
            a.Registry().emplace<SceneLayer>(e, SceneLayer{SceneKind::Static});
        }
        expect(SaveScene(a, layerNamed), "save a scene named like a level layer");

        Scene b;
        const entt::entity e = b.CreateEntity("Actor");
        b.Registry().emplace<Transform>(e);
        b.Registry().emplace<SceneLayer>(e, SceneLayer{SceneKind::Dynamic});
        expect(SaveScene(b, sibling), "save the sibling .dynamic scene");
    }
    SceneData layerData;
    expect(ParseSceneFile(layerNamed, layerData), "a .static.hbscene-named file parses");
    expect(layerData.entities.size() == 2,
           "a .static.hbscene-named file yields ONLY its own entities "
           "(no sibling .dynamic layer composed in)");
    bool sawActor = false;
    for (const EntityData& d : layerData.entities)
        if (d.name == "Actor") sawActor = true;
    expect(!sawActor, "the sibling .dynamic scene's entities must NOT appear");
    expect(layerData.kind == SceneKind::Full,
           "a saved scene's header kind is Full - a file is not a layer");

    // 3) The per-object Static/Dynamic tag is the thing the navmesh filter and the
    //    painterly exemption read. It is per ENTITY and must survive a round trip
    //    now that it no longer rides on the file's header.
    {
        Scene m;
        const entt::entity st = m.CreateEntity("Rock");
        m.Registry().emplace<Transform>(st);
        m.Registry().emplace<SceneLayer>(st, SceneLayer{SceneKind::Static});
        const entt::entity dy = m.CreateEntity("Crate");
        m.Registry().emplace<Transform>(dy);
        m.Registry().emplace<SceneLayer>(dy, SceneLayer{SceneKind::Dynamic});
        const entt::entity un = m.CreateEntity("Untagged");
        m.Registry().emplace<Transform>(un);

        const fs::path merged = dir / "Merged.hbscene";
        expect(SaveScene(m, merged), "save a merged scene");
        SceneData d;
        expect(ParseSceneFile(merged, d), "the merged scene parses");
        expect(d.entities.size() == 3, "the merged scene round-trips all 3 entities");
        int nStatic = 0, nDynamic = 0, nNone = 0;
        for (const EntityData& e : d.entities) {
            if (!e.hasSceneLayerTag) ++nNone;
            else if (e.sceneLayerKind == SceneKind::Static) ++nStatic;
            else if (e.sceneLayerKind == SceneKind::Dynamic) ++nDynamic;
        }
        expect(nStatic == 1 && nDynamic == 1 && nNone == 1,
               "per-entity Static/Dynamic/untagged survives the round trip");

        // 4) A round trip preserves the scene's CONTENT exactly - every entity, by
        //    guid, with its name, its Static/Dynamic layer and its parent. Rebuild
        //    from the parsed data (the fields this test wrote; a full rebuild is
        //    scene::Instantiate's job and needs a GPU) and re-save.
        //
        //    ONE round trip is now byte-identical. It used not to be: the gather
        //    walked entt views, which iterate their pool in REVERSE insertion order,
        //    so every save flipped the file's entity array and it took TWO round
        //    trips to come back. BuildSceneJson now sequences the write from a
        //    single pool in creation order (see the note there), so the transform is
        //    the identity. Two round trips are still what is asserted - if one is
        //    identity, two are too, and the two-trip form also catches a writer that
        //    is merely an involution rather than stable.
        const auto rebuild = [](const SceneData& src, Scene& dst) {
            std::vector<entt::entity> made;
            for (const EntityData& e : src.entities) {
                const entt::entity ne = dst.CreateEntity(e.name);
                if (e.guid != 0) dst.Registry().emplace_or_replace<Guid>(ne, Guid{e.guid});
                if (e.hasTransform) dst.Registry().emplace<Transform>(ne, e.transform);
                if (e.hasSceneLayerTag)
                    dst.Registry().emplace<SceneLayer>(ne, SceneLayer{e.sceneLayerKind});
                made.push_back(ne);
            }
            for (usize i = 0; i < src.entities.size(); ++i) {
                const int p = src.entities[i].parent;
                if (p >= 0 && p < static_cast<int>(made.size()))
                    dst.Registry().emplace<Parent>(made[i],
                                                   Parent{made[static_cast<usize>(p)]});
            }
        };
        // Fingerprint: guid -> (name, layer, parent guid). Order-independent, which
        // is the point.
        const auto fingerprint = [](const SceneData& src) {
            std::unordered_map<u64, std::string> fp;
            for (const EntityData& e : src.entities) {
                const int p = e.parent;
                const u64 pg = (p >= 0 && p < static_cast<int>(src.entities.size()))
                                   ? src.entities[static_cast<usize>(p)].guid
                                   : 0;
                fp[e.guid] = e.name + "|" +
                             (e.hasSceneLayerTag ? ToString(e.sceneLayerKind) : "none") + "|" +
                             std::to_string(pg);
            }
            return fp;
        };

        const fs::path r1 = dir / "Round1.hbscene";
        const fs::path r2 = dir / "Round2.hbscene";
        Scene s1;
        rebuild(d, s1);
        expect(SaveScene(s1, r1), "re-save the rebuilt scene");
        SceneData d1;
        expect(ParseSceneFile(r1, d1), "the re-saved scene parses");
        expect(fingerprint(d) == fingerprint(d1) && fingerprint(d).size() == 3,
               "a round trip preserves every entity by guid, with its name, layer "
               "and parent");
        Scene s2;
        rebuild(d1, s2);
        expect(SaveScene(s2, r2), "re-save after a second round trip");

        const auto readAll = [](const fs::path& p2) {
            std::ifstream in(p2, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        };
        const std::string t0 = readAll(merged), t2 = readAll(r2);
        if (t0.empty() || t0 != t2) {
            usize at = 0;
            while (at < t0.size() && at < t2.size() && t0[at] == t2[at]) ++at;
            HBE_ERROR("noleveltypes: two round trips differ at byte {} ({} vs {} bytes)",
                      at, t0.size(), t2.size());
        }
        expect(!t0.empty() && t0 == t2,
               "two round trips return byte-for-byte to the original save");
    }

    fs::remove_all(dir, ec);
    return ok;
}


// --- --test-sceneslice --------------------------------------------------------

namespace {

// The world's CONTENT, keyed by stable guid, read back through the shipping
// writers (SaveSceneToString -> EntityToJson, runtimeTags on) rather than a
// hand-rolled component walk - so this compares all ~130 serialized fields and
// cannot drift as components are added.
//
// "parent" is a per-file ARRAY INDEX, so it is meaningless across two differently
// ordered worlds: it is lifted out of the blob and re-expressed as the PARENT'S
// GUID (0 = root), which is comparable.
struct WorldFingerprint {
    std::map<u64, std::string> blob;   // guid -> entity JSON minus "parent"
    std::map<u64, u64> parentOf;       // guid -> parent guid (0 = root)
    usize entities = 0;
    usize parentLinks = 0;
};

WorldFingerprint Fingerprint(const Scene& s) {
    WorldFingerprint fp;
    json root;
    try {
        root = json::parse(SaveSceneToString(s));
    } catch (const std::exception&) {
        return fp;
    }
    const json& ents = root.value("entities", json::array());
    const auto guidAt = [&](usize i) -> u64 {
        if (i >= ents.size() || !ents[i].is_object()) return 0;
        const auto g = ents[i].find("guid");
        return (g != ents[i].end() && g->is_string()) ? guid::FromHex(g->get<std::string>()) : 0;
    };
    for (usize i = 0; i < ents.size(); ++i) {
        json je = ents[i];
        const u64 g = guidAt(i);
        u64 pg = 0;
        if (const auto it = je.find("parent"); it != je.end() && it->is_number_integer()) {
            const int p = it->get<int>();
            if (p >= 0) {
                pg = guidAt(static_cast<usize>(p));
                ++fp.parentLinks;
            }
            je.erase("parent");
        }
        fp.blob[g] = je.dump();
        fp.parentOf[g] = pg;
        ++fp.entities;
    }
    return fp;
}

// LIVE entities. Not Scene::EntityCount(), which reports the entity storage's
// size (released slots included) - after a destroy-and-reload that would count
// recycled slots and hide exactly the leak this test is looking for.
usize LiveCount(const Scene& s) {
    const auto& reg = s.Registry();
    // EnTT's CONST storage<T>() returns a pointer (null when absent), as Scene.h notes.
    const auto* st = reg.storage<entt::entity>();
    if (!st) return 0;
    usize n = 0;
    for (const entt::entity e : *st)
        if (reg.valid(e)) ++n;
    return n;
}

// Live entities carrying a Parent, and whether every one of those links is sane.
// This is the direct B1 pin: the old parent pass emplaced Parent on entt::null,
// which is an ENTT_ASSERT in Debug and a sparse-set write out of bounds in Release.
struct ParentAudit {
    usize links = 0;
    bool allValid = true; // both the holder and the target are live entities
};
ParentAudit AuditParents(const Scene& s) {
    ParentAudit a;
    const auto& reg = s.Registry();
    for (const entt::entity e : reg.view<const Parent>()) {
        ++a.links;
        if (!reg.valid(e)) { a.allValid = false; continue; }
        if (!reg.valid(reg.get<const Parent>(e).entity)) a.allValid = false;
    }
    return a;
}

} // namespace

bool SceneSliceSelfTest() {
    bool ok = true;
    const auto expect = [&ok](bool cond, const char* what) {
        if (!cond) {
            ok = false;
            HBE_ERROR("sceneslice: FAILED - {}", what);
        }
    };

    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "hbe_sceneslice";
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "Materials", ec);

    // No GPU: a device-less Renderer's UploadMesh/UploadTexture return invalid
    // handles instead of touching a device, which is exactly the headless contract
    // the other serializer self-tests keep.
    Renderer renderer;

    {
        MaterialAsset a;
        a.name = "A";
        a.roughness = 0.25f;
        MaterialAsset b;
        b.name = "B";
        b.roughness = 0.75f;
        expect(assets::SaveMaterial(dir / "Materials" / "A.hbmat", a) &&
                   assets::SaveMaterial(dir / "Materials" / "B.hbmat", b),
               "write two scratch .hbmat assets (one per slice)");
    }

    // --- 1. Author ONE scene file --------------------------------------------
    // Deliberately contains everything that makes slicing hard: a 3-deep chain
    // inside one slice, a CROSS-SLICE parent link, a parent stored AFTER its child
    // in the file, two entities sharing a name (names are not identity), streaming
    // Tags, and header environment values nothing defaults to.
    const fs::path file = dir / "Slice.hbscene";
    {
        Scene s;
        auto& reg = s.Registry();
        SceneEnvironment& env = s.Environment();
        env.ambientIntensity = 0.37f;
        env.exposure = 2.5f;
        env.shadowDistance = 777.0f;
        env.post.vignette = 0.42f;
        env.post.bloomEnabled = 0;
        env.post.fogDensity = 0.05f;

        const auto make = [&](const char* n, const glm::vec3& p) {
            const entt::entity e = s.CreateEntity(n);
            Transform t;
            t.position = p;
            reg.emplace<Transform>(e, t);
            return e;
        };

        const entt::entity hub = make("Hub", {1.0f, 0.0f, 0.0f});
        reg.emplace<PointLightComponent>(hub, PointLightComponent{{0.2f, 0.4f, 0.6f}, 3.0f, 12.0f});
        const entt::entity mid = make("Mid", {2.0f, 0.0f, 0.0f});
        reg.emplace<Parent>(mid, Parent{hub});
        Health hp;
        hp.max = 55.0f;
        hp.current = 33.0f;
        reg.emplace<Health>(mid, hp);
        const entt::entity leaf = make("Leaf", {3.0f, 0.0f, 0.0f});
        reg.emplace<Parent>(leaf, Parent{mid});
        Interactable ia;
        ia.prompt = "Pry open";
        ia.range = 4.25f;
        reg.emplace<Interactable>(leaf, ia);

        const entt::entity camp = make("Camp", {40.0f, 0.0f, 0.0f});
        Spawner sp;
        sp.prefab = "Prefabs/Guard.hbprefab";
        sp.count = 4;
        reg.emplace<Spawner>(camp, sp);
        tags::Assign(reg, camp, tags::Intern("Camp"), 2);
        const entt::entity guard = make("Guard", {41.0f, 0.0f, 0.0f});
        reg.emplace<Parent>(guard, Parent{camp});
        reg.emplace<Health>(guard, Health{});
        tags::Assign(reg, guard, tags::Intern("Camp"), 2);

        // THE CROSS-SLICE CASE: authored as a child of Camp (slice B) but assigned
        // to slice A. Nothing in the shipping authoring tools produces this
        // (tags::AssignSubtree tags whole subtrees) - it is here precisely because
        // the fallback has to be proven.
        const entt::entity orphan = make("Orphan", {42.0f, 1.0f, 0.0f});
        reg.emplace<Parent>(orphan, Parent{camp});

        const entt::entity wire = make("Wire", {50.0f, 0.0f, 0.0f});
        TriggerVolume tv;
        tv.halfExtents = {9.0f, 3.0f, 9.0f};
        tv.flag = "camp_entered";
        reg.emplace<TriggerVolume>(wire, tv);

        const entt::entity lonely = make("Lonely", {-10.0f, 0.0f, 0.0f});
        AudioSource as;
        as.asset = "Audio/Wind.uaf";
        reg.emplace<AudioSource>(lonely, as);

        // Material carriers: a prim mesh (no file IO in staging) plus a MaterialRef,
        // which is what puts "mesh.material" in the file for StageAssets to find.
        for (const auto& [n, mat] : std::initializer_list<std::pair<const char*, const char*>>{
                 {"MatA", "Materials/A.hbmat"}, {"MatB", "Materials/B.hbmat"}}) {
            const entt::entity e = make(n, {0.0f, 5.0f, 0.0f});
            reg.emplace<MeshInstance>(e, MeshInstance{});
            reg.emplace<MeshRef>(e, MeshRef{"prim:cube"});
            reg.emplace<MaterialRef>(e, MaterialRef{mat});
        }

        // Two entities with the SAME name, one per slice.
        make("Twin", {7.0f, 0.0f, 0.0f});
        make("Twin", {8.0f, 0.0f, 0.0f});

        expect(SaveScene(s, file), "save the authored slice-test scene");
    }

    SceneData data;
    expect(ParseSceneFile(file, data), "the slice-test scene parses");
    if (!ok) {
        fs::remove_all(dir, ec);
        return false;
    }
    expect(data.entities.size() == 12, "all 12 authored entities round-trip");
    expect(data.ambientIntensity == 0.37f && data.exposure == 2.5f &&
               data.shadowDistance == 777.0f && data.post.vignette == 0.42f &&
               data.post.bloomEnabled == 0u,
           "the file header carries the authored environment");

    // Rows by name (save REVERSES the entity array, so nothing may assume order).
    const auto rowOf = [&](const std::string& n) -> int {
        for (usize i = 0; i < data.entities.size(); ++i)
            if (data.entities[i].name == n) return static_cast<int>(i);
        return -1;
    };
    const int rHub = rowOf("Hub"), rMid = rowOf("Mid"), rLeaf = rowOf("Leaf");
    const int rCamp = rowOf("Camp"), rGuard = rowOf("Guard"), rOrphan = rowOf("Orphan");
    const int rWire = rowOf("Wire"), rLonely = rowOf("Lonely");
    const int rMatA = rowOf("MatA"), rMatB = rowOf("MatB");
    expect(rHub >= 0 && rMid >= 0 && rLeaf >= 0 && rCamp >= 0 && rGuard >= 0 &&
               rOrphan >= 0 && rWire >= 0 && rLonely >= 0 && rMatA >= 0 && rMatB >= 0,
           "every authored entity is findable by name");
    if (!ok) {
        fs::remove_all(dir, ec);
        return false;
    }
    const u64 gOrphan = data.entities[static_cast<usize>(rOrphan)].guid;
    const u64 gCamp = data.entities[static_cast<usize>(rCamp)].guid;

    // Slice A = Hub chain + Orphan + Lonely + MatA + one Twin.
    // Slice B = Camp + Guard + Wire + MatB + the other Twin.
    // Disjoint, and together exactly the file.
    std::vector<u32> sliceA{static_cast<u32>(rHub), static_cast<u32>(rMid),
                            static_cast<u32>(rLeaf), static_cast<u32>(rOrphan),
                            static_cast<u32>(rLonely), static_cast<u32>(rMatA)};
    std::vector<u32> sliceB{static_cast<u32>(rCamp), static_cast<u32>(rGuard),
                            static_cast<u32>(rWire), static_cast<u32>(rMatB)};
    {
        std::unordered_set<u32> taken(sliceA.begin(), sliceA.end());
        taken.insert(sliceB.begin(), sliceB.end());
        for (u32 i = 0; i < static_cast<u32>(data.entities.size()); ++i)
            if (!taken.contains(i)) (sliceA.size() <= sliceB.size() ? sliceA : sliceB).push_back(i);
        expect(sliceA.size() + sliceB.size() == data.entities.size(),
               "the two slices partition the file exactly");
    }

    // --- 2. StageAssets stages strictly the slice's assets --------------------
    {
        StagedAssets all, a, b;
        StageAssets(data, dir, all);
        StageAssets(data, dir, a, sliceA.data(), static_cast<u32>(sliceA.size()));
        StageAssets(data, dir, b, sliceB.data(), static_cast<u32>(sliceB.size()));
        expect(all.materials.size() == 2, "a full stage loads both materials");
        expect(a.materials.contains("Materials/A.hbmat") &&
                   !a.materials.contains("Materials/B.hbmat"),
               "slice A stages its own material and NOT slice B's");
        expect(b.materials.contains("Materials/B.hbmat") &&
                   !b.materials.contains("Materials/A.hbmat"),
               "slice B stages its own material and NOT slice A's");
        StagedAssets empty;
        const u32 none[1] = {0};
        StageAssets(data, dir, empty, none, 0); // non-null, count 0 = a real empty slice
        expect(empty.materials.empty(), "an empty slice stages nothing");
    }

    // --- 3. Full load = the reference world ----------------------------------
    Scene full;
    StagedAssets stagedFull;
    StageAssets(data, dir, stagedFull);
    Instantiate(full, renderer, data, stagedFull, LoadMode::Replace);
    const WorldFingerprint fpFull = Fingerprint(full);
    const ParentAudit paFull = AuditParents(full);
    expect(fpFull.entities == data.entities.size(),
           "the full load creates every entity in the file");
    expect(paFull.links == 4 && paFull.allValid,
           "the full load links all four authored parents, all handles live");

    // --- 4. Two disjoint slices build the SAME world --------------------------
    Scene sliced;
    std::vector<entt::entity> createdA, createdB;
    BindWorld(sliced, renderer, data); // B2: the explicit environment step
    expect(sliced.Environment().ambientIntensity == 0.37f &&
               sliced.Environment().exposure == 2.5f &&
               sliced.Environment().shadowDistance == 777.0f &&
               sliced.Environment().post.vignette == 0.42f &&
               sliced.Environment().post.bloomEnabled == 0u &&
               sliced.Environment().post.fogDensity == 0.05f,
           "BindWorld applies the file's environment (B2: Replace is not available "
           "to a slice)");
    {
        StagedAssets sa, sb;
        StageAssets(data, dir, sa, sliceA.data(), static_cast<u32>(sliceA.size()));
        StageAssets(data, dir, sb, sliceB.data(), static_cast<u32>(sliceB.size()));
        // Poke the environment between the slices: a slice must never stamp it.
        sliced.Environment().exposure = 9.0f;
        Instantiate(sliced, renderer, data, sa, LoadMode::Additive, &createdA, {},
                    sliceA.data(), static_cast<u32>(sliceA.size()));
        Instantiate(sliced, renderer, data, sb, LoadMode::Additive, &createdB, {},
                    sliceB.data(), static_cast<u32>(sliceB.size()));
        expect(sliced.Environment().exposure == 9.0f,
               "an Additive slice load does NOT re-apply the environment "
               "(exactly once, by BindWorld)");
        sliced.Environment().exposure = 2.5f; // restore for the fingerprint compare
    }
    expect(createdA.size() == sliceA.size() && createdB.size() == sliceB.size(),
           "createdOut is exactly the slice, per slice");

    const WorldFingerprint fpSliced = Fingerprint(sliced);
    const ParentAudit paSliced = AuditParents(sliced);
    expect(fpSliced.entities == fpFull.entities,
           "two disjoint slices create exactly as many entities as one full load");
    expect(fpSliced.blob == fpFull.blob,
           "every entity, keyed by guid, has byte-identical component state under "
           "slicing");

    // The ONLY licensed difference: the cross-slice parent link becomes a root.
    {
        std::map<u64, u64> expected = fpFull.parentOf;
        const auto it = expected.find(gOrphan);
        expect(it != expected.end() && it->second == gCamp,
               "the full load parents Orphan to Camp");
        expected[gOrphan] = 0; // cross-slice -> ROOT, deliberately
        expect(fpSliced.parentOf == expected,
               "the hierarchy is identical EXCEPT that the cross-slice child is a root");
        expect(fpSliced.parentOf.count(gOrphan) == 1 && fpSliced.parentOf.at(gOrphan) == 0,
               "the cross-slice child survives as a ROOT (it is never dropped)");
    }
    expect(paSliced.links == paFull.links - 1,
           "one parent link fewer: the cross-slice one");
    // B1 REGRESSION PIN. Pre-fix, slice A's pass reached row `Guard`/`Camp` (outside
    // the slice, so created[] is null there) and emplaced Parent on entt::null.
    expect(paSliced.allValid,
           "no Parent component exists on, or points at, a dead handle (B1)");
    {
        const auto& reg = sliced.Registry();
        usize orphanRoots = 0;
        for (const entt::entity e : reg.view<const Guid>())
            if (reg.get<const Guid>(e).value == gOrphan && !reg.all_of<Parent>(e)) ++orphanRoots;
        expect(orphanRoots == 1,
               "the cross-slice child carries no Parent component at all");
    }

    // --- 5. Guids: adopted once, unique across slices ------------------------
    {
        const auto& reg = sliced.Registry();
        std::unordered_set<u64> seen;
        bool unique = true, nonZero = true;
        for (const entt::entity e : reg.view<const Guid>()) {
            const u64 g = reg.get<const Guid>(e).value;
            if (g == 0) nonZero = false;
            if (!seen.insert(g).second) unique = false;
        }
        expect(nonZero && unique, "guids are unique and non-zero across both slices");
        expect(seen.size() == data.entities.size(),
               "every file guid is adopted exactly once across the slices "
               "(slice 2's Claim sees slice 1's)");
        std::unordered_set<u64> fileGuids;
        for (const EntityData& d : data.entities) fileGuids.insert(d.guid);
        expect(seen == fileGuids,
               "the sliced world's guids ARE the file's guids (nothing re-minted)");
    }

    // --- 6. A second bind re-binds, it does not stack a second world ---------
    {
        StagedAssets sa, sb;
        StageAssets(data, dir, sa, sliceA.data(), static_cast<u32>(sliceA.size()));
        StageAssets(data, dir, sb, sliceB.data(), static_cast<u32>(sliceB.size()));
        BindWorld(sliced, renderer, data);
        expect(LiveCount(sliced) == 0,
               "BindWorld destroys the previous world");
        Instantiate(sliced, renderer, data, sa, LoadMode::Additive, nullptr, {},
                    sliceA.data(), static_cast<u32>(sliceA.size()));
        Instantiate(sliced, renderer, data, sb, LoadMode::Additive, nullptr, {},
                    sliceB.data(), static_cast<u32>(sliceB.size()));
        const WorldFingerprint again = Fingerprint(sliced);
        expect(again.entities == fpFull.entities,
               "a SECOND bind + slices leaves one world, not two (B2)");
        expect(again.blob == fpSliced.blob,
               "the re-bound world is identical to the first bind's");
        expect(sliced.Environment().ambientIntensity == 0.37f &&
                   sliced.Environment().exposure == 2.5f,
               "the second bind re-applies the environment");
    }

    // --- 7. Replace with a slice is refused, and changes nothing -------------
    {
        Scene guarded;
        StagedAssets st;
        StageAssets(data, dir, st, sliceA.data(), static_cast<u32>(sliceA.size()));
        Instantiate(guarded, renderer, data, st, LoadMode::Replace, nullptr, {},
                    sliceA.data(), static_cast<u32>(sliceA.size()));
        expect(LiveCount(guarded) == 0,
               "Replace + slice creates nothing");
        expect(guarded.Environment().shadowDistance != 777.0f,
               "Replace + slice does not apply the environment either - it is refused "
               "outright, so a caller cannot half-load a world");
    }

    // --- 8. Degenerate slices ------------------------------------------------
    {
        Scene s;
        StagedAssets st;
        const u32 one[1] = {static_cast<u32>(rWire)};
        BindWorld(s, renderer, data);
        Instantiate(s, renderer, data, st, LoadMode::Additive, nullptr, {}, one, 0);
        expect(LiveCount(s) == 0,
               "an empty slice (non-null indices, count 0) creates nothing");

        // Out-of-range and repeated indices are skipped, not fatal, and never
        // create a second copy of a row.
        const u32 bad[4] = {static_cast<u32>(rWire), static_cast<u32>(rWire), 9999u,
                            static_cast<u32>(rLonely)};
        std::vector<entt::entity> made;
        Instantiate(s, renderer, data, st, LoadMode::Additive, &made, {}, bad, 4);
        expect(made.size() == 2,
               "a repeated index and an out-of-range index are both skipped");
    }

    fs::remove_all(dir, ec);
    return ok;
}


// --- --test-lightingparity ----------------------------------------------------
//
// THE POINT OF THIS TEST, stated once: an interior surrounded by cubes rendered
// correctly DARK in the shipped game and wrongly BRIGHT in the editor, which makes
// lighting unauthorable. Nothing in the tree compared an editor-loaded environment
// against a runtime-loaded one, so the two could drift apart with no signal. This
// runs the SAME file through BOTH REAL PATHS and compares the whole environment
// value.
//
// It must be impossible to pass while the paths disagree, and equally impossible
// to pass trivially: every stamp is also checked against the AUTHORED numbers, so
// "both paths are identically broken" fails too.
namespace {

// Writes a syntactically valid `.hbgi` (the format BakeGIVolume emits) with a
// recognisable grid, so the test can assert the loaded volume IS this file.
bool WriteScratchGi(const fs::path& path, const glm::ivec3& dims, const glm::vec3& origin,
                    f32 spacing) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f{path, std::ios::binary};
    if (!f) return false;
    const u32 magic = 0x56494748u, ver = 1; // 'HGIV'
    const auto wr = [&](const void* p, usize n) {
        f.write(reinterpret_cast<const char*>(p), static_cast<std::streamsize>(n));
    };
    wr(&magic, 4);
    wr(&ver, 4);
    wr(&dims.x, 4); wr(&dims.y, 4); wr(&dims.z, 4);
    wr(&origin.x, 12);
    wr(&spacing, 4);
    const usize cells = static_cast<usize>(dims.x) * dims.y * dims.z;
    const std::vector<glm::vec4> sh(4 * cells, glm::vec4(0.5f, 0.4f, 0.3f, 1.0f));
    const std::vector<glm::vec4> depth(64 * cells, glm::vec4(3.0f, 9.0f, 0.0f, 0.0f));
    wr(sh.data(), sh.size() * sizeof(glm::vec4));
    wr(depth.data(), depth.size() * sizeof(glm::vec4));
    return true;
}

// Authors a scratch level: header values nothing defaults to, plus enough entities
// (and a cross-shard parent link) that the runtime path has real slices to load.
void AuthorParityScene(const fs::path& file, const std::string& giSource) {
    Scene s;
    auto& reg = s.Registry();
    SceneEnvironment& env = s.Environment();
    env.ambientIntensity = 0.6337f;   // the sealed-interior value, deliberately not 1.0
    env.exposure = 1.25f;
    env.shadowDistance = 1200.0f;
    env.post.vignette = 0.91f;
    env.post.bloomEnabled = 0;
    env.post.ssgiEnabled = 0;
    env.post.fogDensity = 0.037f;
    // NOT shadowCascades: it lives in PostSettings but is project-global quality,
    // deliberately not serialized per scene (the engine stamps it every frame from
    // the graphics preset). Setting it here would assert a round-trip the format
    // does not promise.
    env.giSource = giSource;

    const auto make = [&](const char* n, const glm::vec3& p) {
        const entt::entity e = s.CreateEntity(n);
        Transform t;
        t.position = p;
        reg.emplace<Transform>(e, t);
        return e;
    };
    const entt::entity room = make("Room", {0.0f, 0.0f, 0.0f});
    const entt::entity lamp = make("Lamp", {0.0f, 2.0f, 0.0f});
    reg.emplace<Parent>(lamp, Parent{room});
    reg.emplace<PointLightComponent>(lamp, PointLightComponent{{1.0f, 0.9f, 0.7f}, 6.0f, 9.0f});
    const entt::entity outside = make("Blocker", {30.0f, 0.0f, 0.0f});
    reg.emplace<MeshInstance>(outside, MeshInstance{});
    reg.emplace<MeshRef>(outside, MeshRef{"prim:cube"});
    const entt::entity far1 = make("FarProp", {90.0f, 0.0f, 0.0f});
    reg.emplace<Parent>(far1, Parent{outside}); // the cross-slice link
    make("Marker", {-20.0f, 0.0f, 5.0f});
    SaveScene(s, file);
}

} // namespace

bool LightingParitySelfTest() {
    bool ok = true;
    const auto expect = [&ok](bool cond, const char* what) {
        if (!cond) {
            ok = false;
            HBE_ERROR("lightingparity: FAILED - {}", what);
        }
    };

    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "hbe_lightingparity";
    fs::remove_all(dir, ec);
    // A real scratch PROJECT, because ApplyEnvironment resolves giSource against
    // Project::Active().AssetsDir() - without one the `.hbgi` cases are untestable.
    if (!Project::Active().Create(dir, "LightingParity")) {
        HBE_ERROR("lightingparity: FAILED - could not create the scratch project");
        return false;
    }
    const fs::path assets = Project::Active().AssetsDir();

    // No GPU: a device-less Renderer uploads nothing, which is the same headless
    // contract the other serializer self-tests keep. LoadGIVolume still PARSES the
    // file in that state (that is why it no longer early-outs on the device), so
    // Loaded / Missing / Corrupt remain distinguishable here.
    Renderer renderer;

    const glm::ivec3 kGiDims{3, 4, 5};
    const glm::vec3 kGiOrigin{-7.5f, -1.25f, 3.0f};
    constexpr f32 kGiSpacing = 2.75f;
    expect(WriteScratchGi(assets / "GI" / "lit.hbgi", kGiDims, kGiOrigin, kGiSpacing),
           "write a scratch .hbgi");
    {   // A file that is present but is not a GI volume.
        fs::create_directories(assets / "GI", ec);
        std::ofstream bad{assets / "GI" / "bad.hbgi", std::ios::binary};
        bad << "this is not a baked irradiance volume";
    }

    const fs::path fLit = assets / "Scenes" / "Lit.hbscene";
    const fs::path fNoGi = assets / "Scenes" / "NoGi.hbscene";
    const fs::path fMissing = assets / "Scenes" / "MissingGi.hbscene";
    const fs::path fCorrupt = assets / "Scenes" / "CorruptGi.hbscene";
    fs::create_directories(assets / "Scenes", ec);
    AuthorParityScene(fLit, "GI/lit.hbgi");
    AuthorParityScene(fNoGi, "");
    AuthorParityScene(fMissing, "GI/nope.hbgi");
    AuthorParityScene(fCorrupt, "GI/bad.hbgi");

    // THE EDITOR PATH. Editor::LoadSceneInEditor does exactly this - the same
    // overload with the same DEFAULTED mode (which is LoadMode::Replace).
    const auto loadAsEditor = [&](Scene& s, const fs::path& p) {
        return LoadScene(s, renderer, p);
    };

    // THE RUNTIME PATH. stream::Streamer::BindLevel does exactly this: BindWorld
    // once for the level, then every shard's rows Additive. `reverse` loads the
    // slices back-to-front, which is legal (residency order is a function of where
    // the player is standing) and must not change the environment.
    const auto loadAsRuntime = [&](Scene& s, const fs::path& p, bool reverse) {
        SceneData d;
        if (!ParseSceneFile(p, d)) return false;
        BindWorld(s, renderer, d);
        std::vector<u32> a, b;
        for (u32 i = 0; i < static_cast<u32>(d.entities.size()); ++i)
            (i % 2 == 0 ? a : b).push_back(i);
        std::vector<const std::vector<u32>*> order{&a, &b};
        if (reverse) std::swap(order[0], order[1]);
        for (const std::vector<u32>* rows : order) {
            StagedAssets st;
            StageAssets(d, assets, st, rows->data(), static_cast<u32>(rows->size()));
            Instantiate(s, renderer, d, st, LoadMode::Additive, nullptr, {}, rows->data(),
                        static_cast<u32>(rows->size()));
        }
        return true;
    };

    // --- 1. The scene WITH a baked .hbgi -------------------------------------
    EnvironmentStamp litEditor, litRuntime;
    {
        Scene e, r;
        expect(loadAsEditor(e, fLit), "the editor path loads Lit.hbscene");
        expect(loadAsRuntime(r, fLit, false), "the runtime path loads Lit.hbscene");
        litEditor = StampOf(e.Environment());
        litRuntime = StampOf(r.Environment());
        if (litEditor != litRuntime)
            HBE_ERROR("lightingparity: stamps differ ->{}", DescribeStampDiff(litEditor, litRuntime));
        expect(litEditor == litRuntime,
               "WITH a baked GI volume, the editor path and the runtime path produce "
               "an IDENTICAL environment");
    }
    // ... and it is the AUTHORED environment, not two identically-wrong ones.
    expect(litEditor.ambientIntensity == 0.6337f && litEditor.exposure == 1.25f &&
               litEditor.shadowDistance == 1200.0f && litEditor.post.vignette == 0.91f &&
               litEditor.post.bloomEnabled == 0u && litEditor.post.ssgiEnabled == 0u &&
               litEditor.post.fogDensity == 0.037f,
           "both paths apply the FILE's authored header (not a default, and not the "
           "project's look)");
    expect(litEditor.giSource == "GI/lit.hbgi" && litEditor.giStatus == GiStatus::Loaded,
           "the baked volume named by giSource actually loaded");
    expect(litEditor.giDims == kGiDims && litEditor.giOrigin == kGiOrigin &&
               litEditor.giSpacing == glm::vec3(kGiSpacing),
           "the loaded volume IS the file that was written (grid round-trips)");

    // --- 2. The scene WITHOUT a baked .hbgi ----------------------------------
    {
        Scene e, r;
        expect(loadAsEditor(e, fNoGi), "the editor path loads NoGi.hbscene");
        expect(loadAsRuntime(r, fNoGi, false), "the runtime path loads NoGi.hbscene");
        const EnvironmentStamp se = StampOf(e.Environment()), sr = StampOf(r.Environment());
        if (se != sr)
            HBE_ERROR("lightingparity: stamps differ ->{}", DescribeStampDiff(se, sr));
        expect(se == sr,
               "WITHOUT a baked GI volume, the editor path and the runtime path produce "
               "an IDENTICAL environment");
        expect(se.giSource.empty() && se.giStatus == GiStatus::None && !se.giShValid &&
                   se.giDims == glm::ivec3(0),
               "no giSource = no volume, and the absence is REPORTED (None), not guessed");
        expect(se.ambientIntensity == litEditor.ambientIntensity,
               "the header is applied identically whether or not there is a GI volume");
    }

    // --- 3. Shard ORDER does not change the environment ----------------------
    {
        Scene fwd, rev;
        expect(loadAsRuntime(fwd, fLit, false) && loadAsRuntime(rev, fLit, true),
               "the runtime path loads the slices in both orders");
        expect(StampOf(fwd.Environment()) == StampOf(rev.Environment()),
               "loading the shards back-to-front leaves the same environment "
               "(residency order is where the player stands, not a look setting)");
    }

    // --- 4. An Additive load contributes NO environment ----------------------
    {
        Scene s;
        SceneData d;
        expect(ParseSceneFile(fLit, d), "parse Lit.hbscene for the additive probe");
        BindWorld(s, renderer, d);
        // Poke every field the header owns, then stream a shard in on top.
        SceneEnvironment& env = s.Environment();
        env.ambientIntensity = 9.0f;
        env.exposure = 9.0f;
        env.shadowDistance = 9.0f;
        env.post.vignette = 0.09f;
        env.giSource = "poked";
        env.giStatus = GiStatus::Corrupt;
        const EnvironmentStamp poked = StampOf(env);
        std::vector<u32> rows;
        for (u32 i = 0; i < static_cast<u32>(d.entities.size()); ++i) rows.push_back(i);
        StagedAssets st;
        StageAssets(d, assets, st, rows.data(), static_cast<u32>(rows.size()));
        Instantiate(s, renderer, d, st, LoadMode::Additive, nullptr, {}, rows.data(),
                    static_cast<u32>(rows.size()));
        expect(StampOf(s.Environment()) == poked,
               "an Additive load applies NO part of the environment - the open document "
               "owns the look, a streamed-in scene contributes entities only");
    }

    // --- 5. A GI failure is REPORTED and never INHERITED ----------------------
    // The regression this pins: ApplyEnvironment used to test `if (vol.valid)` with
    // no else, so scene B with a broken `.hbgi` kept scene A's volume bound while
    // advertising its own giSource. Lighting silently depended on which level had
    // been open before.
    for (int path = 0; path < 2; ++path) {
        const char* which = path == 0 ? "editor" : "runtime";
        Scene s;
        const auto load = [&](const fs::path& p) {
            return path == 0 ? loadAsEditor(s, p) : loadAsRuntime(s, p, false);
        };
        expect(load(fLit), "load the GOOD volume first");
        expect(s.Environment().giStatus == GiStatus::Loaded &&
                   s.Environment().giDims == kGiDims,
               "the good volume is bound before the failure case");
        expect(load(fMissing), "load a scene whose .hbgi does not exist");
        const EnvironmentStamp miss = StampOf(s.Environment());
        expect(miss.giStatus == GiStatus::Missing, "a missing .hbgi is reported as Missing");
        expect(!miss.giShValid && !miss.giDepthValid && miss.giDims == glm::ivec3(0),
               "a missing .hbgi CLEARS the volume - it does not inherit the previously "
               "loaded scene's GI");
        expect(miss.giSource == "GI/nope.hbgi",
               "giSource is still what the file said (the editor can show the broken path)");
        expect(load(fLit) && load(fCorrupt), "load a scene whose .hbgi is not a GI volume");
        const EnvironmentStamp bad = StampOf(s.Environment());
        expect(bad.giStatus == GiStatus::Corrupt, "a corrupt .hbgi is reported as Corrupt");
        expect(!bad.giShValid && bad.giDims == glm::ivec3(0),
               "a corrupt .hbgi CLEARS the volume too");
        expect(bad.ambientIntensity == 0.6337f && bad.exposure == 1.25f,
               "a GI failure does not stop the other four header fields from applying");
        if (!ok) HBE_ERROR("lightingparity: (the failures above are on the {} path)", which);
    }

    // --- 6. Both paths agree on the FAILURE cases too ------------------------
    for (const fs::path& p : {fMissing, fCorrupt}) {
        Scene e, r;
        expect(loadAsEditor(e, p) && loadAsRuntime(r, p, false),
               "both paths load the broken-GI scene");
        const EnvironmentStamp se = StampOf(e.Environment()), sr = StampOf(r.Environment());
        if (se != sr) HBE_ERROR("lightingparity: stamps differ ->{}", DescribeStampDiff(se, sr));
        expect(se == sr,
               "the editor and the runtime agree about a BROKEN GI volume, not just a "
               "working one");
    }

    // --- 7. Re-binding is idempotent -----------------------------------------
    {
        Scene s;
        expect(loadAsRuntime(s, fLit, false), "first bind");
        const EnvironmentStamp first = StampOf(s.Environment());
        expect(loadAsRuntime(s, fLit, false), "second bind");
        expect(StampOf(s.Environment()) == first,
               "a second bind re-applies the same environment (it does not accumulate)");
        expect(loadAsEditor(s, fLit) && StampOf(s.Environment()) == first,
               "and an editor load onto an already-bound world lands on the same value");
    }

    // --- 8. DAY/NIGHT MODULATES THE AUTHORED LOOK, IT DOES NOT REPLACE IT -----
    // The second, separate authoring bug: with `dynamicSky` on, UpdateDayNight
    // overwrote ambientIntensity (and MakeView overwrote exposure) with absolute
    // constants, so the authored numbers were dead data and the same room was a
    // different brightness depending on when you looked at it. The contract now is
    // PROPORTIONALITY: double the authored ambient, double what is rendered, at any
    // hour.
    {
        Scene s;
        expect(loadAsEditor(s, fLit), "load for the day/night check");
        Camera cam;
        cam.SetPerspective(60.0f, 1.0f, 0.1f, 1000.0f);
        cam.LookAt({0.0f, 2.0f, 10.0f}, {0.0f, 0.0f, 0.0f});
        SceneEnvironment& env = s.Environment();

        env.dynamicSky = 0;
        const rhi::SceneView off = s.MakeView(cam);
        expect(off.ambientIntensity == env.ambientIntensity && off.exposure == env.exposure,
               "with the dynamic sky OFF the authored ambient/exposure reach the view "
               "untouched");

        env.dynamicSky = 1;
        env.timeOfDay = 12.0f; // noon: the curve is 1.0, so authored passes through
        const rhi::SceneView noon = s.MakeView(cam);
        expect(std::abs(noon.ambientIntensity - env.ambientIntensity) < 1e-5f,
               "at NOON the day/night curve leaves the authored ambient alone "
               "(it used to force 1.0)");
        expect(std::abs(noon.exposure - env.exposure) < 1e-5f,
               "at NOON the day/night curve leaves the authored exposure alone");

        env.timeOfDay = 0.0f; // midnight
        const rhi::SceneView night = s.MakeView(cam);
        expect(night.ambientIntensity < noon.ambientIntensity,
               "midnight is darker than noon (the cycle still does something)");
        const f32 authored = env.ambientIntensity;
        const f32 nightAt1x = night.ambientIntensity;
        env.ambientIntensity = authored * 2.0f;
        const rhi::SceneView night2x = s.MakeView(cam);
        env.ambientIntensity = authored;
        expect(std::abs(night2x.ambientIntensity - nightAt1x * 2.0f) < 1e-4f,
               "the night curve is a MULTIPLIER on the authored ambient: doubling the "
               "authored value doubles the rendered one (it used to ignore it entirely)");
        const DayNight dn = EvalDayNight(12.0f);
        expect(dn.day > 0.99f && dn.tint == glm::vec3(1.0f),
               "the shared curve is identity at midday, so 'modulate' cannot quietly "
               "become 'replace'");
    }

    // --- 9. THE CLOCK IS PART OF THE HEADER (when the scene claims it) --------
    // The divergence the five header fields could not fix. The day/night clock
    // FREE-RUNS from process boot, so the editor and the shipped game are at
    // different hours the moment they have been open for different lengths of time -
    // at a 60-second day length, 30 seconds apart is 12 in-game hours apart, ~8x on
    // ambient. A scene may now pin it, and both load paths must agree about that.
    {
        const fs::path fClock = assets / "Scenes" / "Clock.hbscene";
        {   // Author an override: an INTERIOR that opts out of the cycle entirely.
            Scene s;
            SceneEnvironment& env = s.Environment();
            env.ambientIntensity = 0.6337f;
            env.exposure = 1.25f;
            env.dayNightAuthored = 1;
            env.dynamicSky = 0;
            env.timeOfDay = 3.5f;
            env.dayLengthSeconds = 900.0f;
            const entt::entity e = s.CreateEntity("Room");
            s.Registry().emplace<Transform>(e, Transform{});
            expect(SaveScene(s, fClock), "save a scene that claims the clock");
        }
        SceneData parsed;
        expect(ParseSceneFile(fClock, parsed) && parsed.hasDayNight &&
                   parsed.timeOfDay == 3.5f && parsed.dayLengthSeconds == 900.0f &&
                   parsed.dynamicSky == 0,
               "an authored day/night override round-trips through the .hbscene header");

        // Both paths, starting from a world whose clock says something ELSE - which is
        // what a free-running editor session actually looks like.
        Scene e, r;
        for (Scene* s : {&e, &r}) {
            SceneEnvironment& env = s->Environment();
            env.timeOfDay = 17.1f;
            env.dayLengthSeconds = 60.0f;
            env.dynamicSky = 1;
        }
        expect(loadAsEditor(e, fClock) && loadAsRuntime(r, fClock, false),
               "both paths load the clock-claiming scene");
        const EnvironmentStamp se = StampOf(e.Environment()), sr = StampOf(r.Environment());
        if (se != sr) HBE_ERROR("lightingparity: stamps differ ->{}", DescribeStampDiff(se, sr));
        expect(se == sr, "the editor and the runtime agree about the CLOCK, not just the "
                         "five look fields");
        expect(se.dayNightAuthored == 1 && se.dynamicSky == 0 && se.timeOfDay == 3.5f &&
                   se.dayLengthSeconds == 900.0f,
               "the scene's own clock replaces whatever the session was running");
        // And with the override on and the cycle off, the authored numbers are what
        // renders - the interior-authoring case, end to end.
        Camera cam;
        cam.SetPerspective(60.0f, 1.0f, 0.1f, 1000.0f);
        cam.LookAt({0.0f, 2.0f, 10.0f}, {0.0f, 0.0f, 0.0f});
        const rhi::SceneView v = e.MakeView(cam);
        expect(v.ambientIntensity == 0.6337f && v.exposure == 1.25f,
               "an interior that opts out of the cycle renders the EXACT authored "
               "ambient/exposure");

        // A scene that never claimed the clock leaves the session's alone (that is
        // what every pre-existing `.hbscene` does, and its file must not grow keys).
        Scene inherit;
        inherit.Environment().timeOfDay = 17.1f;
        inherit.Environment().dynamicSky = 1;
        expect(loadAsEditor(inherit, fLit), "load a scene with no clock override");
        expect(inherit.Environment().dayNightAuthored == 0 &&
                   inherit.Environment().timeOfDay == 17.1f &&
                   inherit.Environment().dynamicSky == 1,
               "a scene that authored no override inherits the project's clock, "
               "untouched");
        std::ifstream litText(fLit);
        const std::string litBytes((std::istreambuf_iterator<char>(litText)),
                                   std::istreambuf_iterator<char>());
        expect(litBytes.find("timeOfDay") == std::string::npos &&
                   litBytes.find("dynamicSky") == std::string::npos,
               "and its FILE is byte-for-byte what it was before the keys existed");
    }

    // --- 10. A SAVE MAY NOT GIVE ONE SCENE'S LOOK TO ANOTHER SCENE'S FILE -----
    // The streamed-scene save-back. An additive load applies no environment, so the
    // live environment belongs to the file the editor OPENED; writing it into every
    // file the editor has open handed each streamed level the active one's ambient,
    // exposure, post and - silently and worst - its giSource, which resolves and
    // lights the room with a volume baked for different geometry.
    {
        const fs::path fVictim = assets / "Scenes" / "Victim.hbscene";
        AuthorParityScene(fVictim, "GI/lit.hbgi");
        SceneData before;
        expect(ParseSceneFile(fVictim, before), "parse the streamed file's own header");

        // A live world whose environment says something completely different.
        Scene live;
        SceneEnvironment& lenv = live.Environment();
        lenv.ambientIntensity = 9.0f;
        lenv.exposure = 4.0f;
        lenv.shadowDistance = 12.0f;
        lenv.giSource = "GI/someone_elses.hbgi";
        lenv.dayNightAuthored = 1;
        lenv.timeOfDay = 23.0f;
        const entt::entity e = live.CreateEntity("Streamed");
        live.Registry().emplace<Transform>(e, Transform{});
        expect(SaveScene(live, fVictim, {}, SceneKind::Full, nullptr, &before),
               "save the streamed file back with ITS OWN header");

        SceneData after;
        expect(ParseSceneFile(fVictim, after), "re-parse the streamed file");
        expect(after.giSource == before.giSource && after.ambientIntensity ==
                                                        before.ambientIntensity &&
                   after.exposure == before.exposure &&
                   after.shadowDistance == before.shadowDistance &&
                   after.hasDayNight == before.hasDayNight,
               "the streamed file kept its OWN look - the active scene's did not "
               "overwrite it (giSource above all)");
        expect(after.entities.size() == 1,
               "and the entities written are still the live world's");

        // The default (no header handed in) is still the live environment - every
        // other caller depends on that.
        expect(SaveScene(live, fVictim), "save with no header override");
        SceneData plain;
        expect(ParseSceneFile(fVictim, plain) && plain.ambientIntensity == 9.0f &&
                   plain.giSource == "GI/someone_elses.hbgi" && plain.hasDayNight &&
                   plain.timeOfDay == 23.0f,
               "a save with no header override still writes the live environment");
    }

    // --- 11. THE PLAYER'S QUALITY PRESET DEGRADES THE VIEW, NOT THE FILE ------
    // The last editor-vs-ship lighting difference the stamp could not see. The
    // runtime used to stamp the preset onto the live `post` - one of the five
    // stamped fields - so the shipped game rendered a stack the editor could not
    // show, the authored flags were destroyed in memory, and a `.hbsave` written at
    // Medium recorded the degrade as authored data.
    {
        Scene s;
        expect(loadAsEditor(s, fLit), "load for the quality-preset check");
        SceneEnvironment& env = s.Environment();
        env.post.fogEnabled = 1;
        env.post.dofEnabled = 1;
        env.post.shadowCascades = 4;
        const EnvironmentStamp authored = StampOf(env);
        Camera cam;
        cam.SetPerspective(60.0f, 1.0f, 0.1f, 1000.0f);
        cam.LookAt({0.0f, 2.0f, 10.0f}, {0.0f, 0.0f, 0.0f});

        env.postQualityPreset = 0; // High = the authored look, exactly
        const rhi::SceneView high = s.MakeView(cam);
        expect(high.post.fogEnabled == 1 && high.post.dofEnabled == 1 &&
                   high.post.shadowCascades == 4,
               "High leaves the authored post untouched");

        env.postQualityPreset = 1; // what the shipped build runs by default
        const rhi::SceneView med = s.MakeView(cam);
        expect(med.post.fogEnabled == 0 && med.post.dofEnabled == 0 &&
                   med.post.ssgiEnabled == 0 && med.post.shadowCascades == 3,
               "Medium degrades the VIEW the way the shipped build always did");
        expect(StampOf(env) == authored,
               "...and the AUTHORED post is untouched by it - the preset may not "
               "mutate a stamped field (it did, every frame, in the runtime only)");

        env.forceShadowCascades = 4; // the --shadow-cascades A/B override
        expect(s.MakeView(cam).post.shadowCascades == 4,
               "an explicit cascade override still wins over the preset");
        env.forceShadowCascades = 0;
        env.postQualityPreset = 0;
    }

    fs::remove_all(dir, ec);
    return ok;
}

// --- --test-paintcanvas -------------------------------------------------------

bool MorphCacheSelfTest() {
    bool ok = true;
    const auto expect = [&ok](bool cond, const char* what) {
        if (!cond) {
            ok = false;
            HBE_ERROR("morphcache: FAILED - {}", what);
        }
    };

    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "hbe_morphcache";
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "Meshes", ec);
    Renderer renderer; // device-less: uploads return invalid handles (headless contract)

    // --- 0. `.uaf` v8 must actually carry blendshapes -------------------------
    // Before v8 the importer produced morph targets and WriteMesh dropped them, so
    // BuildMorphAtlas could never resolve from any asset on disk. Everything below
    // would be vacuous without this.
    const fs::path meshFile = dir / "Meshes" / "Head.uaf";
    constexpr u32 kVerts = 6;
    {
        MeshData md;
        md.name = "head";
        md.vertices.resize(kVerts);
        for (u32 i = 0; i < kVerts; ++i)
            md.vertices[i].position = glm::vec3(static_cast<f32>(i), 0.0f, 0.0f);
        md.indices = {0, 1, 2, 3, 4, 5};
        MorphTarget jaw;
        jaw.name = "jawOpen";
        jaw.posDelta.assign(kVerts, glm::vec3(0.0f, -1.0f, 0.0f));
        MorphTarget smile;
        smile.name = "smile";
        smile.posDelta.assign(kVerts, glm::vec3(0.25f, 0.0f, 0.0f));
        md.morphTargets = {jaw, smile};
        Model model{md};
        expect(uaf::WriteMesh(meshFile, model), "write a .uaf with two blendshapes");
        const std::optional<Model> back = uaf::ReadMesh(meshFile);
        expect(back && back->size() == 1 && (*back)[0].morphTargets.size() == 2,
               ".uaf v8 round-trips morph targets (pre-v8 dropped them silently)");
        if (back && !back->empty() && (*back)[0].morphTargets.size() == 2) {
            expect((*back)[0].morphTargets[0].name == "jawOpen" &&
                       (*back)[0].morphTargets[1].posDelta.size() == kVerts,
                   "the blendshape names and per-vertex deltas survive byte-for-byte");
        }
    }

    const std::string meshSource = "uaf:Meshes/Head.uaf#0";
    const std::string primSource = "prim:cube";

    // A scene with three morph entities on the same mesh: a plain one, one whose
    // Mesh collider forces the CPU model to be staged every time (B3's leak half),
    // and one on a primitive (which can never have blendshapes).
    SceneData data;
    {
        Scene s;
        auto& reg = s.Registry();
        const auto make = [&](const char* n, const std::string& src) {
            const entt::entity e = s.CreateEntity(n);
            reg.emplace<Transform>(e);
            reg.emplace<MeshInstance>(e, MeshInstance{});
            reg.emplace<MeshRef>(e, MeshRef{src});
            reg.emplace<MorphState>(e, MorphState{});
            return e;
        };
        make("Face", meshSource);
        const entt::entity collider = make("FaceCollider", meshSource);
        RigidBody rb;
        rb.shape = RigidBody::Shape::Mesh;
        reg.emplace<RigidBody>(collider, rb);
        make("Cube", primSource);
        const fs::path sceneFile = dir / "Morph.hbscene";
        expect(SaveScene(s, sceneFile), "save the morph test scene");
        expect(ParseSceneFile(sceneFile, data), "the morph test scene parses");
    }
    const auto rowNamed = [&](const char* n) -> const u32 {
        for (u32 i = 0; i < static_cast<u32>(data.entities.size()); ++i)
            if (data.entities[i].name == n) return i;
        return 0u;
    };
    const u32 rFace = rowNamed("Face"), rCollider = rowNamed("FaceCollider"),
              rCube = rowNamed("Cube");
    expect(data.entities.size() == 3 && data.entities[rFace].hasMorphState,
           "all three morph entities round-trip");

    ClearInstantiateCaches();
    expect(MorphAtlasBuildCount() == 0, "the atlas build counter starts (and resets) at zero");

    // THE PRECONDITION FOR BLOCKER B3, reproduced exactly: the GPU mesh is already
    // cache-resident (some other entity uploaded it, or an earlier spawn did), so
    // StageAssets has every reason to skip loading the CPU model. Headless uploads
    // return invalid handles, so the cache is primed by hand here - the mesh handle
    // is never sampled, only tested for validity.
    CachePutMesh(meshSource, rhi::MeshHandle{1u}, AABB{glm::vec3(-1.0f), glm::vec3(1.0f)}, "");

    const auto spawn = [&](u32 row, Scene& into, StagedAssets& staged) {
        const u32 idx = row;
        StageAssets(data, dir, staged, &idx, 1);
        Instantiate(into, renderer, data, staged, LoadMode::Additive, nullptr, {}, &idx, 1);
    };

    // --- 1. FIRST spawn: the model is force-staged despite the resident mesh ---
    Scene world;
    {
        StagedAssets staged;
        spawn(rFace, world, staged);
        expect(staged.models.contains("Meshes/Head.uaf"),
               "a morph entity whose mesh is already GPU-resident STILL gets its CPU model "
               "staged (once) - otherwise there is nothing to build an atlas from");
        expect(MorphAtlasBuildCount() == 1, "exactly one atlas was built");
        const entt::entity e = world.FindByName("Face");
        const MorphState* ms = world.Registry().try_get<MorphState>(e);
        expect(ms && ms->targetNames.size() == 2 && ms->vertexCount == kVerts,
               "the first spawn resolves both blendshape channels");
        if (ms && ms->targetNames.size() == 2)
            expect(ms->targetNames[0] == "jawOpen" && ms->targetNames[1] == "smile",
                   "in atlas row order");
    }

    // --- 2. SECOND spawn: blendshapes survive, and NOTHING is rebuilt ---------
    // This is the regression B3 describes. Before the fix, StageAssets skipped the
    // model (the mesh is cached), BuildMorphAtlas read from an empty staged.models,
    // and the entity came back with no channels at all - facial animation silently
    // dead for the rest of the session.
    {
        StagedAssets staged;
        spawn(rFace, world, staged);
        expect(!staged.models.contains("Meshes/Head.uaf"),
               "the second spawn does NOT re-read the model (the atlas is cached now)");
        expect(MorphAtlasBuildCount() == 1,
               "no second atlas is built or uploaded - there is no texture release in the "
               "RHI, so a rebuild per respawn is a permanent leak");
        u32 resolved = 0;
        for (const entt::entity e : world.Registry().view<MorphState>()) {
            const MorphState& ms = world.Registry().get<MorphState>(e);
            if (ms.targetNames.size() == 2 && ms.vertexCount == kVerts) ++resolved;
        }
        expect(resolved == 2, "BOTH spawns of the face have their blendshape channels");
    }

    // --- 3. The mesh-collider path stops re-minting an atlas per respawn ------
    // Here StageAssets always loads the model (a Mesh collider needs CPU geometry),
    // so the old code called UploadTexture again on every single spawn.
    {
        for (int i = 0; i < 3; ++i) {
            StagedAssets staged;
            spawn(rCollider, world, staged);
            expect(staged.models.contains("Meshes/Head.uaf"),
                   "a Mesh collider still stages its CPU geometry every spawn");
        }
        expect(MorphAtlasBuildCount() == 1,
               "three collider respawns still share ONE atlas (the leak is closed)");
    }

    // --- 4. A mesh with no blendshapes caches its negative -------------------
    {
        StagedAssets staged;
        spawn(rCube, world, staged);
        expect(MorphAtlasBuildCount() == 1, "a primitive builds no atlas");
        expect(CacheHasMorph(primSource),
               "and the 'no blendshapes' answer is CACHED, so it is not re-derived for "
               "every instance of every prop");
        const MorphState* ms = world.Registry().try_get<MorphState>(world.FindByName("Cube"));
        expect(ms && ms->targetNames.empty(), "a primitive resolves no channels");
    }

    // --- 5. ClearInstantiateCaches drops the atlas cache too -----------------
    ClearInstantiateCaches();
    expect(!CacheHasMorph(meshSource) && MorphAtlasBuildCount() == 0,
           "ClearInstantiateCaches (asset reimport / project switch) clears the atlas "
           "cache and the counter with it");

    fs::remove_all(dir, ec);
    if (ok)
        HBE_INFO("morphcache: .uaf v8 carries blendshapes, a respawned entity keeps its "
                 "channels without re-reading the model, no atlas is ever built twice "
                 "(including on the mesh-collider path), and a morph-less mesh caches its "
                 "negative.");
    return ok;
}

bool PaintCanvasSelfTest() {
    // SavePaintCanvases is the reason a .hbscene can reference paint at all: it
    // assigns each canvas its `source`, and BuildSceneJson silently SKIPS any
    // PaintComponent whose source is still empty. A regression here loses an
    // artist's painting with no error - so it is worth a headless proof.
    namespace fs = std::filesystem;
    bool ok = true;
    const auto expect = [&ok](bool cond, const char* what) {
        if (!cond) {
            ok = false;
            HBE_ERROR("paintcanvas: FAILED - {}", what);
        }
    };
    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / "hbe_paintcanvas";
    fs::remove_all(root, ec);
    if (!Project::Active().Create(root / "P", "P")) {
        HBE_ERROR("paintcanvas: FAILED - cannot create scratch project");
        return false;
    }
    const fs::path assets = Project::Active().AssetsDir();

    Scene s;
    // Two objects share a name (routine: imported meshes do, and a duplicated
    // object clears its inherited source), one is unique, one already has a
    // source that must be left alone.
    for (const char* n : {"Plane", "Plane", "Cube"}) {
        const entt::entity e = s.CreateEntity(n);
        paint::EnsureCanvas(s.Registry().emplace<PaintComponent>(e), 64);
    }
    {
        const entt::entity e = s.CreateEntity("Preset");
        PaintComponent& pc = s.Registry().emplace<PaintComponent>(e);
        paint::EnsureCanvas(pc, 64);
        pc.source = "Paint/Explicit.hbpaint";
    }

    SavePaintCanvases(s, assets, "Zone");

    std::vector<std::string> sources;
    for (const entt::entity e : s.Registry().view<PaintComponent>())
        sources.push_back(s.Registry().get<PaintComponent>(e).source);
    expect(sources.size() == 4, "all four canvases are visited");
    bool allNamed = true, allOnDisk = true, allUnderPaint = true;
    for (const std::string& src : sources) {
        if (src.empty()) { allNamed = false; continue; }
        if (src.rfind("Paint/", 0) != 0 ||
            !src.ends_with(".hbpaint"))
            allUnderPaint = false;
        if (!fs::exists(assets / src, ec)) allOnDisk = false;
    }
    expect(allNamed, "every canvas gets a source (an unnamed one is dropped on save)");
    expect(allUnderPaint, "sources are Paint/<name>.hbpaint, relative to Assets/");
    expect(allOnDisk, "every assigned source exists on disk");
    std::unordered_set<std::string> uniq(sources.begin(), sources.end());
    expect(uniq.size() == sources.size(),
           "two objects with the SAME name get DIFFERENT files (no overwrite)");
    expect(uniq.contains("Paint/Explicit.hbpaint"),
           "an already-assigned source is never reassigned");
    expect(uniq.contains("Paint/Zone_Cube.hbpaint"),
           "a new source is derived from the scene stem + the entity name");

    // A written canvas reloads (the scene's Stage phase does exactly this).
    PaintComponent back;
    expect(paint::Load(assets / "Paint/Zone_Cube.hbpaint", back) && back.resolution == 64 &&
               !back.layers.empty(),
           "a written canvas reloads with its resolution + layers");

    // Idempotent: a second save must not rename anything.
    const std::vector<std::string> before = sources;
    SavePaintCanvases(s, assets, "Zone");
    std::vector<std::string> after;
    for (const entt::entity e : s.Registry().view<PaintComponent>())
        after.push_back(s.Registry().get<PaintComponent>(e).source);
    expect(before == after, "saving twice does not renumber any canvas");

    fs::remove_all(root, ec);
    return ok;
}

// ===========================================================================
// PER-COMPONENT DELTAS (see SceneSerializer.h)
// ===========================================================================

namespace {

// One row per supported component: how to READ it back out of JSON and onto a live
// entity, and how to REMOVE it. The write side needs no entry - it is EntityToJson.
//
// This table IS the coverage. A key absent from it is refused by ApplyComponentJson,
// which is why adding a component is a visible, deliberate act rather than something
// that half-works.
struct DeltaApplier {
    const char* key;
    // `v` is the sub-object EntityToJson wrote for this key. Return false for a shape
    // this build does not understand - never a partially applied component.
    bool (*apply)(entt::registry& reg, entt::entity e, const json& v);
    void (*remove)(entt::registry& reg, entt::entity e);
};

bool ApplyNameComp(entt::registry& reg, entt::entity e, const json& v) {
    if (!v.is_string()) return false;
    reg.emplace_or_replace<Name>(e, Name{v.get<std::string>()});
    return true;
}
void RemoveNameComp(entt::registry& reg, entt::entity e) { reg.remove<Name>(e); }

glm::vec3 DeltaVec3(const json& a, const glm::vec3& def) {
    if (!a.is_array() || a.size() != 3) return def;
    return glm::vec3(a[0].get<f32>(), a[1].get<f32>(), a[2].get<f32>());
}

bool ApplyTransformComp(entt::registry& reg, entt::entity e, const json& v) {
    if (!v.is_object()) return false;
    Transform t = reg.all_of<Transform>(e) ? reg.get<Transform>(e) : Transform{};
    // Each field is optional so a sender may omit an unchanged one. A MISSING field
    // keeps what is already there rather than snapping to a default - resetting it
    // would teleport an object because the sender was being economical.
    if (const auto it = v.find("p"); it != v.end()) t.position = DeltaVec3(*it, t.position);
    if (const auto it = v.find("s"); it != v.end()) t.scale = DeltaVec3(*it, t.scale);
    if (const auto it = v.find("r"); it != v.end() && it->is_array() && it->size() == 4) {
        // THE ORDER IS w,x,y,z - matching ToJson(const glm::quat&), which writes
        // {q.w, q.x, q.y, q.z}. It is NOT the x,y,z,w most formats use, and assuming
        // the common convention here produced a rotation that looked almost right;
        // nobody investigates "slightly off", so the round-trip test is what caught it.
        // Read it through the SAME accessor order the writer used and it cannot drift.
        t.rotation = glm::quat((*it)[0].get<f32>(), (*it)[1].get<f32>(),
                               (*it)[2].get<f32>(), (*it)[3].get<f32>());
    }
    reg.emplace_or_replace<Transform>(e, t);
    return true;
}
void RemoveTransformComp(entt::registry& reg, entt::entity e) { reg.remove<Transform>(e); }

// Runs ONE component's json through THE SCENE FILE'S OWN READER.
//
// The obvious alternative - hand-writing a small parser per row, right here - was
// rejected. It would be a SECOND reader for the same bytes, and the moment the two
// drifted a value would mean one thing when saved and a different thing when sent to a
// colleague. Nobody would think to look for that: both paths would work in isolation.
// The cost is building a one-entity scene document per applied delta, which happens at
// human editing rates, not per frame per entity.
bool ParseOneComponent(const char* key, const json& v, EntityData& out) {
    json ent = json::object();
    ent[key] = v;
    json root = json::object();
    root["entities"] = json::array({ent});
    SceneData sd;
    ParseSceneJson(root, sd);
    if (sd.entities.size() != 1) return false;
    out = std::move(sd.entities[0]);
    return true;
}

// A component that Instantiate applies as a single `reg.emplace<T>(e, d.field)` and
// nothing else. That is the exact condition for being safe here: no renderer, no staged
// assets, no post-load fix-up, nothing to intern on the main thread.
//
// Components that fail that test are deliberately ABSENT rather than approximated -
// `rigidBody` rebuilds collider geometry and resets a physics body id, `mesh` and `paint`
// need staged assets, `tag` interns into a shared table, `characterRig` spawns parts
// after load. Applying those as a plain copy would produce an entity that looks right in
// the inspector and behaves wrong, which is worse than not supporting them: an unknown
// key is REFUSED by ApplyComponentJson and the caller is told.
#define HBE_PLAIN_DELTA(FN, KEY, HAS, VAL, TYPE)                                         \
    bool FN##Apply(entt::registry& reg, entt::entity e, const json& v) {                 \
        EntityData d;                                                                    \
        if (!ParseOneComponent(KEY, v, d) || !d.HAS) return false;                        \
        reg.emplace_or_replace<TYPE>(e, d.VAL);                                          \
        return true;                                                                     \
    }                                                                                    \
    void FN##Remove(entt::registry& reg, entt::entity e) { reg.remove<TYPE>(e); }

HBE_PLAIN_DELTA(DirLight, "light", hasLight, light, DirectionalLightComponent)
HBE_PLAIN_DELTA(PointLight, "pointLight", hasPointLight, pointLight, PointLightComponent)
HBE_PLAIN_DELTA(Vol, "volume", hasVolume, volume, VolumeComponent)
HBE_PLAIN_DELTA(SpotLight, "spotLight", hasSpotLight, spotLight, SpotLightComponent)
HBE_PLAIN_DELTA(RectLight, "rectLight", hasRectLight, rectLight, RectLightComponent)
HBE_PLAIN_DELTA(Cam, "camera", hasCamera, camera, CameraComponent)
HBE_PLAIN_DELTA(CamZone, "cameraZone", hasCameraZone, cameraZone, CameraZone)
HBE_PLAIN_DELTA(CamSpline, "cameraSpline", hasCameraSpline, cameraSpline, CameraSpline)
HBE_PLAIN_DELTA(MusZone, "musicZone", hasMusicZone, musicZone, MusicZone)
HBE_PLAIN_DELTA(PostVol, "postVolume", hasPostVolume, postVolume, PostVolume)
HBE_PLAIN_DELTA(Interact, "interactable", hasInteractable, interactable, Interactable)
HBE_PLAIN_DELTA(Trig, "trigger", hasTrigger, trigger, TriggerVolume)
HBE_PLAIN_DELTA(DlgActor, "dialogueActor", hasDialogueActor, dialogueActor, DialogueActor)
HBE_PLAIN_DELTA(Chk, "checkpoint", hasCheckpoint, checkpoint, Checkpoint)
HBE_PLAIN_DELTA(Hp, "health", hasHealth, health, Health)
HBE_PLAIN_DELTA(Wpn, "weapon", hasWeapon, weapon, Weapon)
HBE_PLAIN_DELTA(AiPerc, "aiPerception", hasAIPerception, aiPerception, AIPerception)
HBE_PLAIN_DELTA(AiBehav, "aiBehavior", hasAIBehavior, aiBehavior, AIBehavior)
HBE_PLAIN_DELTA(Spwn, "spawner", hasSpawner, spawner, Spawner)
HBE_PLAIN_DELTA(Enc, "encounter", hasEncounter, encounter, Encounter)
HBE_PLAIN_DELTA(NavAg, "navAgent", hasNavAgent, navAgent, NavigationAgent)
HBE_PLAIN_DELTA(NavObs, "navObstacle", hasNavObstacle, navObstacle, NavigationObstacle)
HBE_PLAIN_DELTA(NavIn, "navmeshInput", hasNavmeshInput, navmeshInput, NavmeshInput)
HBE_PLAIN_DELTA(Rot, "rotator", hasRotator, rotator, Rotator)
HBE_PLAIN_DELTA(ModelGrp, "modelGroup", hasModelGroup, modelGroup, ModelGroup)
HBE_PLAIN_DELTA(Cens, "censor", hasCensor, censor, CensorComponent)
HBE_PLAIN_DELTA(CharCtl, "character", hasCharacter, character, CharacterController)
HBE_PLAIN_DELTA(Ik, "ik", hasIK, ik, IKConstraint)
HBE_PLAIN_DELTA(WText, "worldText", hasWorldText, worldText, WorldText)
HBE_PLAIN_DELTA(MotMat, "motionMatching", hasMotionMatching, motionMatching, MotionMatching)
HBE_PLAIN_DELTA(Parts, "particles", hasParticles, particles, ParticleEmitter)

#undef HBE_PLAIN_DELTA

const DeltaApplier kAppliers[] = {
    {"name", &ApplyNameComp, &RemoveNameComp},
    {"transform", &ApplyTransformComp, &RemoveTransformComp},
    {"light", &DirLightApply, &DirLightRemove},
    {"pointLight", &PointLightApply, &PointLightRemove},
    {"volume", &VolApply, &VolRemove},
    {"spotLight", &SpotLightApply, &SpotLightRemove},
    {"rectLight", &RectLightApply, &RectLightRemove},
    {"camera", &CamApply, &CamRemove},
    {"cameraZone", &CamZoneApply, &CamZoneRemove},
    {"cameraSpline", &CamSplineApply, &CamSplineRemove},
    {"musicZone", &MusZoneApply, &MusZoneRemove},
    {"postVolume", &PostVolApply, &PostVolRemove},
    {"interactable", &InteractApply, &InteractRemove},
    {"trigger", &TrigApply, &TrigRemove},
    {"dialogueActor", &DlgActorApply, &DlgActorRemove},
    {"checkpoint", &ChkApply, &ChkRemove},
    {"health", &HpApply, &HpRemove},
    {"weapon", &WpnApply, &WpnRemove},
    {"aiPerception", &AiPercApply, &AiPercRemove},
    {"aiBehavior", &AiBehavApply, &AiBehavRemove},
    {"spawner", &SpwnApply, &SpwnRemove},
    {"encounter", &EncApply, &EncRemove},
    {"navAgent", &NavAgApply, &NavAgRemove},
    {"navObstacle", &NavObsApply, &NavObsRemove},
    {"navmeshInput", &NavInApply, &NavInRemove},
    {"rotator", &RotApply, &RotRemove},
    {"modelGroup", &ModelGrpApply, &ModelGrpRemove},
    {"censor", &CensApply, &CensRemove},
    {"character", &CharCtlApply, &CharCtlRemove},
    {"ik", &IkApply, &IkRemove},
    {"worldText", &WTextApply, &WTextRemove},
    {"motionMatching", &MotMatApply, &MotMatRemove},
    {"particles", &PartsApply, &PartsRemove},
};

const DeltaApplier* FindApplier(const std::string& key) {
    for (const DeltaApplier& a : kAppliers)
        if (key == a.key) return &a;
    return nullptr;
}

} // namespace

const char* DeltaApplyName(DeltaApply r) {
    switch (r) {
        case DeltaApply::Applied: return "Applied";
        case DeltaApply::Removed: return "Removed";
        case DeltaApply::UnknownKey: return "UnknownKey";
        case DeltaApply::BadJson: return "BadJson";
        case DeltaApply::NoEntity: return "NoEntity";
    }
    return "?";
}

const std::vector<std::string>& DeltaComponentKeys() {
    static const std::vector<std::string> keys = [] {
        std::vector<std::string> k;
        for (const DeltaApplier& a : kAppliers) k.emplace_back(a.key);
        return k;
    }();
    return keys;
}

bool IsDeltaComponent(const std::string& key) { return FindApplier(key) != nullptr; }

bool ComponentToJson(const Scene& scene, entt::entity e, const std::string& key,
                     std::string& outJson) {
    outJson.clear();
    const entt::registry& reg = scene.Registry();
    if (!reg.valid(e)) return false;
    if (!IsDeltaComponent(key)) return false;
    // REUSE THE SAVE WRITER. Extracting one key from EntityToJson's output is what
    // guarantees a delta and a save can never disagree about a component's shape - a
    // second hand-written writer would drift the first time either one changed.
    static const std::unordered_map<u32, int> kNoParents;
    const json je = EntityToJson(reg, e, kNoParents, false);
    const auto it = je.find(key);
    if (it == je.end()) return false; // the entity does not have this component
    outJson = it->dump();
    return true;
}

DeltaApply ApplyComponentJson(Scene& scene, entt::entity e, const std::string& key,
                              const std::string& jsonText) {
    entt::registry& reg = scene.Registry();
    if (!reg.valid(e)) return DeltaApply::NoEntity;
    const DeltaApplier* a = FindApplier(key);
    // REFUSED, not ignored. A silent no-op is a divergence with no symptom: the sender
    // believes the edit landed and every other machine never saw it.
    if (!a) return DeltaApply::UnknownKey;

    if (jsonText.empty()) {
        a->remove(reg, e);
        return DeltaApply::Removed;
    }
    const json v = json::parse(jsonText, nullptr, false);
    if (v.is_discarded()) return DeltaApply::BadJson;
    return a->apply(reg, e, v) ? DeltaApply::Applied : DeltaApply::BadJson;
}

bool ComponentDeltaSelfTest() {
    int fails = 0;
    const auto check = [&fails](bool c, const char* what) {
        if (c) return;
        ++fails;
        std::printf("componentdelta FAIL: %s\n", what);
    };

    Scene s;
    const entt::entity src = s.CreateEntity("Source");
    const entt::entity dst = s.CreateEntity("Dest");
    entt::registry& reg = s.Registry();

    Transform t;
    t.position = {1.5f, -2.25f, 3.75f};
    t.scale = {2.0f, 0.5f, 1.25f};
    t.rotation = glm::normalize(glm::quat(0.3f, 0.1f, -0.5f, 0.8f));
    reg.emplace_or_replace<Transform>(src, t);

    // Every registered key must round-trip onto a DIFFERENT entity and land identical.
    // The second entity is the point - this is the cross-machine case, and a test that
    // applied back onto the source would pass even if apply did nothing at all.
    for (const std::string& key : DeltaComponentKeys()) {
        std::string payload;
        if (!ComponentToJson(s, src, key, payload)) continue; // src lacks it: fine
        check(!payload.empty(), "a serialized component was empty");
        const DeltaApply r = ApplyComponentJson(s, dst, key, payload);
        check(r == DeltaApply::Applied, "a registered component did not apply");
        std::string back;
        check(ComponentToJson(s, dst, key, back), "the applied component did not read back");
        check(back == payload, "a component did not survive the round trip byte-for-byte");
    }

    // The transform specifically - the thing an artist actually moves - must be
    // numerically right, not merely byte-equal to something that is also wrong.
    {
        check(reg.all_of<Transform>(dst), "the transform did not reach the destination");
        const Transform& g = reg.get<Transform>(dst);
        check(glm::length(g.position - t.position) < 1e-4f, "position did not survive");
        check(glm::length(g.scale - t.scale) < 1e-4f, "scale did not survive");
        check(std::fabs(glm::dot(g.rotation, t.rotation)) > 0.999f, "rotation did not survive");
    }

    // A partial transform must MERGE, not reset the omitted fields to defaults.
    {
        const DeltaApply r = ApplyComponentJson(s, dst, "transform", "{\"p\":[9.0,9.0,9.0]}");
        check(r == DeltaApply::Applied, "a partial transform should apply");
        const Transform& g = reg.get<Transform>(dst);
        check(glm::length(g.position - glm::vec3(9.0f)) < 1e-4f, "the partial position applied");
        check(glm::length(g.scale - t.scale) < 1e-4f,
              "an OMITTED field was reset instead of preserved - that teleports objects");
    }

    // Name round-trip, then removal.
    {
        check(ApplyComponentJson(s, dst, "name", "\"Renamed\"") == DeltaApply::Applied,
              "a name delta should apply");
        check(reg.get<Name>(dst).value == "Renamed", "the name did not change");
        check(ApplyComponentJson(s, dst, "name", "") == DeltaApply::Removed,
              "an empty payload should REMOVE the component");
        check(!reg.all_of<Name>(dst), "the component was not actually removed");
    }

    // THE REFUSAL. An unsupported key must be reported, never silently dropped.
    check(ApplyComponentJson(s, dst, "rigidBody", "{}") == DeltaApply::UnknownKey,
          "an unregistered component key must be REFUSED, not ignored");
    check(!IsDeltaComponent("rigidBody"), "rigidBody is not in the delta table yet");
    check(IsDeltaComponent("transform"), "transform must be in the delta table");
    check(ApplyComponentJson(s, dst, "transform", "{{{not json") == DeltaApply::BadJson,
          "malformed JSON must be reported as BadJson");
    check(ApplyComponentJson(s, dst, "transform", "[1,2,3]") == DeltaApply::BadJson,
          "a wrong-SHAPE payload must be reported, not partially applied");
    check(ApplyComponentJson(s, entt::null, "transform", "{}") == DeltaApply::NoEntity,
          "an invalid entity must be reported");
    {
        std::string sink;
        check(!ComponentToJson(s, src, "nosuchcomponent", sink),
              "an unknown key must not serialize");
    }

    if (fails == 0) {
        std::printf("componentdelta: %zu key(s) round-trip onto a DIFFERENT entity "
                    "byte-for-byte; omitted fields merge rather than reset; removal "
                    "works; unknown keys and bad payloads are REFUSED\n",
                    DeltaComponentKeys().size());
    }
    return fails == 0;
}

} // namespace hbe::scene
