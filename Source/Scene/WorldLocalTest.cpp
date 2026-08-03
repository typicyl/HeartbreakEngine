// Scene/WorldLocalTest.cpp - the WORLD-vs-LOCAL transform contract (--test-worldlocal).
//
// Gameplay code repeatedly computed a WORLD-space answer (a nav step, a look-at
// direction, a spawn point) and assigned it straight to `Transform::position` /
// `Transform::rotation`, which are PARENT-RELATIVE. For a root entity the two
// coincide, so every one of those sites looked correct in a flat test scene and
// broke silently the moment an agent was parented to a moving platform, a room
// root, or a streamed shard root.
//
// Scene::SetWorldPosition / SetWorldRotation are the conversion, and this pins
// them. Every case here INCLUDES the assertion that the old raw-assignment
// behaviour fails it, so the test measures the fix rather than the status quo -
// the same discipline as --test-pasteparent and --test-sceneslice.
//
// Headless: no GPU, no window, no project.
#include "Scene/Scene.h"

#include "Core/Log.h"
#include "Scene/Components.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <cmath>
#include <cstdio>

namespace hbe::scene {
namespace {

bool g_ok = true;

void Check(bool cond, const char* what) {
    if (!cond) {
        g_ok = false;
        HBE_ERROR("worldlocal: FAILED - {}", what);
    }
}

bool NearVec(const glm::vec3& a, const glm::vec3& b, f32 eps = 1e-3f) {
    return glm::length(a - b) <= eps;
}

// World-space translation of an entity.
glm::vec3 WorldPos(const Scene& s, entt::entity e) {
    return glm::vec3(s.WorldMatrix(e)[3]);
}

// World-space forward (-Z), the engine's facing convention everywhere.
glm::vec3 WorldForward(const Scene& s, entt::entity e) {
    const glm::vec3 f = glm::vec3(s.WorldMatrix(e) * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));
    const f32 len = glm::length(f);
    return len > 1e-6f ? f / len : glm::vec3(0.0f, 0.0f, -1.0f);
}

} // namespace

