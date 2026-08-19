// Cinematics/Binding.cpp - resolve sequence bindings to live scene entities.
#include "Cinematics/Binding.h"

#include "Cinematics/Sequence.h"
#include "Core/Log.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

namespace hbe::cine {

entt::entity BindingResolver::Resolve(const Sequence& seq, int id, Scene& scene,
                                      const std::filesystem::path& assetsDir) {
    auto it = cache_.find(id);
    if (it != cache_.end() && scene.Registry().valid(it->second)) return it->second;

    const Binding* b = seq.FindBinding(id);
    if (!b) return entt::null;

    entt::entity e = (b->kind == BindingKind::Spawnable) ? SpawnBinding(*b, scene, assetsDir)
                                                         : ResolvePossessable(*b, scene);
    if (e != entt::null) cache_[id] = e;
    return e;
}

entt::entity BindingResolver::ResolvePossessable(const Binding& b, Scene& scene) {
    auto& reg = scene.Registry();
    // Primary: stable EntityGuid.
    if (b.guid != 0) {
        for (entt::entity e : reg.view<Guid>()) {
            if (reg.get<Guid>(e).value == b.guid) return e;
        }
    }
    // Fallback: hashed Name index (also how the legacy cutscene tracks resolve).
    if (!b.name.empty()) {
        entt::entity e = scene.FindByName(b.name);
        if (e != entt::null) return e;
    }
    return entt::null;
}

entt::entity BindingResolver::SpawnBinding(const Binding& b, Scene& scene,
                                           const std::filesystem::path& assetsDir) {
    (void)assetsDir;
    auto it = spawned_.find(b.id);
    if (it != spawned_.end() && scene.Registry().valid(it->second)) return it->second;

    // v1: a bare entity the sequence's other tracks (transform/animation/visibility)
    // can drive and that is destroyed on release. Full mesh/prefab/character content
    // spawning routes through scene::Instantiate in a follow-up (see the design doc's
    // known-limitations); the binding contract (create-once, own-for-lifetime) holds.
    entt::entity e = scene.CreateEntity(b.label.empty() ? "Spawnable" : b.label);
    scene.Registry().emplace_or_replace<Transform>(e);
    if (!b.spawnAsset.empty()) {
        HBE_WARN("Cinematics: spawnable '{}' (asset '{}') created as a bare entity - "
                 "mesh/prefab content spawn is a follow-up",
                 b.label, b.spawnAsset);
    }
    spawned_[b.id] = e;
    cache_[b.id] = e;
    return e;
}

void BindingResolver::ReleaseSpawnables(Scene& scene) {
    auto& reg = scene.Registry();
    for (auto& [id, e] : spawned_) {
        (void)id;
        if (reg.valid(e)) reg.destroy(e);
    }
    spawned_.clear();
    cache_.clear();
}

bool BindingResolver::IsSpawnable(entt::entity e) const {
    for (const auto& [id, se] : spawned_) {
        (void)id;
        if (se == e) return true;
    }
    return false;
}

} // namespace hbe::cine
