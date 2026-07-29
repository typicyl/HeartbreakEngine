// Scene/CameraRig.h - the CINEMATIC layer that sits on top of a camera pose.
//
// cam::Update produces a "correct" pose: the right place, aimed at the right
// thing, smoothly damped. That reads as a tripod - mathematically perfect and
// completely lifeless. Everything a real camera brings to a shot is what this
// module adds, as a stack of post-pose modifiers:
//
//   HANDHELD  - the operator. Layered value noise on position and rotation, with
//               separate translation/rotation gains, so the frame breathes and
//               drifts the way a shoulder-mounted rig does. Frequency-scaled per
//               axis (roll wanders slower than yaw) because uniform noise on all
//               six axes reads as a vibration, not a person.
//   BREATHING - the operator's lungs. A slow sinusoid (~0.25 Hz) with a slight
//               vertical bias, deliberately separate from handheld: it survives
//               when handheld is dialled out, which is what "locked-off but
//               alive" shots need.
//   SHAKE     - impulse trauma (explosions, impacts). An amplitude that decays
//               over time and drives high-frequency noise; trauma is squared on
//               the way in so small hits stay subtle and big ones hit hard.
//   FRAMING   - composition. Offsets the AIM so the subject sits where a
//               cinematographer would put it (rule-of-thirds, headroom, lead
//               room in the direction of travel) instead of dead-centre.
//
// All of it is pure and stateless-per-call except for the small CinematicState
// the caller owns, so the editor can preview a rig by ticking it with its own
// state. Amplitudes of zero make every layer an exact no-op, so a camera that
// opts out is bit-identical to the pose cam::Update produced.
#pragma once

#include "Core/Types.h"

#include <glm/glm.hpp>

namespace hbe::cam {

// Authored cinematic settings. Lives on CameraComponent (and is overridable by a
// camera zone / cutscene), so a shot can be tuned per camera.
struct CinematicSettings {
    // -- Handheld ------------------------------------------------------------
    bool handheld = false;
    f32 handheldPosAmount = 0.02f;  // metres of positional wander
    f32 handheldRotAmount = 0.35f;  // degrees of rotational wander
    f32 handheldFrequency = 1.1f;   // base Hz of the wander
    f32 handheldRoll = 0.5f;        // extra roll gain (0 = no roll; roll sells "handheld")
    // Higher = sharper, more nervous motion (adds an octave of detail noise).
    f32 handheldSharpness = 0.35f;

    // -- Breathing -----------------------------------------------------------
    bool breathing = false;
    f32 breathAmount = 0.012f;  // metres of travel (mostly vertical)
    f32 breathRate = 0.25f;     // Hz (~15 breaths/min at rest)

    // -- Framing / composition -----------------------------------------------
    // Screen-space position the LOOK TARGET should occupy, in normalized screen
    // coordinates where (0,0) is centre and (+1,+1) is the top-right corner.
    // (0,0) = the default dead-centre framing.
    bool framing = false;
    f32 framingX = 0.0f;   // -1..1; +/-0.33 puts the subject on a thirds line
    f32 framingY = 0.15f;  // >0 lifts the subject = headroom below it
    // Lead room: shifts framing OPPOSITE the target's motion so it has space to
    // move into. 0 = off; 1 = a full framingX-worth of lead at leadSpeed.
    f32 leadAmount = 0.0f;
    f32 leadSpeed = 6.0f;      // target speed (m/s) that produces full lead
    f32 framingDamping = 3.0f; // how fast the framing offset eases (0 = instant)
};

// Per-camera runtime state the caller owns (the Engine for the game camera, the
// editor for a preview). Reset it to snap instead of ease.
struct CinematicState {
    f32 time = 0.0f;            // seconds the rig has been running (drives the noise)
    f32 shakeTrauma = 0.0f;     // 0..1, decays every tick
    glm::vec2 framing{0.0f};    // eased framing offset (normalized screen units)
    glm::vec3 lastTargetPos{0.0f};
    bool hasLastTarget = false;
};

// The pose the rig modifies. Position/forward are world space; `up` lets the rig
// return roll (which a forward vector alone cannot express).
struct CinematicPose {
    glm::vec3 position{0.0f};
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
};

// Adds impulse trauma (explosion, impact, heavy landing). Accumulates, clamped to
// 1. Trauma decays on its own in Apply.
void AddShake(CinematicState& state, f32 trauma);

// Applies the whole cinematic stack to `pose`, in place.
//   dt          - frame delta (advances noise time + decays trauma)
//   targetPos   - the framed subject's world position; pass the camera's look
//                 target. Framing is skipped when `hasTarget` is false.
//   fovYDegrees - needed to convert a normalized screen offset into an angle.
//   aspect      - viewport aspect (width/height), for the horizontal conversion.
void Apply(CinematicPose& pose, CinematicState& state, const CinematicSettings& s, f32 dt,
           const glm::vec3& targetPos, bool hasTarget, f32 fovYDegrees, f32 aspect);

// Smooth value noise in [-1,1]. Exposed because the cutscene shake track and the
// editor preview both want the same curve the rig uses.
f32 Noise1D(f32 x, u32 seed);

} // namespace hbe::cam