bool WorldLocalSelfTest() {
    g_ok = true;
    Scene s;

    // A deliberately awkward parent: translated, rotated 90 deg about +Y, and
    // NON-UNIFORMLY scaled. Non-uniform scale is the case that breaks a naive
    // quat_cast of the parent's upper 3x3, so it belongs in the fixture rather
    // than in a footnote.
    const entt::entity parent = s.CreateEntity("Parent");
    {
        Transform t;
        t.position = {10.0f, 3.0f, -4.0f};
        t.rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        t.scale = {2.0f, 1.0f, 0.5f};
        s.Registry().emplace<Transform>(parent, t);
    }

    const entt::entity child = s.CreateEntity("Child");
    s.Registry().emplace<Transform>(child, Transform{});
    s.Registry().emplace<Parent>(child, Parent{parent});

    // --- 1) SetWorldPosition actually lands the entity in world space -----------
    {
        const glm::vec3 want{1.0f, 2.0f, 3.0f};
        s.SetWorldPosition(child, want);
        Check(NearVec(WorldPos(s, child), want),
              "SetWorldPosition puts the child at the requested WORLD point");

        // The OLD behaviour - assigning the world answer to the local slot - must
        // NOT satisfy that. If it does, this fixture is not adversarial and the
        // test proves nothing.
        s.Registry().get<Transform>(child).position = want; // the bug, reproduced
        Check(!NearVec(WorldPos(s, child), want),
              "raw local assignment does NOT land in world space (fixture is adversarial)");
    }

    // --- 2) The stored local value is genuinely parent-relative -----------------
    {
        const glm::vec3 want{-5.0f, 0.5f, 7.25f};
        s.SetWorldPosition(child, want);
        const glm::vec3 local = s.Registry().get<Transform>(child).position;
        const glm::vec3 rebuilt =
            glm::vec3(s.WorldMatrix(parent) * glm::vec4(local, 1.0f));
        Check(NearVec(rebuilt, want),
              "parentWorld * storedLocal reproduces the requested world point");
    }

    // --- 3) SetWorldRotation aims the child in world space ----------------------
    {
        // Face world +X. quatLookAt takes the direction the -Z axis should point.
        const glm::vec3 dir{1.0f, 0.0f, 0.0f};
        s.SetWorldRotation(child, glm::quatLookAt(dir, glm::vec3(0.0f, 1.0f, 0.0f)));
        Check(NearVec(WorldForward(s, child), dir, 2e-3f),
              "SetWorldRotation aims the child's WORLD forward at the requested direction");

        // Same adversarial check: the raw assignment must miss, because the parent
        // is rotated 90 deg.
        s.Registry().get<Transform>(child).rotation =
            glm::quatLookAt(dir, glm::vec3(0.0f, 1.0f, 0.0f)); // the bug, reproduced
        Check(!NearVec(WorldForward(s, child), dir, 2e-3f),
              "raw local rotation assignment does NOT aim in world space");
    }

    // --- 4) Non-uniform parent scale must not skew the resulting FORWARD --------
    {
        // The parent's scale is (2, 1, 0.5), which SHEARS any child orientation:
        // world = parentRot * parentScale * childLocalRot. No local rotation can
        // undo a shear, so full orientation is not recoverable here - but forward
        // is, and forward is what every facing consumer reads. The naive
        // `inverse(parentRotation) * worldRot` misses this case outright, which is
        // exactly why it is in the fixture.
        const glm::vec3 dir = glm::normalize(glm::vec3(0.3f, 0.0f, -1.0f));
        s.SetWorldRotation(child, glm::quatLookAt(dir, glm::vec3(0.0f, 1.0f, 0.0f)));
        const glm::vec3 got = WorldForward(s, child);
        Check(NearVec(got, dir, 5e-3f),
              "a non-uniformly scaled parent does not skew the world rotation");
        Check(std::fabs(glm::length(got) - 1.0f) < 1e-3f,
              "the resulting world forward is still unit length");
    }

    // --- 5) A ROOT entity is the identity case ----------------------------------
    {
        const entt::entity root = s.CreateEntity("Root");
        s.Registry().emplace<Transform>(root, Transform{});
        const glm::vec3 want{4.0f, -1.0f, 2.0f};
        s.SetWorldPosition(root, want);
        Check(NearVec(s.Registry().get<Transform>(root).position, want),
              "on a root, SetWorldPosition writes the value through unchanged");

        const glm::vec3 dir{0.0f, 0.0f, -1.0f};
        s.SetWorldRotation(root, glm::quatLookAt(dir, glm::vec3(0.0f, 1.0f, 0.0f)));
        Check(NearVec(WorldForward(s, root), dir, 2e-3f),
              "on a root, SetWorldRotation is world rotation");
    }

    // --- 6) Nested parents compose ----------------------------------------------
    {
        const entt::entity mid = s.CreateEntity("Mid");
        Transform mt;
        mt.position = {0.0f, 5.0f, 0.0f};
        mt.rotation = glm::angleAxis(glm::radians(-45.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        s.Registry().emplace<Transform>(mid, mt);
        s.Registry().emplace<Parent>(mid, Parent{parent});

        const entt::entity leaf = s.CreateEntity("Leaf");
        s.Registry().emplace<Transform>(leaf, Transform{});
        s.Registry().emplace<Parent>(leaf, Parent{mid});

        const glm::vec3 want{-2.0f, 8.0f, 11.0f};
        s.SetWorldPosition(leaf, want);
        Check(NearVec(WorldPos(s, leaf), want),
              "SetWorldPosition composes through a two-level parent chain");
    }

    // --- 7) A missing Transform is a no-op, not a crash -------------------------
    {
        const entt::entity bare = s.CreateEntity("NoTransform");
        s.SetWorldPosition(bare, glm::vec3(1.0f));
        s.SetWorldRotation(bare, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        Check(!s.Registry().all_of<Transform>(bare),
              "setting a world transform on a Transform-less entity adds nothing");
    }

    return g_ok;
}

} // namespace hbe::scene
