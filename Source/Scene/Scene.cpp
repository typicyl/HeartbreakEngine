// Scene/Scene.cpp
#include "Scene/Scene.h"

#include "Assets/Mesh.h"
#include "Assets/MeshGenerator.h"
#include "Assets/ModelLoader.h"
#include "Core/Log.h"
#include "Project/Project.h"
#include "Renderer/IBL.h"
#include "Renderer/Renderer.h"
#include "Scene/PaintSystem.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>

#include <cmath>
#include <filesystem>
#include <limits>

namespace hbe {

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
}

entt::entity Scene::CreateEntity(const std::string& name) {
    const entt::entity e = registry_.create();
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
        item.baseColor = instance.baseColor;
        item.metallic = instance.metallic;
        item.roughness = instance.roughness;
        item.albedoTexture = instance.albedoTexture;
        item.normalTexture = instance.normalTexture;
        item.mrTexture = instance.mrTexture;
        item.aoTexture = instance.aoTexture;
        item.emissiveTexture = instance.emissiveTexture;
        item.emissiveColor = instance.emissiveColor;
        item.emissiveIntensity = instance.emissiveIntensity;
        item.subsurfaceColor = instance.subsurfaceColor;
        item.subsurfaceRadius = instance.subsurfaceRadius;
        item.thicknessTexture = instance.thicknessTexture;
        item.materialFlags = instance.materialFlags;
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
                    item.roughness = tc->roughness;
                    if (tc->holeMaskTex.IsValid() && !tc->holeMask.empty()) {
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
                        item.subsurfaceRadius = tc->splatTile;     // overload: tile scale
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
    d.painterlyEnabled = s.painterlyEnabled;
    d.painterlyRadius = s.painterlyRadius;
    d.painterlyWarmCool = s.painterlyWarmCool;
    d.painterlyStrokeFlow = s.painterlyStrokeFlow;
    d.painterlyStrength = s.painterlyStrength;
    // NOT copied (project-global quality): taaEnabled, fxaaEnabled, ssaoEnabled,
    // ssaoRadius, ssaoIntensity.
}

rhi::SceneView Scene::MakeView(const Camera& camera) const {
    const glm::mat4 viewProj = camera.ViewProjection();
    rhi::SceneView v;
    v.viewProj = viewProj;
    v.cameraPos = camera.Position();
    v.exposure = env_.exposure;
    v.ambientIntensity = env_.ambientIntensity;
    v.post = env_.post;
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
    // Day/night owns the exposure: auto-exposure (even from a Post Volume above)
    // would adapt a dark starry night back into looking like daylight, so force a
    // deterministic exposure from the sun height whenever the dynamic sky is on.
    if (env_.dynamicSky != 0) {
        const f32 sunY = -v.light.direction.y; // to-sun = -(from-light direction)
        const f32 day = glm::clamp(sunY * 6.0f + 0.15f, 0.0f, 1.0f);
        v.post.autoExposureEnabled = 0;
        // Night keeps a deep-blue glow (not crushed to black); day is full exposure.
        v.exposure = glm::mix(0.6f, 1.0f, day);
        // Volumetric fog scatters a sun/sky haze that washes the night horizon, so
        // fade the fog with daylight (and kill the sun-shaft term when the sun is
        // down) - keeps a dark night dark.
        v.post.fogDensity *= glm::mix(0.10f, 1.0f, day);
        v.post.fogSunIntensity *= day;
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

void SetupEnvironment(Scene& scene, Renderer& renderer) {
    if (!renderer.SupportsScene()) return;

    // Derive the procedural sky from the project's environment settings (custom
    // skybox), defaulting to the built-in gradient when no project is active.
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

    const IBLMaps ibl = GenerateProceduralIBL(renderer, sky);
    if (!ibl.valid) return;
    SceneEnvironment& se = scene.Environment();
    se.irradiance = ibl.irradiance;
    se.prefiltered = ibl.prefiltered;
    se.brdfLUT = ibl.brdfLUT;
    se.skinLUT = ibl.skinLUT;
    se.sky = ibl.sky;
    se.prefilteredMaxLod = ibl.prefilteredMaxLod;
    se.ambientIntensity = env.ambientIntensity;
    se.exposure = env.exposure;
    se.post = env.post; // project-wide post stack is the scene default
    // Fallback sun (a scene's DirectionalLightComponent entity still wins in
    // MakeView). The light points from the sun toward the scene.
    se.sun.direction = glm::normalize(-env.sky.sunDirection);
    se.sun.color = env.sunColor;
    se.sun.intensity = env.sunLightIntensity;
    // Day/night cycle settings (the engine loop advances + drives the sun when on).
    se.timeOfDay = env.timeOfDay;
    se.dayLengthSeconds = env.dayLengthSeconds;
    se.dynamicSky = env.dynamicSky;
    se.cloudCoverage = env.cloudCoverage;
    se.cloudDensity = env.cloudDensity;
    se.overcast = env.overcast;
    se.windAngle = env.windAngle;
    se.windSpeed = env.windSpeed;
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
                mi.baseColor = {0.8f, 0.8f, 0.85f, 1.0f};
                // Shared mode = ONE material too (varied materials would split the
                // instancing runs, defeating the measurement rig).
                mi.metallic = sharedMesh ? 0.0f : ((x & 1u) ? 1.0f : 0.0f);
                mi.roughness = sharedMesh
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
        mi.baseColor = meshData.material.baseColor;
        mi.metallic  = meshData.material.metallic;
        mi.roughness = meshData.material.roughness;
        mi.emissiveColor = meshData.material.emissive;
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
