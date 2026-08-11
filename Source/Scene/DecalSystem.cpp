// Scene/DecalSystem.cpp - see DecalSystem.h.
#include "Scene/DecalSystem.h"

#include "Assets/AssetLoader.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

namespace hbe::decal {

void Update(Scene& scene, Renderer& renderer, const std::filesystem::path& assetsDir) {
    auto& reg = scene.Registry();
    for (const entt::entity e : reg.view<DecalComponent>()) {
        DecalComponent& d = reg.get<DecalComponent>(e);
        if (d.resolved) continue;
        d.resolved = true; // resolve once; the editor clears this when a path changes
        const auto load = [&](const std::string& rel) -> rhi::TextureHandle {
            if (rel.empty() || assetsDir.empty()) return {};
            return assets::LoadTexture(renderer, assetsDir / rel);
        };
        d.albedo = load(d.albedoTex);
        d.normal = load(d.normalTex);
        d.mr = load(d.mrTex);
    }
}

} // namespace hbe::decal
