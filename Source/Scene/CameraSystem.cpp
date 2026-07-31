// Scene/CameraSystem.cpp
#include "Scene/CameraSystem.h"
#include "Core/Input.h"
#include "Core/Log.h"
#include "Scene/CharacterController.h" // --test-fpslook: the body-facing half
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"     // --test-fpslook: the round-trip half
#include "Renderer/Camera.h"
#include "Renderer/Renderer.h"         // --test-fpslook: device-less Instantiate

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp> // angleAxis / quat_cast (first-person body yaw)

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

// The world rotation whose FORWARD is LookDir(yawDeg, 0). This is the inverse of
// the `atan2(fwd.x, -fwd.z)` yaw used above, and it is expressed against the
// engine-wide entity forward - local -Z, the same one tgtFwd, AISystem's
// perception cone and the render camera all read (world * vec4(0,0,-1,0)).
// Deriving it here rather than reusing CharacterController's own yaw->quat is
// deliberate: see the note on the first-person body write below.
glm::quat YawQuat(f32 yawDeg) {
    return glm::angleAxis(-yawDeg * kDeg2Rad, glm::vec3(0.0f, 1.0f, 0.0f));
}

// Writes a WORLD yaw onto an entity's Transform, rebasing through a parent so a
// player mounted on a moving platform turns by the yaw the player asked for and
// not by that yaw plus the platform's. Same shape as the rebase PhysicsWorld
// does when it writes a parented capsule's world position back. A parent basis
// that is degenerate or sheared is left alone (quat_cast would produce nonsense);
// that case writes the world value directly, which is what the unparented path
// does anyway.
void SetWorldYaw(Scene& scene, entt::entity e, const glm::quat& worldYaw) {
    entt::registry& reg = scene.Registry();
    Transform* t = reg.try_get<Transform>(e);
    if (!t) return;
    if (const Parent* par = reg.try_get<Parent>(e); par && reg.valid(par->entity)) {
        const glm::mat4 pw = scene.WorldMatrix(par->entity);
        const glm::vec3 c0(pw[0]), c1(pw[1]), c2(pw[2]);
        const f32 l0 = glm::length(c0), l1 = glm::length(c1), l2 = glm::length(c2);
        if (l0 > 1e-6f && l1 > 1e-6f && l2 > 1e-6f) {
            const glm::mat3 basis(c0 / l0, c1 / l1, c2 / l2);
            t->rotation = glm::normalize(glm::inverse(glm::quat_cast(basis)) * worldYaw);
            return;
        }
    }
    t->rotation = worldYaw;
}

