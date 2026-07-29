// Game/DestructionSystem.cpp
#include "Game/DestructionSystem.h"

#include "Assets/Fracture.h"
#include "Core/Log.h"
#include "Physics/PhysicsWorld.h"
#include "Project/Project.h"
#include "Renderer/Renderer.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace hbe::destruction {
namespace {

// A loaded fracture plus its GPU meshes. Cached process-wide and keyed by the
// asset path: a level can have hundreds of identical crates, and each should pay
// the parse + upload exactly once.
struct CachedFracture {
    FractureAsset asset;
    std::vector<rhi::MeshHandle> chunkMeshes; // parallel to asset.chunks
    bool ok = false;
};
std::unordered_map<std::string, CachedFracture> g_cache;

const CachedFracture* GetFracture(Renderer& renderer, const std::string& rel) {
    if (rel.empty() || !Project::HasActive()) return nullptr;
    if (const auto it = g_cache.find(rel); it != g_cache.end())
        return it->second.ok ? &it->second : nullptr;

    CachedFracture cf;
    const std::filesystem::path path = Project::Active().AssetsDir() / rel;
    if (auto loaded = assets::LoadFracture(path)) {
        cf.asset = std::move(*loaded);
        cf.chunkMeshes.reserve(cf.asset.chunks.size());
        for (const FractureChunk& c : cf.asset.chunks)
            cf.chunkMeshes.push_back(renderer.UploadMesh(c.mesh));
        cf.ok = true;
        HBE_INFO("Destruction: loaded '{}' ({} chunks).", rel, cf.asset.chunks.size());
    } else {
        HBE_WARN("Destruction: could not load fracture '{}'.", rel);
    }
    auto [ins, _] = g_cache.emplace(rel, std::move(cf));
    return ins->second.ok ? &ins->second : nullptr;
}

// Lazily sizes the per-chunk runtime arrays. Kept separate from activation so a
// damage hit can score chunks before anything has actually broken.
void EnsureChunkState(Destructible& d, usize chunkCount) {
    if (d.chunkState.size() == chunkCount) return;
    d.chunkState.assign(chunkCount, static_cast<u8>(Destructible::ChunkState::Intact));
    d.chunkHp.assign(chunkCount, d.chunkHealth);
    d.chunkEntity.assign(chunkCount, entt::entity{entt::null});
    d.supportScratch.assign(chunkCount, 0);
}

// Turns the single intact object into one entity per chunk, all still STATIC and
// in their baked positions. Visually a no-op - that is the point: the player
// should not be able to see the moment an object became breakable.
void Activate(Scene& scene, entt::entity root, Destructible& d, const CachedFracture& cf) {
    if (d.activated) return;
    entt::registry& reg = scene.Registry();
    EnsureChunkState(d, cf.asset.chunks.size());

    // Inherit the intact object's material so chunks match it exactly.
    MeshInstance base;
    if (const MeshInstance* mi = reg.try_get<MeshInstance>(root)) base = *mi;

    for (usize i = 0; i < cf.asset.chunks.size(); ++i) {
        if (!cf.chunkMeshes[i].IsValid()) continue;
        const entt::entity ce = scene.CreateEntity("chunk");
        // Chunk geometry is in the SOURCE mesh's object space, so an identity local
        // transform parented to the root reproduces the original silhouette exactly.
        reg.emplace<Transform>(ce, Transform{});
        reg.emplace<Parent>(ce, Parent{root});
        MeshInstance mi = base;
        mi.mesh = cf.chunkMeshes[i];
        reg.emplace<MeshInstance>(ce, mi);
        reg.emplace<DebrisChunk>(ce, DebrisChunk{root, static_cast<u32>(i), 0.0f});
        d.chunkEntity[i] = ce;
    }

    // The root stops drawing - its chunks now represent it. The root entity itself
    // stays (it owns the Destructible state and the transform the chunks parent to).
    reg.remove<MeshInstance>(root);
    if (RigidBody* rb = reg.try_get<RigidBody>(root)) {
        // Drop the intact collider; chunks carry their own from here.
        rb->bodyId = RigidBody::kInvalidBody;
        reg.remove<RigidBody>(root);
    }
    d.activated = true;
}

// Releases one chunk: reparent to world (keeping its world pose), give it a convex
// collider and let physics own it.
void DetachChunk(Scene& scene, entt::entity root, Destructible& d, const CachedFracture& cf,
                 usize idx, const glm::vec3& impulse, const glm::vec3& impulsePoint,
                 PhysicsWorld* physics) {
    if (idx >= d.chunkEntity.size()) return;
    if (d.chunkState[idx] == static_cast<u8>(Destructible::ChunkState::Detached)) return;
    const entt::entity ce = d.chunkEntity[idx];
    entt::registry& reg = scene.Registry();
    if (ce == entt::null || !reg.valid(ce)) {
        d.chunkState[idx] = static_cast<u8>(Destructible::ChunkState::Detached);
        return;
    }

    // Bake the current world transform down, then unparent: the chunk must keep
    // its exact pose while changing who owns it, or it visibly jumps at the moment
    // of breaking - which is precisely the frame the player is looking at.
    const glm::mat4 world = scene.WorldMatrix(ce);
    Transform& xf = reg.get<Transform>(ce);
    xf.position = glm::vec3(world[3]);
    const glm::vec3 c0(world[0]), c1(world[1]), c2(world[2]);
    xf.scale = {glm::length(c0), glm::length(c1), glm::length(c2)};
    const glm::vec3 s = glm::max(xf.scale, glm::vec3(1e-6f));
    xf.rotation = glm::normalize(
        glm::quat_cast(glm::mat3(c0 / s.x, c1 / s.y, c2 / s.z)));
    reg.remove<Parent>(ce);

    const FractureChunk& chunk = cf.asset.chunks[idx];
    RigidBody rb;
    // Voronoi cells are convex BY CONSTRUCTION, so the hull is exact geometry, not
    // an approximation - this is the payoff of fracturing by half-space clipping.
    rb.shape = RigidBody::Shape::ConvexHull;
    rb.motion = RigidBody::Motion::Dynamic;
    rb.friction = 0.6f;
    rb.restitution = 0.05f; // rubble should not bounce like a ball
    rb.centerOffset = chunk.centroid;
    rb.collisionVertices.reserve(chunk.mesh.vertices.size());
    for (const Vertex& v : chunk.mesh.vertices) rb.collisionVertices.push_back(v.position);
    reg.emplace_or_replace<RigidBody>(ce, std::move(rb));

    d.chunkState[idx] = static_cast<u8>(Destructible::ChunkState::Detached);

    // Park the impulse: the body does not exist until PhysicsWorld::Update runs, so
    // applying it now would be silently dropped. Update() delivers it once bodyId
    // becomes valid (see the pending-impulse pass there).
    (void)physics;
    if (glm::length(impulse) > 1e-4f) {
        if (DebrisChunk* dc = reg.try_get<DebrisChunk>(ce)) {
            dc->pendingImpulse = impulse;
            dc->pendingPoint = impulsePoint;
            dc->hasImpulse = true;
        }
    }
}

// Structural integrity: flood fill from the anchored chunks across the adjacency
// graph. Anything unreached has lost its load path and is released.
//
// Runs only when a break actually happened (structureDirty), and reuses a scratch
// buffer, so a static level costs nothing.
void ResolveSupport(Scene& scene, entt::entity root, Destructible& d, const CachedFracture& cf,
                    PhysicsWorld* physics) {
    if (!d.structural || !d.structureDirty) return;
    d.structureDirty = false;
    const usize n = cf.asset.chunks.size();
    ComputeSupport(cf.asset, d.chunkState, d.supportScratch);

    usize released = 0;
    for (usize i = 0; i < n; ++i) {
        if (d.supportScratch[i]) continue;
        if (d.chunkState[i] == static_cast<u8>(Destructible::ChunkState::Detached)) continue;
        DetachChunk(scene, root, d, cf, i, glm::vec3(0.0f), glm::vec3(0.0f), physics);
        ++released;
    }
    if (released > 0)
        HBE_INFO("Destruction: {} chunk(s) lost structural support.", released);
}

} // namespace

