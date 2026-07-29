// Scene/CameraSystem.h - drives the render camera from the scene's game cameras.
//
// Evaluates CameraZones to pick the active CameraComponent, computes the desired
// pose from its behaviour preset (Static / FirstPerson / ThirdPerson / Orbit /
// Distance / Spline) and rotation mode (Free / LookAt / SlowFollow / Spin /
// Fixed), and smoothly damps the render camera toward it. Used by the runtime
// every frame and by the editor in play mode.
#pragma once

#include "Core/Types.h"
#include "Scene/CameraRig.h" // the cinematic layer applied on top of the pose

#include <functional>
#include <glm/glm.hpp>
#include <string>

namespace hbe {

class Scene;
class Camera;
class Input;

namespace cam {

// Persistent smoothing/blend state owned by the caller (the Engine). Keeping the
// previous pose here lets camera switches (zones, primary changes) blend instead
// of cutting. Reset `valid` to false to snap on the next update (e.g. when play
// mode begins).
struct CameraState {
    bool valid = false;
    glm::vec3 position{0.0f};
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    f32 fovY = 60.0f;
    // Cinematic rig state (handheld/breathing/shake/framing). Persisting it here
    // keeps the noise continuous across frames and across zone switches.
    CinematicState cinematic;
    // Smoothed collision boom length: collisions shorten it instantly but it
    // extends back out at collisionReturnSpeed, so clearing a corner does not
    // snap the shot. Negative = no constraint recorded yet.
    f32 collisionBoom = -1.0f;
};

// Adds camera shake (0..1 trauma) to a live camera state. Impacts / explosions
// call this; the rig decays it.
void AddShake(CameraState& state, f32 trauma);

// World raycast for camera collision: returns the closest hit distance along
// origin + dir*maxDist, or maxDist when clear. Empty = no collision test.
using RaycastFn = std::function<f32(const glm::vec3& origin, const glm::vec3& dir, f32 maxDist)>;

// Updates `camera` from the scene's active game camera (zone-selected or
// primary). `input` drives third-person player look; `raycast` (optional) pulls
// the boom in off world geometry. A camera zone now OVERRIDES the base camera's
// values for the frame instead of swapping cameras, so runtime look state
// persists across zone boundaries. Returns false when the scene has no usable
// CameraComponent (the caller then keeps the editor/orbit camera).
// `aspect` is the viewport aspect ratio, needed to turn a normalized framing
// offset into a horizontal angle; pass the render camera's.
bool Update(Scene& scene, Camera& camera, CameraState& state, f32 dt, const Input& input,
            const RaycastFn& raycast = {}, f32 aspect = 1.7777778f);

} // namespace cam
} // namespace hbe
