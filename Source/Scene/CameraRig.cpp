// Scene/CameraRig.cpp
#include "Scene/CameraRig.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace hbe::cam {
namespace {

constexpr f32 kDeg2Rad = 0.01745329252f;

// Deterministic hash -> [0,1). Used as the lattice for the value noise below, so
// the motion is identical for a given (time, seed) - important for the movie
// renderer's fixed-dt deterministic capture.
f32 Hash(i32 n, u32 seed) {
    u32 h = static_cast<u32>(n) * 374761393u + seed * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<f32>(h) * (1.0f / 4294967296.0f);
}

} // namespace

f32 Noise1D(f32 x, u32 seed) {
    const f32 fl = std::floor(x);
    const i32 i = static_cast<i32>(fl);
    const f32 t = x - fl;
    // Smoothstep the interpolant: C1 continuity, so the camera never kinks.
    const f32 u = t * t * (3.0f - 2.0f * t);
    const f32 a = Hash(i, seed), b = Hash(i + 1, seed);
    return (a + (b - a) * u) * 2.0f - 1.0f; // [0,1) -> [-1,1)
}

namespace {

// Two octaves: a slow base drift plus a quieter faster detail layer. One octave
// alone reads as a mechanical wobble; the second is what makes it feel human.
f32 Wander(f32 t, u32 seed, f32 sharpness) {
    const f32 base = Noise1D(t, seed);
    const f32 detail = Noise1D(t * 2.7f, seed ^ 0x9E3779B9u);
    return base + detail * std::clamp(sharpness, 0.0f, 1.0f) * 0.5f;
}

} // namespace

void AddShake(CinematicState& state, f32 trauma) {
    state.shakeTrauma = std::clamp(state.shakeTrauma + trauma, 0.0f, 1.0f);
}

