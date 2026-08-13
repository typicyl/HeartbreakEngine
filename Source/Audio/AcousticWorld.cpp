// Audio/AcousticWorld.cpp - see AcousticWorld.h.
#include "Audio/AcousticWorld.h"

#include "Audio/AudioSystem.h"
#include "Physics/PhysicsWorld.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <vector>

namespace hbe {

namespace {

// Average absorption over the 500/1k/2k Hz bands (indices 4,5,6) - the band range the backend
// uses to derive a broadband reflection coefficient.
f32 MidAbsorption(const AcousticMaterial& m) {
    return (m.absorption[4] + m.absorption[5] + m.absorption[6]) / 3.0f;
}

// Reflection coefficient from absorption: coeff = sqrt(1 - avg_absorption). Matches the backend's
// shoebox reflection model.
f32 ReflectionCoefficient(const AcousticMaterial& m) {
    return std::sqrt(std::max(0.0f, 1.0f - MidAbsorption(m)));
}

// Resolve an AcousticSpace wall material name (a preset) to its AcousticMaterial; unknown -> default.
AcousticMaterial MaterialByName(const std::string& name) {
    if (const AcousticMaterial* p = FindAcousticPreset(name)) return *p;
    return AcousticMaterial{};
}

// Transmission of the most-open enabled AcousticPortal containing `point`, or -1 if none does.
f32 PortalTransmissionAt(const Scene& scene, const glm::vec3& point) {
    const entt::registry& reg = scene.Registry();
    f32 best = -1.0f;
    for (const entt::entity e : reg.view<const AcousticPortal>()) {
        const AcousticPortal& p = reg.get<const AcousticPortal>(e);
        if (!p.enabled || !reg.all_of<Transform>(e)) continue;
        const glm::vec3 local =
            glm::abs(glm::vec3(glm::inverse(scene.WorldMatrix(e)) * glm::vec4(point, 1.0f)));
        if (local.x <= p.halfExtents.x && local.y <= p.halfExtents.y && local.z <= p.halfExtents.z)
            best = std::max(best, PortalTransmission(MaterialByName(p.closedMaterial), p.openness));
    }
    return best;
}

f32 NearlyEq(f32 a, f32 b) { return std::fabs(a - b) < 1e-4f; }

bool RoomsEqual(const AcousticRoom& a, const AcousticRoom& b) {
    for (int i = 0; i < 3; ++i)
        if (!NearlyEq(a.position[i], b.position[i]) || !NearlyEq(a.dimensions[i], b.dimensions[i]))
            return false;
    if (!NearlyEq(a.rotation.x, b.rotation.x) || !NearlyEq(a.rotation.y, b.rotation.y) ||
        !NearlyEq(a.rotation.z, b.rotation.z) || !NearlyEq(a.rotation.w, b.rotation.w))
        return false;
    for (int i = 0; i < 6; ++i)
        if (!NearlyEq(a.reflectionCoeff[i], b.reflectionCoeff[i])) return false;
    for (int i = 0; i < 9; ++i)
        if (!NearlyEq(a.rt60[i], b.rt60[i])) return false;
    return NearlyEq(a.reflectionGain, b.reflectionGain) && NearlyEq(a.reverbGain, b.reverbGain) &&
           NearlyEq(a.reflectionCutoffHz, b.reflectionCutoffHz);
}

} // namespace

AcousticRoom ComputeAcousticRoom(const glm::vec3& center, const glm::quat& rotation,
                                 const glm::vec3& dimensions, const AcousticMaterial& wall,
                                 const AcousticMaterial& floor, const AcousticMaterial& ceiling,
                                 f32 reflectionGain, f32 reverbGain, f32 reverbTime) {
    AcousticRoom r;
    r.position = center;
    r.rotation = rotation;
    r.dimensions = dimensions;
    r.reflectionCutoffHz = 800.0f;
    r.reflectionGain = std::max(reflectionGain, 0.0f);

    // Reflection coefficients, world axes: [-x,+x] = wall, [-y] = floor, [+y] = ceiling, [-z,+z] = wall.
    const f32 wc = ReflectionCoefficient(wall);
    r.reflectionCoeff[0] = wc;
    r.reflectionCoeff[1] = wc;
    r.reflectionCoeff[2] = ReflectionCoefficient(floor);
    r.reflectionCoeff[3] = ReflectionCoefficient(ceiling);
    r.reflectionCoeff[4] = wc;
    r.reflectionCoeff[5] = wc;

    // Eyring RT60 per octave band from the box volume + per-face absorption.
    const f32 W = std::max(dimensions.x, 0.1f);
    const f32 H = std::max(dimensions.y, 0.1f);
    const f32 D = std::max(dimensions.z, 0.1f);
    const f32 V = W * H * D;
    const f32 floorA = W * D;              // -y
    const f32 ceilA = W * D;               // +y
    const f32 wallA = 2.0f * W * H + 2.0f * D * H; // the four side walls
    const f32 S = std::max(floorA + ceilA + wallA, 1e-3f);
    const f32 timeScale = std::max(reverbTime, 0.0f);
    // Air absorption per octave band (m, ~20 C / 50 % RH): negligible at low frequency, strong at
    // high - it shortens the high-frequency reverb tail, so large rooms sound "darker" in their
    // reverberation. Added to the Eyring denominator as 4*m*V.
    static const f32 kAir[kAcousticBands] = {0.0000f, 0.0000f, 0.0001f, 0.0003f, 0.0006f,
                                             0.0010f, 0.0025f, 0.0080f, 0.0260f};
    for (int b = 0; b < kAcousticBands; ++b) {
        const f32 A = floorA * floor.absorption[static_cast<usize>(b)] +
                      ceilA * ceiling.absorption[static_cast<usize>(b)] +
                      wallA * wall.absorption[static_cast<usize>(b)];
        const f32 aBar = std::min(std::max(A / S, 0.0f), 0.99f); // clamp: avoid ln(0)
        const f32 denom = -S * std::log(1.0f - aBar) + 4.0f * kAir[b] * V; // Eyring + air absorption
        f32 rt = denom > 1e-4f ? (0.161f * V / denom) : 0.0f;
        rt *= timeScale;
        r.rt60[b] = std::min(std::max(rt, 0.0f), 12.0f);
    }
    // Late-reverb gain: backend default (0.045) scaled by the space's reverb-gain knob.
    r.reverbGain = 0.045f * std::max(reverbGain, 0.0f);
    return r;
}

f32 PortalTransmission(const AcousticMaterial& closedMaterial, f32 openness) {
    return glm::mix(closedMaterial.transmission, 1.0f, glm::clamp(openness, 0.0f, 1.0f));
}

f32 AcousticWorld::SegmentTransmission(const PhysicsWorld& physics, const Scene& scene,
                                       const glm::vec3& a, const glm::vec3& b,
                                       const std::filesystem::path& assetsDir) {
    const glm::vec3 d = b - a;
    const f32 len = glm::length(d);
    if (len < 1e-4f) return 1.0f;
    std::vector<PhysicsWorld::RayHitAll> hits;
    physics.RaycastAll(a, d, len, hits);
    f32 trans = 1.0f;
    for (const PhysicsWorld::RayHitAll& h : hits) {
        // Skip hits right at the source or listener ends (self/adjacent geometry).
        if (h.distance <= 0.02f || h.distance >= len - 0.02f) continue;
        f32 t = ResolveEntityAcoustic(scene, h.entity, assetsDir, matCache_).transmission;
        const f32 pt = PortalTransmissionAt(scene, h.point);
        if (pt >= 0.0f) t = pt; // an opening (doorway/window) overrides the wall struck here
        trans *= glm::clamp(t, 0.0f, 1.0f);
        if (trans < 1e-3f) return 0.0f; // fully blocked - stop early
    }
    return glm::clamp(trans, 0.0f, 1.0f);
}

void AcousticWorld::ClearCaches() {
    matCache_.Clear();
    hasPushed_ = false;
    lastSpace_ = entt::null;
}

void AcousticWorld::Update(Scene& scene, const glm::vec3& listenerPos,
                           const std::filesystem::path& assetsDir, AudioSystem& audio) {
    (void)assetsDir; // P2 rooms use preset materials (no disk); assetsDir is for P3 per-surface.
    entt::registry& reg = scene.Registry();

    // Highest-priority enabled AcousticSpace whose OBB contains the listener (>= tie-break =
    // last-wins, matching MusicZone/CameraZone).
    entt::entity best = entt::null;
    int bestPri = 0;
    for (const entt::entity e : reg.view<AcousticSpace>()) {
        AcousticSpace& sp = reg.get<AcousticSpace>(e);
        sp.active = false;
        if (!sp.enabled || !reg.all_of<Transform>(e)) continue;
        const glm::vec3 local =
            glm::abs(glm::vec3(glm::inverse(scene.WorldMatrix(e)) * glm::vec4(listenerPos, 1.0f)));
        if (local.x <= sp.halfExtents.x && local.y <= sp.halfExtents.y &&
            local.z <= sp.halfExtents.z && (best == entt::null || sp.priority >= bestPri)) {
            best = e;
            bestPri = sp.priority;
        }
    }

    if (best == entt::null) {
        // Outdoors / no room: disable room effects (push once on the transition out).
        if (!hasPushed_ || lastEnabled_) {
            audio.SetRoom(AcousticRoom{}, false);
            lastEnabled_ = false;
            lastSpace_ = entt::null;
            hasPushed_ = true;
        }
        return;
    }

    AcousticSpace& sp = reg.get<AcousticSpace>(best);
    sp.active = true;

    const glm::mat4 W = scene.WorldMatrix(best);
    const glm::vec3 center = glm::vec3(W[3]);
    // World scale + rotation from the columns (rooms are box regions, usually near-orthonormal).
    const glm::vec3 cx(W[0]), cy(W[1]), cz(W[2]);
    const glm::vec3 scale(glm::length(cx), glm::length(cy), glm::length(cz));
    const glm::vec3 dims = 2.0f * sp.halfExtents * scale;
    glm::mat3 rotM;
    rotM[0] = scale.x > 1e-6f ? cx / scale.x : glm::vec3(1, 0, 0);
    rotM[1] = scale.y > 1e-6f ? cy / scale.y : glm::vec3(0, 1, 0);
    rotM[2] = scale.z > 1e-6f ? cz / scale.z : glm::vec3(0, 0, 1);
    const glm::quat rot = glm::quat_cast(rotM);

    const AcousticRoom room = ComputeAcousticRoom(
        center, rot, dims, MaterialByName(sp.wallMaterial), MaterialByName(sp.floorMaterial),
        MaterialByName(sp.ceilingMaterial), sp.reflectionGain, sp.reverbGain, sp.reverbTime);

    // Push only when the room actually changed (space switch, moved region, or live param edit) so
    // the reverb network is not reconfigured every frame while standing in a static room.
    if (!hasPushed_ || !lastEnabled_ || best != lastSpace_ || !RoomsEqual(room, lastRoom_)) {
        audio.SetRoom(room, true);
        lastRoom_ = room;
        lastEnabled_ = true;
        lastSpace_ = best;
        hasPushed_ = true;
    }
}

std::vector<AcousticCellCluster> ClusterAcousticCells(const std::vector<glm::vec3>& cells,
                                                     f32 spacing) {
    std::vector<AcousticCellCluster> out;
    const f32 pad = std::max(spacing * 0.5f, 0.5f);
    const f32 adj2 = (spacing * 1.75f) * (spacing * 1.75f);
    std::vector<char> visited(cells.size(), 0);
    for (std::size_t i = 0; i < cells.size(); ++i) {
        if (visited[i]) continue;
        std::vector<std::size_t> stack{i};
        visited[i] = 1;
        glm::vec3 mn = cells[i];
        glm::vec3 mx = cells[i];
        int n = 0;
        while (!stack.empty()) {
            const std::size_t c = stack.back();
            stack.pop_back();
            ++n;
            mn = glm::min(mn, cells[c]);
            mx = glm::max(mx, cells[c]);
            for (std::size_t j = 0; j < cells.size(); ++j) {
                if (visited[j]) continue;
                const glm::vec3 d = cells[c] - cells[j];
                if (glm::dot(d, d) <= adj2) {
                    visited[j] = 1;
                    stack.push_back(j);
                }
            }
        }
        AcousticCellCluster cl;
        cl.min = mn - glm::vec3(pad);
        cl.max = mx + glm::vec3(pad);
        cl.cellCount = n;
        out.push_back(cl);
    }
    return out;
}

bool AcousticRoomSelfTest() {
    bool ok = true;
    const auto check = [&](bool cond, const char* msg) {
        if (!cond) {
            std::printf("  [acoustic-room] FAIL: %s\n", msg);
            ok = false;
        }
    };

    const glm::vec3 center(1.0f, 2.0f, 3.0f);
    const glm::quat rot(1.0f, 0.0f, 0.0f, 0.0f);
    const glm::vec3 dims(8.0f, 4.0f, 6.0f);

    const AcousticMaterial* marble = FindAcousticPreset("Marble / Polished Stone");
    const AcousticMaterial* open = FindAcousticPreset("Open / Air");
    const AcousticMaterial* fiber = FindAcousticPreset("Fiberglass Insulation");
    check(marble && open && fiber, "required presets missing");
    if (!marble || !open || !fiber) return false;

    const AcousticRoom hard =
        ComputeAcousticRoom(center, rot, dims, *marble, *marble, *marble, 1.0f, 1.0f, 1.0f);
    const AcousticRoom openR =
        ComputeAcousticRoom(center, rot, dims, *open, *open, *open, 1.0f, 1.0f, 1.0f);
    const AcousticRoom soft =
        ComputeAcousticRoom(center, rot, dims, *fiber, *fiber, *fiber, 1.0f, 1.0f, 1.0f);

    // Geometry passes through.
    check(NearlyEq(hard.position.x, 1.0f) && NearlyEq(hard.position.z, 3.0f), "center not passed through");
    check(NearlyEq(hard.dimensions.x, 8.0f) && NearlyEq(hard.dimensions.y, 4.0f), "dims not passed through");

    // A hard stone room rings; an open/absorptive room is dry.
    const f32 mid = 4; // 500 Hz band index
    check(hard.rt60[static_cast<usize>(mid)] > 2.0f, "hard room RT60 too short (should ring)");
    check(openR.rt60[static_cast<usize>(mid)] < 0.2f, "open room RT60 too long (should be dry)");
    check(soft.rt60[static_cast<usize>(mid)] < 0.6f, "absorptive room RT60 too long");
    check(hard.rt60[static_cast<usize>(mid)] > soft.rt60[static_cast<usize>(mid)],
          "hard room should ring longer than the absorptive room");

    // Reflection coefficients: hard = strong reflector, open = none.
    check(hard.reflectionCoeff[0] > 0.9f, "hard wall reflection coefficient too low");
    check(openR.reflectionCoeff[0] < 0.05f, "open wall should not reflect");
    for (int i = 0; i < 6; ++i)
        check(hard.reflectionCoeff[i] >= 0.0f && hard.reflectionCoeff[i] <= 1.0f,
              "reflection coefficient out of [0,1]");
    for (int b = 0; b < kAcousticBands; ++b)
        check(hard.rt60[b] >= 0.0f && hard.rt60[b] <= 12.0f, "RT60 out of clamp range");

    // reverbTime scales the tail; reverbGain scales the level.
    const AcousticRoom longer =
        ComputeAcousticRoom(center, rot, dims, *marble, *marble, *marble, 1.0f, 1.0f, 2.0f);
    check(longer.rt60[static_cast<usize>(mid)] > hard.rt60[static_cast<usize>(mid)] - 1e-3f,
          "reverbTime=2 should not shorten RT60");
    const AcousticRoom quieter =
        ComputeAcousticRoom(center, rot, dims, *marble, *marble, *marble, 1.0f, 0.5f, 1.0f);
    check(quieter.reverbGain < hard.reverbGain, "reverbGain=0.5 should lower the tail level");

    // Air absorption: in a LARGE hard room the high-frequency tail is shorter than the low band
    // (air absorbs highs), so the reverb reads "darker".
    {
        const glm::vec3 big(30.0f, 15.0f, 30.0f);
        const AcousticRoom hall =
            ComputeAcousticRoom(center, rot, big, *marble, *marble, *marble, 1.0f, 1.0f, 1.0f);
        check(hall.rt60[8] < hall.rt60[0] - 1e-3f,
              "air absorption should shorten the HF tail in a big room");
    }

    // Portal transmission: closed = the closed material's transmission, open = passes fully.
    if (const AcousticMaterial* wood = FindAcousticPreset("Wood Panel")) {
        check(NearlyEq(PortalTransmission(*wood, 0.0f), wood->transmission),
              "closed portal should equal the closed material transmission");
        check(NearlyEq(PortalTransmission(*wood, 1.0f), 1.0f), "open portal should pass fully");
        const f32 half = PortalTransmission(*wood, 0.5f);
        check(half > wood->transmission && half < 1.0f, "half-open portal should be between");
    } else {
        check(false, "Wood Panel preset missing");
    }

    // Bake cell clustering: two well-separated groups -> two rooms with sensible padded AABBs.
    {
        const std::vector<glm::vec3> cells = {
            {0, 0, 0},  {3, 0, 0},  {0, 0, 3}, {3, 0, 3}, // room A: a 3 m grid clump near origin
            {40, 0, 40}, {43, 0, 40},                     // room B: far away
        };
        const std::vector<AcousticCellCluster> cl = ClusterAcousticCells(cells, 3.0f);
        check(cl.size() == 2, "clustering should find two rooms");
        if (cl.size() == 2) {
            const AcousticCellCluster& a = cl[0].min.x < cl[1].min.x ? cl[0] : cl[1];
            const AcousticCellCluster& b = cl[0].min.x < cl[1].min.x ? cl[1] : cl[0];
            check(a.cellCount == 4 && b.cellCount == 2, "cluster cell counts wrong");
            check(a.max.x > 3.0f && a.min.x < 0.0f, "room A AABB should cover its cells + padding");
            check(b.min.x > 30.0f, "room B should be the far cluster");
        }
    }

    return ok;
}

} // namespace hbe
