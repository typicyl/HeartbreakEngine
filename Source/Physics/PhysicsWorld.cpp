// Physics/PhysicsWorld.cpp - Jolt-backed implementation.
#include "Physics/PhysicsWorld.h"

#include "Core/Log.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
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
#include <thread>
#include <unordered_map>

namespace hbe {
namespace {

// One-time global Jolt setup (allocator, type factory, collision dispatch).
void EnsureJoltGlobalInit() {
    static bool done = false;
    if (done) return;
    done = true;
    JPH::RegisterDefaultAllocator();
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

// Position/rotation/scale of a world matrix (no shear support).
void DecomposeWorld(const glm::mat4& m, glm::vec3& pos, glm::quat& rot, glm::vec3& scale) {
    pos = glm::vec3(m[3]);
    const glm::vec3 c0(m[0]), c1(m[1]), c2(m[2]);
    scale = {glm::length(c0), glm::length(c1), glm::length(c2)};
    const glm::vec3 s = glm::max(scale, glm::vec3(1e-6f));
    rot = glm::normalize(glm::quat_cast(glm::mat3(c0 / s.x, c1 / s.y, c2 / s.z)));
}

} // namespace

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

    Impl() {
        constexpr u32 kMaxBodies = 8192;
        constexpr u32 kMaxBodyPairs = 8192;
        constexpr u32 kMaxContacts = 4096;
        system.Init(kMaxBodies, 0, kMaxBodyPairs, kMaxContacts, bpLayers, objVsBp, objPair);
        system.SetGravity({0.0f, -9.81f, 0.0f});
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
    impl_->characters.clear(); // Refs free the CharacterVirtuals (no system body)
    impl_->charToEntity.clear();
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

f32 PhysicsWorld::Raycast(const glm::vec3& origin, const glm::vec3& dir, f32 maxDist) const {
    if (!impl_ || maxDist <= 0.0f) return maxDist;
    const JPH::RRayCast ray{ToJph(origin), ToJph(dir) * maxDist};
    JPH::RayCastResult result;
    if (impl_->system.GetNarrowPhaseQuery().CastRay(ray, result)) {
        return result.mFraction * maxDist; // mFraction in [0,1] along the ray
    }
    return maxDist;
}

void PhysicsWorld::Update(Scene& scene, f32 dt) {
    if (!impl_) return;
    auto& reg = scene.Registry();
    JPH::BodyInterface& bi = impl_->system.GetBodyInterface();

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
