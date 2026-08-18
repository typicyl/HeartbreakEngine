// Scene/Scene.cpp
#include "Scene/Scene.h"

#include "Assets/Mesh.h"
#include "Assets/MeshGenerator.h"
#include "Assets/ModelLoader.h"
#include "Core/Log.h"
#include "Project/Project.h"
#include "Renderer/IBL.h"
#include "Renderer/Renderer.h"
#include "RHI/MaterialCompiler.h" // material::ComputeShaderVariant (OpenPBR shader specialization)
#include "Scene/EntityGuid.h"
#include "Scene/PaintSystem.h"
#include "Scene/TerrainSystem.h" // hole-mask usability (one rule for every consumer)

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>

#include <cmath>
#include <filesystem>
#include <limits>
#include <sstream>

namespace hbe {

EnvironmentStamp StampOf(const SceneEnvironment& env) {
    EnvironmentStamp s;
    s.ambientIntensity = env.ambientIntensity;
    s.exposure = env.exposure;
    s.shadowDistance = env.shadowDistance;
    s.post = env.post;
    s.giSource = env.giSource;
    s.giStatus = env.giStatus;
    s.giOrigin = env.giOrigin;
    s.giSpacing = env.giSpacing;
    s.giDims = env.giDims;
    s.giShValid = env.giSh.IsValid();
    s.giDepthValid = env.giDepth.IsValid();
    s.timeOfDay = env.timeOfDay;
    s.dayLengthSeconds = env.dayLengthSeconds;
    s.dynamicSky = env.dynamicSky;
    s.dayNightAuthored = env.dayNightAuthored;
    return s;
}

std::string DescribeStampDiff(const EnvironmentStamp& a, const EnvironmentStamp& b) {
    std::ostringstream o;
    const auto cmp = [&o](const char* name, auto x, auto y) {
        if (!(x == y)) o << " " << name << "(" << x << " vs " << y << ")";
    };
    cmp("ambientIntensity", a.ambientIntensity, b.ambientIntensity);
    cmp("exposure", a.exposure, b.exposure);
    cmp("shadowDistance", a.shadowDistance, b.shadowDistance);
    if (!(a.post == b.post)) o << " post(differs)";
    cmp("giSource", a.giSource, b.giSource);
    if (a.giStatus != b.giStatus)
        o << " giStatus(" << ToString(a.giStatus) << " vs " << ToString(b.giStatus) << ")";
    if (a.giOrigin != b.giOrigin) o << " giOrigin(differs)";
    if (a.giSpacing != b.giSpacing) o << " giSpacing(differs)";
    if (a.giDims != b.giDims) o << " giDims(differs)";
    cmp("giShValid", a.giShValid, b.giShValid);
    cmp("giDepthValid", a.giDepthValid, b.giDepthValid);
    cmp("timeOfDay", a.timeOfDay, b.timeOfDay);
    cmp("dayLengthSeconds", a.dayLengthSeconds, b.dayLengthSeconds);
    cmp("dynamicSky", a.dynamicSky, b.dynamicSky);
    cmp("dayNightAuthored", a.dayNightAuthored, b.dayNightAuthored);
    return o.str();
}

// The day/night curve, evaluated ONCE per definition (see Scene.h `struct DayNight`).
// The sun arc is overhead at noon and underfoot at midnight, rising near 6am and
// setting near 6pm, tilted by `lat` so it crosses toward the south rather than
// dead overhead.
DayNight EvalDayNight(f32 hours) {
    DayNight d;
    const f32 ang = (hours - 12.0f) / 12.0f * 3.14159265f; // 0 at noon, +/-PI midnight
    const f32 ax = std::sin(ang);   // -1 at 6am, +1 at 6pm (rises -X, sets +X)
    const f32 ay = std::cos(ang);   // +1 noon (up), -1 midnight (down)
    const f32 lat = 0.55f;          // ~31 degrees of southward tilt
    d.toSun = glm::normalize(glm::vec3(ax, ay * std::cos(lat), -ay * std::sin(lat)));
    const f32 elev = d.toSun.y;     // sun height, -1..1
    d.day = glm::clamp(elev * 6.0f + 0.15f, 0.0f, 1.0f);
    const f32 warm = glm::clamp(1.0f - glm::max(elev, 0.0f) * 3.0f, 0.0f, 1.0f);
    // A MULTIPLIER on the authored sun colour, not a replacement for it: white at
    // midday, sunset-orange at the horizon. Normalized so midday leaves the
    // authored colour untouched.
    d.tint = glm::mix(glm::vec3(1.0f), glm::vec3(1.0f, 0.51f, 0.23f), warm);
    return d;
}

Scene::Scene() {
    // UI structure signals: constructing/destroying/replacing a hierarchy-shaping
    // component invalidates the UI layout's cached parent->children map (the
    // version is compared by ui::UIContext). Direct FIELD writes don't fire
    // signals - those sites (panel.active flips, in-place reparents) call
    // BumpUIVersion() explicitly. NOTE: connections capture `this`; Scene must
    // not be moved/copied (registry_ is move-only, and no call site moves Scene).
    registry_.on_construct<Parent>().connect<&Scene::OnUIStructural>(*this);
    registry_.on_destroy<Parent>().connect<&Scene::OnUIStructural>(*this);
    registry_.on_update<Parent>().connect<&Scene::OnUIStructural>(*this);
    registry_.on_construct<UIElement>().connect<&Scene::OnUIStructural>(*this);
    registry_.on_destroy<UIElement>().connect<&Scene::OnUIStructural>(*this);
    registry_.on_construct<UICanvas>().connect<&Scene::OnUIStructural>(*this);
    registry_.on_destroy<UICanvas>().connect<&Scene::OnUIStructural>(*this);
    registry_.on_construct<UIPanel>().connect<&Scene::OnUIStructural>(*this);
    registry_.on_destroy<UIPanel>().connect<&Scene::OnUIStructural>(*this);
    // Name index invalidation (see FindByName). A Name written IN PLACE through a
    // reference does not fire on_update, so the editor's rename path calls
    // registry.patch<Name> / replace<Name>; anything that assigns the field
    // directly must invalidate too, which is why the index is rebuilt from
    // scratch rather than incrementally patched.
    registry_.on_construct<Name>().connect<&Scene::OnNameChanged>(*this);
    registry_.on_destroy<Name>().connect<&Scene::OnNameChanged>(*this);
    registry_.on_update<Name>().connect<&Scene::OnNameChanged>(*this);
}

entt::entity Scene::FindByName(const std::string& name) const {
    if (name.empty()) return entt::null;
    // At most two passes: the second only happens if the first hit a stale handle
    // (an entity destroyed without its Name signal firing, e.g. a bulk registry
    // clear). A rebuild only inserts entities the view yields, so they are all
    // valid afterwards and the loop cannot spin.
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (nameIndexDirty_) {
            nameIndex_.clear();
            // First writer wins, matching the old linear scan's "first match".
            for (const entt::entity e : registry_.view<const Name>())
                nameIndex_.emplace(registry_.get<const Name>(e).value, e);
            nameIndexDirty_ = false;
        }
        const auto it = nameIndex_.find(name);
        if (it == nameIndex_.end()) return entt::null;
        if (registry_.valid(it->second)) return it->second;
        nameIndexDirty_ = true; // stale: rebuild and try once more
    }
    return entt::null;
}

entt::entity Scene::CreateEntity(const std::string& name) {
    const entt::entity e = registry_.create();
    // THE mint site. This is the only wrapper around registry_.create() in the
    // tree, so stamping identity here covers every authored, loaded and spawned
    // entity. A load overwrites this with the guid from the file
    // (scene::Instantiate -> guid::Apply); everything else keeps the fresh one,
    // which is exactly what makes a duplicate a new object. See Scene/EntityGuid.h.
    registry_.emplace<Guid>(e, Guid{guid::Mint()});
    // THE SIBLING-ORDER MINT, for the same reason and at the same place. Entity
    // handles are recycled, so they are not a creation sequence; this counter is.
    // A load overwrites it with the file's "order" (scene::Instantiate), exactly
    // like the guid above; everything else keeps the fresh value, which puts a
    // newly created entity LAST among its siblings. See Scene/Hierarchy.h.
    registry_.emplace<HierarchyOrder>(e, HierarchyOrder{NextHierarchyOrder()});
    if (!name.empty()) {
        registry_.emplace<Name>(e, name);
    }
    return e;
}

