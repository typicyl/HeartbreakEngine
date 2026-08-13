// Physics/PhysicsWorld.h - rigid-body simulation (Jolt) behind a plain API.
//
// The world owns a Jolt PhysicsSystem (pimpl; no Jolt types leak into engine
// headers). Each Update it lazily creates bodies for RigidBody components,
// steps the simulation at a fixed 60 Hz, and writes body poses back into the
// entities' Transforms. Bodies whose entities vanish are removed.
#pragma once

#include "Core/Types.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <functional>
#include <memory>
#include <vector>

namespace hbe {

class Scene;

class PhysicsWorld : public NonCopyable {
public:
    PhysicsWorld();
    ~PhysicsWorld();

    // Creates missing bodies, advances the fixed-step simulation by `dt`
    // seconds, and syncs dynamic body poses into entity Transforms.
    // Does nothing when not running (see SetRunning).
    void Update(Scene& scene, f32 dt);

    // Pauses/resumes stepping (body creation/cleanup still happens). The
    // editor keeps this off until "Simulate" is enabled.
    void SetRunning(bool running) { running_ = running; }
    bool IsRunning() const { return running_; }

    // While the editor is dragging an entity (gizmo), its body must follow
    // the Transform instead of overwriting it - even mid-simulation. Pass
    // entt::null when nothing is being edited.
    void SetEditedEntity(entt::entity e) { edited_ = e; }

    // Drops all bodies (e.g. when reloading a scene).
    void Clear();

    // Live Jolt body count, terrain heightfields included. Diagnostics + leak
    // checks: sculpting terrain repeatedly must not grow this (the heightfield is
    // edited in place, so it stays at one body per terrain).
    u32 BodyCount() const;

    // -- Scripting hooks ------------------------------------------------------
    // Direct body control for gameplay code. No-ops when the entity has no
    // live body yet (bodies are created lazily during Update).
    void SetLinearVelocity(Scene& scene, entt::entity e, const glm::vec3& velocity);
    void AddImpulse(Scene& scene, entt::entity e, const glm::vec3& impulse);
    // Impulse applied AT A WORLD POINT, so it imparts spin as well as linear
    // motion. Destruction needs this: chunks driven from the actual impact point
    // tumble, whereas a centre-of-mass impulse makes debris slide out flatly.
    void AddImpulseAtPoint(Scene& scene, entt::entity e, const glm::vec3& impulse,
                           const glm::vec3& worldPoint);

    // Water buoyancy: for every DYNAMIC rigid body, sample the fluid surface height under it
    // via surfaceHeightAt(worldX, worldZ) and apply Jolt's buoyancy impulse (float + drag) to
    // the submerged part. `buoyancy` > 1 floats; the surface normal is world-up. Call once per
    // frame before the step. No Jolt types cross this header (surfaceHeightAt is plain floats).
    void ApplyBuoyancy(Scene& scene, f32 dt,
                       const std::function<f32(f32 x, f32 z)>& surfaceHeightAt,
                       f32 buoyancy, f32 linearDrag, f32 angularDrag);

    // Closest world hit along `origin + dir * maxDist` (dir need not be unit).
    // Returns the hit distance, or `maxDist` when nothing is hit. Used for
    // camera collision (the player's CharacterVirtual has no body, so it is
    // never hit). Safe to call any time.
    f32 Raycast(const glm::vec3& origin, const glm::vec3& dir, f32 maxDist) const;

    // Full hit information. The distance-only Raycast above is enough for camera
    // collision, but anything that has to ACT on what it hit (damage routing,
    // impact-driven destruction, decals) needs to know WHAT was hit and WHERE.
    struct RayHit {
        bool hit = false;
        f32 distance = 0.0f;
        glm::vec3 point{0.0f};
        glm::vec3 normal{0.0f};
        entt::entity entity = entt::null; // entt::null when the body has no entity
    };
    RayHit RaycastDetailed(const glm::vec3& origin, const glm::vec3& dir, f32 maxDist) const;

    // ALL world hits along `origin + dir*maxDist`, nearest first. A single closest hit is enough
    // for line-of-sight, but acoustics needs every wall between a source and the listener (each
    // contributes transmission loss), so this collects them all. `dir` need not be unit; distances
    // are true world distances; normals are not computed (cheaper). Main-thread only, like the
    // other raycasts.
    struct RayHitAll {
        f32 distance = 0.0f;
        glm::vec3 point{0.0f};
        entt::entity entity = entt::null;
    };
    void RaycastAll(const glm::vec3& origin, const glm::vec3& dir, f32 maxDist,
                    std::vector<RayHitAll>& out) const;

    // -- Contact events -------------------------------------------------------
    // A collision that exceeded `SetContactReportThreshold`. Destruction breaks on
    // impact, so it needs to know that two things hit each other and how hard.
    //
    // THREADING: Jolt invokes its contact listener from PHYSICS WORKER THREADS,
    // mid-step, where touching the ECS would be a data race. Events are therefore
    // queued under a mutex during the step and drained by the main thread via
    // PopContact() after Update() returns.
    struct ContactEvent {
        entt::entity a = entt::null;
        entt::entity b = entt::null;
        glm::vec3 point{0.0f};   // world-space contact point
        glm::vec3 normal{0.0f};  // world-space contact normal (a -> b)
        f32 impulse = 0.0f;      // approximate collision impulse magnitude
    };
    // Contacts whose estimated impulse is below this are dropped without queueing,
    // so resting/sliding bodies cost nothing. 0 = report everything (expensive).
    void SetContactReportThreshold(f32 impulse);
    // FIFO drain; false when the queue is empty. Call once per frame after Update.
    bool PopContact(ContactEvent& out);

private:
    // Creates / updates / reaps the ONE static Jolt HeightFieldShape body per
    // TerrainComponent. Runs at the top of Update (so a sculpt landing this frame is
    // collidable this frame) and, like RigidBody creation, runs even while paused so
    // the editor can walk and paint on terrain without pressing Simulate.
    //
    // Terrain deliberately does NOT go through RigidBody: a heightfield is not a
    // triangle soup, it can be edited in place per sample block, and the old
    // per-chunk mesh colliders were both 256x more bodies and never rebuilt after a
    // brush stroke (collision silently diverged from the visible ground).
    void SyncTerrainColliders(Scene& scene);

    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool running_ = true;
    f32 accumulator_ = 0.0f;
    entt::entity edited_ = entt::null;
};

} // namespace hbe
