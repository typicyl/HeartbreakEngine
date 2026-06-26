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

#include <memory>

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

    // -- Scripting hooks ------------------------------------------------------
    // Direct body control for gameplay code. No-ops when the entity has no
    // live body yet (bodies are created lazily during Update).
    void SetLinearVelocity(Scene& scene, entt::entity e, const glm::vec3& velocity);
    void AddImpulse(Scene& scene, entt::entity e, const glm::vec3& impulse);

    // Closest world hit along `origin + dir * maxDist` (dir need not be unit).
    // Returns the hit distance, or `maxDist` when nothing is hit. Used for
    // camera collision (the player's CharacterVirtual has no body, so it is
    // never hit). Safe to call any time.
    f32 Raycast(const glm::vec3& origin, const glm::vec3& dir, f32 maxDist) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool running_ = true;
    f32 accumulator_ = 0.0f;
    entt::entity edited_ = entt::null;
};

} // namespace hbe
