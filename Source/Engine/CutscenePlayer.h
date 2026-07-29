// Engine/CutscenePlayer.h - stateless evaluation of a .hbcutscene timeline.
//
// The per-frame cutscene work is split into two pieces so both the runtime
// (Engine::UpdateCutscene) and the editor's timeline preview can drive it:
//   - Evaluate(): applies the POSE at an absolute time t (camera + transform
//     keyframes). Pure and idempotent, so an editor scrubber can call it every
//     frame at any time without side effects.
//   - FireMarkers(): fires the EVENTS crossed within a [prev, t) interval
//     (skeletal-clip triggers + dialogue markers). Not idempotent - only call
//     while the timeline is actually advancing.
#pragma once

#include "Assets/CutsceneAsset.h"
#include "Core/Types.h"

namespace hbe {
class Scene;
class Camera;

namespace cutscene {

// Depth-of-field the camera track asks for at a given time. The player computes
// it but does NOT apply it - the caller owns PostSettings and decides whether a
// cutscene may drive focus (the editor preview does; a headless bake might not).
struct FocusState {
    bool valid = false;   // false = the cutscene has no camera track / no opinion
    f32 distance = 0.0f;  // world units to the focus plane
    f32 range = 3.0f;     // sharp band around it
    f32 aperture = 0.0f;  // 0 = leave the scene's blur strength alone
};

// Apply the cutscene's POSE at absolute time t: the camera (only when
// applyCamera and a camera track exists) and every animation track's transform
// keyframes. Deterministic and idempotent - safe to drive from an editor
// scrubber every frame. `outFocus` (optional) receives the rack-focus state.
void Evaluate(const CutsceneAsset& cs, f32 t, Scene& scene, Camera& camera,
              bool applyCamera, FocusState* outFocus = nullptr);

// Fire the EVENTS whose time lies in the half-open interval [prev, t): skeletal
// clip triggers (restart the target's Animator) and dialogue/voiceline markers
// (queued via game::). Not idempotent - only call while the timeline advances
// (playback), never on a scrub jump, or events fire repeatedly. fireDialogue is
// false for the editor preview (the game-command queue only drains while the
// game is running, so editor-fired dialogue would pile up stale); clip triggers
// still fire so the skeleton animates in the preview.
void FireMarkers(const CutsceneAsset& cs, f32 prev, f32 t, Scene& scene,
                 bool fireDialogue = true);

} // namespace cutscene
} // namespace hbe
