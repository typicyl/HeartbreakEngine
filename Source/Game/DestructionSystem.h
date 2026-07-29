// Game/DestructionSystem.h - runtime breaking of pre-fractured objects.
//
// The fracture itself is baked offline (Assets/Fracture.h); this is what turns a
// baked `.hbfrac` into an object that actually comes apart when it is shot, hit,
// or blown up.
//
// LIFECYCLE of a Destructible entity:
//   INTACT     one draw, one static body, zero chunk entities. The common case,
//              and the reason a level can be full of breakables for free.
//   ACTIVATED  something exceeded the break threshold. The root's MeshInstance is
//              removed and every chunk becomes a child entity: still STATIC and
//              exactly filling the original silhouette, so activation is visually
//              invisible - the object has not appeared to change, it is just now
//              made of parts.
//   BREAKING   chunks that lose their hp (or their structural support) flip to a
//              DYNAMIC convex-hull body and fall. Voronoi cells are convex by
//              construction, so the collider is exact, not an approximation.
//
// STRUCTURAL INTEGRITY: chunks form an adjacency graph (baked). Some are anchored
// (touching the ground / the rest of the world). After every break a flood fill
// runs from the anchored set; any chunk that can no longer reach an anchor has
// nothing holding it up and is released. That is what makes shooting out a support
// column drop the wall above it, rather than leaving it floating.
#pragma once

#include "Core/Types.h"

#include "Assets/Fracture.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <vector>

namespace hbe {

class Scene;
class Renderer;
class PhysicsWorld;

namespace destruction {

// Per-frame tick: drains physics contacts into break events, resolves structural
// support, and ages out debris. Call while the simulation is running, AFTER
// PhysicsWorld::Update (so this frame's contacts are available).
void Update(Scene& scene, Renderer& renderer, PhysicsWorld& physics, f32 dt);

// Applies damage to whichever chunks lie within `radius` of `worldPoint`, falling
// off with distance. `impulse` is the world-space push applied to any chunk that
// comes loose as a result. Safe to call on an entity without a Destructible.
// Returns true if anything actually broke.
bool ApplyDamageAt(Scene& scene, Renderer& renderer, entt::entity e,
                   const glm::vec3& worldPoint, f32 damage, const glm::vec3& impulse);

// Breaks the object completely: every chunk detaches at once. Used by explosions
// and by scripted destruction ("Shatter" node).
void Shatter(Scene& scene, Renderer& renderer, entt::entity e, const glm::vec3& impulseOrigin,
             f32 impulseStrength);

// Drops the process-wide fracture-asset cache (project switch / re-bake).
void ClearFractureCache();

// Structural support solve, as a PURE function so it can be tested without a GPU
// or a scene. Flood-fills the chunk adjacency graph outward from every chunk that
// is both anchored and still present; `outSupported[i] != 0` means chunk i still
// has a load path to an anchor. Detached chunks neither anchor nor conduct.
//
// Anything present but unsupported has physically nothing holding it up and should
// be released - that is what makes shooting out a support column drop the wall.
// NOTE: an object with NO anchored chunks yields an all-zero result, i.e. total
// collapse on first break. That is correct for a free-standing prop and is why
// anchoring is what distinguishes "wall" from "crate".
void ComputeSupport(const FractureAsset& asset, const std::vector<u8>& chunkState,
                    std::vector<u8>& outSupported);

} // namespace destruction
} // namespace hbe
