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
// primary). `input` drives FIRST- and third-person player look; `raycast`
// (optional) pulls the boom in off world geometry. A camera zone now OVERRIDES
// the base camera's values for the frame instead of swapping cameras, so runtime
// look state persists across zone boundaries. Returns false when the scene has
// no usable CameraComponent (the caller then keeps the editor/orbit camera).
// `aspect` is the viewport aspect ratio, needed to turn a normalized framing
// offset into a horizontal angle; pass the render camera's.
//
// `lookEnabled` is the CURSOR-LOCK GATE. Mouse deltas accumulate whether the
// cursor is locked or free (Input_Win32 feeds both paths into the same delta),
// so the motion that clicks a menu button would otherwise also turn the camera.
// The shipped runtime passes false whenever it has freed the cursor for a menu
// or a dialogue choice; the editor - which never locks the cursor, because that
// would trap it away from the panels - always passes true. A false frame still
// SEEDS and CLAMPS look state, it only skips the accumulation, so nothing snaps
// when the menu closes.
bool Update(Scene& scene, Camera& camera, CameraState& state, f32 dt, const Input& input,
            const RaycastFn& raycast = {}, f32 aspect = 1.7777778f,
            bool lookEnabled = true);

// --test-fpslook: proves the FIRST-PERSON look contract headlessly - a mouse
// delta produces the expected yaw/pitch at a known sensitivity, pitch clamps at
// lookPitchMin/Max, invertLookY flips it, the right stick drives it, the
// CHARACTER's yaw follows the camera while its pitch does not, first and third
// person accumulate look identically, the cursor-lock gate suppresses it, and a
// scene round-trips the camera unchanged (runtime look state excluded). No GPU,
// no window, no project. Same contract as --test-seamweld.
bool FirstPersonLookSelfTest();

} // namespace cam
} // namespace hbe
