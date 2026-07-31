// Physics/PhysicsWorld.cpp - Jolt-backed implementation.
#include "Physics/PhysicsWorld.h"

#include "Core/Log.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scene/TerrainSystem.h" // heightfield layout + hole-mask semantics

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hbe {
namespace {

#ifdef JPH_ENABLE_ASSERTS
// JOLT'S ASSERTS MUST NOT BE A SILENT __debugbreak. Jolt enables assertions in
// Debug builds only, and its default handler returns true = "break here". With no
// debugger attached that is an unhandled 0x80000003 and the process dies with one
// line in the log: an ADDRESS, no file, no condition. That made every Debug-only
// physics assert look like a mystery crash in whatever ran last.
//
// Log it and return false ("do not break") instead, so a Debug run behaves like
// Release - the assert is reported, loudly and with its condition, and the frame
// continues. It stays a real signal (HBE_ERROR, so a test harness sees it) without
// being fatal in a headless run that has no debugger to catch it.
bool JoltAssertFailed(const char* expr, const char* message, const char* file, u32 line) {
    HBE_ERROR("Jolt ASSERT: {} ({}:{}) - {}", expr ? expr : "?", file ? file : "?", line,
              message ? message : "");
    return false; // do not breakpoint
}
#endif

// One-time global Jolt setup (allocator, type factory, collision dispatch).
void EnsureJoltGlobalInit() {
    static bool done = false;
    if (done) return;
    done = true;
    JPH::RegisterDefaultAllocator();
#ifdef JPH_ENABLE_ASSERTS
    JPH::AssertFailed = JoltAssertFailed;
#endif
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
}

// Two object layers: static world vs. simulated bodies.
namespace Layers {
constexpr JPH::ObjectLayer NON_MOVING = 0;
constexpr JPH::ObjectLayer MOVING = 1;
constexpr u32 NUM_LAYERS = 2;
} // namespace Layers

namespace BroadPhaseLayers {
constexpr JPH::BroadPhaseLayer NON_MOVING(0);
constexpr JPH::BroadPhaseLayer MOVING(1);
constexpr u32 NUM_LAYERS = 2;
} // namespace BroadPhaseLayers

class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::NUM_LAYERS; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return layer == Layers::NON_MOVING ? BroadPhaseLayers::NON_MOVING
                                           : BroadPhaseLayers::MOVING;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return layer == BroadPhaseLayers::NON_MOVING ? "NON_MOVING" : "MOVING";
    }
#endif
};

class ObjectVsBroadPhaseLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer1, JPH::BroadPhaseLayer layer2) const override {
        if (layer1 == Layers::NON_MOVING) return layer2 == BroadPhaseLayers::MOVING;
        return true;
    }
};

class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        if (a == Layers::NON_MOVING) return b == Layers::MOVING;
        return true;
    }
};

