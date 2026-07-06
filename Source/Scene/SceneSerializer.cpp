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
#include "Scene/PaintSystem.h"
#include "Scene/PostSettingsSerialization.h"
#include "Scene/Scene.h"
#include "UI/UISystem.h" // PreloadUIAssets (eager UI font/texture load)

#include <nlohmann/json.hpp>

#include <cctype>
#include <fstream>
#include <functional>
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
                {"collisionPadding", c.collisionPadding}};
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

namespace {
json EntityToJson(const entt::registry& reg, entt::entity e,
                  const std::unordered_map<u32, int>& indexOf, bool runtimeTags = false);

// Builds the .hbscene JSON document (shared by file saves and snapshots). When
// `include` is set, only entities it accepts are written (used to save each
// loaded scene back to its own file - the active scene vs. streamed-in ones).
json BuildSceneJson(const Scene& scene,
                    const std::function<bool(entt::entity)>& include = {},
                    SceneKind kind = SceneKind::Full, bool runtimeTags = false) {
    const auto& reg = scene.Registry();

    // Stable entity -> index mapping for parent links. Entities are gathered
    // from every component type the serializer writes (anything with none of
    // them would round-trip to nothing anyway).
    std::vector<entt::entity> order;
    std::unordered_map<u32, int> indexOf;
    const auto add = [&](entt::entity e) {
        // Terrain chunks are runtime-generated from a TerrainComponent; never
        // serialize them (they're rebuilt on load). Same for world-UI surface
        // quads (rebuilt from their UICanvas by ui::UpdateWorldSurfaces).
        if (reg.try_get<TerrainChunk>(e)) return;
        if (reg.try_get<UISurface>(e)) return;
        // Dialogue Choice buttons are transient UI the conversation player spawns
        // at runtime; a checkpoint/quicksave landing mid-Choice must NOT bake them
        // into the .hbsave (they'd reload as permanent, tag-less, un-clearable UI).
        if (reg.try_get<DialogueChoiceButton>(e)) return;
        if (reg.all_of<InteractPromptTag>(e)) return; // transient interaction prompt UI (empty tag)
        if (include && !include(e)) return; // not part of the scene being saved
        const u32 key = static_cast<u32>(e);
        if (indexOf.find(key) == indexOf.end()) {
            indexOf[key] = static_cast<int>(order.size());
            order.push_back(e);
        }
    };
    for (const entt::entity e : reg.view<const Transform>()) add(e);
    for (const entt::entity e : reg.view<const Name>()) add(e);
    for (const entt::entity e : reg.view<const MeshInstance>()) add(e);
    for (const entt::entity e : reg.view<const DirectionalLightComponent>()) add(e);
    for (const entt::entity e : reg.view<const PointLightComponent>()) add(e);
    for (const entt::entity e : reg.view<const SpotLightComponent>()) add(e);
    for (const entt::entity e : reg.view<const RectLightComponent>()) add(e);
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
    for (const entt::entity e : reg.view<const CensorComponent>()) add(e);
    for (const entt::entity e : reg.view<const IKConstraint>()) add(e);
    for (const entt::entity e : reg.view<const UIElement>()) add(e);
    for (const entt::entity e : reg.view<const UICanvas>()) add(e);
    for (const entt::entity e : reg.view<const NavigationAgent>()) add(e);
    for (const entt::entity e : reg.view<const NavigationObstacle>()) add(e);
    for (const entt::entity e : reg.view<const NavmeshInput>()) add(e);
    for (const entt::entity e : reg.view<const PostVolume>()) add(e);
    for (const entt::entity e : reg.view<const ReflectionProbe>()) add(e);
    for (const entt::entity e : reg.view<const ParticleEmitter>()) add(e);
    for (const entt::entity e : reg.view<const SchematicComponent>()) add(e);
    for (const entt::entity e : reg.view<const Checkpoint>()) add(e);
    for (const entt::entity e : reg.view<const Interactable>()) add(e);
    for (const entt::entity e : reg.view<const TriggerVolume>()) add(e);

    json root;
    root["version"] = 1;
    if (kind != SceneKind::Full) root["kind"] = ToString(kind);
    root["ambientIntensity"] = scene.Environment().ambientIntensity;
    root["exposure"] = scene.Environment().exposure;
    root["shadowDistance"] = scene.Environment().shadowDistance;
    root["post"] = PostToJson(scene.Environment().post);
    if (!scene.Environment().giSource.empty())
        root["giSource"] = scene.Environment().giSource;

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
        if (const Name* n = reg.try_get<Name>(e)) je["name"] = n->value;
        // Prefab-instance link (root entity of a placed .hbprefab).
        if (const PrefabInstance* pi = reg.try_get<PrefabInstance>(e); pi && !pi->source.empty())
            je["prefab"] = pi->source;
        // Editor-only visibility (hidden but loaded). Runtime ignores it.
        if (reg.all_of<EditorHidden>(e)) je["editorHidden"] = true;
        if (const Transform* t = reg.try_get<Transform>(e)) {
            je["transform"] = {{"p", ToJson(t->position)},
                               {"r", ToJson(t->rotation)},
                               {"s", ToJson(t->scale)}};
        }
        if (const Parent* p = reg.try_get<Parent>(e)) {
            if (auto it = indexOf.find(static_cast<u32>(p->entity)); it != indexOf.end()) {
                je["parent"] = it->second;
            }
        }
        if (const MeshInstance* mi = reg.try_get<MeshInstance>(e)) {
            const MeshRef* ref = reg.try_get<MeshRef>(e);
            je["mesh"] = {{"source", ref ? ref->source : std::string()},
                          {"baseColor", ToJson(mi->baseColor)},
                          {"metallic", mi->metallic},
                          {"roughness", mi->roughness},
                          {"flags", mi->materialFlags},
                          {"subsurfaceColor", ToJson(mi->subsurfaceColor)},
                          {"subsurfaceRadius", mi->subsurfaceRadius},
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
        if (const Checkpoint* cp = reg.try_get<Checkpoint>(e)) {
            je["checkpoint"] = {{"id", cp->id},
                                {"setObjective", cp->setObjective},
                                {"completesObjective", cp->completesObjective},
                                {"halfExtents", ToJson(cp->halfExtents)},
                                {"triggerOnEnter", cp->triggerOnEnter},
                                {"saveOnReach", cp->saveOnReach},
                                {"once", cp->once}};
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
                                  {"requiredFlag", ia->requiredFlag}};
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
                             {"requiredFlag", tv->requiredFlag}};
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
                chains.push_back({{"endJoint", ch.endJoint},
                                  {"target", ToJson(ch.target)},
                                  {"pole", ToJson(ch.pole)},
                                  {"hasPole", ch.hasPole},
                                  {"weight", ch.weight},
                                  {"enabled", ch.enabled},
                                  {"targetEntity", ch.targetEntity}});
            }
            je["ik"] = {{"chains", std::move(chains)}};
        }
        if (const UIElement* el = reg.try_get<UIElement>(e)) {
            je["ui"] = {{"type", static_cast<int>(el->type)},
                        {"text", el->text},
                        {"anchorMin", json::array({el->anchorMin.x, el->anchorMin.y})},
                        {"anchorMax", json::array({el->anchorMax.x, el->anchorMax.y})},
                        {"pivot", json::array({el->pivot.x, el->pivot.y})},
                        {"offset", json::array({el->offset.x, el->offset.y})},
                        {"size", json::array({el->size.x, el->size.y})},
                        {"color", ToJson(el->color)},
                        {"textSize", el->textSize},
                        {"hAlign", static_cast<int>(el->hAlign)},
                        {"vAlign", static_cast<int>(el->vAlign)},
                        {"visible", el->visible},
                        {"texture", el->texture},
                        {"fill", el->fill},
                        {"fillColor", ToJson(el->fillColor)},
                        {"radial", el->radial},
                        {"fullscreen", el->fullscreen},
                        {"action", el->action},
                        {"font", el->font},
                        {"rotation", el->rotation},
                        {"scale", json::array({el->scale.x, el->scale.y})},
                        {"value", el->value},
                        {"toggled", el->toggled},
                        {"selected", el->selected},
                        {"options", el->options},
                        {"frames", el->frames},
                        {"contentSize", json::array({el->contentSize.x, el->contentSize.y})},
                        {"scrollPos", json::array({el->scrollPos.x, el->scrollPos.y})},
                        {"scrollSpeed", el->scrollSpeed},
                        {"scrollVertical", el->scrollVertical},
                        {"scrollHorizontal", el->scrollHorizontal},
                        {"autoScroll", el->autoScroll},
                        {"autoScrollLoop", el->autoScrollLoop},
                        {"placeholder", el->placeholder},
                        {"maxLength", el->maxLength},
                        {"hoverColor", ToJson(el->hoverColor)},
                        {"pressedColor", ToJson(el->pressedColor)},
                        {"disabledColor", ToJson(el->disabledColor)},
                        {"enabled", el->enabled},
                        {"hoverSound", el->hoverSound},
                        {"clickSound", el->clickSound},
                        {"trackTexture", el->trackTexture},
                        {"fillTexture", el->fillTexture},
                        {"handleTexture", el->handleTexture},
                        {"handleSize", el->handleSize},
                        {"onTexture", el->onTexture},
                        {"offTexture", el->offTexture},
                        {"hoverTexture", el->hoverTexture},
                        {"pressedTexture", el->pressedTexture},
                        {"disabledTexture", el->disabledTexture},
                        {"cellTexture", el->cellTexture},
                        {"slice", ToJson(el->slice)},
                        {"wrap", el->wrap}};
        }
        if (const UICanvas* canvas = reg.try_get<UICanvas>(e)) {
            je["uiCanvas"] = {{"scaleMode", canvas->scaleMode},
                              {"refWidth", canvas->refWidth},
                              {"refHeight", canvas->refHeight},
                              {"sortOrder", canvas->sortOrder},
                              {"visible", canvas->visible},
                              {"worldSpace", canvas->worldSpace},
                              {"worldWidth", canvas->worldWidth},
                              {"emissive", canvas->emissive},
                              {"rtWidth", canvas->rtWidth},
                              {"rtHeight", canvas->rtHeight}};
        }
        if (const UIAnimator* an = reg.try_get<UIAnimator>(e)) {
            je["uiAnimator"] = {{"clip", an->clip}, {"trigger", static_cast<int>(an->trigger)}};
        }
        if (const UIPanel* p = reg.try_get<UIPanel>(e)) {
            je["uiPanel"] = {{"name", p->name}, {"startVisible", p->startVisible}};
        }
        if (const UILayoutGroup* lg = reg.try_get<UILayoutGroup>(e)) {
            je["uiLayoutGroup"] = {{"kind", static_cast<int>(lg->kind)},
                                   {"spacing", lg->spacing},
                                   {"cellSize", json::array({lg->cellSize.x, lg->cellSize.y})},
                                   {"padding", ToJson(lg->padding)},
                                   {"columns", lg->columns},
                                   {"fitContent", lg->fitContent}};
        }
        if (const UICanvasGroup* cg = reg.try_get<UICanvasGroup>(e)) {
            je["uiCanvasGroup"] = {{"opacity", cg->opacity},
                                   {"interactable", cg->interactable}};
        }
        if (const WorldText* wt = reg.try_get<WorldText>(e)) {
            je["worldText"] = {{"text", wt->text},
                               {"size", wt->size},
                               {"color", json::array({wt->color.r, wt->color.g,
                                                      wt->color.b, wt->color.a})},
                               {"font", wt->font},
                               {"billboard", wt->billboard}};
        }
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
                // True volumetric VFX (all optional; omit == billboard-only defaults).
                {"volumetric", pe->volumetric}, {"volDensity", pe->volDensity},
                {"volRadiusScale", pe->volRadiusScale}, {"volTemperature", pe->volTemperature},
                {"volEmission", pe->volEmission}, {"volExtinction", pe->volExtinction},
                {"volSteps", pe->volSteps}, {"volResolution", pe->volResolution},
                {"volDetail", pe->volDetail}};
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
        if (const Animator* an = reg.try_get<Animator>(e)) {
            je["animator"] = {{"source", an->sourceAsset},
                              {"clip", an->clip},
                              {"speed", an->speed},
                              {"loop", an->loop},
                              {"playing", an->playing},
                              {"rootMotion", an->rootMotion}};
        }
        if (const AnimationTrack* a = reg.try_get<AnimationTrack>(e)) {
            json keys = json::array();
            for (const auto& k : a->keys) {
                keys.push_back({{"t", k.time},
                                {"p", ToJson(k.position)},
                                {"r", ToJson(k.rotation)},
                                {"s", ToJson(k.scale)}});
            }
            je["animation"] = {{"duration", a->duration},
                               {"speed", a->speed},
                               {"loop", a->loop},
                               {"playing", a->playing},
                               {"keys", std::move(keys)}};
        }
        // Per-object Static/Dynamic layer is authored INTO the one scene file (the
        // editor works in a single scene; the build splits .static/.dynamic from
        // these tags). Always written on disk now, not just snapshots.
        if (const SceneLayer* sl = reg.try_get<SceneLayer>(e))
            je["sceneLayer"] = ToString(sl->kind);
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
    while (!stack.empty()) {
        const entt::entity e = stack.back();
        stack.pop_back();
        if (reg.try_get<TerrainChunk>(e)) continue; // runtime-generated chunks
        if (reg.try_get<UISurface>(e)) continue;    // runtime world-UI quads
        const u32 key = static_cast<u32>(e);
        if (indexOf.count(key)) continue;
        indexOf[key] = static_cast<int>(order.size());
        order.push_back(e);
        // Queue children (any entity parented to this one).
        for (const entt::entity c : reg.view<const Parent>()) {
            const Parent* pp = reg.try_get<Parent>(c);
            if (pp && static_cast<u32>(pp->entity) == key) stack.push_back(c);
        }
    }
    for (const entt::entity e : order) {
        json je = EntityToJson(reg, e, indexOf);
        // Preserve each entity's level layer (Static/Dynamic) so copy/paste, duplicate
        // and prefab instancing keep the same layer instead of being re-auto-classified
        // on paste. (sceneSrc is deliberately NOT copied - a clone belongs to whatever
        // scene it is pasted into, but it should stay on the layer it was authored on.)
        if (const SceneLayer* sl = reg.try_get<SceneLayer>(e))
            je["sceneLayer"] = ToString(sl->kind);
        arr.push_back(std::move(je));
    }
    return doc;
}