void Apply(CinematicPose& pose, CinematicState& state, const CinematicSettings& s, f32 dt,
           const glm::vec3& targetPos, bool hasTarget, f32 fovYDegrees, f32 aspect) {
    state.time += dt;

    // Trauma decays toward 0 over ~1s; squaring it on use keeps small hits subtle.
    if (state.shakeTrauma > 0.0f)
        state.shakeTrauma = std::max(0.0f, state.shakeTrauma - dt);

    // Build the camera basis once; every layer works in it so the motion is
    // relative to where the camera looks, not to world axes.
    glm::vec3 fwd = pose.forward;
    if (glm::length(fwd) < 1e-5f) fwd = glm::vec3(0, 0, -1);
    fwd = glm::normalize(fwd);
    glm::vec3 up = glm::length(pose.up) > 1e-5f ? glm::normalize(pose.up) : glm::vec3(0, 1, 0);
    glm::vec3 right = glm::cross(fwd, up);
    if (glm::length(right) < 1e-5f) right = glm::vec3(1, 0, 0);
    right = glm::normalize(right);
    up = glm::normalize(glm::cross(right, fwd));

    f32 yawDeg = 0.0f, pitchDeg = 0.0f, rollDeg = 0.0f;
    glm::vec3 posOffset(0.0f);

    // --- Framing / composition ----------------------------------------------
    // Offsetting the AIM (not the position) is what makes this composition rather
    // than a dolly: the camera stays put and re-points so the subject lands on the
    // requested screen coordinate.
    if (s.framing && hasTarget) {
        glm::vec2 want{s.framingX, s.framingY};

        // Lead room: push the subject toward the BACK of frame relative to its
        // motion, leaving empty screen in the direction it is heading.
        if (s.leadAmount > 0.0f && state.hasLastTarget && dt > 1e-6f) {
            const glm::vec3 vel = (targetPos - state.lastTargetPos) / dt;
            const f32 speedAlongRight = glm::dot(vel, right);
            const f32 norm = s.leadSpeed > 1e-3f ? speedAlongRight / s.leadSpeed : 0.0f;
            want.x -= std::clamp(norm, -1.0f, 1.0f) * s.leadAmount * std::abs(s.framingX > 0.0f ? s.framingX : 0.33f);
        }

        const f32 a = s.framingDamping <= 0.0f ? 1.0f
                                               : 1.0f - std::exp(-s.framingDamping * dt);
        state.framing = glm::mix(state.framing, want, std::clamp(a, 0.0f, 1.0f));

        // Normalized screen offset -> angle. A point at normalized y maps to
        // atan(y * tan(fov/2)); the x axis additionally scales by aspect.
        const f32 tanHalf = std::tan(std::max(fovYDegrees, 1.0f) * 0.5f * kDeg2Rad);
        pitchDeg += std::atan(state.framing.y * tanHalf) / kDeg2Rad;
        yawDeg -= std::atan(state.framing.x * tanHalf * std::max(aspect, 0.01f)) / kDeg2Rad;
    }
    state.lastTargetPos = targetPos;
    state.hasLastTarget = hasTarget;

    // --- Handheld operator ---------------------------------------------------
    if (s.handheld) {
        const f32 f = std::max(s.handheldFrequency, 0.0f);
        const f32 t = state.time * f;
        // Distinct seeds per axis, and deliberately different rates: roll drifts
        // slowest, yaw/pitch faster. Uniform rates read as a buzz, not an operator.
        const f32 nx = Wander(t * 1.00f, 11u, s.handheldSharpness);
        const f32 ny = Wander(t * 1.13f, 23u, s.handheldSharpness);
        const f32 nz = Wander(t * 0.79f, 37u, s.handheldSharpness);
        const f32 ry = Wander(t * 0.91f, 53u, s.handheldSharpness);
        const f32 rp = Wander(t * 1.07f, 71u, s.handheldSharpness);
        const f32 rr = Wander(t * 0.61f, 97u, s.handheldSharpness);

        posOffset += right * (nx * s.handheldPosAmount);
        posOffset += up * (ny * s.handheldPosAmount);
        posOffset += fwd * (nz * s.handheldPosAmount * 0.5f); // less along the lens
        yawDeg += ry * s.handheldRotAmount;
        pitchDeg += rp * s.handheldRotAmount;
        rollDeg += rr * s.handheldRotAmount * s.handheldRoll;
    }

    // --- Breathing -----------------------------------------------------------
    // Separate from handheld on purpose: a locked-off shot with breathing but no
    // handheld is a real and useful look.
    if (s.breathing) {
        const f32 phase = state.time * s.breathRate * 6.2831853f;
        const f32 b = std::sin(phase);
        // Chest rise is mostly vertical with a little forward sway, and the
        // half-rate horizontal term keeps it from looking like a pure bob.
        posOffset += up * (b * s.breathAmount);
        posOffset += right * (std::sin(phase * 0.5f) * s.breathAmount * 0.35f);
        pitchDeg += b * s.breathAmount * 12.0f; // small sympathetic tilt
    }

    // --- Impulse shake -------------------------------------------------------
    if (state.shakeTrauma > 0.0f) {
        // Squaring makes the falloff feel physical: a big hit dominates, a small
        // one stays polite.
        const f32 amp = state.shakeTrauma * state.shakeTrauma;
        const f32 t = state.time * 22.0f; // high frequency: this is an impact
        posOffset += right * (Noise1D(t, 131u) * amp * 0.25f);
        posOffset += up * (Noise1D(t, 151u) * amp * 0.25f);
        yawDeg += Noise1D(t * 1.1f, 173u) * amp * 3.0f;
        pitchDeg += Noise1D(t * 1.3f, 191u) * amp * 3.0f;
        rollDeg += Noise1D(t * 0.9f, 211u) * amp * 2.0f;
    }

    // --- Compose -------------------------------------------------------------
    pose.position += posOffset;
    if (yawDeg != 0.0f || pitchDeg != 0.0f) {
        // Rotate the forward vector in the camera's own basis so the offsets stay
        // frame-relative regardless of where the camera is pointing.
        const glm::mat4 ry = glm::rotate(glm::mat4(1.0f), -yawDeg * kDeg2Rad, up);
        const glm::mat4 rp = glm::rotate(glm::mat4(1.0f), pitchDeg * kDeg2Rad, right);
        fwd = glm::normalize(glm::vec3(rp * ry * glm::vec4(fwd, 0.0f)));
    }
    if (rollDeg != 0.0f) {
        // Roll tilts UP about the (new) view axis - the forward vector is unchanged.
        const glm::mat4 rr = glm::rotate(glm::mat4(1.0f), rollDeg * kDeg2Rad, fwd);
        up = glm::normalize(glm::vec3(rr * glm::vec4(up, 0.0f)));
    }
    pose.forward = fwd;
    // Re-orthogonalize so the caller always gets a clean basis.
    glm::vec3 r2 = glm::cross(fwd, up);
    if (glm::length(r2) < 1e-5f) r2 = right;
    pose.up = glm::normalize(glm::cross(glm::normalize(r2), fwd));
}

} // namespace hbe::cam