JPH::Vec3 ToJph(const glm::vec3& v) { return {v.x, v.y, v.z}; }
JPH::Quat ToJph(const glm::quat& q) { return {q.x, q.y, q.z, q.w}; }
glm::vec3 ToGlm(JPH::RVec3Arg v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
glm::quat ToGlm(JPH::QuatArg q) { return {q.GetW(), q.GetX(), q.GetY(), q.GetZ()}; }

// Builds an exact collider from the RigidBody's mesh geometry (local space,
// unscaled - the caller wraps it in a ScaledShape). Dynamic bodies can't use a
// triangle mesh in Jolt, so a dynamic Mesh degrades to a convex hull. Returns
// null when there's no geometry (the caller then falls back to a box).
JPH::ShapeRefC BuildMeshCollider(const RigidBody& rb, bool dynamic) {
    if (rb.collisionVertices.empty()) return nullptr;
    const bool hull = rb.shape == RigidBody::Shape::ConvexHull || dynamic;
    if (hull) {
        JPH::Array<JPH::Vec3> pts;
        pts.reserve(rb.collisionVertices.size());
        for (const glm::vec3& v : rb.collisionVertices) pts.push_back({v.x, v.y, v.z});
        JPH::ConvexHullShapeSettings settings(pts);
        JPH::ShapeSettings::ShapeResult res = settings.Create();
        return res.IsValid() ? res.Get() : JPH::ShapeRefC{};
    }
    JPH::VertexList verts;
    verts.reserve(rb.collisionVertices.size());
    for (const glm::vec3& v : rb.collisionVertices) verts.push_back({v.x, v.y, v.z});
    JPH::IndexedTriangleList tris;
    if (!rb.collisionIndices.empty()) {
        tris.reserve(rb.collisionIndices.size() / 3);
        for (usize i = 0; i + 2 < rb.collisionIndices.size(); i += 3) {
            tris.push_back(JPH::IndexedTriangle(rb.collisionIndices[i], rb.collisionIndices[i + 1],
                                                rb.collisionIndices[i + 2], 0));
        }
    } else { // non-indexed: consecutive triples
        for (u32 i = 0; i + 2 < static_cast<u32>(verts.size()); i += 3) {
            tris.push_back(JPH::IndexedTriangle(i, i + 1, i + 2, 0));
        }
    }
    if (tris.empty()) return nullptr;
    JPH::MeshShapeSettings settings(verts, tris);
    settings.Sanitize(); // drop degenerate / duplicate triangles
    JPH::ShapeSettings::ShapeResult res = settings.Create();
    return res.IsValid() ? res.Get() : JPH::ShapeRefC{};
}

// --- Terrain heightfield collider ------------------------------------------
//
// Vertical headroom (metres) added on each side of the authored height span when a
// terrain heightfield is created. HeightFieldShape quantizes to 16 bits against the
// range it was CREATED with and SetHeights SILENTLY CLAMPS anything outside it, so
// without this pad a terrain that starts flat could never be sculpted upward - the
// ground would just refuse to rise, with no error anywhere. 64 m of slack costs
// about 2 mm of quantization on a typical terrain, and sculpting past it is caught
// and answered with a full shape rebuild (see SyncTerrainColliders).
constexpr f32 kTerrainHeightPad = 64.0f;

// Heightfield block size. 2 is Jolt's default: it is the finest SetHeights
// granularity (a brush stroke then recompresses only the 2x2 sample blocks it
// touched) and the fastest to query; larger blocks trade query speed for a smaller
// acceleration structure.
constexpr u32 kTerrainBlockSize = 2;

// A dirty rect at least this fraction of the whole field is answered with a fresh
// shape instead of SetHeights - at that size the in-place path has no advantage and
// a rebuild also re-derives the quantization range.
constexpr f32 kTerrainFullRebuildFraction = 0.7f;

// Writes the heightfield samples for the rect [x0, x0+sx) x [z0, z0+sz) into `out`
// (row-major, `stride` floats per row).
//
// Two substitutions, both mandatory:
//   * a HOLE becomes Jolt's cNoCollisionValue, so a hole painted in the terrain is a
//     hole you fall through and not just a hole you can see through;
//   * samples past GridN() are the PADDING Jolt adds when it rounds the sample count
//     up to a multiple of the block size. They must stay no-collision, or the
//     terrain grows a phantom lip along its far edge.
void FillTerrainSamples(const TerrainComponent& t, u32 x0, u32 z0, u32 sx, u32 sz, f32* out,
                        u32 stride) {
    const u32 n = t.GridN();
    for (u32 j = 0; j < sz; ++j) {
        const u32 gz = z0 + j;
        f32* row = out + static_cast<usize>(j) * stride;
        for (u32 i = 0; i < sx; ++i) {
            const u32 gx = x0 + i;
            if (gx >= n || gz >= n ||
                terrain::IsHole(t, static_cast<i32>(gx), static_cast<i32>(gz))) {
                row[i] = JPH::HeightFieldShapeConstants::cNoCollisionValue;
                continue;
            }
            row[i] = t.heights[static_cast<usize>(gz) * n + gx];
        }
    }
}

// Builds the (unscaled, terrain-local) heightfield shape for `t`. The surface Jolt
// derives is `offset + scale * (x, sample, z)`, which is exactly the layout
// TerrainSystem documents - terrain centred on the entity origin, `heights` indexed
// [gz * GridN + gx]. Returns null (and logs) when Jolt rejects the field.
JPH::Ref<JPH::HeightFieldShape> BuildTerrainField(const TerrainComponent& t) {
    const u32 n = t.GridN();
    if (n < 3 || t.heights.size() != static_cast<usize>(n) * n) return {};

    const f32 step = terrain::SampleStep(t);
    const f32 half = terrain::ExtentXZ(t) * 0.5f;

    JPH::HeightFieldShapeSettings settings;
    settings.mOffset = JPH::Vec3(-half, 0.0f, -half);
    settings.mScale = JPH::Vec3(step, 1.0f, step);
    settings.mSampleCount = n;
    settings.mBlockSize = kTerrainBlockSize;
    settings.mBitsPerSample = 8; // relative to each block's own range: sub-mm here
    settings.mHeightSamples.resize(static_cast<usize>(n) * n);
    FillTerrainSamples(t, 0, 0, n, n, settings.mHeightSamples.data(), n);

    // Widen the quantization range beyond the authored span so later sculpting has
    // somewhere to go (see kTerrainHeightPad). Jolt only widens: any sample outside
    // these bounds overrides them.
    f32 lo = 0.0f, hi = 0.0f;
    bool any = false;
    for (const f32 h : settings.mHeightSamples) {
        if (h == JPH::HeightFieldShapeConstants::cNoCollisionValue) continue;
        lo = any ? glm::min(lo, h) : h;
        hi = any ? glm::max(hi, h) : h;
        any = true;
    }
    settings.mMinHeightValue = lo - kTerrainHeightPad;
    settings.mMaxHeightValue = hi + kTerrainHeightPad;

    // Constructed directly rather than through settings.Create(), because the
    // concrete HeightFieldShape - not the base Shape the factory hands back - is
    // what SetHeights lives on, and that reference has to survive.
    JPH::ShapeSettings::ShapeResult res;
    JPH::Ref<JPH::HeightFieldShape> field = new JPH::HeightFieldShape(settings, res);
    if (!res.IsValid()) {
        HBE_ERROR("Physics: terrain heightfield rejected by Jolt ({} samples/side): {}", n,
                  res.GetError().c_str());
        return {};
    }
    return field;
}

// Position/rotation/scale of a world matrix (no shear support).
void DecomposeWorld(const glm::mat4& m, glm::vec3& pos, glm::quat& rot, glm::vec3& scale) {
    pos = glm::vec3(m[3]);
    const glm::vec3 c0(m[0]), c1(m[1]), c2(m[2]);
    scale = {glm::length(c0), glm::length(c1), glm::length(c2)};
    const glm::vec3 s = glm::max(scale, glm::vec3(1e-6f));
    rot = glm::normalize(glm::quat_cast(glm::mat3(c0 / s.x, c1 / s.y, c2 / s.z)));
}

} // namespace

// Queues significant collisions for the main thread.
//
// Jolt calls this from PHYSICS WORKER THREADS in the middle of a step, so it must
// not touch the ECS or any engine state - it records body ids + geometry under a
// mutex and the main thread resolves them to entities after Update() returns.
// The impulse threshold is applied HERE so resting/sliding contacts (which fire
// every frame for every stacked body) are discarded before they ever allocate.
class ContactCollector final : public JPH::ContactListener {
public:
    struct Raw {
        u32 bodyA = 0, bodyB = 0;
        glm::vec3 point{0.0f};
        glm::vec3 normal{0.0f};
        f32 impulse = 0.0f;
    };

    void SetThreshold(f32 t) { threshold_ = t; }

