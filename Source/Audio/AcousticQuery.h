// Audio/AcousticQuery.h - resolve the acoustic material of a surface at runtime.
//
// The physics-integration seam for physically-informed audio: given a scene entity (or a ray
// hit), find the AcousticMaterial of the surface. Materials live on .hbmat assets, referenced
// per surface by the MaterialRef component; this resolves that link (with a small cache so
// per-ray / per-frame queries do not hit disk). Built ABOVE PhysicsWorld (which already returns
// the hit entity via RaycastDetailed), so no physics-layer coupling to materials is needed.
//
// This is Heartbreak integration-layer code: it knows about the scene, physics and asset
// systems. The spatial backend never sees any of it.
#pragma once

#include "Assets/AcousticMaterial.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <filesystem>
#include <string>
#include <unordered_map>

namespace hbe {

class Scene;
class PhysicsWorld;

// Cache from .hbmat path -> resolved AcousticMaterial, owned by the caller (like the texture
// cache). Clear() after a material is re-imported/edited so the next query reflects it.
struct AcousticMaterialCache {
    std::unordered_map<std::string, AcousticMaterial> byPath;
    void Clear() { byPath.clear(); }
};

// Acoustic material of the surface an entity represents: reads its MaterialRef (.hbmat) and
// returns that material's AcousticMaterial. Entities with no MaterialRef (inline-only materials,
// terrain, invalid/null) return the default AcousticMaterial.
AcousticMaterial ResolveEntityAcoustic(const Scene& scene, entt::entity e,
                                       const std::filesystem::path& assetsDir,
                                       AcousticMaterialCache& cache);

// A ray hit carrying the acoustic material of the surface struck.
struct AcousticRayHit {
    bool hit = false;
    f32 distance = 0.0f;
    glm::vec3 point{0.0f};
    glm::vec3 normal{0.0f};
    entt::entity entity = entt::null;
    AcousticMaterial material; // default when nothing was hit / the surface has no material
};

// Casts a ray through the physics world and resolves the acoustic material at the hit surface
// (via the hit entity's MaterialRef). `dir` need not be normalized; `out.distance` is a true
// world distance. Returns false on a miss (out is reset).
bool AcousticRaycast(const PhysicsWorld& physics, const Scene& scene, const glm::vec3& origin,
                     const glm::vec3& dir, f32 maxDist, const std::filesystem::path& assetsDir,
                     AcousticMaterialCache& cache, AcousticRayHit& out);

// Headless self-test (--test-acoustics): preset-library integrity, .hbmat acoustic round-trip
// (save/load), and entity -> AcousticMaterial resolution + caching. Returns true on pass.
bool AcousticSelfTest();

// Runs the HDS-Resonance acoustics LIBRARY self-test (hdsr::SelfTest) when the backend is built in.
// The gate + fork include live here (engine-lib side) so the exe can call it without the fork's
// compile definitions. Returns true when the library is unavailable (nothing to test).
bool HdsrLibrarySelfTest();

} // namespace hbe
