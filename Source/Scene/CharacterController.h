// Scene/CharacterController.h - player / character movement.
//
// Drives every entity carrying a CharacterController from this frame's movement
// input (WASD + gamepad left stick), with gravity, jump and optional turn-to-
// face. Pair the moved entity with a CameraComponent whose `target` is it
// (First/Third Person) for a player-controlled view. Kinematic first pass (flat
// ground); capsule-vs-world collision via the physics world is the next step.
#pragma once

#include "Core/Types.h"

#include <glm/glm.hpp>

namespace hbe {

class Scene;
class Input;

namespace character {

// Moves every CharacterController entity. Call once per simulated frame (the
// editor's play mode / the runtime). `cameraForward` is the active view's
// forward, used for camera-relative movement (pass the render camera's).
void Update(Scene& scene, const Input& input, f32 dt, const glm::vec3& cameraForward);

} // namespace character
} // namespace hbe
