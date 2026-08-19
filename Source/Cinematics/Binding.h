// Cinematics/Binding.h - resolves a Sequence's Bindings to live scene entities.
//
// A sequence references actors by stable EntityGuid (with a Name fallback) rather
// than by entt handle, so it is reusable across loads and compatible scenes
// (spec §12). BindingResolver caches binding-id -> entt::entity per running
// instance, re-resolving lazily if the cache goes stale (an entity was destroyed).
// It also owns SPAWNABLE actors: entities the sequence itself spawns for its
// lifetime and destroys on stop.
#pragma once

#include "Core/Types.h"

#include <entt/entt.hpp>

#include <filesystem>
#include <unordered_map>
#include <vector>

namespace hbe {
class Scene;

namespace cine {

struct Sequence;
struct Binding;

class BindingResolver {
public:
    // Resolves binding `id` in `seq` to a live entity, using the cache when valid.
    // Possessable bindings resolve by guid (then Name); Spawnable bindings spawn
    // their actor on first resolve. Returns entt::null when unresolvable.
    entt::entity Resolve(const Sequence& seq, int id, Scene& scene,
                         const std::filesystem::path& assetsDir);

    // Destroys every spawnable this resolver created and clears the cache. Called
    // when an instance stops (spec §12: spawnables live only for the sequence).
    void ReleaseSpawnables(Scene& scene);

    // Drops cached resolutions without touching spawnables (e.g. after a scene
    // reload where possessables must be re-found but spawnables persist).
    void InvalidateCache() { cache_.clear(); }

    // True if `e` was spawned by this resolver (so the engine knows not to persist
    // it and the editor knows it is sequence-owned).
    bool IsSpawnable(entt::entity e) const;

private:
    entt::entity ResolvePossessable(const Binding& b, Scene& scene);
    entt::entity SpawnBinding(const Binding& b, Scene& scene,
                              const std::filesystem::path& assetsDir);

    std::unordered_map<int, entt::entity> cache_;      // binding id -> entity
    std::unordered_map<int, entt::entity> spawned_;    // binding id -> spawned entity
};

} // namespace cine
} // namespace hbe