// Fills SceneData from a parsed .hbscene JSON document.
void ParseSceneJson(const json& root, SceneData& out) {
    out.kind = SceneKindFromString(root.value("kind", std::string("full")));
    out.ambientIntensity = root.value("ambientIntensity", 1.0f);
    out.giSource = root.value("giSource", std::string());
    out.exposure = root.value("exposure", 1.0f);
    out.shadowDistance = root.value("shadowDistance", 150.0f);
    if (const auto it = root.find("post"); it != root.end() && it->is_object()) {
        PostFromJson(*it, out.post); // out.post starts at defaults (effects on)
    }

    for (const json& je : root.value("entities", json::array())) {
        EntityData d;
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
        if (auto it = je.find("interactable"); it != je.end()) {
            d.hasInteractable = true;
            Interactable& ia = d.interactable;
            ia.action = static_cast<InteractAction>(glm::clamp(
                it->value("action", 0u), 0u, static_cast<u32>(InteractAction::None)));
            ia.prompt = it->value("prompt", "Interact");
            ia.asset = it->value("asset", "");
            ia.flag = it->value("flag", "");
            ia.flagValue = it->value("flagValue", 1.0f);
            ia.text = it->value("text", "");
            ia.range = it->value("range", 2.5f);
            ia.once = it->value("once", false);
            ia.requiredFlag = it->value("requiredFlag", "");
            ia.fired = it->value("fired", false); // persists "once" state across saves
        }
        if (auto it = je.find("trigger"); it != je.end()) {
            d.hasTrigger = true;
            TriggerVolume& tv = d.trigger;
            tv.action = static_cast<InteractAction>(glm::clamp(
                it->value("action", 0u), 0u, static_cast<u32>(InteractAction::None)));
            tv.asset = it->value("asset", "");
            tv.flag = it->value("flag", "");
            tv.flagValue = it->value("flagValue", 1.0f);
            tv.text = it->value("text", "");
            tv.halfExtents = Vec3(it->value("halfExtents", json{}), glm::vec3(2.0f));
            tv.once = it->value("once", true);
            tv.requiredFlag = it->value("requiredFlag", "");
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
        if (auto it = je.find("censor"); it != je.end()) {
            d.hasCensor = true;
            CensorComponent& ce = d.censor;
            ce.radius = it->value("radius", ce.radius);
            ce.feather = it->value("feather", ce.feather);
            ce.strength = it->value("strength", ce.strength);
            ce.offset = Vec3(it->value("offset", json()), ce.offset);
            ce.enabled = it->value("enabled", ce.enabled);
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
        if (auto it = je.find("ui"); it != je.end()) {
            d.hasUI = true;
            const int type = it->value("type", 1);
            d.uiElement.type = static_cast<UIElement::Type>(glm::clamp(type, 0, 9));
            d.uiElement.text = it->value("text", "");
            const auto vec2Of = [&](const char* key, glm::vec2 def) {
                const json arr = it->value(key, json::array());
                if (arr.is_array() && arr.size() >= 2) {
                    return glm::vec2{arr[0].get<f32>(), arr[1].get<f32>()};
                }
                return def;
            };
            // v2 scenes stored one "anchor" point: both anchors collapse to it.
            const glm::vec2 legacyAnchor = vec2Of("anchor", {0.5f, 0.5f});
            d.uiElement.anchorMin = vec2Of("anchorMin", legacyAnchor);
            d.uiElement.anchorMax = vec2Of("anchorMax", legacyAnchor);
            d.uiElement.pivot = vec2Of("pivot", {0.5f, 0.5f});
            const json offset = it->value("offset", json::array());
            if (offset.is_array() && offset.size() >= 2) {
                d.uiElement.offset = {offset[0].get<f32>(), offset[1].get<f32>()};
            }
            const json size = it->value("size", json::array());
            if (size.is_array() && size.size() >= 2) {
                d.uiElement.size = {size[0].get<f32>(), size[1].get<f32>()};
            }
            d.uiElement.color = Vec4(it->value("color", json()), glm::vec4(1.0f));
            // v1 scenes stored a multiplier ("textScale"); v2 stores canvas px.
            d.uiElement.textSize = it->contains("textSize")
                                       ? it->value("textSize", 28.0f)
                                       : it->value("textScale", 1.0f) * 28.0f;
            d.uiElement.hAlign = static_cast<UIElement::HAlign>(
                glm::clamp(it->value("hAlign", 1), 0, 2));
            d.uiElement.vAlign = static_cast<UIElement::VAlign>(
                glm::clamp(it->value("vAlign", 1), 0, 2));
            d.uiElement.visible = it->value("visible", true);
            d.uiElement.texture = it->value("texture", "");
            d.uiElement.fill = it->value("fill", 0.65f);
            d.uiElement.fillColor = Vec4(it->value("fillColor", json()),
                                         {0.86f, 0.27f, 0.33f, 1.0f});
            d.uiElement.radial = it->value("radial", false);
            d.uiElement.fullscreen = it->value("fullscreen", false);
            d.uiElement.action = it->value("action", "");
            d.uiElement.font = it->value("font", "");
            d.uiElement.rotation = it->value("rotation", 0.0f);
            d.uiElement.scale = vec2Of("scale", {1.0f, 1.0f});
            d.uiElement.value = it->value("value", 0.5f);
            d.uiElement.toggled = it->value("toggled", false);
            d.uiElement.selected = it->value("selected", 0);
            d.uiElement.options = it->value("options", std::vector<std::string>{});
            d.uiElement.frames = it->value("frames", std::vector<std::string>{});
            d.uiElement.contentSize = glm::max(vec2Of("contentSize", {0.0f, 0.0f}),
                                               glm::vec2(0.0f));
            d.uiElement.scrollPos = vec2Of("scrollPos", {0.0f, 0.0f});
            d.uiElement.scrollSpeed =
                glm::clamp(it->value("scrollSpeed", 40.0f), 1.0f, 4000.0f);
            d.uiElement.scrollVertical = it->value("scrollVertical", true);
            d.uiElement.scrollHorizontal = it->value("scrollHorizontal", false);
            d.uiElement.autoScroll =
                glm::clamp(it->value("autoScroll", 0.0f), -4000.0f, 4000.0f);
            d.uiElement.autoScrollLoop = it->value("autoScrollLoop", false);
            d.uiElement.placeholder = it->value("placeholder", "");
            d.uiElement.maxLength = glm::clamp(it->value("maxLength", 64), 1, 4096);
            d.uiElement.hoverColor = Vec4(it->value("hoverColor", json()), glm::vec4(0.0f));
            d.uiElement.pressedColor =
                Vec4(it->value("pressedColor", json()), glm::vec4(0.0f));
            d.uiElement.disabledColor =
                Vec4(it->value("disabledColor", json()), glm::vec4(0.0f));
            d.uiElement.enabled = it->value("enabled", true);
            d.uiElement.hoverSound = it->value("hoverSound", "");
            d.uiElement.clickSound = it->value("clickSound", "");
            d.uiElement.trackTexture = it->value("trackTexture", "");
            d.uiElement.fillTexture = it->value("fillTexture", "");
            d.uiElement.handleTexture = it->value("handleTexture", "");
            d.uiElement.handleSize = glm::max(it->value("handleSize", 0.0f), 0.0f);
            d.uiElement.onTexture = it->value("onTexture", "");
            d.uiElement.offTexture = it->value("offTexture", "");
            d.uiElement.hoverTexture = it->value("hoverTexture", "");
            d.uiElement.pressedTexture = it->value("pressedTexture", "");
            d.uiElement.disabledTexture = it->value("disabledTexture", "");
            d.uiElement.cellTexture = it->value("cellTexture", "");
            d.uiElement.slice = glm::max(Vec4(it->value("slice", json()), glm::vec4(0.0f)),
                                         glm::vec4(0.0f));
            d.uiElement.wrap = it->value("wrap", false);
        }
        if (auto it = je.find("uiAnimator"); it != je.end()) {
            d.hasUIAnimator = true;
            d.uiAnimator.clip = it->value("clip", "");
            d.uiAnimator.trigger = static_cast<UIAnimator::Trigger>(
                glm::clamp(it->value("trigger", 1), 0, 5));
        }
        if (auto it = je.find("uiPanel"); it != je.end()) {
            d.hasUIPanel = true;
            d.uiPanel.name = it->value("name", "");
            d.uiPanel.startVisible = it->value("startVisible", false);
        }
        if (auto it = je.find("uiLayoutGroup"); it != je.end()) {
            d.hasUILayoutGroup = true;
            UILayoutGroup& lg = d.uiLayoutGroup;
            lg.kind = static_cast<UILayoutGroup::Kind>(
                glm::clamp(it->value("kind", 0), 0, 2));
            lg.spacing = it->value("spacing", 8.0f);
            if (const json cs = it->value("cellSize", json());
                cs.is_array() && cs.size() >= 2) {
                lg.cellSize = {cs[0].get<f32>(), cs[1].get<f32>()};
            }
            lg.padding = Vec4(it->value("padding", json()), glm::vec4(0.0f));
            lg.columns = glm::max(it->value("columns", 1), 1);
            lg.fitContent = it->value("fitContent", false);
        }
        if (auto it = je.find("uiCanvasGroup"); it != je.end()) {
            d.hasUICanvasGroup = true;
            d.uiCanvasGroup.opacity = glm::clamp(it->value("opacity", 1.0f), 0.0f, 1.0f);
            d.uiCanvasGroup.interactable = it->value("interactable", true);
        }
        if (auto it = je.find("worldText"); it != je.end()) {
            d.hasWorldText = true;
            d.worldText.text = it->value("text", "Text");
            d.worldText.size = glm::clamp(it->value("size", 0.25f), 0.001f, 100.0f);
            d.worldText.color = Vec4(it->value("color", json()), glm::vec4(1.0f));
            d.worldText.font = it->value("font", "");
            d.worldText.billboard = it->value("billboard", false);
        }
        if (auto it = je.find("uiCanvas"); it != je.end()) {
            d.hasUICanvas = true;
            d.uiCanvas.scaleMode = it->value("scaleMode", 1u);
            d.uiCanvas.refWidth = it->value("refWidth", 1920.0f);
            d.uiCanvas.refHeight = it->value("refHeight", 1080.0f);
            d.uiCanvas.sortOrder = it->value("sortOrder", 0);
            d.uiCanvas.visible = it->value("visible", true);
            d.uiCanvas.worldSpace = it->value("worldSpace", false);
            d.uiCanvas.worldWidth = glm::clamp(it->value("worldWidth", 1.0f), 0.01f, 1000.0f);
            d.uiCanvas.emissive = glm::clamp(it->value("emissive", 0.0f), 0.0f, 10.0f);
            const u32 rw = it->value("rtWidth", 0u);
            const u32 rh = it->value("rtHeight", 0u);
            d.uiCanvas.rtWidth = rw ? glm::clamp(rw, 64u, 4096u) : 0u;  // 0 = ref size
            d.uiCanvas.rtHeight = rh ? glm::clamp(rh, 64u, 4096u) : 0u;
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
            p.volumetric = it->value("volumetric", p.volumetric);
            p.volDensity = it->value("volDensity", p.volDensity);
            p.volRadiusScale = it->value("volRadiusScale", p.volRadiusScale);
            p.volTemperature = it->value("volTemperature", p.volTemperature);
            p.volEmission = it->value("volEmission", p.volEmission);
            p.volExtinction = it->value("volExtinction", p.volExtinction);
            p.volSteps = glm::clamp(it->value("volSteps", p.volSteps), 4, 256);
            p.volResolution = glm::clamp(it->value("volResolution", p.volResolution), 32, 192);
            p.volDetail = glm::clamp(it->value("volDetail", p.volDetail), 0.0f, 1.0f);
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
        // Runtime tags written only into in-memory snapshots (see SaveSceneToString).
        if (auto it = je.find("sceneSrc"); it != je.end() && it->is_string()) {
            d.hasSceneSourceTag = true;
            d.sceneSourceTag = it->get<std::string>();
        }
        if (auto it = je.find("sceneLayer"); it != je.end() && it->is_string()) {
            d.hasSceneLayerTag = true;
            d.sceneLayerKind = SceneKindFromString(it->get<std::string>());
        }
        if (auto it = je.find("animator"); it != je.end()) {
            d.hasAnimator = true;
            d.animator.sourceAsset = it->value("source", "");
            d.animator.clip = it->value("clip", 0);
            d.animator.speed = it->value("speed", 1.0f);
            d.animator.loop = it->value("loop", true);
            d.animator.playing = it->value("playing", true);
            d.animator.rootMotion = it->value("rootMotion", false);
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
                d.anim.keys.push_back(k);
            }
        }
        out.entities.push_back(std::move(d));
    }
}
} // namespace

bool SaveScene(const Scene& scene, const fs::path& path,
               const std::function<bool(entt::entity)>& include, SceneKind kind) {
    const json root = BuildSceneJson(scene, include, kind);

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path);
    if (!out) {
        HBE_ERROR("Scene: cannot write '{}'.", path.string());
        return false;
    }
    out << root.dump(2);
    HBE_INFO("Scene: saved {} entities to '{}'.", root["entities"].size(), path.string());
    return true;
}