    void OnContactAdded(const JPH::Body& a, const JPH::Body& b,
                        const JPH::ContactManifold& manifold,
                        JPH::ContactSettings&) override {
        Record(a, b, manifold);
    }
    // Deliberately NOT hooking OnContactPersisted: a resting stack re-fires it
    // every step forever, which would flood the queue with non-events.

private:
    void Record(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold) {
        // Jolt does not hand the applied impulse to OnContactAdded (it has not been
        // solved yet), so estimate the collision severity from the RELATIVE NORMAL
        // VELOCITY scaled by the reduced mass - which is what an impulse is, and is
        // stable regardless of timestep.
        const JPH::Vec3 n = manifold.mWorldSpaceNormal;
        const JPH::RVec3 p = manifold.GetWorldSpaceContactPointOn1(0);
        const JPH::Vec3 va = a.GetLinearVelocity(), vb = b.GetLinearVelocity();
        const f32 vRel = std::abs((va - vb).Dot(n));

        // ORDER MATTERS, and it was backwards. `Body::GetMotionProperties()` asserts
        // `!IsStatic()` INSIDE the getter, so calling it first and testing IsStatic
        // second fired that assert on every contact involving a static body - i.e.
        // most of them. In Release the getter just returns null and the guard held,
        // so this was invisible outside Debug, where it was a bare __debugbreak that
        // killed the process. Test the state first, and read the pointer with the
        // explicitly-unchecked accessor.
        const f32 invMa = !a.IsStatic() && a.GetMotionPropertiesUnchecked()
                              ? a.GetMotionPropertiesUnchecked()->GetInverseMass() : 0.0f;
        const f32 invMb = !b.IsStatic() && b.GetMotionPropertiesUnchecked()
                              ? b.GetMotionPropertiesUnchecked()->GetInverseMass() : 0.0f;
        const f32 invSum = invMa + invMb;
        if (invSum <= 0.0f) return;              // static vs static: nothing to report
        const f32 reducedMass = 1.0f / invSum;
        const f32 impulse = vRel * reducedMass;
        if (impulse < threshold_) return;        // cheap reject before any locking

        Raw r;
        r.bodyA = a.GetID().GetIndexAndSequenceNumber();
        r.bodyB = b.GetID().GetIndexAndSequenceNumber();
        r.point = {static_cast<f32>(p.GetX()), static_cast<f32>(p.GetY()),
                   static_cast<f32>(p.GetZ())};
        r.normal = {n.GetX(), n.GetY(), n.GetZ()};
        r.impulse = impulse;

        std::lock_guard<std::mutex> lock(mutex_);
        // Hard cap: a pile-up must never grow the queue without bound if the game
        // stops draining it.
        constexpr usize kMaxQueued = 512;
        if (queue_.size() < kMaxQueued) queue_.push_back(r);
    }

public:
    bool Pop(Raw& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        out = queue_.front();
        queue_.pop_front();
        return true;
    }
    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
    }

private:
    std::mutex mutex_;
    std::deque<Raw> queue_;
    // Default is high enough that walking/resting never reports, but a thrown prop
    // or a bullet does. Tuned in impulse units (kg*m/s).
    f32 threshold_ = 2.0f;
};

struct PhysicsWorld::Impl {
    JPH::TempAllocatorImpl tempAlloc{10 * 1024 * 1024};
    JPH::JobSystemThreadPool jobs{
        JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
        std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 1)};
    BPLayerInterfaceImpl bpLayers;
    ObjectVsBroadPhaseLayerFilterImpl objVsBp;
    ObjectLayerPairFilterImpl objPair;
    JPH::PhysicsSystem system;
    std::unordered_map<u32, entt::entity> bodyToEntity;
    // CharacterVirtual capsules for CharacterController entities (player movement
    // with gravity + world collision). Keyed by the synthetic id stored on the
    // component; the Ref keeps the character alive.
    std::unordered_map<u32, JPH::Ref<JPH::CharacterVirtual>> characters;
    std::unordered_map<u32, entt::entity> charToEntity;
    u32 nextCharId = 1;
    ContactCollector contacts;

    // One static heightfield body per TerrainComponent, keyed by the SAME body key
    // bodyToEntity uses (GetIndexAndSequenceNumber). The entity finds its record
    // through TerrainComponent::colliderBodyId; `entity` is stored back so a
    // DUPLICATED terrain component (which copies colliderBodyId) is detected instead
    // of hijacking the original's body.
    //
    // `field` is the INNER shape. The body may hold a ScaledShape wrapper, but
    // SetHeights lives on the heightfield itself, so the Ref has to be kept.
    struct TerrainCollider {
        entt::entity entity = entt::null;
        JPH::Ref<JPH::HeightFieldShape> field;
        u32 sampleCount = 0;   // TerrainComponent::GridN() the shape was built from
        f32 step = 0.0f;       // XZ sample spacing baked into the shape
        glm::vec3 scale{1.0f}; // world scale baked into the ScaledShape wrapper
    };
    std::unordered_map<u32, TerrainCollider> terrainColliders;
    // Entities whose heightfield body could not be created (the world is out of
    // bodies). Creation is retried every frame; this only stops the LOG from
    // repeating every frame with it.
    std::unordered_set<u32> terrainBuildFailed;

    Impl() {
        constexpr u32 kMaxBodies = 8192;
        constexpr u32 kMaxBodyPairs = 8192;
        constexpr u32 kMaxContacts = 4096;
        system.Init(kMaxBodies, 0, kMaxBodyPairs, kMaxContacts, bpLayers, objVsBp, objPair);
        system.SetGravity({0.0f, -9.81f, 0.0f});
        system.SetContactListener(&contacts);
    }

    // bodyToEntity keys on GetIndexAndSequenceNumber(), matching what the
    // collector records. Terrain heightfields live in their own map (they are not
    // RigidBody-driven) but must still resolve, or a raycast against the ground
    // would report "no entity" - which is exactly what surface painting and
    // impact routing need to know.
    entt::entity EntityForBody(u32 bodyKey) const {
        const auto it = bodyToEntity.find(bodyKey);
        if (it != bodyToEntity.end()) return it->second;
        const auto tit = terrainColliders.find(bodyKey);
        return tit != terrainColliders.end() ? tit->second.entity : entt::entity{entt::null};
    }
};

PhysicsWorld::PhysicsWorld() {
    EnsureJoltGlobalInit();
    impl_ = std::make_unique<Impl>();
    HBE_INFO("Physics: Jolt world ready.");
}

PhysicsWorld::~PhysicsWorld() {
    Clear();
}