glm::mat4 Scene::WorldMatrix(entt::entity e) const {
    glm::mat4 m(1.0f);
    int depth = 0; // guards against accidental parent cycles
    for (entt::entity cur = e; cur != entt::null && depth < 64; ++depth) {
        if (const Transform* t = registry_.try_get<Transform>(cur)) {
            m = t->Matrix() * m;
        }
        const Parent* p = registry_.try_get<Parent>(cur);
        cur = (p && registry_.valid(p->entity)) ? p->entity : entt::null;
    }
    return m;
}

glm::mat4 Scene::ParentWorldMatrix(entt::entity e) const {
    const Parent* p = registry_.try_get<Parent>(e);
    if (!p || !registry_.valid(p->entity)) return glm::mat4(1.0f);
    return WorldMatrix(p->entity);
}

void Scene::SetWorldPosition(entt::entity e, const glm::vec3& worldPos) {
    Transform* t = registry_.try_get<Transform>(e);
    if (!t) return;
    const Parent* p = registry_.try_get<Parent>(e);
    if (!p || !registry_.valid(p->entity)) { // root: local IS world
        t->position = worldPos;
        return;
    }
    t->position = glm::vec3(glm::inverse(WorldMatrix(p->entity)) * glm::vec4(worldPos, 1.0f));
}

void Scene::SetWorldRotation(entt::entity e, const glm::quat& worldRot) {
    Transform* t = registry_.try_get<Transform>(e);
    if (!t) return;
    const Parent* p = registry_.try_get<Parent>(e);
    if (!p || !registry_.valid(p->entity)) {
        t->rotation = worldRot;
        return;
    }
    // Solve for the local rotation that makes the child's WORLD BASIS come out as
    // asked, by pulling the wanted axes back through the parent's full 3x3.
    //
    // The obvious `inverse(parentRotation) * worldRot` is WRONG whenever the parent
    // has non-uniform scale: the child's world orientation is parentRot * parentScale
    // * childLocalRot, and a non-uniform scale sitting in the middle SHEARS the
    // child's axes. No local rotation can undo a shear, so full orientation is not
    // recoverable in that case - but the FORWARD direction is, exactly, and forward
    // (local -Z) is what every facing consumer in this engine reads: cam::Update's
    // tgtFwd, ai::ForwardDir's sight cone, the render camera. So forward is solved
    // exactly and roll about it is fitted; under uniform scale (the normal case)
    // both are exact.
    const glm::mat3 m(WorldMatrix(p->entity));
    if (std::fabs(glm::determinant(m)) < 1e-8f) { // degenerate parent: best effort
        t->rotation = worldRot;
        return;
    }
    const glm::mat3 inv = glm::inverse(m);
    glm::vec3 fwd = inv * (worldRot * glm::vec3(0.0f, 0.0f, -1.0f));
    if (glm::length(fwd) < 1e-8f) {
        t->rotation = worldRot;
        return;
    }
    fwd = glm::normalize(fwd);
    glm::vec3 up = inv * (worldRot * glm::vec3(0.0f, 1.0f, 0.0f));
    up = glm::length(up) < 1e-8f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::normalize(up);
    // quatLookAt degenerates when up is parallel to the direction.
    if (std::fabs(glm::dot(fwd, up)) > 0.999f)
        up = std::fabs(fwd.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f);
    t->rotation = glm::normalize(glm::quatLookAt(fwd, up));
}

bool Scene::IsEditorHidden(entt::entity e) const {
    // Walk self -> ancestors; hidden if any of them carries EditorHidden.
    int depth = 0;
    for (entt::entity cur = e; cur != entt::null && depth < 64; ++depth) {
        if (registry_.all_of<EditorHidden>(cur)) return true;
        const Parent* p = registry_.try_get<Parent>(cur);
        cur = (p && registry_.valid(p->entity)) ? p->entity : entt::null;
    }
    return false;
}

void Scene::ForgetMotionHistory(entt::entity e) {
    prevWorld_.erase(e);
    curWorld_.erase(e);
    prevPalette_.erase(e);
    curPalette_.erase(e);
}

