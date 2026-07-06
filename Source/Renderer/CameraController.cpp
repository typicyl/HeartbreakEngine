// Renderer/CameraController.cpp
#include "Renderer/CameraController.h"

#include "Core/Input.h"
#include "Renderer/Camera.h"
#include "Renderer/Renderer.h"

#include <cmath>

namespace hbe {

void FlyCameraController::SyncFrom(const Camera& camera) {
    pos_ = camera.Position();
    const glm::vec3 fwd = camera.Forward();
    pitch_ = glm::degrees(std::asin(glm::clamp(fwd.y, -1.0f, 1.0f)));
    yaw_ = glm::degrees(std::atan2(fwd.z, fwd.x));
}

void FlyCameraController::TakeControl(Renderer& renderer) {
    SyncFrom(renderer.GetCamera());
    renderer.SetOrbitEnabled(false);
}

void FlyCameraController::Focus(Renderer& renderer, const glm::vec3& center, f32 radius) {
    SyncFrom(renderer.GetCamera()); // adopt the current view angle (no look snap)
    const f32 cy = std::cos(glm::radians(yaw_));
    const f32 sy = std::sin(glm::radians(yaw_));
    const f32 cp = std::cos(glm::radians(pitch_));
    const f32 sp = std::sin(glm::radians(pitch_));
    const glm::vec3 forward = glm::normalize(glm::vec3(cy * cp, sp, sy * cp));
    const f32 dist = glm::max(radius * 2.5f, 1.0f); // frame the sphere with margin
    pos_ = center - forward * dist;
    renderer.SetOrbitEnabled(false); // controller owns the camera; holds the framed view
    flying_ = false;                 // repositioned, not RMB-flying
    renderer.GetCamera().LookAt(pos_, pos_ + forward);
}

void FlyCameraController::Update(const Input& input, Renderer& renderer, f32 dt,
                                 bool wantMouseFly) {
    const Input::GamepadState& pad = input.Gamepad(0);
    const bool padSteering =
        pad.connected && (std::abs(pad.leftX) > 0.0f || std::abs(pad.leftY) > 0.0f ||
                          std::abs(pad.rightX) > 0.0f || std::abs(pad.rightY) > 0.0f ||
                          pad.leftTrigger > 0.0f || pad.rightTrigger > 0.0f);

    // Take over from the auto-orbit camera without a view snap.
    const bool engage = wantMouseFly || padSteering;
    if (engage && (!flying_ || renderer.IsOrbitEnabled())) {
        TakeControl(renderer);
    }
    flying_ = engage;

    // --- Look ---------------------------------------------------------------
    if (wantMouseFly) {
        const f32 sens = 0.12f; // degrees per pixel
        yaw_ += input.MouseDeltaX() * sens;
        pitch_ -= input.MouseDeltaY() * sens;
    }
    if (padSteering) {
        const f32 padLookSpeed = 140.0f; // degrees per second at full deflection
        yaw_ += pad.rightX * padLookSpeed * dt;
        pitch_ += pad.rightY * padLookSpeed * 0.7f * dt;
    }
    pitch_ = glm::clamp(pitch_, -89.0f, 89.0f);

    const f32 cy = std::cos(glm::radians(yaw_));
    const f32 sy = std::sin(glm::radians(yaw_));
    const f32 cp = std::cos(glm::radians(pitch_));
    const f32 sp = std::sin(glm::radians(pitch_));
    const glm::vec3 forward = glm::normalize(glm::vec3(cy * cp, sp, sy * cp));
    const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0, 1, 0)));
    const glm::vec3 up{0.0f, 1.0f, 0.0f};

    // --- Move ---------------------------------------------------------------
    if (wantMouseFly) {
        const f32 speed = (input.IsKeyDown(Key::Shift) ? 20.0f : 6.0f) * dt;
        if (input.IsKeyDown(Key::W)) pos_ += forward * speed;
        if (input.IsKeyDown(Key::S)) pos_ -= forward * speed;
        if (input.IsKeyDown(Key::D)) pos_ += right * speed;
        if (input.IsKeyDown(Key::A)) pos_ -= right * speed;
        if (input.IsKeyDown(Key::E)) pos_ += up * speed;
        if (input.IsKeyDown(Key::Q)) pos_ -= up * speed;
    }
    if (padSteering) {
        const f32 padSpeed = (pad.IsDown(Gamepad_LShoulder) ? 20.0f : 6.0f) * dt;
        pos_ += forward * (pad.leftY * padSpeed);
        pos_ += right * (pad.leftX * padSpeed);
        pos_ += up * ((pad.rightTrigger - pad.leftTrigger) * padSpeed);
    }

    // Drive the camera whenever auto-orbit is off (the controller owns it).
    if (!renderer.IsOrbitEnabled()) {
        renderer.GetCamera().LookAt(pos_, pos_ + forward);
    }
}

} // namespace hbe
