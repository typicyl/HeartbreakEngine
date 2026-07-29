// Scene/CameraSystem.cpp
#include "Scene/CameraSystem.h"
#include "Core/Input.h"
#include "Scene/Scene.h"
#include "Renderer/Camera.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <climits>
#include <cmath>

namespace hbe {
namespace cam {
namespace {

constexpr f32 kDeg2Rad = 0.01745329252f;
constexpr f32 kRad2Deg = 57.2957795131f;

// Scene keeps a hashed name index; this used to be a full linear scan with a
// string compare per entity, run several times PER FRAME from Update (follow
// target, zone track, zone camera, spline), so camera cost scaled with total
// scene size for no reason.
entt::entity FindByName(const Scene& scene, const std::string& name) {
    return scene.FindByName(name);
}

// View direction from a yaw (around +Y, 0 = -Z) and a downward pitch in degrees
// (positive looks down): the camera "looks" along this.
glm::vec3 LookDir(f32 yawDeg, f32 pitchDownDeg) {
    const f32 y = yawDeg * kDeg2Rad;
    const f32 p = pitchDownDeg * kDeg2Rad;
    const f32 cp = std::cos(p);
    return glm::normalize(glm::vec3(std::sin(y) * cp, -std::sin(p), -std::cos(y) * cp));
}

glm::vec3 CatmullRom(const std::vector<glm::vec3>& pts, bool loop, f32 t01,
                     glm::vec3* tangentOut) {
    const int n = static_cast<int>(pts.size());
    if (n == 0) {
        if (tangentOut) *tangentOut = glm::vec3(0, 0, -1);
        return glm::vec3(0.0f);
    }
    if (n == 1) {
        if (tangentOut) *tangentOut = glm::vec3(0, 0, -1);
        return pts[0];
    }
    const int segs = loop ? n : (n - 1);
    const f32 tt = glm::clamp(t01, 0.0f, 1.0f) * segs;
    int i = static_cast<int>(std::floor(tt));
    if (i >= segs) i = segs - 1;
    const f32 u = tt - static_cast<f32>(i);
    const auto P = [&](int k) -> glm::vec3 {
        if (loop) return pts[((k % n) + n) % n];
        return pts[glm::clamp(k, 0, n - 1)];
    };
    const glm::vec3 p0 = P(i - 1), p1 = P(i), p2 = P(i + 1), p3 = P(i + 2);
    const f32 u2 = u * u, u3 = u2 * u;
    const glm::vec3 pos = 0.5f * ((2.0f * p1) + (-p0 + p2) * u +
                                  (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * u2 +
                                  (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * u3);
    if (tangentOut) {
        const glm::vec3 d = 0.5f * ((-p0 + p2) + 2.0f * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * u +
                                    3.0f * (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * u2);
        *tangentOut = glm::length(d) > 1e-5f ? glm::normalize(d) : glm::vec3(0, 0, -1);
    }
    return pos;
}

// Resolves which CameraComponent should drive the view this frame: the
// highest-priority active zone's camera (an inline override or a referenced
// entity), else the base/primary camera. Outputs the chosen CameraComponent and
// the world matrix to evaluate it in, and updates every zone's `active` flag.
void ResolveActiveCamera(Scene& scene, entt::entity baseCam, CameraComponent*& outCam,
                         glm::mat4& outWorld) {
    auto& reg = scene.Registry();
    for (const entt::entity ze : reg.view<CameraZone>()) reg.get<CameraZone>(ze).active = false;

    const CameraComponent* base =
        baseCam != entt::null ? &reg.get<CameraComponent>(baseCam) : nullptr;
    outCam = baseCam != entt::null ? &reg.get<CameraComponent>(baseCam) : nullptr;
    outWorld = baseCam != entt::null ? scene.WorldMatrix(baseCam) : glm::mat4(1.0f);

    entt::entity chosenZone = entt::null;
    CameraComponent* zoneCam = nullptr;
    glm::mat4 zoneWorld(1.0f);
    int bestPriority = INT_MIN;

    for (const entt::entity ze : reg.view<CameraZone, Transform>()) {
        CameraZone& zone = reg.get<CameraZone>(ze);
        if (!zone.enabled) continue;

        // The camera this zone provides: its inline settings (evaluated in the
        // zone's own transform) or a referenced CameraComponent entity.
        CameraComponent* zc = nullptr;
        glm::mat4 zw(1.0f);
        if (zone.useSettings) {
            zc = &zone.settings;
            zw = scene.WorldMatrix(ze);
        } else if (!zone.camera.empty()) {
            const entt::entity camE = FindByName(scene, zone.camera);
            if (camE != entt::null && reg.all_of<CameraComponent>(camE)) {
                zc = &reg.get<CameraComponent>(camE);
                zw = scene.WorldMatrix(camE);
            }
        }
        if (!zc) continue;

        // Point tested against the volume: the zone's own track, else its inline
        // target, else the base camera's target, else the base camera itself.
        entt::entity trackE = FindByName(scene, zone.track);
        if (trackE == entt::null && zone.useSettings)
            trackE = FindByName(scene, zone.settings.target);
        if (trackE == entt::null && base) trackE = FindByName(scene, base->target);
        if (trackE == entt::null) trackE = baseCam;
        if (trackE == entt::null) continue;

        const glm::vec3 p = glm::vec3(scene.WorldMatrix(trackE)[3]);
        const glm::vec3 local = glm::vec3(glm::inverse(scene.WorldMatrix(ze)) * glm::vec4(p, 1.0f));
        const glm::vec3 h = glm::max(zone.halfExtents, glm::vec3(1e-3f));
        if (std::abs(local.x) <= h.x && std::abs(local.y) <= h.y && std::abs(local.z) <= h.z) {
            if (zone.priority >= bestPriority) {
                bestPriority = zone.priority;
                chosenZone = ze;
                zoneCam = zc;
                zoneWorld = zw;
            }
        }
    }
    if (chosenZone != entt::null) {
        reg.get<CameraZone>(chosenZone).active = true;
        outCam = zoneCam;
        outWorld = zoneWorld;
    }
}

// Copies AUTHORED camera values from `src` onto `dst`, leaving `dst`'s runtime
// state (look/orbit/spline) intact. A camera zone applies its values this way so
// the base camera keeps accumulating look across zone boundaries.
void OverlayAuthored(CameraComponent& dst, const CameraComponent& src) {
    dst.fovY = src.fovY;
    dst.nearZ = src.nearZ;
    dst.farZ = src.farZ;
    dst.mode = src.mode;
    dst.rotation = src.rotation;
    dst.target = src.target;
    dst.offset = src.offset;
    dst.distance = src.distance;
    dst.yaw = src.yaw;
    dst.pitch = src.pitch;
    dst.positionDamping = src.positionDamping;
    dst.rotationDamping = src.rotationDamping;
    dst.spinSpeed = src.spinSpeed;
    dst.fixedEuler = src.fixedEuler;
    dst.spline = src.spline;
    dst.splineSpeed = src.splineSpeed;
    dst.splineLoop = src.splineLoop;
    dst.playerLook = src.playerLook;
    dst.lookSensitivity = src.lookSensitivity;
    dst.lookStickSpeed = src.lookStickSpeed;
    dst.invertLookY = src.invertLookY;
    dst.lookPitchMin = src.lookPitchMin;
    dst.lookPitchMax = src.lookPitchMax;
    dst.collide = src.collide;
    dst.collisionMinDistance = src.collisionMinDistance;
    dst.collisionPadding = src.collisionPadding;
    dst.collisionReturnSpeed = src.collisionReturnSpeed;
    // The cinematic rig is authored data like everything else here, so a zone can
    // hand a region its own handheld/framing look.
    dst.cinematic = src.cinematic;
    // NOT copied: primary, lookYaw/lookPitch/lookInit, orbitAngle, splineT.
}

} // namespace

void AddShake(CameraState& state, f32 trauma) { cam::AddShake(state.cinematic, trauma); }

bool Update(Scene& scene, Camera& camera, CameraState& state, f32 dt, const Input& input,
            const RaycastFn& raycast, f32 aspect) {
    auto& reg = scene.Registry();

    // Find the primary (or any) camera entity.
    entt::entity primaryCam = entt::null, anyCam = entt::null;
    for (const entt::entity e : reg.view<CameraComponent, Transform>()) {
        if (anyCam == entt::null) anyCam = e;
        if (reg.get<CameraComponent>(e).primary) { primaryCam = e; break; }
    }
    const entt::entity baseCam = primaryCam != entt::null ? primaryCam : anyCam;
    const std::string baseTarget =
        baseCam != entt::null ? reg.get<CameraComponent>(baseCam).target : std::string();

    // Resolve the override SOURCE: the base camera when no zone is active, or the
    // active zone's settings/referenced camera. A zone OVERRIDES the base
    // camera's authored values for this frame (see OverlayAuthored) rather than
    // swapping cameras, so runtime look/orbit state stays on the base and never
    // resets at a zone boundary. Bail only when nothing at all resolves.
    CameraComponent* srcPtr = nullptr;
    glm::mat4 camWorld(1.0f);
    ResolveActiveCamera(scene, baseCam, srcPtr, camWorld);
    if (!srcPtr) return false;
    CameraComponent* basePtr =
        baseCam != entt::null ? &reg.get<CameraComponent>(baseCam) : srcPtr;
    CameraComponent& base = *basePtr;       // persistent (holds runtime state)
    CameraComponent eff = base;             // working copy, evaluated this frame
    if (srcPtr != basePtr) OverlayAuthored(eff, *srcPtr); // a zone overrides values
    CameraComponent& cam = eff;

    // Effective follow/look target: the active camera's own, else inherit the
    // base camera's (so a zone can change the mode while still tracking the
    // player without re-specifying the target).
    const std::string targetName = cam.target.empty() ? baseTarget : cam.target;
    const entt::entity tgt = FindByName(scene, targetName);
    const bool hasTarget = tgt != entt::null;
    const glm::mat4 tgtWorld = hasTarget ? scene.WorldMatrix(tgt) : glm::mat4(1.0f);
    const glm::vec3 tgtPos = glm::vec3(tgtWorld[3]);
    const glm::vec3 tgtFwd = glm::normalize(glm::vec3(tgtWorld * glm::vec4(0, 0, -1, 0)));
    const f32 tgtYaw = std::atan2(tgtFwd.x, -tgtFwd.z) * kRad2Deg;
    const glm::vec3 pivot = tgtPos + cam.offset; // world-space pivot for boom modes

    // Advance time-based runtime state for the live camera.
    if (cam.rotation == CameraComponent::RotationMode::Spin ||
        cam.mode == CameraComponent::Mode::Orbit) {
        cam.orbitAngle = std::fmod(cam.orbitAngle + cam.spinSpeed * dt, 360.0f);
    }

    // --- Position + base forward from the mode --------------------------------
    glm::vec3 pos = glm::vec3(camWorld[3]);
    glm::vec3 fwd = glm::normalize(glm::vec3(camWorld * glm::vec4(0, 0, -1, 0)));
    const glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
    using Mode = CameraComponent::Mode;
    Mode effMode = cam.mode;
    // Modes that need a target degrade to Static when none is set.
    if (!hasTarget && (effMode == Mode::FirstPerson || effMode == Mode::ThirdPerson ||
                       effMode == Mode::Orbit || effMode == Mode::Distance)) {
        effMode = Mode::Static;
    }

    switch (effMode) {
        case Mode::Static: {
            pos = glm::vec3(camWorld[3]);
            fwd = glm::normalize(glm::vec3(camWorld * glm::vec4(0, 0, -1, 0)));
            break;
        }
        case Mode::FirstPerson: {
            // Eye at the target with a local-space offset; faces the target.
            pos = tgtPos + glm::vec3(tgtWorld * glm::vec4(cam.offset, 0.0f));
            fwd = tgtFwd;
            break;
        }
        case Mode::ThirdPerson: {
            if (cam.playerLook) {
                // Player orbits the camera with the mouse / right stick. Seed the
                // orbit behind the target the first time so it starts sensibly.
                if (!cam.lookInit) {
                    cam.lookYaw = tgtYaw + cam.yaw;
                    cam.lookPitch = cam.pitch;
                    cam.lookInit = true;
                }
                const f32 inv = cam.invertLookY ? -1.0f : 1.0f;
                cam.lookYaw += input.MouseDeltaX() * cam.lookSensitivity;
                cam.lookPitch += inv * input.MouseDeltaY() * cam.lookSensitivity;
                const Input::GamepadState& pad = input.Gamepad(0);
                if (pad.connected) {
                    cam.lookYaw += pad.rightX * cam.lookStickSpeed * dt;
                    cam.lookPitch += inv * pad.rightY * cam.lookStickSpeed * dt;
                }
                cam.lookPitch = glm::clamp(cam.lookPitch, cam.lookPitchMin, cam.lookPitchMax);
                fwd = LookDir(cam.lookYaw, cam.lookPitch);
            } else {
                fwd = LookDir(tgtYaw + cam.yaw, cam.pitch); // trails the target's facing
            }
            pos = pivot - fwd * cam.distance;
            break;
        }
        case Mode::Orbit: {
            fwd = LookDir(cam.orbitAngle, cam.pitch);
            pos = pivot - fwd * cam.distance;
            break;
        }
        case Mode::Distance: {
            fwd = LookDir(cam.yaw, cam.pitch);
            pos = pivot - fwd * cam.distance;
            break;
        }
        case Mode::Spline: {
            const entt::entity se = FindByName(scene, cam.spline);
            if (se != entt::null && reg.all_of<CameraSpline>(se)) {
                const CameraSpline& sp = reg.get<CameraSpline>(se);
                cam.splineT += cam.splineSpeed * dt;
                if (cam.splineLoop) {
                    cam.splineT = std::fmod(cam.splineT, 1.0f);
                    if (cam.splineT < 0.0f) cam.splineT += 1.0f;
                } else {
                    cam.splineT = glm::clamp(cam.splineT, 0.0f, 1.0f);
                }
                glm::vec3 tangent;
                pos = CatmullRom(sp.points, sp.loop, cam.splineT, &tangent);
                fwd = hasTarget ? glm::normalize(tgtPos - pos) : tangent;
            } else {
                pos = glm::vec3(camWorld[3]);
                fwd = glm::normalize(glm::vec3(camWorld * glm::vec4(0, 0, -1, 0)));
            }
            break;
        }
    }

    // --- Camera collision: pull the boom in off world geometry ---------------
    // Cast from the target's pivot toward the camera; if a wall is closer than
    // the boom, shorten it (the position damping below eases the change).
    if (raycast && cam.collide && hasTarget &&
        (effMode == Mode::ThirdPerson || effMode == Mode::Orbit ||
         effMode == Mode::Distance)) {
        const glm::vec3 boom = pos - pivot;
        const f32 dist = glm::length(boom);
        if (dist > 1e-3f) {
            const glm::vec3 dir = boom / dist;
            const f32 pad = glm::max(cam.collisionPadding, 0.0f);
            const f32 hit = raycast(pivot, dir, dist + pad);
            const f32 wanted = glm::clamp(hit - pad, cam.collisionMinDistance, dist);
            // ASYMMETRIC easing: pull IN immediately (any delay puts the camera
            // inside the wall for those frames), but ease back OUT at a bounded
            // speed so clearing a corner doesn't snap the shot to full boom in one
            // frame. This is the difference between "functional" and "shot".
            f32 applied = wanted;
            if (state.valid && state.collisionBoom >= 0.0f) {
                if (wanted > state.collisionBoom) {
                    const f32 rate = glm::max(cam.collisionReturnSpeed, 0.0f);
                    applied = rate <= 0.0f
                                  ? wanted // 0 = legacy instant return
                                  : glm::min(wanted, state.collisionBoom + rate * dt);
                }
            }
            state.collisionBoom = applied;
            pos = pivot + dir * applied;
        } else {
            state.collisionBoom = -1.0f;
        }
    } else {
        state.collisionBoom = -1.0f; // no constraint active; re-arm on next collide
    }

    // --- Rotation mode overrides the aim -------------------------------------
    using Rot = CameraComponent::RotationMode;
    switch (cam.rotation) {
        case Rot::Free: break; // keep the mode's forward
        case Rot::LookAt:
        case Rot::SlowFollow:
            if (hasTarget) fwd = glm::normalize(pivot - pos);
            break;
        case Rot::Spin:
            fwd = LookDir(cam.orbitAngle, cam.pitch);
            break;
        case Rot::Fixed:
            fwd = LookDir(cam.fixedEuler.y, cam.fixedEuler.x);
            break;
    }
    if (glm::length(fwd) < 1e-5f) fwd = glm::vec3(0, 0, -1);
    fwd = glm::normalize(fwd);

    // --- Smooth / snap toward the desired pose -------------------------------
    // The legacy Static+Free combination snaps exactly (no startup ease).
    const bool legacyStatic =
        cam.mode == Mode::Static && cam.rotation == Rot::Free;
    if (!state.valid || legacyStatic) {
        state.position = pos;
        state.forward = fwd;
        state.fovY = cam.fovY;
    } else {
        const f32 pa = cam.positionDamping <= 0.0f
                           ? 1.0f
                           : 1.0f - std::exp(-cam.positionDamping * dt);
        state.position = glm::mix(state.position, pos, glm::clamp(pa, 0.0f, 1.0f));
        // Player-controlled third-person look tracks the input directly (1:1
        // aim); the position still springs behind the target for the usual
        // trailing feel. Other modes ease the aim by rotationDamping.
        if (cam.mode == Mode::ThirdPerson && cam.playerLook) {
            state.forward = fwd;
        } else {
            const f32 rd = cam.rotation == Rot::SlowFollow ? cam.rotationDamping
                                                           : glm::max(cam.rotationDamping, 12.0f);
            const f32 ra = rd <= 0.0f ? 1.0f : 1.0f - std::exp(-rd * dt);
            state.forward =
                glm::normalize(glm::mix(state.forward, fwd, glm::clamp(ra, 0.0f, 1.0f)));
        }
        state.fovY = glm::mix(state.fovY, cam.fovY, glm::clamp(pa, 0.0f, 1.0f));
    }
    state.up = worldUp;
    state.valid = true;

    // --- Cinematic rig -------------------------------------------------------
    // Applied AFTER damping, on the settled pose: handheld wander, breathing,
    // impulse shake and compositional framing. Damping the rig's own noise would
    // just low-pass it back out, which is why this is the last step and does not
    // write back into `state.position/forward` (the smoother must keep tracking
    // the clean pose, or the noise would integrate and drift).
    CinematicPose pose;
    pose.position = state.position;
    pose.forward = state.forward;
    pose.up = state.up;
    Apply(pose, state.cinematic, cam.cinematic, dt, pivot, hasTarget, state.fovY, aspect);

    camera.SetFovY(state.fovY);
    camera.SetClipPlanes(glm::max(cam.nearZ, 0.001f), glm::max(cam.farZ, cam.nearZ + 0.01f));
    camera.LookAt(pose.position, pose.position + pose.forward, pose.up);

    // Persist this frame's runtime state back to the base camera (eff is a copy),
    // so look/orbit/spline progress survive across zone overrides.
    base.lookYaw = cam.lookYaw;
    base.lookPitch = cam.lookPitch;
    base.lookInit = cam.lookInit;
    base.orbitAngle = cam.orbitAngle;
    base.splineT = cam.splineT;
    return true;
}

} // namespace cam
} // namespace hbe