void PhysicsWorld::Clear() {
    if (!impl_) return;
    JPH::BodyInterface& bi = impl_->system.GetBodyInterface();
    for (const auto& [id, e] : impl_->bodyToEntity) {
        const JPH::BodyID body(id);
        bi.RemoveBody(body);
        bi.DestroyBody(body);
    }
    impl_->bodyToEntity.clear();
    for (const auto& [id, rec] : impl_->terrainColliders) {
        const JPH::BodyID body(id);
        bi.RemoveBody(body);
        bi.DestroyBody(body);
    }
    impl_->terrainColliders.clear(); // Refs free the heightfield shapes
    impl_->terrainBuildFailed.clear();
    impl_->characters.clear();       // Refs free the CharacterVirtuals (no system body)
    impl_->charToEntity.clear();
    // Queued contacts reference bodies that no longer exist; dropping them here
    // stops a scene reload from delivering impacts into the new world.
    impl_->contacts.Clear();
    accumulator_ = 0.0f;
}

void PhysicsWorld::SetLinearVelocity(Scene& scene, entt::entity e, const glm::vec3& v) {
    if (!impl_) return;
    const RigidBody* rb = scene.Registry().try_get<RigidBody>(e);
    if (!rb || rb->bodyId == RigidBody::kInvalidBody) return;
    JPH::BodyInterface& bi = impl_->system.GetBodyInterface();
    const JPH::BodyID body(rb->bodyId);
    bi.SetLinearVelocity(body, JPH::Vec3(v.x, v.y, v.z));
    bi.ActivateBody(body); // a sleeping body would swallow the change
}

void PhysicsWorld::AddImpulse(Scene& scene, entt::entity e, const glm::vec3& impulse) {
    if (!impl_) return;
    const RigidBody* rb = scene.Registry().try_get<RigidBody>(e);
    if (!rb || rb->bodyId == RigidBody::kInvalidBody) return;
    JPH::BodyInterface& bi = impl_->system.GetBodyInterface();
    const JPH::BodyID body(rb->bodyId);
    bi.AddImpulse(body, JPH::Vec3(impulse.x, impulse.y, impulse.z));
    bi.ActivateBody(body);
}

void PhysicsWorld::AddImpulseAtPoint(Scene& scene, entt::entity e, const glm::vec3& impulse,
                                     const glm::vec3& worldPoint) {
    if (!impl_) return;
    const RigidBody* rb = scene.Registry().try_get<RigidBody>(e);
    if (!rb || rb->bodyId == RigidBody::kInvalidBody) return;
    JPH::BodyInterface& bi = impl_->system.GetBodyInterface();
    const JPH::BodyID body(rb->bodyId);
    // Off-centre impulse: Jolt derives the resulting torque from the offset to the
    // centre of mass, so debris driven from the real impact point tumbles.
    bi.AddImpulse(body, JPH::Vec3(impulse.x, impulse.y, impulse.z),
                  JPH::RVec3(worldPoint.x, worldPoint.y, worldPoint.z));
    bi.ActivateBody(body);
}

f32 PhysicsWorld::Raycast(const glm::vec3& origin, const glm::vec3& dir, f32 maxDist) const {
    if (!impl_ || maxDist <= 0.0f) return maxDist;
    const JPH::RRayCast ray{ToJph(origin), ToJph(dir) * maxDist};
    JPH::RayCastResult result;
    if (impl_->system.GetNarrowPhaseQuery().CastRay(ray, result)) {
        return result.mFraction * maxDist; // mFraction in [0,1] along the ray
    }
    return maxDist;
}

PhysicsWorld::RayHit PhysicsWorld::RaycastDetailed(const glm::vec3& origin, const glm::vec3& dir,
                                                   f32 maxDist) const {
    RayHit out;
    if (!impl_ || maxDist <= 0.0f) return out;
    const glm::vec3 unit = glm::length(dir) > 1e-6f ? glm::normalize(dir) : glm::vec3(0, 0, -1);
    const JPH::RRayCast ray{ToJph(origin), ToJph(unit) * maxDist};
    JPH::RayCastResult result;
    if (!impl_->system.GetNarrowPhaseQuery().CastRay(ray, result)) return out;

    out.hit = true;
    out.distance = result.mFraction * maxDist;
    out.point = origin + unit * out.distance;
    out.entity = impl_->EntityForBody(result.mBodyID.GetIndexAndSequenceNumber());
    // The surface normal needs the body locked - it is queried off the shape at the
    // hit sub-shape, which requires the body to stay alive for the duration.
    const JPH::BodyLockInterfaceLocking& lockIface = impl_->system.GetBodyLockInterface();
    JPH::BodyLockRead lock(lockIface, result.mBodyID);
    if (lock.Succeeded()) {
        const JPH::Body& body = lock.GetBody();
        const JPH::Vec3 n = body.GetWorldSpaceSurfaceNormal(result.mSubShapeID2, ToJph(out.point));
        out.normal = {n.GetX(), n.GetY(), n.GetZ()};
    }
    return out;
}

void PhysicsWorld::SetContactReportThreshold(f32 impulse) {
    if (impl_) impl_->contacts.SetThreshold(glm::max(impulse, 0.0f));
}

bool PhysicsWorld::PopContact(ContactEvent& out) {
    if (!impl_) return false;
    ContactCollector::Raw raw;
    // Resolve body -> entity HERE on the main thread; a contact whose bodies have
    // since been destroyed is skipped rather than reported with a dangling handle.
    while (impl_->contacts.Pop(raw)) {
        const entt::entity a = impl_->EntityForBody(raw.bodyA);
        const entt::entity b = impl_->EntityForBody(raw.bodyB);
        if (a == entt::null && b == entt::null) continue; // neither side is an entity
        out.a = a;
        out.b = b;
        out.point = raw.point;
        out.normal = raw.normal;
        out.impulse = raw.impulse;
        return true;
    }
    return false;
}

u32 PhysicsWorld::BodyCount() const {
    return impl_ ? impl_->system.GetNumBodies() : 0u;
}