void Scene::CollectDrawItems(std::vector<rhi::DrawItem>& out) const {
    // Promote last frame's recorded transforms/palettes to "previous". The
    // pointers handed out last frame were already consumed by DrawScene, so it
    // is safe to recycle the now-stale "current" maps for this frame.
    prevWorld_.swap(curWorld_);     curWorld_.clear();
    prevPalette_.swap(curPalette_); curPalette_.clear();

    // Editor-only: cull entities hidden via EditorHidden (self or ancestor). Skip
    // the per-entity walk entirely when nothing is hidden (the common case).
    const auto* hiddenStore = registry_.storage<EditorHidden>();
    const bool cullHidden = editorView_ && hiddenStore && hiddenStore->size() > 0;
    // Skip the per-draw blendshape lookup entirely when no entity has a MorphState
    // (the common case) - same idiom as cullHidden above.
    const auto* morphStore = registry_.storage<MorphState>();
    const bool anyMorph = morphStore && morphStore->size() > 0;

    auto view = registry_.view<const Transform, const MeshInstance>();
    out.reserve(out.size() + view.size_hint());
    for (const entt::entity e : view) {
        const auto& instance  = view.get<const MeshInstance>(e);
        if (!instance.mesh.IsValid()) continue;
        if (cullHidden && IsEditorHidden(e)) continue;

        rhi::DrawItem item;
        item.mesh = instance.mesh;
        item.transform = WorldMatrix(e);
        // Motion vectors: previous-frame world matrix (== current when the
        // entity is new -> zero velocity), recorded for next frame.
        if (auto it = prevWorld_.find(e); it != prevWorld_.end())
            item.prevTransform = it->second;
        else
            item.prevTransform = item.transform;
        curWorld_[e] = item.transform;
        // Skeletal: resolve which entity OWNS the pose for this draw. A modular-
        // character PART (SkinnedPartRef) borrows its Character root's single shared
        // Animator.palette - one pose drives every part, which is also what makes the
        // seam weld bit-exact (all parts read the same palette). A plain skinned
        // entity has no SkinnedPartRef, so poseOwner == e (unchanged behaviour).
        entt::entity poseOwner = e;
        if (const SkinnedPartRef* pr = registry_.try_get<SkinnedPartRef>(e);
            pr && registry_.valid(pr->character))
            poseOwner = pr->character;
        if (const Animator* an = registry_.try_get<Animator>(poseOwner);
            an && !an->palette.empty()) {
            item.bones = an->palette.data();
            item.boneCount = static_cast<u32>(an->palette.size());
            // Previous-frame palette for skinned motion vectors, keyed by the POSE
            // OWNER (root) so every part that borrows it shares one history and it is
            // independent of entity iteration order. Only use it when the joint count
            // matches (clip/rig changes invalidate it). Recorded once per frame
            // (try_emplace) - multiple parts of one character must not each re-copy it.
            if (auto pit = prevPalette_.find(poseOwner);
                pit != prevPalette_.end() && pit->second.size() == an->palette.size())
                item.prevBones = pit->second.data();
            curPalette_.try_emplace(poseOwner, an->palette);
        }
        item.surface = instance.surface; // full OpenPBR material-value copy
        item.albedoTexture = instance.albedoTexture;
        item.normalTexture = instance.normalTexture;
        item.mrTexture = instance.mrTexture;
        item.aoTexture = instance.aoTexture;
        item.emissiveTexture = instance.emissiveTexture;
        item.thicknessTexture = instance.thicknessTexture;
        item.materialFlags = instance.materialFlags;
        // OpenPBR shader specialization (P3): pick the curated MeshPBR variant for this material.
        // Uses the shading-model flags (hair/eye/subsurface/cloth) from the instance; the world/
        // painterly flags added below (splat/hole/exempt/censored) don't affect the variant.
        item.shaderVariant = material::ComputeShaderVariant(instance.surface, instance.materialFlags);
        // Facial blendshapes: fill the top-8 active channels from a resolved MorphState
        // (channel name -> atlas row via targetNames; weights from the driver/schematic).
        // The atlas is uploaded once at spawn (SceneSerializer::Instantiate resolve).
        if (anyMorph) {
            if (const MorphState* mo = registry_.try_get<MorphState>(e);
                mo && mo->resolved && mo->morphTexture.IsValid() && !mo->targetNames.empty()) {
                item.morphTexture = mo->morphTexture;
                item.morphVertexCount = mo->vertexCount;
                u32 n = 0;
                for (usize r = 0; r < mo->targetNames.size() && n < 8u; ++r) {
                    const auto wit = mo->weights.find(mo->targetNames[r]);
                    if (wit == mo->weights.end() || wit->second <= 1e-3f) continue;
                    item.morphTargets[n] = static_cast<u32>(r);
                    item.morphWeights[n] = wit->second;
                    ++n;
                }
                item.morphCount = n;
            }
        }
        // Dynamic-layer objects (player / NPCs / interactables) are exempt from the
        // painterly finish so they stand out crisp against the painted static world.
        // Tag them here; the forward pass writes the flag into HDR alpha and a post
        // composite restores their lit colour over the painterly result (they keep
        // full lighting/shadows/GI/fog - only the painted stylisation is skipped).
        if (const SceneLayer* sl = registry_.try_get<SceneLayer>(e);
            sl && sl->kind == SceneKind::Dynamic)
            item.materialFlags |= rhi::MaterialFlag_PainterlyExempt;
        // Painterly censor target: an entity carrying a CensorComponent gets the
        // painted brush-stroke look on ITS OWN surface (static or dynamic). The
        // forward pass writes this into HDR alpha so the censor passes confine the
        // sphere to the object's pixels (not the floor/walls inside the sphere).
        if (const CensorComponent* cc = registry_.try_get<CensorComponent>(e);
            cc && cc->enabled && cc->strength > 0.0f && cc->radius > 0.0f)
            item.materialFlags |= rhi::MaterialFlag_Censored;
        // Terrain chunk: inherit the parent terrain's painted masks. The hole mask
        // (thickness slot + TerrainHole flag) clips pixels so cliff/cave models show
        // through. Splat blends 4 tiling layer albedos (overloaded into the albedo/
        // normal/mr/ao slots) by the weight mask (emissive slot); subsurfaceRadius
        // carries the tile scale. Both coexist.
        if (registry_.all_of<TerrainChunk>(e)) {
            if (const Parent* par = registry_.try_get<Parent>(e);
                par && registry_.valid(par->entity)) {
                if (const TerrainComponent* tc = registry_.try_get<TerrainComponent>(par->entity)) {
                    // Live terrain roughness (the inspector slider takes effect without a
                    // chunk rebuild; also the splat roughness floor reads this).
                    item.surface.specular_roughness = tc->roughness;
                    // HoleMaskUsable, not !empty(): a stale (wrong-sized) mask is
                    // ignored by terrain::IsHole and therefore by the collider, so
                    // flagging the shader to clip against a stale texture would show
                    // holes where physics has solid ground. One rule, three consumers.
                    if (tc->holeMaskTex.IsValid() && terrain::HoleMaskUsable(*tc)) {
                        item.thicknessTexture = tc->holeMaskTex;
                        item.materialFlags |= rhi::MaterialFlag_TerrainHole;
                    }
                    if (tc->splatEnabled && tc->splatWeightTex.IsValid()) {
                        for (int li = 0; li < 4; ++li) {
                            item.splatAlbedo[li] = tc->splatAlbedoTex[li];
                            item.splatNormal[li] = tc->splatNormalTex[li];
                            item.splatMR[li]     = tc->splatMRTex[li];
                            item.splatRough[li]  = tc->splatRoughFactor[li];
                        }
                        item.emissiveTexture = tc->splatWeightTex; // weight mask
                        item.surface.subsurface_radius = tc->splatTile;     // overload: tile scale
                        item.materialFlags |= rhi::MaterialFlag_TerrainSplat;
                    }
                }
            }
        }
        // Art Editor surface paint: composite the canvas over the material when
        // the entity has an enabled, GPU-resident PaintComponent. Terrain chunks
        // share ONE whole-terrain canvas that lives on the parent terrain entity
        // (their UVs are terrain-wide), so inherit it.
        const PaintComponent* pc = registry_.try_get<PaintComponent>(e);
        if (!pc && registry_.all_of<TerrainChunk>(e)) {
            if (const Parent* par = registry_.try_get<Parent>(e); par && registry_.valid(par->entity))
                pc = registry_.try_get<PaintComponent>(par->entity);
        }
        if (pc && pc->enabled && pc->gpuReady && pc->colorTex.IsValid()) {
            item.paintColorTexture = pc->colorTex;
            item.paintHeightTexture = pc->matTex; // material+height (R metal,G rough,B height)
            item.paintOpacity = pc->opacity;
            // Relief (normal deformation) is opt-in per object.
            item.paintHeightScale = pc->reliefEnabled ? pc->heightScale : 0.0f;
            item.paintLodBias = pc->lodBias;
            item.paintTexel = pc->resolution > 0 ? 1.0f / static_cast<f32>(pc->resolution) : 0.0f;
            item.paintProjMode = static_cast<u32>(pc->projection);
            if (pc->projection == 1) { // box projection: derive params from AABB + world scale
                glm::vec3 wmin(0.0f), wmax(0.0f);
                if (const AABB* box = registry_.try_get<AABB>(e)) { wmin = box->min; wmax = box->max; }
                const glm::mat4& m = item.transform;
                const glm::vec3 worldScale(glm::length(glm::vec3(m[0])),
                                           glm::length(glm::vec3(m[1])),
                                           glm::length(glm::vec3(m[2])));
                const paint::BoxParams bp = paint::ComputeBoxParams(wmin, wmax, worldScale);
                item.paintBoxCenter = bp.center;
                item.paintBoxScale = bp.scale;
                item.paintBoxInvM = bp.invM;
            }
        }
        out.push_back(item);
    }
}

// Copies the post LOOK fields from `s` onto `d`, leaving the project-global
// quality fields (TAA / FXAA / GTAO) on `d` untouched. Used to overlay a Post
// Volume onto the base post without disturbing project-wide AA/AO.
static void OverlayPostLook(rhi::PostSettings& d, const rhi::PostSettings& s) {
    d.bloomEnabled = s.bloomEnabled;
    d.bloomIntensity = s.bloomIntensity;
    d.bloomThreshold = s.bloomThreshold;
    d.vignette = s.vignette;
    d.saturation = s.saturation;
    d.contrast = s.contrast;
    d.tonemapOperator = s.tonemapOperator;
    d.dofEnabled = s.dofEnabled;
    d.dofFocusDistance = s.dofFocusDistance;
    d.dofFocusRange = s.dofFocusRange;
    d.dofMaxBlur = s.dofMaxBlur;
    d.motionBlurEnabled = s.motionBlurEnabled;
    d.motionBlurIntensity = s.motionBlurIntensity;
    d.motionBlurMaxRadius = s.motionBlurMaxRadius;
    d.ssrEnabled = s.ssrEnabled;
    d.ssrIntensity = s.ssrIntensity;
    d.ssrMaxDistance = s.ssrMaxDistance;
    d.autoExposureEnabled = s.autoExposureEnabled;
    d.autoExposureKey = s.autoExposureKey;
    d.autoExposureSpeed = s.autoExposureSpeed;
    d.autoExposureMin = s.autoExposureMin;
    d.autoExposureMax = s.autoExposureMax;
    d.fogEnabled = s.fogEnabled;
    d.fogDensity = s.fogDensity;
    d.fogHeightFalloff = s.fogHeightFalloff;
    d.fogHeight = s.fogHeight;
    d.fogAnisotropy = s.fogAnisotropy;
    d.fogSunIntensity = s.fogSunIntensity;
    d.fogMaxDistance = s.fogMaxDistance;
    d.fogAmbient = s.fogAmbient;
    d.fogStepCount = s.fogStepCount;
    d.fogColor = s.fogColor;
    d.fogGodRays = s.fogGodRays;
    d.ssgiEnabled = s.ssgiEnabled;
    d.ssgiIntensity = s.ssgiIntensity;
    d.ssgiRadius = s.ssgiRadius;
    d.ssgiSamples = s.ssgiSamples;
    // Painterly is a LOOK field set, so a volume overrides ALL of it - every knob the
    // inspector exposes (Editor::DrawPostLookControls) and the serializer round-trips.
    // Keep this list in lockstep with the painterly block of rhi::PostSettings; a field
    // missing here is silently un-overridable per volume (it edits and saves, but the
    // scene default keeps winning at render time).
    d.painterlyEnabled = s.painterlyEnabled;
    d.painterlyRadius = s.painterlyRadius;
    d.painterlyWarmCool = s.painterlyWarmCool;
    d.painterlyStrokeFlow = s.painterlyStrokeFlow;
    d.painterlyStrength = s.painterlyStrength;
    d.painterlyEdge = s.painterlyEdge;
    d.painterlyLightTint = s.painterlyLightTint;
    d.painterlyStrokeDetail = s.painterlyStrokeDetail;
    d.painterlyCanvasScale = s.painterlyCanvasScale;
    d.painterlyCanvasStrength = s.painterlyCanvasStrength;
    d.painterlyPosterize = s.painterlyPosterize;
    d.painterlyStrokes = s.painterlyStrokes;
    d.painterlyStrokeLength = s.painterlyStrokeLength;
    d.painterlyStrokeDensity = s.painterlyStrokeDensity;
    d.painterlyStrokeSharp = s.painterlyStrokeSharp;
    d.painterlyStrokeBoil = s.painterlyStrokeBoil;
    d.painterlyStrokeMask = s.painterlyStrokeMask;
    d.painterlyStrokeMaskMinX = s.painterlyStrokeMaskMinX;
    d.painterlyStrokeMaskMinY = s.painterlyStrokeMaskMinY;
    d.painterlyStrokeMaskMaxX = s.painterlyStrokeMaskMaxX;
    d.painterlyStrokeMaskMaxY = s.painterlyStrokeMaskMaxY;
    // NOT copied (project-global quality): taaEnabled, fxaaEnabled, ssaoEnabled,
    // ssaoRadius, ssaoIntensity. Nor shadowCascades (a runtime perf preset, re-applied
    // every frame by ApplyGraphicsPreset AFTER volumes) or painterly3D (removed feature,
    // kept only so old scene files still load).
}

