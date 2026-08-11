// Source/Volume/ProceduralVolumeSimulation.cpp - see the header.
#include "Volume/ProceduralVolumeSimulation.h"

#include <cmath>

namespace hbe::volume {
namespace {

// Gradient (Perlin) noise - direction-neutral, no axis-aligned lattice signature (the same
// reason VolumeSplat.hlsl uses gradient noise for its warp). CPU port; deterministic.
glm::vec3 Hash33(glm::vec3 p) {
    p = glm::fract(p * glm::vec3(0.1031f, 0.1030f, 0.0973f));
    p += glm::dot(p, glm::vec3(p.y, p.x, p.z) + 33.33f);
    return glm::fract((glm::vec3(p.x, p.x, p.y) + glm::vec3(p.y, p.z, p.z)) *
                      glm::vec3(p.z, p.y, p.x)) *
               2.0f -
           1.0f;
}
f32 GradNoise(glm::vec3 p) {
    const glm::vec3 i = glm::floor(p);
    const glm::vec3 f = glm::fract(p);
    const glm::vec3 u = f * f * (3.0f - 2.0f * f);
    auto g = [&](glm::vec3 o) { return glm::dot(Hash33(i + o), f - o); };
    const f32 n =
        glm::mix(glm::mix(glm::mix(g({0, 0, 0}), g({1, 0, 0}), u.x),
                          glm::mix(g({0, 1, 0}), g({1, 1, 0}), u.x), u.y),
                 glm::mix(glm::mix(g({0, 0, 1}), g({1, 0, 1}), u.x),
                          glm::mix(g({0, 1, 1}), g({1, 1, 1}), u.x), u.y), u.z);
    return n * 0.5f + 0.5f; // -> [0,1]
}
f32 Fbm(glm::vec3 p) {
    f32 s = 0.0f, a = 0.5f;
    for (int i = 0; i < 4; ++i) {
        s += a * GradNoise(p);
        p = p * 2.03f + 19.19f;
        a *= 0.5f;
    }
    return s;
}
f32 Saturate(f32 x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

} // namespace

ProceduralVolumeSimulation::ProceduralVolumeSimulation(glm::ivec3 dim) {
    bounds_.worldMin = glm::vec3(-2.0f, 0.0f, -2.0f);
    bounds_.worldMax = glm::vec3(2.0f, 8.0f, 2.0f);
    bounds_.dim = glm::max(dim, glm::ivec3(1));
}

void ProceduralVolumeSimulation::Reset() { time_ = 0.0f; }
void ProceduralVolumeSimulation::Step(f32 dt) { time_ += dt; }

void ProceduralVolumeSimulation::ReadbackFrame(VolumeFrame& out) {
    out.time = time_;
    out.bounds = bounds_;
    // Ensure BOTH fields first, THEN take references: ensureField push_backs into out.fields, so a
    // reference taken before a later ensureField() would dangle when the vector reallocates.
    out.ensureField("density", FieldType::Scalar, 0.0f);
    out.ensureField("temperature", FieldType::Scalar, 0.0f);
    VolumeField& density = *out.field("density");
    VolumeField& temperature = *out.field("temperature");
    density.data.assign(bounds_.voxelCount(), 0.0f);
    temperature.data.assign(bounds_.voxelCount(), 0.0f);

    const f32 t = time_;
    const glm::vec3 ext = bounds_.extent();
    const f32 riseSpeed = 6.0f;           // world units/sec the plume front climbs
    const f32 front = riseSpeed * t;      // current plume-top world height
    for (int z = 0; z < bounds_.dim.z; ++z)
        for (int y = 0; y < bounds_.dim.y; ++y)
            for (int x = 0; x < bounds_.dim.x; ++x) {
                const glm::vec3 wp = bounds_.voxelCenter(x, y, z);
                const f32 h = (wp.y - bounds_.worldMin.y) / glm::max(ext.y, 1e-3f); // 0..1
                const f32 r = std::sqrt(wp.x * wp.x + wp.z * wp.z);
                const f32 colRadius = 0.35f + 1.25f * h; // widen with height
                const f32 radial = Saturate(1.0f - r / colRadius);
                // Rising, drifting turbulence (world-anchored so it flows, not crawls).
                const f32 n = Fbm(wp * 1.6f + glm::vec3(0.0f, -t * 1.3f, 0.0f) + t * 0.15f);
                const f32 heightFade = Saturate((front - wp.y) * 0.6f + 0.4f) * (1.0f - h * 0.35f);
                f32 d = radial * n * heightFade * 2.2f;
                d = Saturate(d - 0.16f) * 1.6f; // threshold -> wispy + sparse
                if (d <= 0.002f) continue;
                const usize idx = VoxelIndex(bounds_, x, y, z);
                density.data[idx] = d;
                // Hot at the base, cooling with height; only where there is smoke.
                temperature.data[idx] = Saturate(1.0f - h * 2.6f) * Saturate(d * 2.0f);
            }
}

} // namespace hbe::volume
