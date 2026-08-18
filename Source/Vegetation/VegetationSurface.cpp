// Vegetation/VegetationSurface.cpp - terrain/water/weather query bridge.
#include "Vegetation/VegetationSurface.h"
#include "Scene/Scene.h"
#include "Scene/Components.h"
#include "Scene/TerrainSystem.h"
#include "Scene/WaterSystem.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace hbe::veg {

entt::entity FindTerrain(const Scene& scene) {
    const auto& reg = scene.Registry();
    for (const entt::entity e : reg.view<const TerrainComponent>()) return e;
    return entt::null;
}

SurfaceQueryFn MakeTerrainSurfaceQuery(Scene& scene) {
    const entt::entity te = FindTerrain(scene);
    if (te == entt::null) return {}; // no terrain -> empty query (callers scatter on a plane)

    // Capture the terrain component + its world/inverse transforms once. Valid while the
    // terrain is not being sculpted (the streamer/editor fences background bakes vs edits).
    const TerrainComponent& t = scene.Registry().get<const TerrainComponent>(te);
    const glm::mat4 world = scene.WorldMatrix(te);
    const glm::mat4 inv = glm::inverse(world);
    const glm::mat3 normalMat = glm::mat3(world); // uniform/axis-aligned terrains (P2)
    Scene* scenePtr = &scene;

    return [&t, world, inv, normalMat, scenePtr](const glm::vec2& xz) -> SpawnSample {
        SpawnSample s;

        // World XZ -> terrain-local (Y arbitrary; the ground is a height field of local XZ).
        const glm::vec3 local = glm::vec3(inv * glm::vec4(xz.x, 0.0f, xz.y, 1.0f));

        f32 h = 0.0f, dhdx = 0.0f, dhdz = 0.0f;
        bool hole = false;
        const bool inside = terrain::SampleSurface(t, local.x, local.z, h, dhdx, dhdz, hole);
        s.onTerrain = inside && !hole;

        // Altitude: the local ground point pushed back into world space.
        const glm::vec3 worldPos = glm::vec3(world * glm::vec4(local.x, h, local.z, 1.0f));
        s.height = worldPos.y;

        // Normal + slope. Local surface normal from the per-unit gradients.
        const glm::vec3 localNormal = glm::normalize(glm::vec3(-dhdx, 1.0f, -dhdz));
        s.normal = glm::normalize(normalMat * localNormal);
        s.slopeDeg = glm::degrees(std::acos(glm::clamp(s.normal.y, -1.0f, 1.0f)));

        // Dominant splat layer = biome/material proxy. GUARD the mask size (a stale mask
        // is routine - the shipped ref project carries a mismatched one).
        s.splatLayer = -1;
        const u32 gridN = t.GridN();
        if (t.splatEnabled &&
            t.splatWeight.size() == static_cast<usize>(gridN) * gridN * 4u) {
            const f32 step = terrain::SampleStep(t);
            const f32 ext = terrain::ExtentXZ(t);
            if (step > 0.0f) {
                const i32 gx = static_cast<i32>(std::lround((local.x + ext * 0.5f) / step));
                const i32 gz = static_cast<i32>(std::lround((local.z + ext * 0.5f) / step));
                if (gx >= 0 && gz >= 0 && gx < static_cast<i32>(gridN) &&
                    gz < static_cast<i32>(gridN)) {
                    const usize base = (static_cast<usize>(gz) * gridN + gx) * 4u;
                    u8 best = 0; i32 bestLayer = 0;
                    for (i32 c = 0; c < 4; ++c) {
                        const u8 w = t.splatWeight[base + c];
                        if (w >= best) { best = w; bestLayer = c; }
                    }
                    s.splatLayer = bestLayer;
                }
            }
        }

        // Water proximity + a crude moisture proxy (no spatial moisture field exists).
        const f32 waterY = water::SurfaceHeightAt(*scenePtr, xz.x, xz.y);
        s.waterDepth = waterY - s.height; // > 0 => the ground is below the water surface
        const f32 nearWater = glm::clamp(1.0f - glm::clamp(-s.waterDepth, 0.0f, 4.0f) / 4.0f,
                                         0.0f, 1.0f);
        const f32 wetness = scenePtr->Environment().wetness;
        s.moisture = glm::clamp(0.35f * wetness + 0.65f * nearWater, 0.0f, 1.0f);
        return s;
    };
}

} // namespace hbe::veg
