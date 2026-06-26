// Scene/Scene.h - ECS-backed scene container plus draw/view extraction.
#pragma once

#include "Scene/Components.h"
#include "Renderer/Camera.h"
#include "RHI/RHI.h"

#include <entt/entt.hpp>

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

    // Day/night cycle. The sky is rendered analytically from the sun direction
    // (real-time atmosphere), so moving the sun moves the whole sky + lighting.
    // When `dynamicSky` is on, the engine advances `timeOfDay` (hours, [0,24)) at
    // `dayLengthSeconds` real seconds per full cycle and recomputes the sun
    // direction/colour/intensity + ambient each frame. 0 dayLength = time is held
    // (scrub it from the editor). Off = the authored `sun` is used as-is.
    f32  timeOfDay = 10.0f;       // hours [0,24)
    f32  dayLengthSeconds = 0.0f; // real seconds for one 24h cycle (0 = paused)
    u32  dynamicSky = 0;          // 1 = drive the sun from timeOfDay

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

    // HDR post-process stack (bloom/SSAO/FXAA/grade); defaults are sensible.
    rhi::PostSettings post;
};

// Owns an entt::registry. Entities with Transform + MeshInstance are drawn.
class Scene {
public:
    entt::registry&       Registry()       { return registry_; }
    const entt::registry& Registry() const { return registry_; }

    SceneEnvironment&       Environment()       { return env_; }
    const SceneEnvironment& Environment() const { return env_; }

    // Creates an entity, optionally tagging it with a Name component.
    entt::entity CreateEntity(const std::string& name = {});

    // World-space matrix of an entity: composes Transform up the Parent chain.
    glm::mat4 WorldMatrix(entt::entity e) const;

    // Editor visibility mode: when on, entities carrying EditorHidden (or parented
    // under one) are skipped by CollectDrawItems / the UI build, so they stay
    // loaded but invisible. The runtime leaves this off (EditorHidden has no
    // effect there). The editor turns it on at init.
    void SetEditorView(bool on) { editorView_ = on; }
    bool EditorView() const { return editorView_; }
    // True when `e` or any ancestor carries EditorHidden (only meaningful while
    // EditorView() is on). Cheap no-op when nothing is hidden.
    bool IsEditorHidden(entt::entity e) const;

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
    entt::registry   registry_;
    SceneEnvironment env_;
    bool             editorView_ = false; // editor-only EditorHidden culling

    // Per-object motion-vector history (for TAA + motion blur). Double-buffered:
    // CollectDrawItems hands out pointers/values from the "previous" maps (kept
    // alive through DrawScene) and records this frame into the "current" maps;
    // the two are swapped at the start of the next collect. Keyed by entity, so
    // they self-clean as entities disappear. mutable: CollectDrawItems is const.
    mutable std::unordered_map<entt::entity, glm::mat4> prevWorld_, curWorld_;
    mutable std::unordered_map<entt::entity, std::vector<glm::mat4>> prevPalette_, curPalette_;
};

namespace scene {

// Generates the procedural-sky IBL environment (irradiance/prefiltered/BRDF
// LUT + background sky) and installs it on `scene`. Call once per scene.
void SetupEnvironment(Scene& scene, Renderer& renderer);

// Builds the minimal default world (an editable sun entity in an otherwise
// empty scene) when no startup scene or model is provided.
void BuildDefaultScene(Scene& scene);

// Loads a model file (glTF/GLB/OBJ/FBX) as entities. Returns false on failure.
bool LoadModel(Scene& scene, Renderer& renderer, const std::string& path);

// Spawns `count` mesh instances (draw-call stress test).
void SpawnStress(Scene& scene, Renderer& renderer, u32 count);

} // namespace scene
} // namespace hbe
