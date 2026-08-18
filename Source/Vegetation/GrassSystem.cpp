// Vegetation/GrassSystem.cpp - dense grass-patch mesh + terrain-instanced placement.
#include "Vegetation/GrassSystem.h"
#include "Vegetation/VegetationInterfaces.h"
#include "Assets/MeshDerive.h"   // RecomputeNormalsTangents
#include "Assets/MeshOptimize.h" // OptimizeForGpu
#include "Scene/Scene.h"
#include "Scene/Components.h"
#include "Renderer/Renderer.h"
#include "Core/Rng.h"
#include "Core/Log.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/constants.hpp>

#include <cmath>

namespace hbe::veg {
namespace {

// A single grass BLADE quad (base wide -> tip narrow), leaning by `lean`. Its normal is set
// UP so the blade is lit as if facing the sky (bright, stylized grass) rather than edge-on;
// the patch keeps these manual normals (no RecomputeNormals, which would zero a two-sided
// blade). Multiple crossed blades per tuft give coverage from most view angles.
void AddBlade(MeshData& md, const glm::vec3& base, const glm::vec3& right, f32 width,
              f32 height, const glm::vec3& lean) {
    const glm::vec3 tip = base + glm::vec3(0.0f, height, 0.0f) + lean;
    const glm::vec3 hw = right * (width * 0.5f);
    const glm::vec3 tw = right * (width * 0.18f); // taper toward the tip
    // Sky-facing normal tilted slightly toward the lean, so a clump reads as a soft mound.
    const glm::vec3 n = glm::normalize(glm::vec3(0.0f, 1.0f, 0.0f) + lean * 0.5f);
    const u32 b = md.VertexCount();
    auto push = [&](const glm::vec3& p, const glm::vec2& uv) {
        Vertex v;
        v.position = p;
        v.normal = n;
        v.uv = uv;
        md.vertices.push_back(v);
    };
    push(base - hw, {0.0f, 0.0f});
    push(base + hw, {1.0f, 0.0f});
    push(tip + tw, {1.0f, 1.0f});
    push(tip - tw, {0.0f, 1.0f});
    md.indices.push_back(b + 0); md.indices.push_back(b + 1); md.indices.push_back(b + 2);
    md.indices.push_back(b + 0); md.indices.push_back(b + 2); md.indices.push_back(b + 3);
}

} // namespace

MeshData BuildGrassPatch(u64 seed, const GrassParams& p) {
    MeshData md;
    md.name = "grass_patch";
    md.material.baseColor = p.color;
    md.material.roughness = 0.75f;

    Rng rng(seed);
    const f32 half = p.patchSize * 0.5f;
    const u32 tufts = glm::max(1u, p.tuftsPerPatch);
    for (u32 i = 0; i < tufts; ++i) {
        const glm::vec3 pos(rng.Range(-half, half), 0.0f, rng.Range(-half, half));
        // A tuft = a few crossed blades at slightly different yaws for volume.
        const u32 blades = 2u + (rng.NextU32() % 2u); // 2-3 blades per tuft
        const f32 h = p.bladeHeight * (0.7f + 0.6f * rng.NextFloat());
        for (u32 bld = 0; bld < blades; ++bld) {
            const f32 yaw = rng.NextFloat() * glm::two_pi<f32>();
            const glm::vec3 right(std::cos(yaw), 0.0f, std::sin(yaw));
            const glm::vec3 lean(rng.Signed() * 0.12f, 0.0f, rng.Signed() * 0.12f);
            AddBlade(md, pos, right, p.bladeWidth, h, lean * h);
        }
    }

    // Keep the manual sky-facing normals (do NOT RecomputeNormals - it would zero the
    // vertical blades / average crossed blades to nothing). Optimize vertex/index order
    // only; the default tangent is fine (grass carries no normal map).
    mesh::OptimizeForGpu(md);
    return md;
}

u32 PopulateGrass(Scene& scene, Renderer& renderer, const SurfaceQueryFn& surface,
                  const glm::vec2& aabbMin, const glm::vec2& aabbMax, u64 seed,
                  const GrassParams& p) {
    const MeshData patch = BuildGrassPatch(seed, p);
    if (patch.Empty()) return 0;
    const rhi::MeshHandle mesh = renderer.UploadMesh(patch);
    if (!mesh.IsValid()) return 0;

    const f32 step = glm::max(0.5f, p.spacing);
    const f32 jit = p.jitter * step;
    u32 spawned = 0;
    for (f32 z = aabbMin.y; z < aabbMax.y; z += step) {
        for (f32 x = aabbMin.x; x < aabbMax.x; x += step) {
            // Deterministic per-cell jitter + yaw.
            Hasher hsh; hsh.Mix(seed);
            hsh.Mix(static_cast<i32>(std::lround(x / step)));
            hsh.Mix(static_cast<i32>(std::lround(z / step)));
            Rng cell(hsh.Value());
            const glm::vec2 xz(x + cell.Signed() * jit, z + cell.Signed() * jit);

            if (surface) {
                const SpawnSample ss = surface(xz);
                if (!ss.onTerrain || ss.waterDepth > 0.0f) continue;
                if (ss.slopeDeg > p.slopeLimitDeg) continue;

                Transform t;
                t.position = glm::vec3(xz.x, ss.height - 0.02f, xz.y);
                t.rotation = glm::angleAxis(cell.NextFloat() * glm::two_pi<f32>(),
                                            glm::vec3(0.0f, 1.0f, 0.0f));
                const entt::entity e = scene.CreateEntity("Grass");
                scene.Registry().emplace<Transform>(e, t);
                MeshInstance mi;
                mi.mesh = mesh;
                mi.surface.base_color = p.color;
                mi.surface.specular_roughness = 0.75f;
                scene.Registry().emplace<MeshInstance>(e, mi);
                ++spawned;
            }
        }
    }
    HBE_INFO("grass: spawned {} patches", spawned);
    return spawned;
}

} // namespace hbe::veg