void PhysicsWorld::SyncTerrainColliders(Scene& scene) {
    auto& reg = scene.Registry();
    JPH::BodyInterface& bi = impl_->system.GetBodyInterface();

    const auto destroy = [&](u32 bodyKey) {
        const auto it = impl_->terrainColliders.find(bodyKey);
        if (it == impl_->terrainColliders.end()) return;
        const JPH::BodyID body(bodyKey);
        bi.RemoveBody(body);
        bi.DestroyBody(body);
        impl_->terrainColliders.erase(it);
    };

    for (const entt::entity e : reg.view<Transform, TerrainComponent>()) {
        TerrainComponent& t = reg.get<TerrainComponent>(e);
        const u32 n = t.GridN();
        // No heightmap yet. terrain::Update seeds it (EnsureHeights) on the first
        // build; until then there is nothing to collide against.
        if (n < 3 || t.heights.size() != static_cast<usize>(n) * n) continue;

        glm::vec3 pos, scale;
        glm::quat rot;
        DecomposeWorld(scene.WorldMatrix(e), pos, rot, scale);
        const f32 step = terrain::SampleStep(t);

        // Find this terrain's record. A copied component points at somebody else's
        // body, so the record has to agree about whose it is.
        auto it = impl_->terrainColliders.find(t.colliderBodyId);
        if (it != impl_->terrainColliders.end() && it->second.entity != e) {
            it = impl_->terrainColliders.end();
            t.colliderBodyId = TerrainComponent::kInvalidCollider;
        }

        // The sample count and the XZ spacing are baked into the heightfield itself,
        // so a resolution / chunk-count / chunk-size edit means a new shape.
        if (it != impl_->terrainColliders.end()) {
            const Impl::TerrainCollider& rec = it->second;
            if (rec.sampleCount != n || std::abs(rec.step - step) > 1e-6f) {
                destroy(t.colliderBodyId);
                t.colliderBodyId = TerrainComponent::kInvalidCollider;
                it = impl_->terrainColliders.end();
            }
        }
        // World SCALE, by contrast, only lives in the ScaledShape wrapper. Swapping
        // the wrapper keeps the heightfield (and its acceleration structure) intact,
        // which matters because dragging the scale gizmo would otherwise rebuild a
        // 641x641 field every frame - measured at ~11 ms a go.
        if (it != impl_->terrainColliders.end() &&
            glm::any(glm::greaterThan(glm::abs(it->second.scale - scale), glm::vec3(1e-4f)))) {
            Impl::TerrainCollider& rec = it->second;
            JPH::ShapeRefC shape(rec.field.GetPtr());
            const bool scaled = std::abs(scale.x - 1.0f) > 1e-4f ||
                                std::abs(scale.y - 1.0f) > 1e-4f ||
                                std::abs(scale.z - 1.0f) > 1e-4f;
            if (scaled) shape = new JPH::ScaledShape(shape, ToJph(scale));
            bi.SetShape(JPH::BodyID(t.colliderBodyId), shape.GetPtr(),
                        /*inUpdateMassProperties=*/false, JPH::EActivation::DontActivate);
            rec.scale = scale;
        }

        // --- Drain the sculpt dirty rect into the LIVE shape ------------------
        const bool pending = t.colliderDirtyMaxX >= t.colliderDirtyMinX &&
                             t.colliderDirtyMaxZ >= t.colliderDirtyMinZ;
        if (it != impl_->terrainColliders.end() && pending) {
            Impl::TerrainCollider& rec = it->second;
            // Jolt rounded the sample count up to a block multiple; SetHeights works
            // in whole blocks, so snap the rect outward to block boundaries.
            const u32 sc = rec.field->GetSampleCount();
            const u32 bs = glm::max(rec.field->GetBlockSize(), 1u);
            // Clamp to the CURRENT grid before trusting the rect. A layout change
            // rebuilds the shape (so a stale, too-large rect cannot reach here today),
            // but SetHeights asserts rather than fails, and that reasoning is exactly
            // the kind that rots as callers are added.
            const u32 last = n - 1;
            const u32 minX = glm::min(static_cast<u32>(t.colliderDirtyMinX), last);
            const u32 minZ = glm::min(static_cast<u32>(t.colliderDirtyMinZ), last);
            const u32 maxX = glm::min(static_cast<u32>(t.colliderDirtyMaxX), last);
            const u32 maxZ = glm::min(static_cast<u32>(t.colliderDirtyMaxZ), last);
            const u32 x0 = (minX / bs) * bs;
            const u32 z0 = (minZ / bs) * bs;
            const u32 x1 = glm::min((maxX / bs + 1) * bs, sc);
            const u32 z1 = glm::min((maxZ / bs + 1) * bs, sc);
            const u32 sx = x1 > x0 ? x1 - x0 : 0;
            const u32 sz = z1 > z0 ? z1 - z0 : 0;

            const f64 covered = static_cast<f64>(sx) * sz;
            const f64 whole = static_cast<f64>(sc) * sc;
            bool rebuild = sx == 0 || sz == 0 ||
                           covered >= whole * static_cast<f64>(kTerrainFullRebuildFraction);

            std::vector<f32> samples;
            if (!rebuild) {
                samples.resize(static_cast<usize>(sx) * sz);
                FillTerrainSamples(t, x0, z0, sx, sz, samples.data(), sx);
                // SetHeights CLAMPS to the range the shape was created with, without
                // reporting it - sculpting past the headroom would silently flatten
                // against an invisible ceiling. Detect it and rebuild with a fresh
                // range instead.
                const f32 lo = rec.field->GetMinHeightValue();
                const f32 hi = rec.field->GetMaxHeightValue();
                for (const f32 h : samples) {
                    if (h == JPH::HeightFieldShapeConstants::cNoCollisionValue) continue;
                    if (h < lo || h > hi) { rebuild = true; break; }
                }
            }

            if (rebuild) {
                destroy(t.colliderBodyId);
                t.colliderBodyId = TerrainComponent::kInvalidCollider;
                it = impl_->terrainColliders.end();
            } else {
                // THREADING: Jolt documents SetHeights as racing against concurrent
                // queries, and offers Clone() + SetShape as the escape. We do not need
                // it: this runs at the top of PhysicsWorld::Update on the main thread,
                // before system.Update() launches any job, and every caller of
                // Raycast/RaycastDetailed (camera collision, AI line of sight, combat
                // line of fire, audio occlusion) is a synchronous main-thread call from
                // the same frame loop. Nothing may raycast from a worker without
                // revisiting this.
                rec.field->SetHeights(x0, z0, sx, sz, samples.data(),
                                      static_cast<intptr_t>(sx), impl_->tempAlloc);
                // The shape's bounds moved with the ground; a static body's broadphase
                // box is cached, so without this a raised hill is invisible to queries.
                bi.NotifyShapeChanged(JPH::BodyID(t.colliderBodyId), JPH::Vec3::sZero(),
                                      /*inUpdateMassProperties=*/false,
                                      JPH::EActivation::DontActivate);
            }
            t.colliderDirtyMinX = 0;
            t.colliderDirtyMinZ = 0;
            t.colliderDirtyMaxX = -1;
            t.colliderDirtyMaxZ = -1;
        }

        // --- Create ----------------------------------------------------------
        if (it == impl_->terrainColliders.end()) {
            Impl::TerrainCollider rec;
            rec.entity = e;
            rec.field = BuildTerrainField(t);
            if (!rec.field) continue; // BuildTerrainField logged the reason
            rec.sampleCount = n;
            rec.step = step;
            rec.scale = scale;

            JPH::ShapeRefC shape(rec.field.GetPtr());
            const bool scaled = std::abs(scale.x - 1.0f) > 1e-4f ||
                                std::abs(scale.y - 1.0f) > 1e-4f ||
                                std::abs(scale.z - 1.0f) > 1e-4f;
            if (scaled) shape = new JPH::ScaledShape(shape, ToJph(scale));

            JPH::BodyCreationSettings bcs(shape, JPH::RVec3(pos.x, pos.y, pos.z), ToJph(rot),
                                          JPH::EMotionType::Static, Layers::NON_MOVING);
            // Match what the old per-chunk mesh colliders used (RigidBody defaults),
            // so the change of shape does not also change how the ground feels.
            bcs.mFriction = 0.5f;
            bcs.mRestitution = 0.2f;
            const JPH::BodyID body = bi.CreateAndAddBody(bcs, JPH::EActivation::DontActivate);
            if (body.IsInvalid()) {
                // Creation is retried every frame, so the message must not be. Every
                // log line is flushed unbuffered (see Core/Log.h) - a per-frame error
                // here would cost more than the missing collider.
                if (impl_->terrainBuildFailed.insert(static_cast<u32>(entt::to_integral(e)))
                        .second) {
                    HBE_ERROR("Physics: out of bodies - terrain heightfield not created.");
                }
                continue;
            }
            impl_->terrainBuildFailed.erase(static_cast<u32>(entt::to_integral(e)));
            const u32 key = body.GetIndexAndSequenceNumber();
            t.colliderBodyId = key;
            t.colliderDirtyMinX = 0;
            t.colliderDirtyMinZ = 0;
            t.colliderDirtyMaxX = -1; // the fresh shape already has every sample
            t.colliderDirtyMaxZ = -1;
            impl_->terrainColliders.emplace(key, std::move(rec));
            HBE_INFO("Physics: terrain heightfield collider ready ({0}x{0} samples, {1:.2f} m "
                     "spacing, {2:.0f} m across).",
                     n, step, terrain::ExtentXZ(t));
            continue; // created at the right pose already
        }

        // --- Follow the entity ------------------------------------------------
        // A static body does not move itself, so a gizmo drag (or an animated
        // parent) has to be pushed in. Scale was already handled by the wrapper swap.
        const JPH::BodyID id(t.colliderBodyId);
        JPH::RVec3 bpos;
        JPH::Quat brot;
        bi.GetPositionAndRotation(id, bpos, brot);
        if (glm::distance(ToGlm(bpos), pos) > 1e-4f ||
            std::abs(glm::dot(ToGlm(brot), rot)) < 0.999999f) {
            bi.SetPositionAndRotation(id, JPH::RVec3(pos.x, pos.y, pos.z), ToJph(rot),
                                      JPH::EActivation::DontActivate);
        }
    }

    // --- Reap: entity destroyed, TerrainComponent removed, or the component no
    // longer claims this body (a rebuild above already re-pointed it).
    for (auto it = impl_->terrainColliders.begin(); it != impl_->terrainColliders.end();) {
        const entt::entity e = it->second.entity;
        const TerrainComponent* t =
            reg.valid(e) ? reg.try_get<TerrainComponent>(e) : nullptr;
        if (t && reg.all_of<Transform>(e) && t->colliderBodyId == it->first) {
            ++it;
            continue;
        }
        const JPH::BodyID body(it->first);
        bi.RemoveBody(body);
        bi.DestroyBody(body);
        it = impl_->terrainColliders.erase(it);
    }
}