// Accumulates PLAYER LOOK (mouse + right stick) into cam.lookYaw/lookPitch and
// returns the aim direction. Shared by First and Third Person: the two modes
// differ in where the camera SITS, never in how it aims, and a second copy of
// this loop is exactly how they would drift apart.
//
// `lookEnabled` gates only the accumulation (see CameraSystem.h): the seed and
// the clamp still run, so a frame spent in a menu neither snaps the aim nor
// leaves an out-of-range pitch behind when the clamp is re-authored.
glm::vec3 PlayerLook(CameraComponent& cam, const Input& input, f32 dt, bool lookEnabled,
                     f32 seedYaw, f32 seedPitch) {
    // Seed once so the camera starts where the target is already facing rather
    // than snapping to world north on the first frame.
    if (!cam.lookInit) {
        cam.lookYaw = seedYaw;
        cam.lookPitch = seedPitch;
        cam.lookInit = true;
    }
    if (lookEnabled) {
        const f32 inv = cam.invertLookY ? -1.0f : 1.0f;
        cam.lookYaw += input.MouseDeltaX() * cam.lookSensitivity;
        cam.lookPitch += inv * input.MouseDeltaY() * cam.lookSensitivity;
        const Input::GamepadState& pad = input.Gamepad(0);
        if (pad.connected) {
            cam.lookYaw += pad.rightX * cam.lookStickSpeed * dt;
            cam.lookPitch += inv * pad.rightY * cam.lookStickSpeed * dt;
        }
    }
    cam.lookPitch = glm::clamp(cam.lookPitch, cam.lookPitchMin, cam.lookPitchMax);
    // THE AUTHOR'S CLAMP CANNOT BE ALLOWED TO REACH +/-90. At exactly straight up or
    // straight down LookDir returns (0,-+1,0), which is PARALLEL to the world up that
    // Camera::View passes to glm::lookAtRH - the cross product is zero and the whole
    // view matrix is NaN for that frame. The inspector's drag is limited to +/-89,
    // but lookPitchMin/Max are parsed with .value() and also ride inline in a
    // CameraZone's settings, so a hand-edited or pre-inspector file can hold 90. The
    // length guard downstream cannot catch it: the vector is unit length, just
    // degenerate against up.
    cam.lookPitch = glm::clamp(cam.lookPitch, -89.9f, 89.9f);
    return LookDir(cam.lookYaw, cam.lookPitch);
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
            const RaycastFn& raycast, f32 aspect, bool lookEnabled) {
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
    // A first-person camera the PLAYER aims changes three things downstream: the
    // rotation-mode override stands down (it would fight the aim), the aim is
    // 1:1 instead of damped, and the eye position is not damped either - the eye
    // is rigidly attached to a head, and easing it is how a first-person camera
    // becomes nauseating. Derived from effMode, so a target-less first-person
    // camera that degraded to Static claims none of it.
    const bool fpLook = effMode == Mode::FirstPerson && cam.playerLook;

    switch (effMode) {
        case Mode::Static: {
            pos = glm::vec3(camWorld[3]);
            fwd = glm::normalize(glm::vec3(camWorld * glm::vec4(0, 0, -1, 0)));
            break;
        }
        case Mode::FirstPerson: {
            if (cam.playerLook) {
                // The player aims the eye directly. Seeded from the target's own
                // facing, so the first frame is a continuation, not a snap.
                fwd = PlayerLook(cam, input, dt, lookEnabled, tgtYaw + cam.yaw, cam.pitch);
                // HORIZONTAL LOOK TURNS THE BODY; pitch stays camera-only (a
                // pitched capsule is not a thing). Movement is camera-relative
                // through CharacterController, so the character then walks where
                // the player is looking; the body's own rotation is only what
                // the mesh and anything parented to it (weapon, aim socket)
                // follow.
                //
                // The yaw is written through YawQuat - the convention the rest of
                // the engine reads a facing with - rather than through
                // CharacterController's own yaw->quat, which mirrors X (see the
                // note in that file's Update). Writing it that way keeps this
                // path self-consistent: toggle playerLook off and the `else`
                // branch below re-derives the SAME aim from the body it just
                // wrote, instead of jumping to its mirror image.
                const glm::quat bodyYaw = YawQuat(cam.lookYaw);
                SetWorldYaw(scene, tgt, bodyYaw);
                if (CharacterController* cc = reg.try_get<CharacterController>(tgt))
                    cc->externalFacing = true; // stand faceMoveDir down (one-shot)
                // The eye rides the yaw JUST written, not `tgtWorld` (captured
                // before it): reading the stale matrix would make an off-centre
                // eye offset lag the aim by a frame, which is visible.
                pos = tgtPos + bodyYaw * cam.offset;
            } else {
                // Legacy: eye at the target with a local-space offset, facing the
                // target's forward. Unchanged.
                pos = tgtPos + glm::vec3(tgtWorld * glm::vec4(cam.offset, 0.0f));
                fwd = tgtFwd;
            }
            break;
        }
        case Mode::ThirdPerson: {
            if (cam.playerLook) {
                // Player orbits the camera with the mouse / right stick. Seed the
                // orbit behind the target the first time so it starts sensibly.
                fwd = PlayerLook(cam, input, dt, lookEnabled, tgtYaw + cam.yaw, cam.pitch);
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
    // ...except under first-person player look, where the aim IS the player's
    // input and an override would silently discard it. LookAt is degenerate
    // there anyway (the pivot and the eye are the same point, so `pivot - pos`
    // is a near-zero vector that falls through to the (0,0,-1) guard below), and
    // Fixed/Spin would pin the view while the mouse still turned the body. The
    // inspector says so where the mode is chosen.
    using Rot = CameraComponent::RotationMode;
    switch (fpLook ? Rot::Free : cam.rotation) {
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
        const f32 damp = cam.positionDamping <= 0.0f
                             ? 1.0f
                             : 1.0f - std::exp(-cam.positionDamping * dt);
        // FIRST-PERSON POSITION IS NOT DAMPED. The eye is bolted to a head: any
        // easing slides the whole world against the player's own movement, which
        // is the classic first-person motion-sickness bug rather than a "feel"
        // setting. FOV keeps the authored damping either way, so a FOV kick still
        // eases. Third person is untouched - its position still springs behind
        // the target for the usual trailing feel.
        const f32 pa = fpLook ? 1.0f : damp;
        state.position = glm::mix(state.position, pos, glm::clamp(pa, 0.0f, 1.0f));
        // Player-controlled look tracks the input directly (1:1 aim), in first
        // person as in third: routing a mouse delta through the rotation smoother
        // is felt immediately as lag. Other modes ease the aim by rotationDamping.
        if (fpLook || (cam.mode == Mode::ThirdPerson && cam.playerLook)) {
            state.forward = fwd;
        } else {
            const f32 rd = cam.rotation == Rot::SlowFollow ? cam.rotationDamping
                                                           : glm::max(cam.rotationDamping, 12.0f);
            const f32 ra = rd <= 0.0f ? 1.0f : 1.0f - std::exp(-rd * dt);
            state.forward =
                glm::normalize(glm::mix(state.forward, fwd, glm::clamp(ra, 0.0f, 1.0f)));
        }
        state.fovY = glm::mix(state.fovY, cam.fovY, glm::clamp(damp, 0.0f, 1.0f));
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

// --- --test-fpslook -----------------------------------------------------------
// See CameraSystem.h for what this claims. Everything below is headless: a Scene,
// a bare Camera, a real Input driven through its platform event sink, and (for
// the round-trip section) a device-less Renderer whose uploads return invalid
// handles - the same headless contract the serializer self-tests keep.

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

// A camera + a body it drives, wired the way a level would wire them.
struct LookRig {
    Scene scene;
    Camera camera;
    CameraState state;
    Input input;
    entt::entity player = entt::null;
    entt::entity camEnt = entt::null;

    CameraComponent& Cam() { return scene.Registry().get<CameraComponent>(camEnt); }
    CharacterController& CC() { return scene.Registry().get<CharacterController>(player); }
    Transform& Body() { return scene.Registry().get<Transform>(player); }

    // The BODY's facing read the way every other system reads a facing: the
    // entity's world -Z. Deliberately not read back off the quaternion the
    // camera wrote - that would only prove the camera can remember its own
    // number.
    glm::vec3 BodyForward() {
        return glm::normalize(glm::vec3(scene.WorldMatrix(player) * glm::vec4(0, 0, -1, 0)));
    }

    // One frame of mouse look. The zero-stick injection NEUTRALISES any real
    // controller plugged into the machine running this: NewFrame polls XInput,
    // and a resting stick with drift would otherwise add look nobody asked for
    // and make the expected numbers below machine-dependent.
    bool Step(f32 dx, f32 dy, f32 dt = kDt, bool lookEnabled = true) {
        input.NewFrame(); // clears last frame's delta, polls the pad
        input.InjectGamepadSticksForTest(0.0f, 0.0f, 0.0f, 0.0f);
        input.OnMouseLockedDelta(dx, dy);
        return Update(scene, camera, state, dt, input, {}, 1.7777778f, lookEnabled);
    }
    // One frame of right-stick look.
    bool StepPad(f32 rx, f32 ry, f32 dt = kDt) {
        input.NewFrame();
        input.InjectGamepadSticksForTest(0.0f, 0.0f, rx, ry);
        return Update(scene, camera, state, dt, input, {}, 1.7777778f, true);
    }
};

void BuildRig(LookRig& r, CameraComponent::Mode mode, bool playerLook = true) {
    entt::registry& reg = r.scene.Registry();
    r.player = r.scene.CreateEntity("Player");
    reg.emplace<Transform>(r.player, Transform{});
    reg.emplace<CharacterController>(r.player, CharacterController{});
    r.camEnt = r.scene.CreateEntity("Cam");
    reg.emplace<Transform>(r.camEnt, Transform{});
    CameraComponent c;
    c.mode = mode;
    c.primary = true;
    c.target = "Player";
    c.offset = glm::vec3(0.0f, 1.7f, 0.0f);
    c.distance = 4.0f;
    c.yaw = 0.0f;
    c.pitch = 0.0f; // a 0 seed keeps the arithmetic below readable
    c.playerLook = playerLook;
    c.lookSensitivity = 0.2f;   // degrees per pixel
    c.lookStickSpeed = 160.0f;  // degrees per second at full deflection
    c.lookPitchMin = -80.0f;
    c.lookPitchMax = 80.0f;
    reg.emplace<CameraComponent>(r.camEnt, c);
}

bool Near(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }
bool Near(const glm::vec3& a, const glm::vec3& b, f32 eps = 1e-3f) {
    return Near(a.x, b.x, eps) && Near(a.y, b.y, eps) && Near(a.z, b.z, eps);
}

} // namespace

bool FirstPersonLookSelfTest() {
    bool ok = true;
    const auto expect = [&ok](bool cond, const char* what) {
        if (!cond) {
            ok = false;
            HBE_ERROR("fpslook: FAILED - {}", what);
        }
    };

    // --- 1. A mouse delta produces the expected yaw/pitch --------------------
    {
        LookRig r;
        BuildRig(r, CameraComponent::Mode::FirstPerson);
        expect(r.Step(100.0f, 0.0f), "(1) the first-person camera drives the view");
        expect(r.Cam().lookInit, "(1) look state is seeded on the first frame");
        // 100 px * 0.2 deg/px = 20 deg, applied on the SAME frame as the seed.
        expect(Near(r.Cam().lookYaw, 20.0f), "(1) 100 px at 0.2 deg/px = 20 deg of yaw");
        expect(Near(r.Cam().lookPitch, 0.0f), "(1) a horizontal delta produces no pitch");
        r.Step(0.0f, 50.0f);
        expect(Near(r.Cam().lookPitch, 10.0f), "(1) 50 px at 0.2 deg/px = 10 deg of pitch");
        expect(Near(r.Cam().lookYaw, 20.0f), "(1) a vertical delta produces no yaw");
        r.Step(-100.0f, -50.0f);
        expect(Near(r.Cam().lookYaw, 0.0f) && Near(r.Cam().lookPitch, 0.0f),
               "(1) look accumulates (an equal, opposite delta returns to the seed)");
        // The aim is 1:1: the rendered forward IS this frame's look direction,
        // not a damped approach to it. LookDir(20,0) = (sin20, 0, -cos20).
        r.Step(100.0f, 0.0f);
        expect(Near(r.camera.Forward(),
                    glm::vec3(std::sin(20.0f * kDeg2Rad), 0.0f, -std::cos(20.0f * kDeg2Rad))),
               "(1) the rendered forward is the look direction, undamped (1:1 aim)");
    }

    // --- 2. Pitch clamps at the configured limits ----------------------------
    {
        LookRig r;
        BuildRig(r, CameraComponent::Mode::FirstPerson);
        r.Cam().lookPitchMin = -35.0f;
        r.Cam().lookPitchMax = 55.0f;
        for (int i = 0; i < 40; ++i) r.Step(0.0f, 500.0f); // look down, hard
        expect(Near(r.Cam().lookPitch, 55.0f), "(2) pitch clamps at lookPitchMax");
        for (int i = 0; i < 80; ++i) r.Step(0.0f, -500.0f); // and back up
        expect(Near(r.Cam().lookPitch, -35.0f), "(2) pitch clamps at lookPitchMin");
        // Yaw does NOT clamp - it wraps freely, which is the whole point of a
        // first-person camera.
        const f32 before = r.Cam().lookYaw;
        for (int i = 0; i < 20; ++i) r.Step(500.0f, 0.0f);
        expect(r.Cam().lookYaw > before + 1000.0f, "(2) yaw is unclamped");
    }

    // --- 3. Invert Y --------------------------------------------------------
    {
        LookRig r;
        BuildRig(r, CameraComponent::Mode::FirstPerson);
        r.Cam().invertLookY = true;
        r.Step(0.0f, 50.0f);
        expect(Near(r.Cam().lookPitch, -10.0f), "(3) invertLookY flips the pitch delta");
        LookRig s;
        BuildRig(s, CameraComponent::Mode::FirstPerson);
        s.Step(0.0f, 50.0f);
        expect(Near(s.Cam().lookPitch, 10.0f) && Near(r.Cam().lookPitch, -10.0f),
               "(3) invert is the only difference between the two");
    }

    // --- 4. The right stick drives it ---------------------------------------
    {
        LookRig r;
        BuildRig(r, CameraComponent::Mode::FirstPerson);
        r.StepPad(1.0f, 0.0f, 0.5f); // 160 deg/s for half a second
        expect(Near(r.Cam().lookYaw, 80.0f), "(4) the right stick yaws at lookStickSpeed*dt");
        r.StepPad(0.0f, 0.25f, 0.5f); // 0.25 * 160 * 0.5 = 20 deg
        expect(Near(r.Cam().lookPitch, 20.0f), "(4) stick pitch scales with deflection");
        // Stick look is frame-rate independent (it is a rate); the mouse is not
        // (it is already a displacement). Half the dt, half the travel.
        const f32 y0 = r.Cam().lookYaw;
        r.StepPad(1.0f, 0.0f, 0.25f);
        expect(Near(r.Cam().lookYaw, y0 + 40.0f), "(4) stick look scales with dt");
        // A resting stick contributes nothing (this is also what makes the mouse
        // sections above independent of whatever pad is plugged in).
        LookRig s;
        BuildRig(s, CameraComponent::Mode::FirstPerson);
        s.Step(0.0f, 0.0f, 0.5f);
        expect(Near(s.Cam().lookYaw, 0.0f) && Near(s.Cam().lookPitch, 0.0f),
               "(4) a resting stick and a still mouse produce no look");
    }

    // --- 5. The character's YAW follows the camera; its pitch does not -------
    {
        LookRig r;
        BuildRig(r, CameraComponent::Mode::FirstPerson);
        r.Step(100.0f, 50.0f); // 20 deg yaw, 10 deg (downward) pitch
        const glm::vec3 camFwd = r.camera.Forward();
        const glm::vec3 bodyFwd = r.BodyForward();
        expect(camFwd.y < -0.15f, "(5) the CAMERA is pitched down");
        expect(Near(bodyFwd.y, 0.0f, 1e-4f), "(5) the BODY is not pitched - yaw only");
        // Same heading, independently computed: the body's world -Z must equal
        // the camera's forward flattened onto the ground plane.
        const glm::vec3 flat = glm::normalize(glm::vec3(camFwd.x, 0.0f, camFwd.z));
        expect(Near(bodyFwd, flat), "(5) the body's facing IS the camera's heading");
        // And it is the heading the arithmetic says: LookDir(20, 0).
        expect(Near(bodyFwd,
                    glm::vec3(std::sin(20.0f * kDeg2Rad), 0.0f, -std::cos(20.0f * kDeg2Rad))),
               "(5) the body's facing is the accumulated look yaw");
        // Turning further keeps them locked together.
        r.Step(350.0f, 0.0f); // +70 deg -> 90 total
        expect(Near(r.Cam().lookYaw, 90.0f), "(5) yaw accumulated to 90 deg");
        expect(Near(r.BodyForward(), glm::vec3(1.0f, 0.0f, 0.0f), 1e-3f),
               "(5) 90 deg of yaw puts the body's forward on +X");
        // Toggling player look OFF must not move the aim: the legacy branch
        // re-derives the forward from the body this branch just wrote, so the
        // two conventions have to agree.
        const glm::vec3 aimed = r.camera.Forward();
        r.Cam().playerLook = false;
        r.Step(0.0f, 0.0f);
        const glm::vec3 kept = r.camera.Forward();
        expect(Near(glm::normalize(glm::vec3(kept.x, 0.0f, kept.z)),
                    glm::normalize(glm::vec3(aimed.x, 0.0f, aimed.z)), 2e-3f),
               "(5) switching player look off keeps the heading (conventions agree)");
    }

    // --- 6. The eye rides the yaw, and is not damped -------------------------
    {
        LookRig r;
        BuildRig(r, CameraComponent::Mode::FirstPerson);
        r.Cam().offset = glm::vec3(0.3f, 1.7f, 0.0f); // deliberately off-centre
        r.Step(450.0f, 0.0f);                          // 90 deg of yaw
        expect(Near(r.Cam().lookYaw, 90.0f), "(6) 450 px at 0.2 deg/px = 90 deg");
        // At 90 deg the body faces +X, so its local +X points at world +Z:
        // the eye offset lands at (0, 1.7, 0.3) with no reference to the helper
        // that produced it.
        expect(Near(r.camera.Position(), glm::vec3(0.0f, 1.7f, 0.3f)),
               "(6) the eye offset rotates with the body's yaw");
        // No positional damping: teleport the body and the eye is there the same
        // frame (positionDamping is 10 by default, which would leave it ~85% behind).
        r.Body().position = glm::vec3(10.0f, 0.0f, -4.0f);
        r.Step(0.0f, 0.0f);
        expect(Near(r.camera.Position(), glm::vec3(10.0f, 1.7f, -3.7f)),
               "(6) the first-person eye is not position-damped");
    }

    // --- 7. The cursor-lock gate --------------------------------------------
    {
        LookRig r;
        BuildRig(r, CameraComponent::Mode::FirstPerson);
        // A menu is up on the very first frame: the look still SEEDS (so nothing
        // snaps when the menu closes) but must not accumulate.
        r.Step(400.0f, 400.0f, kDt, /*lookEnabled=*/false);
        expect(r.Cam().lookInit, "(7) look state seeds even while the cursor is free");
        expect(Near(r.Cam().lookYaw, 0.0f) && Near(r.Cam().lookPitch, 0.0f),
               "(7) a free cursor contributes no look (menu / dialogue choice)");
        r.Step(100.0f, 0.0f, kDt, false);
        expect(Near(r.Cam().lookYaw, 0.0f), "(7) still nothing on a later gated frame");
        r.Step(100.0f, 0.0f, kDt, true);
        expect(Near(r.Cam().lookYaw, 20.0f), "(7) look resumes exactly where it left off");
        // The gate is not a mode: the stick is gated too.
        LookRig s;
        BuildRig(s, CameraComponent::Mode::FirstPerson);
        s.input.NewFrame();
        s.input.InjectGamepadSticksForTest(0.0f, 0.0f, 1.0f, 0.0f);
        Update(s.scene, s.camera, s.state, 0.5f, s.input, {}, 1.7777778f, false);
        expect(Near(s.Cam().lookYaw, 0.0f), "(7) the stick is gated too");
    }

    // --- 8. faceMoveDir stands down while the camera owns the body -----------
    {
        LookRig r;
        BuildRig(r, CameraComponent::Mode::FirstPerson);
        r.Step(100.0f, 0.0f);
        expect(r.CC().externalFacing,
               "(8) the camera latches externalFacing on the body it turned");
        const glm::quat aimed = r.Body().rotation;
        // A frame of character movement: full left stick to the right, which
        // faceMoveDir would normally turn the body toward.
        r.input.NewFrame();
        r.input.InjectGamepadSticksForTest(1.0f, 0.0f, 0.0f, 0.0f);
        character::Update(r.scene, r.input, kDt, r.camera.Forward());
        expect(r.Body().rotation == aimed,
               "(8) faceMoveDir did not fight the camera for the body's yaw");
        expect(!r.CC().externalFacing, "(8) the latch is CONSUMED, not sticky");
        // Positive control: with the latch spent and no camera frame in between,
        // the same input DOES turn the body - so section 8 measures suppression
        // rather than a controller that never turns anything.
        r.input.NewFrame();
        r.input.InjectGamepadSticksForTest(1.0f, 0.0f, 0.0f, 0.0f);
        character::Update(r.scene, r.input, kDt, r.camera.Forward());
        expect(r.Body().rotation != aimed,
               "(8) POSITIVE CONTROL: faceMoveDir turns the body once unlatched");
    }

    // --- 9. First and third person accumulate look IDENTICALLY ---------------
    {
        LookRig fp, tp;
        BuildRig(fp, CameraComponent::Mode::FirstPerson);
        BuildRig(tp, CameraComponent::Mode::ThirdPerson);
        const f32 dx[] = {120.0f, -30.0f, 0.0f, 400.0f, -12.5f};
        const f32 dy[] = {40.0f, 15.0f, -300.0f, 7.0f, 0.0f};
        for (int i = 0; i < 5; ++i) {
            fp.Step(dx[i], dy[i]);
            tp.Step(dx[i], dy[i]);
        }
        // Bit-exact: they run the same accumulator over the same fields. If this
        // ever drifts, one of them grew a private copy of the look loop.
        expect(fp.Cam().lookYaw == tp.Cam().lookYaw && fp.Cam().lookPitch == tp.Cam().lookPitch,
               "(9) first and third person accumulate look bit-identically");
        // Third person is UNCHANGED by this work: its boom still trails at
        // `distance` behind the pivot along the look direction, and its POSITION
        // is still damped (so it needs settling frames to arrive - the very thing
        // first person now skips).
        const glm::vec3 pivot = tp.Body().position + tp.Cam().offset;
        expect(glm::length(tp.camera.Position() - (pivot - tp.camera.Forward() *
                                                               tp.Cam().distance)) > 0.05f,
               "(9) third-person position is STILL damped (it lags a hard turn)");
        for (int i = 0; i < 240; ++i) tp.Step(0.0f, 0.0f);
        expect(Near(tp.camera.Position(), pivot - tp.camera.Forward() * tp.Cam().distance, 1e-2f),
               "(9) third person settles onto its boom");
        // ...and third person does NOT write the body's facing.
        expect(Near(tp.BodyForward(), glm::vec3(0.0f, 0.0f, -1.0f)) && !tp.CC().externalFacing,
               "(9) third person never turns the body itself");
    }

    // --- 10. Aim modes stand down for first-person look (and only there) -----
    {
        LookRig r;
        BuildRig(r, CameraComponent::Mode::FirstPerson);
        r.Cam().rotation = CameraComponent::RotationMode::LookAt;
        r.Step(100.0f, 50.0f);
        expect(Near(r.camera.Forward(), LookDir(r.Cam().lookYaw, r.Cam().lookPitch)),
               "(10) first-person look survives a Look At aim (it would be degenerate)");
        LookRig t;
        BuildRig(t, CameraComponent::Mode::ThirdPerson);
        t.Cam().rotation = CameraComponent::RotationMode::Fixed;
        t.Cam().fixedEuler = glm::vec3(0.0f, 90.0f, 0.0f);
        t.Step(100.0f, 50.0f);
        expect(Near(t.camera.Forward(), LookDir(90.0f, 0.0f), 0.05f),
               "(10) third person's aim override is UNCHANGED");
    }

    // --- 11. playerLook OFF is exactly the old behaviour ---------------------
    {
        LookRig r;
        BuildRig(r, CameraComponent::Mode::FirstPerson, /*playerLook=*/false);
        r.Body().rotation = YawQuat(35.0f);
        const glm::quat authored = r.Body().rotation;
        r.Step(500.0f, 500.0f);
        expect(r.Body().rotation == authored, "(11) look off: the body is never written");
        expect(!r.CC().externalFacing, "(11) look off: faceMoveDir is not suppressed");
        expect(!r.Cam().lookInit, "(11) look off: no look state is seeded");
        expect(Near(r.camera.Forward(), LookDir(35.0f, 0.0f)),
               "(11) look off: the eye still faces the target's own forward");
        expect(Near(r.camera.Position(), glm::vec3(0.0f, 1.7f, 0.0f)),
               "(11) look off: the eye still sits at the target + offset");
    }

    // --- 12. A scene round-trips the camera unchanged ------------------------
    {
        LookRig r;
        BuildRig(r, CameraComponent::Mode::FirstPerson);
        CameraComponent& c = r.Cam();
        // Author every look field to a value nothing defaults to.
        c.lookSensitivity = 0.137f;
        c.lookStickSpeed = 211.0f;
        c.invertLookY = true;
        c.lookPitchMin = -63.5f;
        c.lookPitchMax = 47.25f;
        c.offset = glm::vec3(0.05f, 1.62f, -0.11f);
        c.yaw = 12.5f;
        c.pitch = -3.25f;
        const CameraComponent authored = c;
        r.Step(1000.0f, 200.0f); // leave runtime look state behind too
        expect(r.Cam().lookInit && r.Cam().lookYaw != 0.0f, "(12) runtime look state exists");

        const std::string text = scene::SaveSceneToString(r.scene);
        scene::SceneData data;
        expect(scene::ParseSceneString(text, data),
               "(12) the scene round-trips through the parser");
        Scene back;
        Renderer renderer; // device-less: uploads return invalid handles
        scene::StagedAssets staged;
        scene::Instantiate(back, renderer, data, staged, scene::LoadMode::Replace);
        const entt::entity e = back.FindByName("Cam");
        expect(e != entt::null && back.Registry().all_of<CameraComponent>(e),
               "(12) the camera entity survives the round-trip");
        if (e != entt::null && back.Registry().all_of<CameraComponent>(e)) {
            const CameraComponent& g = back.Registry().get<CameraComponent>(e);
            expect(g.mode == authored.mode && g.rotation == authored.rotation &&
                       g.target == authored.target && g.playerLook == authored.playerLook,
                   "(12) mode / aim / target / playerLook round-trip");
            expect(g.lookSensitivity == authored.lookSensitivity &&
                       g.lookStickSpeed == authored.lookStickSpeed &&
                       g.invertLookY == authored.invertLookY &&
                       g.lookPitchMin == authored.lookPitchMin &&
                       g.lookPitchMax == authored.lookPitchMax,
                   "(12) every look field round-trips bit-exactly");
            expect(g.offset == authored.offset && g.yaw == authored.yaw &&
                       g.pitch == authored.pitch,
                   "(12) eye offset and the look SEED round-trip");
            // Runtime look state is not authored data and must not be written:
            // a saved lookYaw would reload as a camera pointing wherever the
            // last play session happened to leave it.
            expect(!g.lookInit && g.lookYaw == 0.0f && g.lookPitch == 0.0f,
                   "(12) accumulated look state is NOT serialized");
        }
    }

    return ok;
}

} // namespace cam
} // namespace hbe
