// Audio/AcousticWorld.cpp - see AcousticWorld.h.
#include "Audio/AcousticWorld.h"

#include "Audio/AudioSystem.h"
#include "Physics/PhysicsWorld.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#if HBE_HAVE_RESONANCE
#include "hdsr/acoustics.h" // the HDS-Resonance acoustics library (materials/rooms/propagation)
#endif

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <vector>

namespace hbe {

namespace {

#if HBE_HAVE_RESONANCE
// Translate Heartbreak's engine-side material into the library's material type. (The acoustic
// MODEL - reflection/RT60/propagation - lives in the library; Heartbreak only translates.)
hdsr::AcousticMaterial ToHdsr(const AcousticMaterial& m) {
    hdsr::AcousticMaterial out;
    for (int b = 0; b < kAcousticBands; ++b)
        out.absorption[b] = m.absorption[static_cast<usize>(b)];
    out.scattering = m.scattering;
    out.transmission = m.transmission;
    return out;
}
#endif

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

// Compute an AcousticSpace's room acoustics from its world transform + wall/floor/ceiling materials.
AcousticRoom RoomForSpace(const Scene& scene, entt::entity e, const AcousticSpace& sp) {
    const glm::mat4 W = scene.WorldMatrix(e);
    const glm::vec3 center = glm::vec3(W[3]);
    const glm::vec3 cx(W[0]), cy(W[1]), cz(W[2]);
    const glm::vec3 scale(glm::length(cx), glm::length(cy), glm::length(cz));
    const glm::vec3 dims = 2.0f * sp.halfExtents * scale;
    glm::mat3 rotM;
    rotM[0] = scale.x > 1e-6f ? cx / scale.x : glm::vec3(1, 0, 0);
    rotM[1] = scale.y > 1e-6f ? cy / scale.y : glm::vec3(0, 1, 0);
    rotM[2] = scale.z > 1e-6f ? cz / scale.z : glm::vec3(0, 0, 1);
    const glm::quat rot = glm::quat_cast(rotM);
    return ComputeAcousticRoom(center, rot, dims, MaterialByName(sp.wallMaterial),
                               MaterialByName(sp.floorMaterial), MaterialByName(sp.ceilingMaterial),
                               sp.reflectionGain, sp.reverbGain, sp.reverbTime);
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
    r.reverbGain = 0.0f;
    for (int i = 0; i < 6; ++i) r.reflectionCoeff[i] = 0.0f;
    for (int b = 0; b < kAcousticBands; ++b) r.rt60[b] = 0.0f;
#if HBE_HAVE_RESONANCE
    // The acoustic model lives in the HDS-Resonance library: translate the per-face materials,
    // compute reflection coefficients + Eyring RT60 (with air absorption) there, and map the result
    // onto the engine's room POD. Wall order: [-x,+x]=wall, [-y]=floor, [+y]=ceiling, [-z,+z]=wall.
    const hdsr::AcousticMaterial hw = ToHdsr(wall);
    const hdsr::AcousticMaterial hf = ToHdsr(floor);
    const hdsr::AcousticMaterial hc = ToHdsr(ceiling);
    const hdsr::AcousticMaterial* walls[6] = {&hw, &hw, &hf, &hc, &hw, &hw};
    const float dims[3] = {dimensions.x, dimensions.y, dimensions.z};
    const hdsr::RoomAcoustics ra =
        hdsr::ComputeRoomAcoustics(dims, walls, reflectionGain, reverbGain, reverbTime);
    for (int i = 0; i < 6; ++i) r.reflectionCoeff[i] = ra.reflectionCoefficients[i];
    r.reflectionCutoffHz = ra.cutoffFrequencyHz;
    r.reflectionGain = ra.reflectionGain;
    for (int b = 0; b < kAcousticBands; ++b) r.rt60[b] = ra.rt60[b];
    r.reverbGain = ra.reverbGain;
#else
    (void)wall;
    (void)floor;
    (void)ceiling;
    (void)reverbGain;
    (void)reverbTime;
#endif
    return r;
}

f32 PortalTransmission(const AcousticMaterial& closedMaterial, f32 openness) {
#if HBE_HAVE_RESONANCE
    return hdsr::PortalTransmission(ToHdsr(closedMaterial), openness);
#else
    return glm::mix(closedMaterial.transmission, 1.0f, glm::clamp(openness, 0.0f, 1.0f));
#endif
}

f32 AcousticWorld::SegmentTransmission(const PhysicsWorld& physics, const Scene& scene,
                                       const glm::vec3& a, const glm::vec3& b,
                                       const std::filesystem::path& assetsDir) {
    const glm::vec3 d = b - a;
    const f32 len = glm::length(d);
    if (len < 1e-4f) return 1.0f;
    std::vector<PhysicsWorld::RayHitAll> hits;
    physics.RaycastAll(a, d, len, hits);
    // Collect each intervening surface's transmission (an open portal overrides the wall it covers),
    // then let the library combine them into a total - the propagation model lives in HDS-Resonance,
    // the geometry query is Heartbreak's.
    std::vector<float> transmissions;
    transmissions.reserve(hits.size());
    for (const PhysicsWorld::RayHitAll& h : hits) {
        // Skip hits right at the source or listener ends (self/adjacent geometry).
        if (h.distance <= 0.02f || h.distance >= len - 0.02f) continue;
        f32 t = ResolveEntityAcoustic(scene, h.entity, assetsDir, matCache_).transmission;
        const f32 pt = PortalTransmissionAt(scene, h.point);
        if (pt >= 0.0f) t = pt; // an opening (doorway/window) overrides the wall struck here
        transmissions.push_back(glm::clamp(t, 0.0f, 1.0f));
    }
#if HBE_HAVE_RESONANCE
    return hdsr::CombineTransmission(transmissions.data(), static_cast<int>(transmissions.size()));
#else
    float prod = 1.0f;
    for (float t : transmissions) prod *= t;
    return glm::clamp(prod, 0.0f, 1.0f);
#endif
}

void AcousticWorld::ClearCaches() {
    matCache_.Clear();
    hasPushed_ = false;
    lastSpace_ = entt::null;
}

void AcousticWorld::Update(Scene& scene, const glm::vec3& listenerPos,
                           const std::filesystem::path& assetsDir, AudioSystem& audio,
                           const PhysicsWorld* physics, bool environmentReverbEnabled) {
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

    // Multi-environment reverb topology: declare EVERY enabled space coupled to the listener - the
    // listener's own room at coupling 1, and each other room by how much of its tail reaches the
    // listener through portals/propagation (measured with the physics geometry). The library mixes
    // all of them up to its capacity (not just the loudest). Runs even outdoors (best == null): a
    // source in a nearby room still leaks its reverb out.
    audio.SetEnvironmentReverbEnabled(environmentReverbEnabled);
    if (environmentReverbEnabled) {
        std::vector<AcousticEnvironment> envs;
        for (const entt::entity e : reg.view<AcousticSpace>()) {
            const AcousticSpace& sp = reg.get<AcousticSpace>(e);
            if (!sp.enabled || !reg.all_of<Transform>(e)) continue;
            f32 coupling = 1.0f;
            if (e != best) {
                if (physics == nullptr) continue; // coupling needs geometry queries
                const glm::vec3 c = glm::vec3(scene.WorldMatrix(e)[3]);
                coupling = SegmentTransmission(*physics, scene, c, listenerPos, assetsDir);
                if (coupling < 0.03f) continue; // negligibly coupled -> skip
            }
            const AcousticRoom rm = RoomForSpace(scene, e, sp);
            AcousticEnvironment env;
            // Non-negative, stable per valid entity (masks the sign bit so a recycled entity's id
            // is never negative). Must match AudioSystem's per-voice room id exactly.
            env.id = static_cast<int>(entt::to_integral(e) & 0x7FFFFFFFu);
            for (int b = 0; b < 9; ++b) env.rt60[b] = rm.rt60[b];
            env.coupling = coupling;
            env.gain = 1.0f;
            envs.push_back(env);
            if (envs.size() >= 16u) break;
        }
        audio.SetEnvironments(envs.empty() ? nullptr : envs.data(), static_cast<int>(envs.size()));
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
    AcousticRoom room = RoomForSpace(scene, best, sp);
    // With the environment reverb on, IT owns the late reverb tail (the listener's room is one
    // environment at coupling 1); the single-room path then supplies only the early reflections.
    if (environmentReverbEnabled) room.reverbGain = 0.0f;

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

    // The material/room/propagation MODEL now lives in the HDS-Resonance library (tested by
    // hdsr::SelfTest); these checks validate Heartbreak's translation + wrapper end-to-end. Skip
    // when the library is not built in (no presets).
    if (AcousticPresets().empty()) {
        std::printf("  [acoustic-room] SKIP: HDS-Resonance acoustics library not built in.\n");
        return true;
    }
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
