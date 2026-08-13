// Audio/AcousticQuery.cpp - see AcousticQuery.h.
#include "Audio/AcousticQuery.h"

#include "Assets/MaterialAsset.h"
#include "Physics/PhysicsWorld.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <glm/geometric.hpp>

#include <cmath>
#include <cstdio>
#include <optional>
#include <system_error>

namespace hbe {

AcousticMaterial ResolveEntityAcoustic(const Scene& scene, entt::entity e,
                                       const std::filesystem::path& assetsDir,
                                       AcousticMaterialCache& cache) {
    static const AcousticMaterial kDefault{};
    if (e == entt::null) return kDefault;
    const entt::registry& reg = scene.Registry();
    if (!reg.valid(e)) return kDefault;
    const MaterialRef* mr = reg.try_get<MaterialRef>(e);
    if (!mr || mr->asset.empty()) return kDefault;

    if (const auto it = cache.byPath.find(mr->asset); it != cache.byPath.end()) return it->second;

    AcousticMaterial result = kDefault;
    if (const std::optional<MaterialAsset> mat = assets::LoadMaterial(assetsDir / mr->asset))
        result = mat->acoustic;
    cache.byPath[mr->asset] = result; // negative results cached too (avoid re-hitting a bad path)
    return result;
}

bool AcousticRaycast(const PhysicsWorld& physics, const Scene& scene, const glm::vec3& origin,
                     const glm::vec3& dir, f32 maxDist, const std::filesystem::path& assetsDir,
                     AcousticMaterialCache& cache, AcousticRayHit& out) {
    out = AcousticRayHit{};
    const f32 len = glm::length(dir);
    if (len < 1e-6f || maxDist <= 0.0f) return false;
    const PhysicsWorld::RayHit rh = physics.RaycastDetailed(origin, dir / len, maxDist);
    if (!rh.hit) return false;
    out.hit = true;
    out.distance = rh.distance;
    out.point = rh.point;
    out.normal = rh.normal;
    out.entity = rh.entity;
    out.material = ResolveEntityAcoustic(scene, rh.entity, assetsDir, cache);
    return true;
}

bool AcousticSelfTest() {
    bool ok = true;
    const auto check = [&](bool cond, const char* msg) {
        if (!cond) {
            std::printf("  [acoustics] FAIL: %s\n", msg);
            ok = false;
        }
    };

    // 1) Preset library integrity.
    const std::vector<AcousticPreset>& presets = AcousticPresets();
    check(!presets.empty(), "preset table empty");
    check(!presets.empty() && std::string(presets[0].name) == "Default", "preset[0] != Default");
    for (const AcousticPreset& p : presets) {
        for (f32 a : p.material.absorption) check(a >= 0.0f && a <= 1.0f, "absorption out of [0,1]");
        check(p.material.scattering >= 0.0f && p.material.scattering <= 1.0f, "scattering out of range");
        check(p.material.transmission >= 0.0f && p.material.transmission <= 1.0f,
              "transmission out of range");
        check(FindAcousticPreset(p.name) != nullptr, "FindAcousticPreset missed a listed preset");
    }
    if (const AcousticMaterial* open = FindAcousticPreset("Open / Air")) {
        check(open->transmission >= 0.999f, "Open/Air not fully transmissive");
        check(open->absorption[0] >= 0.999f, "Open/Air not fully absorbing (should not reflect)");
    } else {
        check(false, "Open / Air preset missing");
    }
    check(FindAcousticPreset("does-not-exist") == nullptr, "FindAcousticPreset found a bogus name");

    // 2) .hbmat acoustic round-trip (save -> load), including a hand value that no preset holds.
    const std::filesystem::path tmp =
        std::filesystem::temp_directory_path() / "hbe_acoustic_selftest.hbmat";
    MaterialAsset m;
    m.acousticPreset = "Concrete (sealed)";
    if (const AcousticMaterial* c = FindAcousticPreset("Concrete (sealed)")) m.acoustic = *c;
    m.acoustic.transmission = 0.4321f;
    m.acoustic.absorption[3] = 0.777f;
    check(assets::SaveMaterial(tmp, m), "SaveMaterial failed");
    const std::optional<MaterialAsset> loaded = assets::LoadMaterial(tmp);
    check(loaded.has_value(), "LoadMaterial failed");
    if (loaded) {
        check(loaded->acousticPreset == "Concrete (sealed)", "preset label not round-tripped");
        check(std::fabs(loaded->acoustic.transmission - 0.4321f) < 1e-4f,
              "transmission not round-tripped");
        check(std::fabs(loaded->acoustic.absorption[3] - 0.777f) < 1e-4f,
              "absorption band not round-tripped");
    }

    // 3) entity -> material resolution + caching. MaterialRef resolves to the .hbmat's acoustic;
    //    a bare entity and entt::null return the default; the cache stores exactly one path.
    {
        Scene scene;
        AcousticMaterialCache cache;
        const std::filesystem::path assetsDir = tmp.parent_path();
        const std::string rel = tmp.filename().string();
        const AcousticMaterial kDefault{};

        const entt::entity withMat = scene.Registry().create();
        scene.Registry().emplace<MaterialRef>(withMat, MaterialRef{rel});
        const AcousticMaterial r = ResolveEntityAcoustic(scene, withMat, assetsDir, cache);
        check(std::fabs(r.transmission - 0.4321f) < 1e-4f, "entity resolve: wrong transmission");
        // Repeat hit is served from the cache (same value).
        const AcousticMaterial r2 = ResolveEntityAcoustic(scene, withMat, assetsDir, cache);
        check(std::fabs(r2.transmission - 0.4321f) < 1e-4f, "cached resolve: wrong transmission");

        const entt::entity bare = scene.Registry().create();
        check(std::fabs(ResolveEntityAcoustic(scene, bare, assetsDir, cache).transmission -
                        kDefault.transmission) < 1e-6f,
              "bare entity (no MaterialRef) not default");
        check(std::fabs(ResolveEntityAcoustic(scene, entt::null, assetsDir, cache).transmission -
                        kDefault.transmission) < 1e-6f,
              "null entity not default");
        check(cache.byPath.size() == 1, "cache did not store exactly one resolved path");
    }

    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    return ok;
}

} // namespace hbe