bool SplitSceneFile(const fs::path& src, const fs::path& staticOut, const fs::path& dynamicOut) {
    // The editor authors ONE merged scene; the build splits it into the two layer
    // files the runtime's level loader expects. Done at the JSON level (no GPU /
    // instantiate): partition entities by their "sceneLayer" tag (default static) and
    // REMAP parent indices per partition. A tagged subtree shares one layer (the
    // Inspector toggle tags whole subtrees), so a parent is always in the same file;
    // any cross-layer parent gracefully becomes a root. Returns true only when the
    // source actually had BOTH layers (otherwise it's a plain scene, no split needed).
    std::ifstream in(src, std::ios::binary);
    if (!in) return false;
    json doc;
    try {
        in >> doc;
    } catch (...) {
        return false;
    }
    if (!doc.contains("entities") || !doc["entities"].is_array()) return false;
    const json& ents = doc["entities"];
    const auto isDynamic = [](const json& e) {
        return e.value("sceneLayer", std::string("static")) == "dynamic";
    };
    int nStatic = 0, nDynamic = 0;
    for (const json& e : ents) (isDynamic(e) ? nDynamic : nStatic)++;
    if (nStatic == 0 || nDynamic == 0) return false; // single-layer -> not a level

    const auto buildLayer = [&](bool wantDynamic) -> json {
        json out = doc;
        out["kind"] = wantDynamic ? "dynamic" : "static";
        std::unordered_map<int, int> remap; // old index -> new index in this partition
        std::vector<int> keep;
        for (int i = 0; i < static_cast<int>(ents.size()); ++i)
            if (isDynamic(ents[static_cast<usize>(i)]) == wantDynamic) {
                remap[i] = static_cast<int>(keep.size());
                keep.push_back(i);
            }
        json arr = json::array();
        for (int oldIdx : keep) {
            json e = ents[static_cast<usize>(oldIdx)];
            if (auto it = e.find("parent"); it != e.end() && it->is_number_integer()) {
                const int p = it->get<int>();
                const auto rit = remap.find(p);
                *it = (rit != remap.end()) ? rit->second : -1; // cross-layer parent -> root
            }
            arr.push_back(std::move(e));
        }
        out["entities"] = std::move(arr);
        return out;
    };

    std::error_code ec;
    fs::create_directories(staticOut.parent_path(), ec);
    const auto writeJson = [](const fs::path& p, const json& d) {
        std::ofstream o(p, std::ios::binary | std::ios::trunc);
        if (!o) return false;
        o << d.dump(2);
        return static_cast<bool>(o);
    };
    bool ok = writeJson(staticOut, buildLayer(false));
    ok = writeJson(dynamicOut, buildLayer(true)) && ok;
    return ok;
}

