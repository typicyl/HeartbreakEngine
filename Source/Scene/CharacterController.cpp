// Scene/CharacterController.cpp
#include "Scene/CharacterController.h"

#include "Core/Input.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <glm/gtc/quaternion.hpp>

#include <cmath>

namespace hbe::character {

void Update(Scene& scene, const Input& input, f32 dt, const glm::vec3& cameraForward) {
    auto& reg = scene.Registry();

    // Camera basis flattened onto the ground plane (for camera-relative input):
    // "forward" follows where the player is looking, "right" is its perpendicular.
    glm::vec3 camF(cameraForward.x, 0.0f, cameraForward.z);
    camF = glm::length(camF) > 1e-4f ? glm::normalize(camF) : glm::vec3(0.0f, 0.0f, -1.0f);
    const glm::vec3 camR = glm::normalize(glm::cross(camF, glm::vec3(0.0f, 1.0f, 0.0f)));

    const Input::GamepadState& pad = input.Gamepad(0);

    for (const entt::entity e : reg.view<CharacterController, Transform>()) {
        CharacterController& cc = reg.get<CharacterController>(e);
        if (!cc.enabled) continue;
        Transform& t = reg.get<Transform>(e);

        // --- Movement intent (each axis in -1..1) -----------------------------
        glm::vec2 mv(0.0f);
        if (cc.useKeyboard) {
            if (input.IsKeyDown(Key::W)) mv.y += 1.0f;
            if (input.IsKeyDown(Key::S)) mv.y -= 1.0f;
            if (input.IsKeyDown(Key::D)) mv.x += 1.0f;
            if (input.IsKeyDown(Key::A)) mv.x -= 1.0f;
        }
        if (cc.useGamepad && pad.connected) {
            mv.x += pad.leftX;
            mv.y += pad.leftY;
        }
        if (glm::length(mv) > 1.0f) mv = glm::normalize(mv); // no diagonal speed boost

        const glm::vec3 dir =
            cc.cameraRelative
                ? (camF * mv.y + camR * mv.x)
                : (glm::vec3(0.0f, 0.0f, -1.0f) * mv.y + glm::vec3(1.0f, 0.0f, 0.0f) * mv.x);

        const bool sprint = (cc.useKeyboard && input.IsKeyDown(Key::Shift)) ||
                            (cc.useGamepad && pad.connected && pad.IsDown(Gamepad_LShoulder));
        const f32 speed = cc.moveSpeed * (sprint ? cc.sprintMultiplier : 1.0f);

        // Hand the horizontal move intent + jump to PhysicsWorld, which steps the
        // CharacterVirtual capsule (gravity, ground, collision vs. the world).
        cc.desiredVelocity = glm::vec3(dir.x, 0.0f, dir.z) * speed;
        if ((cc.useKeyboard && input.WasKeyPressed(Key::Space)) ||
            (cc.useGamepad && pad.connected && pad.WasPressed(Gamepad_A))) {
            cc.jumpRequested = true; // physics consumes it if grounded
        }

        // --- Turn the body to face its movement (position is physics-owned) ---
        // A first-person camera writes this body's yaw itself (the head IS the
        // aim), so it latches externalFacing and this turn stands down for the
        // frame. CONSUMED here: the camera re-latches it every frame it drives
        // the body, so the moment it stops - mode change, zone change, look
        // switched off - faceMoveDir resumes on its own with no cleanup path to
        // forget. See CameraComponent::playerLook.
        const bool externallyFaced = cc.externalFacing;
        cc.externalFacing = false;
        //
        // KNOWN DEFECT, deliberately left alone here (a separate change, because
        // it alters how every existing third-person character faces): this yaw is
        // MIRRORED IN X against the engine's forward convention. Every other
        // system reads a facing as `world * vec4(0,0,-1,0)` (cam::Update's
        // tgtFwd, AISystem's perception cone, the render camera). Rotating -Z by
        // angleAxis(a, +Y) yields (-sin a, 0, -cos a), so with
        // a = atan2(dir.x, -dir.z) the body's forward comes out as
        // (-dir.x, 0, +dir.z): correct walking forward or back, backwards walking
        // left or right. The fix is `atan2(-dir.x, -dir.z)`. Left as-is here so
        // the first-person work below it is not carrying an unrelated,
        // content-visible behaviour change; cam::YawQuat uses the CORRECT
        // convention, which is why the first-person path does not route through
        // this one.
        if (cc.faceMoveDir && !externallyFaced &&
            glm::length(glm::vec2(dir.x, dir.z)) > 1e-3f) {
            const f32 yaw = std::atan2(dir.x, -dir.z); // 0 faces local -Z
            const glm::quat target = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
            const f32 a = cc.turnSpeed <= 0.0f ? 1.0f : 1.0f - std::exp(-cc.turnSpeed * dt);
            t.rotation = glm::slerp(t.rotation, target, glm::clamp(a, 0.0f, 1.0f));
        }
    }
}

} // namespace hbe::character