void PhysicsWorld::Update(Scene& scene, f32 dt) {
    if (!impl_) return;
    auto& reg = scene.Registry();
    JPH::BodyInterface& bi = impl_->system.GetBodyInterface();

    // Terrain first: the heightfield is what everything else stands on, so a RUNTIME
    // sculpt (terrain::Update, which runs before physics on the spine) is collidable in
    // this frame's step.
    //
    // An EDITOR brush stroke is one frame late, and that is inherent to the frame order,
    // not an oversight: the editor UI pass that runs the brush is invoked ~400 lines
    // AFTER physics, nav and streaming in Engine::Run, so a stroke reaches the collider
    // on frame N+1. 16 ms of lag is invisible to a human dragging a brush; the point of
    // saying it here is that the ordering is known, so nothing gets built on a
    // "same-frame" guarantee that does not exist.
    SyncTerrainColliders(scene);

    // --- Create bodies for RigidBody components that don't have one yet -----
    for (const entt::entity e : reg.view<Transform, RigidBody>()) {
        RigidBody& rb = reg.get<RigidBody>(e);
        if (rb.bodyId != RigidBody::kInvalidBody) continue;

        glm::vec3 pos, scale;
        glm::quat rot;
        DecomposeWorld(scene.WorldMatrix(e), pos, rot, scale);

        const bool dynamic = rb.motion == RigidBody::Motion::Dynamic;
        JPH::ShapeRefC shape;
        switch (rb.shape) {
            case RigidBody::Shape::Sphere: {
                const f32 r = rb.radius * glm::max(scale.x, glm::max(scale.y, scale.z));
                shape = new JPH::SphereShape(glm::max(r, 0.01f));
                break;
            }
            case RigidBody::Shape::Capsule: {
                const f32 r = glm::max(rb.radius * glm::max(scale.x, scale.z), 0.01f);
                const f32 hh = glm::max(rb.halfHeight * scale.y, 0.01f);
                shape = new JPH::CapsuleShape(hh, r);
                break;
            }
            case RigidBody::Shape::Mesh:
            case RigidBody::Shape::ConvexHull: {
                // Exact collider from the mesh; wrap in a ScaledShape for world scale.
                shape = BuildMeshCollider(rb, dynamic);
                const bool scaled = std::abs(scale.x - 1.0f) > 1e-4f ||
                                    std::abs(scale.y - 1.0f) > 1e-4f ||
                                    std::abs(scale.z - 1.0f) > 1e-4f;
                if (shape && scaled) shape = new JPH::ScaledShape(shape, ToJph(scale));
                break;
            }
            case RigidBody::Shape::Box:
            default:
                break;
        }
        if (!shape) { // Box fallback (also when mesh geometry is missing).
            const glm::vec3 he = glm::max(rb.halfExtents * scale, glm::vec3(0.01f));
            const f32 convexRadius = glm::min(0.05f, glm::min(he.x, glm::min(he.y, he.z)) * 0.5f);
            shape = new JPH::BoxShape(ToJph(he), convexRadius);
        }
        // Primitive shapes (box/sphere/capsule) are symmetric about their own
        // origin, so centerOffset shifts them onto the mesh's AABB centre while
        // the body origin stays at the entity origin. Mesh/ConvexHull colliders
        // are built straight from the mesh's local-space vertices, which already
        // encode that offset - applying it again would double-shift the collider
        // (the "misaligned imported mesh" bug), so skip it for those.
        const bool meshCollider = (rb.shape == RigidBody::Shape::Mesh ||
                                   rb.shape == RigidBody::Shape::ConvexHull) &&
                                  !rb.collisionVertices.empty();
        if (!meshCollider && glm::dot(rb.centerOffset, rb.centerOffset) > 1e-10f) {
            shape = new JPH::RotatedTranslatedShape(ToJph(rb.centerOffset * scale),
                                                    JPH::Quat::sIdentity(), shape);
        }

        JPH::BodyCreationSettings bcs(
            shape, JPH::RVec3(pos.x, pos.y, pos.z), ToJph(rot),
            dynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static,
            dynamic ? Layers::MOVING : Layers::NON_MOVING);
        bcs.mFriction = rb.friction;
        bcs.mRestitution = rb.restitution;

        const JPH::BodyID body = bi.CreateAndAddBody(
            bcs, dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
        if (body.IsInvalid()) continue;
        rb.bodyId = body.GetIndexAndSequenceNumber();
        impl_->bodyToEntity[rb.bodyId] = e;
    }

    // --- Create CharacterVirtual capsules for CharacterControllers ----------
    for (const entt::entity e : reg.view<Transform, CharacterController>()) {
        CharacterController& cc = reg.get<CharacterController>(e);
        if (cc.bodyId != CharacterController::kInvalidBody &&
            impl_->characters.count(cc.bodyId)) {
            continue;
        }
        const glm::vec3 wp = glm::vec3(scene.WorldMatrix(e)[3]);
        const f32 r = glm::max(cc.radius, 0.05f);
        const f32 hh = glm::max(cc.height * 0.5f - r, 0.01f); // capsule cylinder half
        JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
        settings->mShape = new JPH::CapsuleShape(hh, r); // centered: pos = capsule center
        settings->mMaxSlopeAngle = glm::radians(50.0f);
        JPH::Ref<JPH::CharacterVirtual> cv = new JPH::CharacterVirtual(
            settings.GetPtr(), JPH::RVec3(wp.x, wp.y, wp.z), JPH::Quat::sIdentity(),
            &impl_->system);
        cc.bodyId = impl_->nextCharId++;
        impl_->characters[cc.bodyId] = cv;
        impl_->charToEntity[cc.bodyId] = e;
    }

    // --- Drop bodies whose entity/RigidBody went away or was invalidated ----
    // (Editing shape properties sets bodyId back to kInvalidBody; the entity
    // gets a fresh body above and the old one is reaped here.)
    for (auto it = impl_->bodyToEntity.begin(); it != impl_->bodyToEntity.end();) {
        const entt::entity e = it->second;
        const RigidBody* rb =
            reg.valid(e) ? reg.try_get<RigidBody>(e) : nullptr;
        if (!rb || rb->bodyId != it->first) {
            const JPH::BodyID body(it->first);
            bi.RemoveBody(body);
            bi.DestroyBody(body);
            it = impl_->bodyToEntity.erase(it);
        } else {
            ++it;
        }
    }

    // --- Drop characters whose entity/CharacterController went away ---------
    for (auto it = impl_->charToEntity.begin(); it != impl_->charToEntity.end();) {
        const entt::entity e = it->second;
        const CharacterController* cc =
            reg.valid(e) ? reg.try_get<CharacterController>(e) : nullptr;
        if (!cc || cc->bodyId != it->first) {
            impl_->characters.erase(it->first); // Ref frees the CharacterVirtual
            it = impl_->charToEntity.erase(it);
        } else {
            ++it;
        }
    }

    if (!running_) {
        // While paused (editing), bodies follow their entities so the gizmo
        // moves colliders too; velocities reset so resuming doesn't carry
        // stale momentum from before the teleport.
        for (const entt::entity e : reg.view<Transform, RigidBody>()) {
            const RigidBody& rb = reg.get<RigidBody>(e);
            if (rb.bodyId == RigidBody::kInvalidBody) continue;

            glm::vec3 pos, scale;
            glm::quat rot;
            DecomposeWorld(scene.WorldMatrix(e), pos, rot, scale);

            const JPH::BodyID id(rb.bodyId);
            JPH::RVec3 bpos;
            JPH::Quat brot;
            bi.GetPositionAndRotation(id, bpos, brot);
            const bool moved = glm::distance(ToGlm(bpos), pos) > 1e-4f ||
                               std::abs(glm::dot(ToGlm(brot), rot)) < 0.999999f;
            if (moved) {
                bi.SetPositionAndRotation(id, JPH::RVec3(pos.x, pos.y, pos.z), ToJph(rot),
                                          JPH::EActivation::DontActivate);
                bi.SetLinearAndAngularVelocity(id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
            }
        }
        // Characters follow their Transform while paused (so the gizmo moves them).
        for (const entt::entity e : reg.view<Transform, CharacterController>()) {
            CharacterController& cc = reg.get<CharacterController>(e);
            const auto cit = impl_->characters.find(cc.bodyId);
            if (cit == impl_->characters.end()) continue;
            const glm::vec3 wp = glm::vec3(scene.WorldMatrix(e)[3]);
            cit->second->SetPosition(JPH::RVec3(wp.x, wp.y, wp.z));
            cc.velocityY = 0.0f;
        }
        return;
    }

    // --- Fixed-step simulation (60 Hz, capped to avoid a death spiral) ------
    constexpr f32 kStep = 1.0f / 60.0f;
    accumulator_ += glm::min(dt, 0.25f);

    // Filters the character collides against (the whole world: static + moving).
    const JPH::DefaultBroadPhaseLayerFilter charBpFilter =
        impl_->system.GetDefaultBroadPhaseLayerFilter(Layers::MOVING);
    const JPH::DefaultObjectLayerFilter charObjFilter =
        impl_->system.GetDefaultLayerFilter(Layers::MOVING);
    const JPH::BodyFilter charBodyFilter;   // pass all bodies
    const JPH::ShapeFilter charShapeFilter; // pass all sub-shapes
    const JPH::CharacterVirtual::ExtendedUpdateSettings charUpdate;

    int steps = 0;
    while (accumulator_ >= kStep && steps < 4) {
        impl_->system.Update(kStep, 1, &impl_->tempAlloc, &impl_->jobs);

        // Step each CharacterVirtual: gravity + jump + the input intent
        // (cc.desiredVelocity / jumpRequested set by character::Update), resolved
        // against the world's colliders.
        for (const entt::entity e : reg.view<Transform, CharacterController>()) {
            CharacterController& cc = reg.get<CharacterController>(e);
            const auto cit = impl_->characters.find(cc.bodyId);
            if (cit == impl_->characters.end()) continue;
            JPH::CharacterVirtual* cv = cit->second.GetPtr();

            const bool onGround =
                cv->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
            if (onGround && cc.velocityY < 0.0f) cc.velocityY = 0.0f; // landed
            if (onGround && cc.jumpRequested && cc.jumpHeight > 0.0f) {
                cc.velocityY = std::sqrt(2.0f * cc.gravity * cc.jumpHeight); // v=sqrt(2gh)
            }
            cc.jumpRequested = false;
            cc.velocityY -= cc.gravity * kStep;

            cv->SetLinearVelocity(
                JPH::Vec3(cc.desiredVelocity.x, cc.velocityY, cc.desiredVelocity.z));
            // Stair step-up MUST scale with the capsule. Jolt's default (0.4 m) is
            // ~half a 1-unit player, so moving into the smallest contact "steps it up"
            // and it floats away. Cap the step to a fraction of the capsule radius.
            JPH::CharacterVirtual::ExtendedUpdateSettings cu = charUpdate;
            cu.mWalkStairsStepUp =
                JPH::Vec3(0.0f, glm::clamp(cc.radius * 0.75f, 0.04f, 0.4f), 0.0f);
            cv->ExtendedUpdate(kStep, JPH::Vec3(0.0f, -cc.gravity, 0.0f), cu,
                               charBpFilter, charObjFilter, charBodyFilter, charShapeFilter,
                               impl_->tempAlloc);
            cc.grounded =
                cv->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;

            // Write the capsule centre back into the entity Transform.
            const glm::vec3 wp = ToGlm(cv->GetPosition());
            Transform& t = reg.get<Transform>(e);
            const Parent* par = reg.try_get<Parent>(e);
            if (par && reg.valid(par->entity)) {
                const glm::mat4 world = glm::translate(glm::mat4(1.0f), wp) *
                                        glm::mat4_cast(t.rotation) *
                                        glm::scale(glm::mat4(1.0f), t.scale);
                const glm::mat4 local = glm::inverse(scene.WorldMatrix(par->entity)) * world;
                glm::vec3 lpos, lscale;
                glm::quat lrot;
                DecomposeWorld(local, lpos, lrot, lscale);
                t.position = lpos;
            } else {
                t.position = wp;
            }
        }

        accumulator_ -= kStep;
        ++steps;
    }
    if (steps == 4) accumulator_ = 0.0f; // running behind; drop the remainder

    // While the gizmo drags an entity, its body FOLLOWS the Transform (and
    // never overwrites it), so objects stay manipulable mid-simulation.
    if (edited_ != entt::null && reg.valid(edited_)) {
        if (const RigidBody* rb = reg.try_get<RigidBody>(edited_);
            rb && rb->bodyId != RigidBody::kInvalidBody) {
            glm::vec3 pos, scale;
            glm::quat rot;
            DecomposeWorld(scene.WorldMatrix(edited_), pos, rot, scale);
            const JPH::BodyID id(rb->bodyId);
            bi.SetPositionAndRotation(id, JPH::RVec3(pos.x, pos.y, pos.z), ToJph(rot),
                                      JPH::EActivation::Activate);
            bi.SetLinearAndAngularVelocity(id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
        }
    }

    // --- Write dynamic body poses back into entity Transforms ---------------
    for (const entt::entity e : reg.view<Transform, RigidBody>()) {
        if (e == edited_) continue; // the user owns this one right now
        const RigidBody& rb = reg.get<RigidBody>(e);
        if (rb.bodyId == RigidBody::kInvalidBody ||
            rb.motion != RigidBody::Motion::Dynamic) {
            continue;
        }
        JPH::RVec3 pos;
        JPH::Quat rot;
        bi.GetPositionAndRotation(JPH::BodyID(rb.bodyId), pos, rot);

        Transform& t = reg.get<Transform>(e);
        const Parent* p = reg.try_get<Parent>(e);
        if (p && reg.valid(p->entity)) {
            // Body poses are world space; convert into the parent's space.
            glm::mat4 world = glm::translate(glm::mat4(1.0f), ToGlm(pos)) *
                              glm::mat4_cast(ToGlm(rot)) *
                              glm::scale(glm::mat4(1.0f), t.scale);
            const glm::mat4 local = glm::inverse(scene.WorldMatrix(p->entity)) * world;
            glm::vec3 lpos, lscale;
            glm::quat lrot;
            DecomposeWorld(local, lpos, lrot, lscale);
            t.position = lpos;
            t.rotation = lrot;
        } else {
            t.position = ToGlm(pos);
            t.rotation = ToGlm(rot);
        }
    }
}

} // namespace hbe