void SavePaintCanvases(Scene& scene, const fs::path& assetsDir,
                       const std::string& sceneStem) {
    auto& reg = scene.Registry();
    std::unordered_set<std::string> used;
    int counter = 0;
    const auto sanitize = [](std::string s) {
        for (char& c : s)
            if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
        return s.empty() ? std::string("paint") : s;
    };
    for (const entt::entity e : reg.view<PaintComponent>()) {
        PaintComponent& pc = reg.get<PaintComponent>(e);
        if (pc.layers.empty()) continue;
        if (pc.source.empty()) {
            const std::string stem = sceneStem.empty() ? "scene" : sceneStem;
            const Name* nm = reg.try_get<Name>(e);
            const std::string base =
                stem + "_" +
                sanitize(nm && !nm->value.empty() ? nm->value
                                                  : ("paint" + std::to_string(counter++)));
            std::string candidate = "Paint/" + base + ".hbpaint";
            for (int n = 1; used.contains(candidate); ++n)
                candidate = "Paint/" + base + "_" + std::to_string(n) + ".hbpaint";
            pc.source = candidate;
        }
        used.insert(pc.source);
        if (!paint::Save(assetsDir / pc.source, pc))
            HBE_WARN("Scene: failed to write paint canvas '{}'.", pc.source);
    }
}