void ClearFractureCache() { g_cache.clear(); }

void ComputeSupport(const FractureAsset& asset, const std::vector<u8>& chunkState,
                    std::vector<u8>& outSupported) {
    const usize n = asset.chunks.size();
    outSupported.assign(n, 0u);
    const auto detached = [&](usize i) {
        return i < chunkState.size() &&
               chunkState[i] == static_cast<u8>(Destructible::ChunkState::Detached);
    };

    // Seed: every anchored chunk that is still present.
    std::vector<u32> stack;
    for (usize i = 0; i < n; ++i) {
        if (detached(i) || !asset.chunks[i].anchored) continue;
        outSupported[i] = 1;
        stack.push_back(static_cast<u32>(i));
    }
    // Conduct support across welded neighbours. A detached chunk is a hole in the
    // graph - it neither anchors nor passes support through.
    while (!stack.empty()) {
        const u32 cur = stack.back();
        stack.pop_back();
        for (const u32 nb : asset.chunks[cur].neighbours) {
            if (nb >= n || outSupported[nb] || detached(nb)) continue;
            outSupported[nb] = 1;
            stack.push_back(nb);
        }
    }
}

bool ApplyDamageAt(Scene& scene, Renderer& renderer, entt::entity e,
                   const glm::vec3& worldPoint, f32 damage, const glm::vec3& impulse) {
    entt::registry& reg = scene.Registry();
    Destructible* d = reg.try_get<Destructible>(e);
    if (!d || damage <= 0.0f) return false;
    const CachedFracture* cf = GetFracture(renderer, d->asset);
    if (!cf || cf->asset.chunks.empty()) return false;

    EnsureChunkState(*d, cf->asset.chunks.size());
    Activate(scene, e, *d, *cf);

    // Damage is applied in the ROOT's local space so the baked centroids can be
    // compared directly against the hit point.
    const glm::mat4 inv = glm::inverse(scene.WorldMatrix(e));
    const glm::vec3 localHit = glm::vec3(inv * glm::vec4(worldPoint, 1.0f));
    const f32 r = glm::max(d->damageRadius, 1e-3f);

    bool broke = false;
    for (usize i = 0; i < cf->asset.chunks.size(); ++i) {
        if (d->chunkState[i] == static_cast<u8>(Destructible::ChunkState::Detached)) continue;
        const f32 dist = glm::distance(cf->asset.chunks[i].centroid, localHit);
        if (dist > r) continue;
        // Linear falloff: the chunk under the hit takes full damage, the edge of
        // the radius takes none.
        const f32 falloff = 1.0f - (dist / r);
        d->chunkHp[i] -= damage * falloff;
        if (d->chunkHp[i] > 0.0f) continue;
        DetachChunk(scene, e, *d, *cf, i, impulse * d->breakImpulseScale, worldPoint, nullptr);
        broke = true;
    }
    if (broke) d->structureDirty = true;
    return broke;
}