rhi::SceneView Scene::MakeView(const Camera& camera) const {
    const glm::mat4 viewProj = camera.ViewProjection();
    rhi::SceneView v;
    v.viewProj = viewProj;
    v.cameraPos = camera.Position();
    v.exposure = env_.exposure;
    v.ambientIntensity = env_.ambientIntensity;
    v.post = env_.post;
    // THE PLAYER'S QUALITY PRESET, applied to the COPY (see SceneEnvironment::
    // postQualityPreset). Position is exact, not approximate: the engine loop used to
    // stamp this onto env_.post BEFORE calling RenderScene, so the degrade landed
    // before the volume overlay below and a Post Volume could re-enable a pass the
    // preset dropped. Applying it here reproduces that ordering byte for byte while
    // leaving the authored `post` untouched. `postQualityPreset` is 0 unless the
    // runtime (or the editor's shipped-look preview) declares otherwise, so this is a
    // no-op everywhere else.
    scene::ApplyGraphicsPreset(v.post, env_.postQualityPreset);
    if (env_.forceShadowCascades > 0) v.post.shadowCascades = env_.forceShadowCascades;
    // Post Volumes: the highest-priority enabled volume that contains the camera
    // overrides the look fields; outside all of them the scene default applies.
    {
        const PostVolume* best = nullptr;
        int bestPri = (std::numeric_limits<int>::min)();
        for (const entt::entity e : registry_.view<const PostVolume, const Transform>()) {
            const PostVolume& pv = registry_.get<const PostVolume>(e);
            if (!pv.enabled) continue;
            const glm::vec3 local =
                glm::vec3(glm::inverse(WorldMatrix(e)) * glm::vec4(v.cameraPos, 1.0f));
            const glm::vec3 h = glm::max(pv.halfExtents, glm::vec3(1e-3f));
            if (std::abs(local.x) <= h.x && std::abs(local.y) <= h.y &&
                std::abs(local.z) <= h.z && pv.priority >= bestPri) {
                bestPri = pv.priority;
                best = &pv;
            }
        }
        if (best) OverlayPostLook(v.post, best->settings);
    }
    // The sun is entity-driven when a DirectionalLightComponent exists (the
    // editor can create/edit it like any component); env_.sun is the fallback.
    v.light.direction = glm::normalize(env_.sun.direction);
    v.light.color = env_.sun.color;
    v.light.intensity = env_.sun.intensity;
    if (const auto sunView = registry_.view<const DirectionalLightComponent>();
        sunView.begin() != sunView.end()) {
        const auto& sun = sunView.get<const DirectionalLightComponent>(*sunView.begin());
        v.light.direction = glm::normalize(sun.direction); // first one wins
        v.light.color = sun.color;
        v.light.intensity = sun.intensity;
    }
    // DAY/NIGHT MODULATES THE AUTHORED LOOK - IT DOES NOT REPLACE IT.
    //
    // This used to be a REPLACEMENT, split across two files: Engine::UpdateDayNight
    // overwrote env.ambientIntensity and every sun's colour/intensity with absolute
    // constants every frame, and this block overwrote the authored exposure. The
    // consequence was that with `dynamicSky` on, a scene's authored ambientIntensity,
    // exposure and sun were DEAD DATA - a sealed interior authored at ambient 0.63
    // rendered at 1.0 and swept 0.12 -> 1.0 once per day cycle, so the same room was
    // a different brightness depending on when you looked at it. That is precisely
    // "I cannot light this scene".
    //
    // Now the curve is a MULTIPLIER applied at READ time, here, on the values the
    // scene file authored. Nothing writes back, so the inspector keeps showing (and
    // editing) the authored numbers, and the GI bake sees what the author sees. The
    // sun DIRECTION is still owned by the cycle (Engine::UpdateDayNight) - a sun that
    // does not move is not a day/night cycle - which is why direction is absent here.
    if (env_.dynamicSky != 0) {
        // From the HOUR, not from the resulting light direction: the direction is
        // whatever the last UpdateDayNight left (and on the first frame after a load,
        // whatever the file authored), so deriving `day` from it made the curve
        // depend on evaluation order.
        const DayNight dn = EvalDayNight(env_.timeOfDay);
        v.light.color *= dn.tint;
        v.light.intensity *= dn.day;                 // dims to ~0 at night
        // Night ambient floor. With dynamicIBL the re-baked irradiance ALREADY encodes the
        // sun's night falloff (+ the SkyColor night glow), so the 0.12 daytime-calibrated
        // floor would double-dim it - use a 1.0 floor there and let the map carry the night.
        v.ambientIntensity *= glm::mix(env_.dynamicIBL != 0u ? 1.0f : 0.12f, 1.0f, dn.day);
        // Auto-exposure (even from a Post Volume above) would adapt a dark starry
        // night back into looking like daylight, so force a deterministic exposure.
        v.post.autoExposureEnabled = 0;
        // Night keeps a deep-blue glow (not crushed to black); day is the authored
        // exposure, exactly.
        v.exposure *= glm::mix(0.6f, 1.0f, dn.day);
        // Volumetric fog scatters a sun/sky haze that washes the night horizon, so
        // fade the fog with daylight (and kill the sun-shaft term when the sun is
        // down) - keeps a dark night dark.
        v.post.fogDensity *= glm::mix(0.10f, 1.0f, dn.day);
        v.post.fogSunIntensity *= dn.day;
    }
    // Lightning: a brief weather-driven flash boosts scene light + ambient + exposure so the
    // world lights up under a storm (independent of the day/night cycle). lightningFlash_ is
    // driven by lightning::Update; 0 when not flashing = no cost.
    if (lightningFlash_ > 0.001f) {
        const f32 fl = lightningFlash_;
        v.ambientIntensity *= (1.0f + fl * 4.0f);
        // ADDITIVE directional boost (a lightning key light) so the flash is not nullified
        // at night, where the day/night curve has already multiplied v.light.intensity to ~0.
        v.light.intensity += fl * 2.0f;
        v.light.color = glm::mix(v.light.color, glm::vec3(0.82f, 0.86f, 1.0f), fl * 0.4f);
        v.exposure *= (1.0f + fl * 0.5f);
    }
    v.irradianceIndex = env_.irradiance.index;
    v.prefilteredIndex = env_.prefiltered.index;
    v.brdfLUTIndex = env_.brdfLUT.index;
    v.skinLUTIndex = env_.skinLUT.index;
    v.prefilteredMaxLod = env_.prefilteredMaxLod;
    v.skyIndex = env_.sky.index;
    v.invViewProj = glm::inverse(viewProj);
    v.cloudCoverage = env_.cloudCoverage;
    v.cloudDensity = env_.cloudDensity;
    v.overcast = env_.overcast;
    const f32 windRad = glm::radians(env_.windAngle);
    v.windVelX = std::cos(windRad) * env_.windSpeed;
    v.windVelZ = std::sin(windRad) * env_.windSpeed;
    v.wetness = env_.wetness;
    v.puddles = env_.puddles;
    v.snowAmount = env_.snowAmount;
    // gWeather2.w drives the puddle RAIN ripples in MeshPBR, so carry rain intensity
    // specifically (0 for snow/none) - a snowy scene with authored puddles must not ripple.
    v.precipIntensity = (env_.precipType == 1u) ? env_.precipIntensity : 0.0f;
    v.puddleScale = env_.puddleScale;
    v.snowScale = env_.snowScale;
    v.cloudVolumetric = env_.volumetricClouds ? 1.0f : 0.0f;
    v.cloudQuality = env_.cloudQuality;
    v.timeSeconds = time_; // shared animation clock (waves/sky/ripples + CPU buoyancy)

    // Water surface: the FIRST WaterComponent drives the GLOBAL scene water look; the
    // interactive ripple ring buffer rides waterRipples_ (aged/spawned by water::Update).
    v.rippleCount = 0;
    for (const glm::vec4& r : waterRipples_) {
        if (v.rippleCount >= rhi::kMaxRipples) break;
        v.ripples[v.rippleCount++] = r;
    }
    // Pick the scene ocean: the first fftOcean water if any (MATCHING ocean::UpdateForScene),
    // else the first water of any kind for Gerstner. The two selectors must agree or the driven
    // GPU ocean and the rendered surface diverge (FFT would silently degrade to Gerstner).
    const WaterComponent* chosenWater = nullptr;
    for (const entt::entity e : registry_.view<const WaterComponent>()) {
        const WaterComponent& w = registry_.get<const WaterComponent>(e);
        if (!chosenWater) chosenWater = &w;           // fallback: first water
        if (w.fftOcean) { chosenWater = &w; break; }  // prefer the first FFT water
    }
    if (chosenWater) {
        const WaterComponent& w = *chosenWater;
        for (int i = 0; i < 4; ++i) {
            const f32 a = glm::radians(w.waveAngle[i]);
            v.waterWaveA[i] =
                glm::vec4(std::cos(a), std::sin(a), w.waveAmplitude[i], w.waveLength[i]);
            v.waterWaveB[i] = glm::vec4(w.waveSpeed[i], w.waveSteepness[i], 0.0f, 0.0f);
        }
        v.waterShallow = glm::vec4(w.shallowColor, w.fresnelPower);
        v.waterDeep = glm::vec4(w.deepColor, w.reflectionRoughness);
        // FFT is active only when the GPU ocean is actually bound this frame (oceanActive_),
        // so the water VS never reads an unbound displacement buffer. .w carries the flag.
        const bool fftOn = w.fftOcean && oceanActive_;
        v.waterParams = glm::vec4(w.foam, w.rippleStrength, w.rippleScale, fftOn ? 1.0f : 0.0f);
        v.fftOcean = fftOn ? 1u : 0u;
        v.fftPatch = w.fftPatchSize;
        v.fftHeight = w.fftHeightScale;
        v.waterAbsorptionDepth = w.absorptionDepth;
        v.waterShorelineWidth = w.shorelineWidth;
        v.waterEdgeFade = w.edgeFade;
    }

    // Local light/reflection probes: upload every baked probe; the PBR shader
    // blends them per-pixel (box-weighted) and falls back to the global sky IBL
    // outside all of them, so a sealed interior is lit by its own lamps with smooth
    // transitions between rooms. World-space axis-aligned box from the Transform.
    v.probeCount = 0;
    for (const entt::entity e : registry_.view<const Transform, const ReflectionProbe>()) {
        if (v.probeCount >= rhi::kMaxProbes) break;
        const auto& rp = registry_.get<const ReflectionProbe>(e);
        if (!rp.baked) continue;
        const glm::mat4 w = WorldMatrix(e);
        const glm::vec3 scale(glm::length(glm::vec3(w[0])), glm::length(glm::vec3(w[1])),
                              glm::length(glm::vec3(w[2])));
        rhi::ProbeData& out = v.probes[v.probeCount++];
        out.center = glm::vec3(w[3]);
        out.halfExtents = glm::max(rp.halfExtents * scale, glm::vec3(1e-3f));
        out.blend = glm::max(glm::min(out.halfExtents.x,
                                      glm::min(out.halfExtents.y, out.halfExtents.z)) *
                                 0.35f,
                             0.1f);
        out.irradianceIndex = rp.irradiance.index;
        out.prefilteredIndex = rp.prefiltered.index;
        out.prefilteredMaxLod = rp.prefilteredMaxLod;
    }

    // Forward projected decals: each DecalComponent box is uploaded so MeshPBR can
    // project its albedo/normal/MR onto the surfaces it overlaps. invWorld maps a world
    // position into the box's unit cube; forwardWS/tangentWS are its world axes.
    v.decalCount = 0;
    for (const entt::entity e : registry_.view<const Transform, const DecalComponent>()) {
        if (v.decalCount >= rhi::kMaxDecals) break;
        const auto& dcp = registry_.get<const DecalComponent>(e);
        const glm::mat4 world = WorldMatrix(e);
        const glm::vec3 fwd = glm::vec3(world[2]); // box local +Z (projection axis)
        const glm::vec3 tan = glm::vec3(world[0]); // box local +X (decal U axis)
        // Skip a degenerate box: if the Transform (or a parent) has a zero-scale axis, a
        // basis column is null, boxWorld is singular, and glm::inverse would produce
        // NaN/Inf invWorld that the shader's |lp|>0.5 reject CANNOT cull (NaN > 0.5 is
        // false) - one such decal would smear garbage over the whole screen.
        if (glm::length(fwd) <= 1e-6f || glm::length(tan) <= 1e-6f ||
            glm::length(glm::vec3(world[1])) <= 1e-6f)
            continue;
        glm::mat4 boxWorld = world;
        const glm::vec3 he = glm::max(dcp.halfExtents, glm::vec3(1e-3f)) * 2.0f; // full box size
        boxWorld[0] *= he.x; // scale the basis so the unit cube maps to the box
        boxWorld[1] *= he.y;
        boxWorld[2] *= he.z;
        rhi::DecalData& out = v.decals[v.decalCount++];
        out.invWorld = glm::inverse(boxWorld);
        out.forwardWS = glm::normalize(fwd);
        out.tangentWS = glm::normalize(tan);
        out.opacity = dcp.opacity;
        out.angleFade = glm::max(dcp.angleFade, 0.0f);
        out.albedoIndex = dcp.albedo.index;
        out.normalIndex = dcp.normal.index;
        out.mrIndex = dcp.mr.index;
        out.flags = 0;
        out.params = glm::vec4(dcp.normalStrength, dcp.roughness, dcp.metallic, 0.0f);
    }

    // Baked SH-L1 irradiance volume (smooth directional diffuse GI; overrides the
    // box-probe diffuse in the shader when present).
    v.giShIndex = env_.giSh.index;
    v.giDepthIndex = env_.giDepth.index;
    v.giOrigin = env_.giOrigin;
    v.giInvSpacing = 1.0f / glm::max(env_.giSpacing, glm::vec3(1e-3f));
    v.giDims = env_.giDims;

    // Punctual lights: every Point/SpotLightComponent with a Transform.
    for (const entt::entity e : registry_.view<const Transform, const PointLightComponent>()) {
        if (v.punctualCount >= rhi::kMaxPunctualLights) break;
        const auto& pl = registry_.get<const PointLightComponent>(e);
        rhi::PunctualLight& out = v.punctualLights[v.punctualCount++];
        out.position = glm::vec3(WorldMatrix(e)[3]);
        out.color = pl.color;
        out.intensity = pl.intensity;
        out.range = glm::max(pl.range, 0.01f);
        out.isSpot = 0;
    }
    for (const entt::entity e : registry_.view<const Transform, const SpotLightComponent>()) {
        if (v.punctualCount >= rhi::kMaxPunctualLights) break;
        const auto& sl = registry_.get<const SpotLightComponent>(e);
        const glm::mat4 world = WorldMatrix(e);
        rhi::PunctualLight& out = v.punctualLights[v.punctualCount++];
        out.position = glm::vec3(world[3]);
        out.color = sl.color;
        out.intensity = sl.intensity;
        out.range = glm::max(sl.range, 0.01f);
        out.isSpot = 1;
        // Cone axis: the entity's local -Y in world space (down by default).
        const glm::vec3 axis = glm::mat3(world) * glm::vec3(0.0f, -1.0f, 0.0f);
        out.direction = glm::length(axis) > 1e-6f ? glm::normalize(axis)
                                                  : glm::vec3(0.0f, -1.0f, 0.0f);
        const f32 outer = glm::clamp(sl.outerAngle, 1.0f, 89.0f);
        const f32 inner = glm::clamp(sl.innerAngle, 0.0f, outer);
        out.innerCos = std::cos(glm::radians(inner));
        out.outerCos = std::cos(glm::radians(outer));
    }
    // Rect / area lights: a third punctual kind (isSpot = 2). Width/height pack into
    // innerCos/outerCos (as half-sizes); direction = the panel's world-space normal.
    for (const entt::entity e : registry_.view<const Transform, const RectLightComponent>()) {
        if (v.punctualCount >= rhi::kMaxPunctualLights) break;
        const auto& rl = registry_.get<const RectLightComponent>(e);
        const glm::mat4 world = WorldMatrix(e);
        rhi::PunctualLight& out = v.punctualLights[v.punctualCount++];
        out.position = glm::vec3(world[3]);
        out.color = rl.color;
        out.intensity = rl.intensity;
        out.range = glm::max(rl.range, 0.01f);
        out.isSpot = 2; // rect/area kind
        const glm::vec3 n = glm::mat3(world) * glm::vec3(0.0f, 0.0f, -1.0f); // local -Z normal
        out.direction = glm::length(n) > 1e-6f ? glm::normalize(n) : glm::vec3(0.0f, 0.0f, -1.0f);
        out.innerCos = glm::max(rl.width, 0.01f) * 0.5f;  // half-width
        out.outerCos = glm::max(rl.height, 0.01f) * 0.5f; // half-height
        out._pad0 = rl.twoSided ? 1.0f : 0.0f;
    }

    // World-anchored painterly censors: every entity with a CensorComponent. The
    // backend tests these spheres in 3D and feathers them into the painterly
    // passes so geometry inside keeps the painted look (CollectCensors).
    v.censorCount = 0;
    for (const entt::entity e : registry_.view<const Transform, const CensorComponent>()) {
        if (v.censorCount >= rhi::kMaxCensors) break;
        const auto& cc = registry_.get<const CensorComponent>(e);
        if (!cc.enabled || cc.strength <= 0.0f || cc.radius <= 0.0f) continue;
        const glm::mat4 world = WorldMatrix(e);
        rhi::CensorData& out = v.censors[v.censorCount++];
        out.center = glm::vec3(world * glm::vec4(cc.offset, 1.0f)); // local offset -> world
        out.radius = cc.radius;
        out.feather = cc.feather;
        out.strength = cc.strength;
    }

    // Cascaded directional shadows: split the camera frustum near -> far and
    // fit one texel-snapped orthographic light frustum per slice. The world
    // bounds of everything that can cast/receive (Transform + AABB entities)
    // clamp the shadow distance and pull each slice's near plane back so
    // off-screen casters still throw shadows into view.
    glm::vec3 wmin(1e30f), wmax(-1e30f);
    auto boundsView = registry_.view<const Transform, const AABB>();
    for (const entt::entity e : boundsView) {
        const glm::mat4 m = WorldMatrix(e);
        const AABB& box = boundsView.get<const AABB>(e);
        for (int c = 0; c < 8; ++c) {
            const glm::vec3 corner((c & 1) ? box.max.x : box.min.x,
                                   (c & 2) ? box.max.y : box.min.y,
                                   (c & 4) ? box.max.z : box.min.z);
            const glm::vec3 w = glm::vec3(m * glm::vec4(corner, 1.0f));
            wmin = glm::min(wmin, w);
            wmax = glm::max(wmax, w);
        }
    }
    // Fit to the RESOLVED sun (entity-driven when present, not env_ fallback).
    if (wmax.x >= wmin.x && v.light.intensity > 0.0f) {
        const glm::vec3 dir = v.light.direction;
        const f32 sceneRadius = glm::max(glm::length(wmax - wmin) * 0.5f, 0.5f);
        const glm::vec3 up = std::abs(dir.y) > 0.99f ? glm::vec3(0, 0, 1)
                                                     : glm::vec3(0, 1, 0);
        const glm::mat4 lightView =
            glm::lookAtRH(glm::vec3(0.0f), dir, up); // rotation-only light frame

        // Scene AABB in light space: bounds every potential caster.
        glm::vec3 lsSceneMin(1e30f), lsSceneMax(-1e30f);
        for (int c = 0; c < 8; ++c) {
            const glm::vec3 corner((c & 1) ? wmax.x : wmin.x,
                                   (c & 2) ? wmax.y : wmin.y,
                                   (c & 4) ? wmax.z : wmin.z);
            const glm::vec3 ls = glm::vec3(lightView * glm::vec4(corner, 1.0f));
            lsSceneMin = glm::min(lsSceneMin, ls);
            lsSceneMax = glm::max(lsSceneMax, ls);
        }

        // Practical split scheme (log/uniform blend) out to the shadow range.
        const f32 camNear = camera.NearPlane();
        // Shadow draw distance is driven by the camera, NOT the full scene radius.
        // A large terrain inflates sceneRadius, which would otherwise stretch every
        // cascade to span the whole world and collapse the NEAR cascade's shadow-map
        // resolution (blocky umbra + wide blobby penumbra on character-scale shadows).
        // Cap it so the near cascades stay tight; geometry past this casts no shadow,
        // which is imperceptible at distance. Small scenes keep their tight fit.
        const f32 maxShadow = glm::clamp(env_.shadowDistance, 10.0f, 5000.0f);
        const f32 shadowFar =
            glm::clamp(sceneRadius * 2.5f, camNear + 1.0f,
                       glm::min(camera.FarPlane(), maxShadow));
        // Quality preset drives how many cascades we actually render (High = 4 =
        // authored; Medium = 3; Low = 2). The split scheme is computed over the ACTIVE
        // count so the full shadowDistance is still covered by whatever cascades remain -
        // fewer slices just means each spans more depth (coarser distant shadows), NOT a
        // shorter shadow range. Directly scales the shadow render pass (cascades x casters).
        const u32 activeCascades =
            glm::clamp(env_.post.shadowCascades, 1u, rhi::kMaxShadowCascades);
        const f32 lambda = 0.75f;
        f32 splits[rhi::kMaxShadowCascades + 1];
        splits[0] = camNear;
        for (u32 i = 1; i <= activeCascades; ++i) {
            const f32 p = static_cast<f32>(i) / static_cast<f32>(activeCascades);
            const f32 logSplit = camNear * std::pow(shadowFar / camNear, p);
            const f32 uniSplit = camNear + (shadowFar - camNear) * p;
            splits[i] = lambda * logSplit + (1.0f - lambda) * uniSplit;
        }

        const glm::mat4 invView = glm::inverse(camera.View());
        const f32 tanHalfFovY = std::tan(camera.FovY() * 0.5f);
        const f32 tanHalfFovX = tanHalfFovY * camera.Aspect();
        constexpr f32 kCascadeTileDim = 2048.0f; // one tile of the 4096 atlas

        for (u32 ci = 0; ci < activeCascades; ++ci) {
            // Light-space AABB of this slice's eight frustum corners.
            glm::vec3 lsMin(1e30f), lsMax(-1e30f);
            for (int c = 0; c < 8; ++c) {
                const f32 z = (c & 4) ? splits[ci + 1] : splits[ci];
                const f32 x = ((c & 1) ? 1.0f : -1.0f) * tanHalfFovX * z;
                const f32 y = ((c & 2) ? 1.0f : -1.0f) * tanHalfFovY * z;
                const glm::vec3 world =
                    glm::vec3(invView * glm::vec4(x, y, -z, 1.0f)); // RH: -Z fwd
                const glm::vec3 ls = glm::vec3(lightView * glm::vec4(world, 1.0f));
                lsMin = glm::min(lsMin, ls);
                lsMax = glm::max(lsMax, ls);
            }

            // Snap the XY extents to the shadow texel grid (kills shimmer when
            // the camera pans) and pull the near plane back to the scene bounds
            // so casters behind the slice still occlude.
            const f32 extent =
                glm::max(lsMax.x - lsMin.x, lsMax.y - lsMin.y) * 1.05f;
            const f32 texel = extent / kCascadeTileDim;
            const glm::vec2 center = (glm::vec2(lsMin) + glm::vec2(lsMax)) * 0.5f;
            const glm::vec2 snapped = glm::floor(center / texel) * texel;
            const f32 half = extent * 0.5f;
            const f32 zNear = -glm::max(lsMax.z, lsSceneMax.z); // light looks -Z
            const f32 zFar  = -glm::min(lsMin.z, lsSceneMin.z);

            const glm::mat4 lightProj = glm::orthoRH_ZO(
                snapped.x - half, snapped.x + half,
                snapped.y - half, snapped.y + half, zNear, zFar);
            v.cascadeViewProj[ci] = lightProj * lightView;
            v.cascadeSplits[static_cast<int>(ci)] = splits[ci + 1];
        }
        v.cascadeCount = activeCascades;
        v.shadowsEnabled = 1;
    }
    return v;
}

