// Renderer/CameraController.h - flythrough camera driven by the engine Input.
//
// Shared by the editor (RMB over the viewport + WASD/QE) and the game runtime
// (same controls full-window, plus gamepad sticks). The caller decides when
// mouse-fly mode is wanted (e.g. only while hovering the editor viewport); a
// connected gamepad always steers once its sticks move.
#pragma once

#include "Core/Types.h"

#include <glm/glm.hpp>

namespace hbe {

class Input;
class Renderer;
class Camera;

class FlyCameraController {
public:
    // Advances the camera. `wantMouseFly` = mouse-look engaged this frame
    // (typically "RMB held"). Movement: WASD/QE (Shift = fast) or left stick /
    // triggers; look: mouse delta or right stick. Disables the renderer's
    // auto-orbit on first use and drives the camera while orbit stays off.
    void Update(const Input& input, Renderer& renderer, f32 dt, bool wantMouseFly);

    // Captures the live camera (prevents a snap when taking over from orbit).
    void SyncFrom(const Camera& camera);

    bool IsFlying() const { return flying_; }

private:
    void TakeControl(Renderer& renderer);

    glm::vec3 pos_{0.0f, 2.5f, 9.5f};
    f32  yaw_ = -90.0f;   // degrees; -90 looks toward -Z
    f32  pitch_ = -12.0f;
    bool flying_ = false;
};

} // namespace hbe