void Shatter(Scene& scene, Renderer& renderer, entt::entity e, const glm::vec3& impulseOrigin,
             f32 impulseStrength) {
    entt::registry& reg = scene.Registry();
    Destructible* d = reg.try_get<Destructible>(e);
    if (!d) return;
    const CachedFracture* cf = GetFracture(renderer, d->asset);
    if (!cf || cf->asset.chunks.empty()) return;
    EnsureChunkState(*d, cf->asset.chunks.size());
    Activate(scene, e, *d, *cf);

    const glm::mat4 world = scene.WorldMatrix(e);
    for (usize i = 0; i < cf->asset.chunks.size(); ++i) {
        if (d->chunkState[i] == static_cast<u8>(Destructible::ChunkState::Detached)) continue;
        // Push each chunk radially away from the blast origin.
        const glm::vec3 wc = glm::vec3(world * glm::vec4(cf->asset.chunks[i].centroid, 1.0f));
        glm::vec3 dir = wc - impulseOrigin;
        const f32 len = glm::length(dir);
        dir = len > 1e-4f ? dir / len : glm::vec3(0.0f, 1.0f, 0.0f);
        DetachChunk(scene, e, *d, *cf, i, dir * impulseStrength * d->breakImpulseScale, wc,
                    nullptr);
    }
    d->structureDirty = false; // everything is already loose
}