std::string SaveSceneToString(const Scene& scene) {
    // Snapshots (play mode, undo/redo) keep the per-entity SceneSource/SceneLayer
    // tags so a Replace restore preserves level grouping instead of dropping it.
    return BuildSceneJson(scene, {}, SceneKind::Full, /*runtimeTags*/ true).dump();
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
    json root;
    try {
        root = json::parse(bytes->begin(), bytes->end());
    } catch (const std::exception& e) {
        HBE_ERROR("Scene: failed to parse '{}': {}", path.string(), e.what());
        return false;
    }
    ParseSceneJson(root, out);
    return true;
}

bool ParseSceneString(const std::string& text, SceneData& out) {
    json root;
    try {
        root = json::parse(text);
    } catch (const std::exception& e) {
        HBE_ERROR("Scene: failed to parse snapshot: {}", e.what());
        return false;
    }
    ParseSceneJson(root, out);
    return true;
}

// --- Persistent GPU caches -------------------------------------------------------
namespace {
struct InstantiateCaches {
    std::unordered_map<std::string, rhi::MeshHandle> mesh; // full "uaf:rel#n"/"prim:*"
    std::unordered_map<std::string, rhi::TextureHandle> textures; // Assets-rel name
    std::unordered_map<std::string, AABB> bounds;
    std::unordered_map<std::string, std::string> submeshMat;
};
InstantiateCaches& Caches() {
    static InstantiateCaches c;
    return c;
}

// The caches are read by StageAssets (which runs on worker threads, e.g.
// SceneStreamer and StreamingWorld) and written by Instantiate (main thread),
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
} // namespace