namespace scene {

// See the header. Moved here VERBATIM from Engine.cpp's anonymous namespace so the
// one table has one home and both the runtime and the editor's preview read it.
void ApplyGraphicsPreset(rhi::PostSettings& p, int preset) {
    if (preset <= 0) return; // High: the authored look, exactly
    // Medium (default in shipped builds): drop the heaviest SCREEN-SPACE passes -
    // they cost the most at native fullscreen resolution and are the least missed.
    p.ssgiEnabled = 0;
    p.motionBlurEnabled = 0;
    p.ssrEnabled = 0;   // screen-space reflections
    p.fogEnabled = 0;   // volumetric fog raymarch
    // DoF + SSAO are the remaining COVERAGE-SCALED costs (they ramp with lit-geometry
    // screen fill = the daytime dip). Dropping them frees the ~2ms to hold 120 with the
    // full-res painterly intact. Painterly is untouched (it's the art style).
    p.dofEnabled = 0;
    p.ssaoEnabled = 0;
    // Shadows are "the dominant DAYTIME GPU cost" (see Common.hlsli ShadowFactor): each
    // cascade re-rasterizes every caster (cost = cascades x casters, a per-draw descriptor
    // bind on Vulkan). Drop the 4th (farthest, coarsest) cascade at Medium - the split
    // scheme redistributes over 3 slices in Scene::MakeView so the full shadow DISTANCE is
    // kept, only distant shadows get slightly coarser. ~25% off the whole shadow pass.
    p.shadowCascades = 3;
    if (preset >= 2) { // Low: minimal post; TAA falls back to cheap FXAA
        p.ssrEnabled = 0;
        p.dofEnabled = 0;
        p.ssaoEnabled = 0;
        p.shadowCascades = 2; // half the shadow-render cost; distant shadows softer
        if (p.taaEnabled) {
            p.taaEnabled = 0;
            p.fxaaEnabled = 1;
        }
    }
}

ProceduralSkyParams ProjectSkyParams() {
    ProceduralSkyParams sky;
    EnvironmentSettings env;
    if (Project::HasActive()) env = Project::Active().Settings().environment;
    sky.horizon = env.sky.horizonColor;
    sky.zenith = env.sky.zenithColor;
    sky.ground = env.sky.groundColor;
    sky.sunDir = env.sky.sunDirection;
    sky.sunTint = env.sky.sunTint;
    sky.sunIntensity = env.sky.sunIntensity;
    sky.skyIntensity = env.sky.skyIntensity;
    return sky;
}

void RebakeSkyLive(Scene& scene, Renderer& renderer) {
    // Dynamic IBL cohesion: regenerate the ambient (irradiance) + reflection (prefiltered)
    // maps IN PLACE from the LIVE day/night sun so ambient light and reflections (incl.
    // water) follow the sun + weather instead of the stale boot bake. Leak-free (reuses the
    // existing bindless handles). Throttled by the caller; a no-op before the boot bake.
    if (!renderer.SupportsScene()) return;
    SceneEnvironment& se = scene.Environment();
    if (!se.irradiance.IsValid() || !se.prefiltered.IsValid()) return;
    ProceduralSkyParams p = ProjectSkyParams();
    p.sunDir = -se.sun.direction; // env.sun points FROM the sun; params.sunDir points TO it
    IBLMaps m;
    m.irradiance = se.irradiance;
    m.prefiltered = se.prefiltered;
    RebakeProceduralIBLInto(renderer, p, m); // UpdateTexture reuses the handles - no writeback
}

void SetupSky(Scene& scene, Renderer& renderer) {
    if (!renderer.SupportsScene()) return;
    // Derive the procedural sky from the project's environment settings (custom
    // skybox), defaulting to the built-in gradient when no project is active.
    const IBLMaps ibl = GenerateProceduralIBL(renderer, ProjectSkyParams());
    if (!ibl.valid) return;
    EnvironmentSettings env;
    if (Project::HasActive()) env = Project::Active().Settings().environment;
    SceneEnvironment& se = scene.Environment();
    se.irradiance = ibl.irradiance;
    se.prefiltered = ibl.prefiltered;
    se.brdfLUT = ibl.brdfLUT;
    se.skinLUT = ibl.skinLUT;
    se.sky = ibl.sky;
    se.prefilteredMaxLod = ibl.prefilteredMaxLod;
    // Fallback sun (a scene's DirectionalLightComponent entity still wins in
    // MakeView). The light points from the sun toward the scene.
    se.sun.direction = glm::normalize(-env.sky.sunDirection);
    se.sun.color = env.sunColor;
    se.sun.intensity = env.sunLightIntensity;
    // Day/night cycle settings (the engine loop advances the clock when on) - the
    // project's, and only while the LOADED SCENE has not claimed the clock for
    // itself. A per-scene override (SceneEnvironment::dayNightAuthored, applied by
    // scene::ApplyEnvironment) is authored data in the `.hbscene`; letting "Rebuild
    // Sky + Lighting" quietly replace it with the project's values would both change
    // the level's lighting and - because the override round-trips - write the
    // project's hour into the level file on the next save.
    if (se.dayNightAuthored == 0) {
        se.timeOfDay = env.timeOfDay;
        se.dayLengthSeconds = env.dayLengthSeconds;
        se.dynamicSky = env.dynamicSky;
    }
    se.dynamicIBL = env.dynamicIBL;
    se.cloudCoverage = env.cloudCoverage;
    se.cloudDensity = env.cloudDensity;
    se.overcast = env.overcast;
    se.windAngle = env.windAngle;
    se.windSpeed = env.windSpeed;
    se.wetness = env.wetness;
    se.puddles = env.puddles;
    se.snowAmount = env.snowAmount;
    se.precipType = env.precipType;
    se.precipIntensity = env.precipIntensity;
    se.dynamicWeather = env.dynamicWeather;
    se.puddleScale = env.puddleScale;
    se.snowScale = env.snowScale;
    se.volumetricClouds = env.volumetricClouds;
    se.cloudQuality = env.cloudQuality;
    se.lightning = env.lightning;
    se.thunderSound = env.thunderSound;
}

void ApplyProjectLookDefaults(Scene& scene) {
    EnvironmentSettings env;
    if (Project::HasActive()) env = Project::Active().Settings().environment;
    SceneEnvironment& se = scene.Environment();
    se.ambientIntensity = env.ambientIntensity;
    se.exposure = env.exposure;
    se.post = env.post; // project-wide post stack is the scene default
}

void SetupEnvironment(Scene& scene, Renderer& renderer) {
    // Boot order, and the only place the two halves belong together: project
    // defaults FIRST, then the startup scene's header overrides them.
    // The device guard is kept where it always was, so a headless/device-less
    // boot leaves the environment exactly as untouched as it used to.
    if (!renderer.SupportsScene()) return;
    SetupSky(scene, renderer);
    ApplyProjectLookDefaults(scene);
}

// Minimal default world when no project/startup scene/model is given: an
// empty scene with just an editable sun entity (the environment's IBL + sky
// background still come from SetupEnvironment).
void BuildDefaultScene(Scene& scene) {
    const entt::entity sun = scene.CreateEntity("Sun");
    scene.Registry().emplace<Transform>(sun);
    DirectionalLightComponent light;
    light.direction = glm::normalize(glm::vec3(-0.5f, -1.0f, -0.4f));
    light.color = {1.0f, 0.98f, 0.95f};
    light.intensity = 4.0f;
    scene.Registry().emplace<DirectionalLightComponent>(sun, light);
    HBE_INFO("Scene: built default scene ({} entities).", scene.EntityCount());
}

void SpawnStress(Scene& scene, Renderer& renderer, u32 count, bool sharedMesh) {
    if (!renderer.SupportsScene() || count == 0) return;

    // Default: each instance gets its OWN higher-poly mesh, exercising unique
    // vertex data / bandwidth (like a real heavy scene). `sharedMesh` spawns N
    // instances of ONE mesh instead - the measurement rig for draw sorting /
    // IA-rebind skipping / GPU instancing (identical-mesh runs).
    MeshData proto = mesh::GenerateSphere(0.4f, 48, 24); // ~1200 verts each
    glm::vec3 pMin, pMax;
    ComputeBounds(proto, pMin, pMax);
    const rhi::MeshHandle shared = sharedMesh ? renderer.UploadMesh(proto) : rhi::MeshHandle{};
    if (sharedMesh && !shared.IsValid()) return;
    const u32 side = static_cast<u32>(std::ceil(std::cbrt(static_cast<f64>(count))));
    usize totalVerts = 0;
    u32 spawned = 0;
    for (u32 z = 0; z < side && spawned < count; ++z) {
        for (u32 y = 0; y < side && spawned < count; ++y) {
            for (u32 x = 0; x < side && spawned < count; ++x, ++spawned) {
                const rhi::MeshHandle handle =
                    sharedMesh ? shared : renderer.UploadMesh(proto);
                if (!handle.IsValid()) continue;
                if (!sharedMesh || spawned == 0) totalVerts += proto.vertices.size();
                const entt::entity e = scene.CreateEntity("Stress");
                Transform t;
                t.position = {static_cast<f32>(x) * 1.2f - side * 0.6f,
                              static_cast<f32>(y) * 1.2f - side * 0.6f,
                              static_cast<f32>(z) * 1.2f - side * 0.6f};
                scene.Registry().emplace<Transform>(e, t);
                MeshInstance mi;
                mi.mesh = handle;
                mi.surface.base_color = {0.8f, 0.8f, 0.85f, 1.0f};
                // Shared mode = ONE material too (varied materials would split the
                // instancing runs, defeating the measurement rig).
                mi.surface.base_metalness = sharedMesh ? 0.0f : ((x & 1u) ? 1.0f : 0.0f);
                mi.surface.specular_roughness = sharedMesh
                                   ? 0.5f
                                   : glm::clamp(0.1f + 0.8f * (static_cast<f32>(y) / side),
                                                0.05f, 1.0f);
                scene.Registry().emplace<MeshInstance>(e, mi);
                scene.Registry().emplace<AABB>(e, AABB{pMin, pMax});
            }
        }
    }
    HBE_INFO("Scene: spawned {} stress meshes ({} verts, {} draws total).", spawned,
             totalVerts, scene.EntityCount());
}

void SpawnParticleStress(Scene& scene, u32 count, bool gpuExpand, bool gpuSim) {
    if (count == 0) return;

    // Particle cost is OVERDRAW bound, not simulation bound, so this rig is tuned
    // to produce fill: big additive sprites, clustered, all overlapping. A grid of
    // tiny non-overlapping puffs would measure nothing interesting.
    constexpr u32 kPerEmitter = 2048;
    const u32 emitters = (count + kPerEmitter - 1u) / kPerEmitter;
    const u32 side = static_cast<u32>(std::ceil(std::sqrt(static_cast<f64>(emitters))));

    u32 spawned = 0, budget = count;
    for (u32 y = 0; y < side && spawned < emitters; ++y) {
        for (u32 x = 0; x < side && spawned < emitters; ++x, ++spawned) {
            const u32 mine = std::min(budget, kPerEmitter);
            budget -= mine;

            const entt::entity e = scene.CreateEntity("ParticleStress");
            Transform t;
            t.position = {static_cast<f32>(x) * 2.5f - side * 1.25f, 0.5f,
                          static_cast<f32>(y) * 2.5f - side * 1.25f};
            scene.Registry().emplace<Transform>(e, t);

            ParticleEmitter em;
            em.maxParticles = mine;
            em.lifetime = 4.0f;
            // Steady state is rate*lifetime, so this fills to the cap and holds there
            // rather than ramping for the whole benchmark window.
            em.rate = static_cast<f32>(mine) / em.lifetime * 1.25f;
            em.lifetimeVariance = 0.1f;
            em.emitRadius = 1.5f;
            em.spread = 1.0f;             // full sphere - keeps the cloud dense
            em.startSpeed = 0.6f;
            em.gravity = {0.0f, 0.05f, 0.0f};
            em.startSize = 0.9f;          // deliberately large: this is a FILL test
            em.endSize = 0.6f;
            em.additive = true;           // additive never depth-rejects -> full overdraw
            em.loop = true;
            em.emitting = true;
            em.gpuExpand = gpuExpand;
            // gpuSim owns the whole path (sim + expansion), so it does not also need
            // the CPU-upload expansion flag - the two are mutually exclusive here so
            // the three benchmark arms stay clean A/B/C rather than overlapping.
            em.gpuSim = gpuSim;
            if (gpuSim) em.gpuExpand = false;
            scene.Registry().emplace<ParticleEmitter>(e, em);
        }
    }
    HBE_INFO("Scene: spawned {} particle emitters targeting {} live particles ({}).", spawned,
             count,
             gpuSim ? "GPU simulation + GPU expansion"
                    : (gpuExpand ? "CPU simulation + GPU expansion"
                                 : "CPU simulation + CPU expansion"));
}

bool LoadModel(Scene& scene, Renderer& renderer, const std::string& path) {
    if (!renderer.SupportsScene()) {
        HBE_WARN("Scene: backend has no geometry support; cannot load '{}'.", path);
        return false;
    }

    auto model = hbe::LoadModel(path);
    if (!model) return false;

    // Group all submeshes under a root entity so the model moves as one unit.
    const std::string rootName = std::filesystem::path(path).stem().string();
    const entt::entity root = scene.CreateEntity(rootName.empty() ? "Model" : rootName);
    scene.Registry().emplace<Transform>(root);

    u32 loaded = 0;
    for (const MeshData& meshData : *model) {
        const rhi::MeshHandle handle = renderer.UploadMesh(meshData);
        if (!handle.IsValid()) continue;

        const entt::entity e = scene.CreateEntity(meshData.name);
        scene.Registry().emplace<Transform>(e); // identity transform
        scene.Registry().emplace<Parent>(e, Parent{root});
        MeshInstance mi;
        mi.mesh = handle;
        mi.surface.base_color = meshData.material.baseColor;
        mi.surface.base_metalness  = meshData.material.metallic;
        mi.surface.specular_roughness = meshData.material.roughness;
        mi.surface.emission_color = meshData.material.emissive;
        scene.Registry().emplace<MeshInstance>(e, mi);
        glm::vec3 mn, mx;
        ComputeBounds(meshData, mn, mx);
        scene.Registry().emplace<AABB>(e, AABB{mn, mx});
        ++loaded;
    }

    if (loaded == 0) {
        scene.Registry().destroy(root);
        HBE_WARN("Scene: model '{}' produced no renderable entities.", path);
        return false;
    }
    HBE_INFO("Scene: loaded '{}' as {} entities.", path, loaded);
    return true;
}

} // namespace scene
} // namespace hbe