void Update(Scene& scene, Renderer& renderer, PhysicsWorld& physics, f32 dt) {
    entt::registry& reg = scene.Registry();

    // --- 1. Contacts -> breaks ---------------------------------------------
    // The physics contact queue is drained here (it had no consumer before). Only
    // impacts above the object's own threshold do anything, so resting/sliding
    // contacts are free.
    PhysicsWorld::ContactEvent ev;
    while (physics.PopContact(ev)) {
        for (const entt::entity side : {ev.a, ev.b}) {
            if (side == entt::null || !reg.valid(side)) continue;
            Destructible* d = reg.try_get<Destructible>(side);
            if (!d || ev.impulse < d->impulseThreshold) continue;
            // Damage scales with how much the impact exceeded the threshold, so a
            // graze chips and a hard hit removes a section.
            const f32 over = ev.impulse - d->impulseThreshold;
            const f32 damage = d->chunkHealth * (0.5f + over / glm::max(d->impulseThreshold, 1.0f));
            ApplyDamageAt(scene, renderer, side, ev.point, damage,
                          -ev.normal * ev.impulse * 0.5f);
        }
    }

    // --- 2. Structural support ---------------------------------------------
    for (const entt::entity e : reg.view<Destructible>()) {
        Destructible& d = reg.get<Destructible>(e);
        if (!d.structureDirty) continue;
        if (const CachedFracture* cf = GetFracture(renderer, d.asset))
            ResolveSupport(scene, e, d, *cf, &physics);
    }

    // --- 3. Deliver parked impulses ----------------------------------------
    // A chunk detaches before its body exists, so the push it was given is applied
    // here on the first tick where PhysicsWorld has actually created the body.
    // Applied AT THE IMPACT POINT so debris tumbles rather than sliding out flat.
    for (const entt::entity e : reg.view<DebrisChunk, RigidBody>()) {
        DebrisChunk& dc = reg.get<DebrisChunk>(e);
        if (!dc.hasImpulse) continue;
        if (reg.get<RigidBody>(e).bodyId == RigidBody::kInvalidBody) continue; // not yet
        physics.AddImpulseAtPoint(scene, e, dc.pendingImpulse, dc.pendingPoint);
        dc.hasImpulse = false;
    }

    // --- 4. Debris ageing ---------------------------------------------------
    // Freed chunks are removed after their lifetime so a long firefight cannot
    // grow the body count without bound.
    std::vector<entt::entity> expired;
    for (const entt::entity e : reg.view<DebrisChunk>()) {
        DebrisChunk& dc = reg.get<DebrisChunk>(e);
        // Only DETACHED chunks age; ones still standing in place are part of the
        // object, not debris.
        const Destructible* owner =
            (dc.owner != entt::null && reg.valid(dc.owner)) ? reg.try_get<Destructible>(dc.owner)
                                                            : nullptr;
        if (!owner || dc.index >= owner->chunkState.size()) continue;
        if (owner->chunkState[dc.index] != static_cast<u8>(Destructible::ChunkState::Detached))
            continue;
        if (owner->debrisLifetime <= 0.0f) continue; // 0 = keep forever
        dc.age += dt;
        if (dc.age >= owner->debrisLifetime) expired.push_back(e);
    }
    for (const entt::entity e : expired) {
        if (const DebrisChunk* dc = reg.try_get<DebrisChunk>(e);
            dc && dc->owner != entt::null && reg.valid(dc->owner)) {
            if (Destructible* od = reg.try_get<Destructible>(dc->owner);
                od && dc->index < od->chunkEntity.size())
                od->chunkEntity[dc->index] = entt::null; // clear the dangling handle
        }
        if (reg.valid(e)) reg.destroy(e);
    }
}

} // namespace hbe::destruction