void ClearInstantiateCaches() {
    {
        std::lock_guard<std::mutex> lk(CachesMutex());
        Caches() = InstantiateCaches{};
    }
    anim::ClearRigCache(); // rigs are loaded from the same assets
}

void StageAssets(const SceneData& data, const fs::path& assetsDir, StagedAssets& out) {
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

    for (const EntityData& d : data.entities) {
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

    for (const EntityData& d : data.entities) {
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
        if (!needsCollisionMesh) {
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
    for (const EntityData& d : data.entities) {
        if (!d.hasTerrain) continue;
        for (const std::string& mat : d.terrain.splatLayerSrc) stageMaterial(mat);
    }
}

void Instantiate(Scene& scene, Renderer& renderer, const SceneData& data,
                 StagedAssets& staged, LoadMode mode,
                 std::vector<entt::entity>* createdOut, const std::string& sceneTag) {
    auto& reg = scene.Registry();
    if (mode == LoadMode::Replace) {
        // Destroy everything EXCEPT entities tagged Persistent (the resident UI layer),
        // so a persistent UI scene survives gameplay scene swaps. With no Persistent
        // entities present this is equivalent to reg.clear() (the common case). Collect
        // first, then destroy - can't mutate the registry mid-iteration.
        {
            std::vector<entt::entity> kill;
            for (const entt::entity e : reg.storage<entt::entity>())
                if (!reg.all_of<Persistent>(e)) kill.push_back(e);
            for (const entt::entity e : kill)
                if (reg.valid(e)) reg.destroy(e);
        }
        scene.Environment().ambientIntensity = data.ambientIntensity;
        scene.Environment().exposure = data.exposure;
        scene.Environment().shadowDistance = data.shadowDistance;
        scene.Environment().post = data.post;
        // Load the cached GI volume (.hbgi) so baked GI lights the scene without a
        // re-bake (falls back to no volume if the cache is missing).
        scene.Environment().giSource = data.giSource;
        if (!data.giSource.empty()) {
            const GiVolume vol = LoadGIVolume(renderer, Project::Active().AssetsDir() / data.giSource);
            if (vol.valid) {
                SceneEnvironment& env = scene.Environment();
                env.giSh = vol.sh;
                env.giDepth = vol.depth;
                env.giOrigin = vol.origin;
                env.giSpacing = vol.spacing;
                env.giDims = vol.dims;
            }
        }
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

    std::vector<entt::entity> created(data.entities.size(), entt::entity{entt::null});
    for (usize i = 0; i < data.entities.size(); ++i) {
        const EntityData& d = data.entities[i];
        const entt::entity e = scene.CreateEntity(d.name);
        created[i] = e;
        if (!sceneTag.empty()) reg.emplace<SceneSource>(e, SceneSource{sceneTag});
        // Stamp the level layer from the file's kind (Full = standalone scene,
        // left untagged so legacy/whole-scene behaviour is unchanged).
        if (data.kind != SceneKind::Full) reg.emplace<SceneLayer>(e, SceneLayer{data.kind});
        // Per-entity runtime tags from an in-memory snapshot win over the global
        // ones, so a play/undo restore puts every entity back in its own scene +
        // level layer instead of collapsing the grouping.
        if (d.hasSceneSourceTag) reg.emplace_or_replace<SceneSource>(e, SceneSource{d.sceneSourceTag});
        if (d.hasSceneLayerTag) reg.emplace_or_replace<SceneLayer>(e, SceneLayer{d.sceneLayerKind});
        if (d.editorHidden) reg.emplace<EditorHidden>(e); // editor-only visibility
        if (!d.prefabSource.empty())
            reg.emplace<PrefabInstance>(e, PrefabInstance{d.prefabSource}); // linked prefab root

        if (d.hasTransform) reg.emplace<Transform>(e, d.transform);

        if (d.hasMesh && !d.meshSource.empty()) {
            MeshInstance mi;
            mi.baseColor = d.baseColor;
            mi.metallic = d.metallic;
            mi.roughness = d.roughness;
            mi.materialFlags = d.materialFlags;
            mi.subsurfaceColor = d.subsurfaceColor;
            mi.subsurfaceRadius = d.subsurfaceRadius;
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

            if (mi.mesh.IsValid()) {
                CachePutMesh(d.meshSource, mi.mesh, bounds, submeshMaterial);
                reg.emplace<MeshInstance>(e, mi);
                reg.emplace<MeshRef>(e, MeshRef{d.meshSource});
                if (!materialRef.empty() && staged.materials.contains(materialRef)) {
                    reg.emplace<MaterialRef>(e, MaterialRef{materialRef});
                }
                reg.emplace<AABB>(e, d.hasAABB ? d.aabb : bounds);
            }
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
        if (d.hasSpotLight) reg.emplace<SpotLightComponent>(e, d.spotLight);
        if (d.hasRectLight) reg.emplace<RectLightComponent>(e, d.rectLight);
        if (d.hasSchematic) {
            SchematicComponent sg;
            sg.asset = d.schematicAsset;
            reg.emplace<SchematicComponent>(e, std::move(sg));
        }
        if (d.hasCheckpoint) reg.emplace<Checkpoint>(e, d.checkpoint);
        if (d.hasInteractable) reg.emplace<Interactable>(e, d.interactable);
        if (d.hasTrigger) reg.emplace<TriggerVolume>(e, d.trigger);
        if (d.hasCamera) reg.emplace<CameraComponent>(e, d.camera);
        if (d.hasCameraZone) reg.emplace<CameraZone>(e, d.cameraZone);
        if (d.hasMusicZone) reg.emplace<MusicZone>(e, d.musicZone);
        if (d.hasCameraSpline) reg.emplace<CameraSpline>(e, d.cameraSpline);
        if (d.hasPaint && !d.paintSource.empty()) {
            if (const auto pit = staged.paints.find(d.paintSource); pit != staged.paints.end()) {
                PaintComponent pc;          // file holds resolution + layer stack;
                pc.resolution = pit->second.resolution; // metadata comes from the scene
                pc.layers = pit->second.layers;
                pc.activeLayer = pit->second.activeLayer;
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
                PaintComponent& placed = reg.emplace<PaintComponent>(e, std::move(pc));
                paint::Sync(renderer, placed); // upload canvas + mips
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
    for (usize i = 0; i < data.entities.size(); ++i) {
        const int p = data.entities[i].parent;
        if (p >= 0 && p < static_cast<int>(created.size()) && created[p] != entt::null) {
            reg.emplace<Parent>(created[i], Parent{created[p]});
        }
    }

    if (createdOut) {
        createdOut->clear();
        createdOut->reserve(created.size());
        for (const entt::entity e : created)
            if (e != entt::null) createdOut->push_back(e);
    }

    // Eager UI asset preload: bake fonts + load every UI texture NOW (scene
    // load) instead of lazily on first draw - kills the blank-text/white-quad
    // first frame and the disk-I/O hitch inside the frame loop.
    bool anyUI = false;
    for (const EntityData& d : data.entities) {
        if (d.hasUI || d.hasWorldText) {
            anyUI = true;
            break;
        }
    }
    if (anyUI && Project::HasActive()) {
        ui::PreloadUIAssets(scene, renderer, Project::Active().AssetsDir());
    }

    HBE_INFO("Scene: instantiated {} entities ({}).", data.entities.size(),
             mode == LoadMode::Replace ? "replace" : "additive");
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

// --- Levels ------------------------------------------------------------------

fs::path LevelPaths::Member(SceneKind kind) const {
    if (kind == SceneKind::Full || base.empty()) return {};
    fs::path p = base;
    p += ".";
    p += ToString(kind); // "static" / "dynamic" / "ui"
    p += ".hbscene";
    return p;
}

bool IsLevelMember(const fs::path& p) {
    // Only static/dynamic are level layers; UI scenes (menu/HUD) are standalone.
    if (p.extension() != ".hbscene") return false;
    const std::string stem = p.stem().string(); // e.g. "Foo.static"
    const auto dot = stem.find_last_of('.');
    if (dot == std::string::npos) return false;
    const std::string suf = stem.substr(dot + 1);
    return suf == "static" || suf == "dynamic";
}

LevelPaths ResolveLevel(const fs::path& memberOrBase) {
    LevelPaths lp;
    fs::path p = memberOrBase;
    if (IsLevelMember(p)) {
        std::string stem = p.stem().string();        // "Foo.static"
        stem = stem.substr(0, stem.find_last_of('.')); // -> "Foo"
        lp.base = p.parent_path() / stem;
    } else {
        if (p.extension() == ".hbscene") p = p.parent_path() / p.stem();
        lp.base = p;
    }
    return lp;
}

bool LoadLevel(Scene& scene, Renderer& renderer, const LevelPaths& level,
               std::vector<entt::entity>* createdOut, bool additive) {
    // A level is static + dynamic ONLY; UI (menus/HUD) are separate standalone
    // scenes (see Scene/Level.h). When not additive the first layer replaces the
    // world (+ owns the environment) and the rest stack on; additive stacks all.
    const SceneKind order[] = {SceneKind::Static, SceneKind::Dynamic};
    bool loadedAny = false;
    for (const SceneKind k : order) {
        const fs::path m = level.Member(k);
        if (m.empty() || !vfs::Exists(m)) continue; // a layer may be absent (pack-aware)
        SceneData data;
        if (!ParseSceneFile(m, data)) continue;
        data.kind = k; // the filename is the source of truth for the layer
        StagedAssets staged;
        StageAssets(data, FindAssetsDir(m), staged);
        const LoadMode mode =
            (additive || loadedAny) ? LoadMode::Additive : LoadMode::Replace;
        Instantiate(scene, renderer, data, staged, mode, createdOut, m.string());
        loadedAny = true;
    }
    return loadedAny;
}

} // namespace hbe::scene
