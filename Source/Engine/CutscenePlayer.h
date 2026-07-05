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

// Apply the cutscene's POSE at absolute time t: the camera (only when
// applyCamera and a camera track exists) and every animation track's transform
// keyframes. Deterministic and idempotent - safe to drive from an editor
// scrubber every frame.
void Evaluate(const CutsceneAsset& cs, f32 t, Scene& scene, Camera& camera,
              bool applyCamera);

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
