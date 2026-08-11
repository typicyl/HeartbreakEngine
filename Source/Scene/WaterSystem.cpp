// Scene/WaterSystem.cpp - see WaterSystem.h.
#include "Scene/WaterSystem.h"

#include "Assets/Mesh.h"
#include "Physics/PhysicsWorld.h"
#include "Renderer/Renderer.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace hbe::water {

namespace {
constexpr u32 kMaxRes = 256;
constexpr f32 kRippleLife = 2.5f; // seconds (matches the shader's age cutoff)

// A flat res x res grid over [-size/2, size/2] in XZ, y = 0 (the water VS displaces it).
MeshData BuildGrid(f32 size, u32 res) {
    res = std::clamp(res, 16u, kMaxRes);
    const f32 half = size * 0.5f;
    const f32 step = size / static_cast<f32>(res);
    MeshData m;
    m.name = "WaterGrid";
    m.vertices.reserve(static_cast<usize>(res + 1) * (res + 1));
    for (u32 j = 0; j <= res; ++j) {
        for (u32 i = 0; i <= res; ++i) {
            Vertex v;
            v.position = {-half + static_cast<f32>(i) * step, 0.0f,
                          -half + static_cast<f32>(j) * step};
            v.normal = {0.0f, 1.0f, 0.0f};
            v.tangent = {1.0f, 0.0f, 0.0f, 1.0f};
            v.uv = {static_cast<f32>(i) / res, static_cast<f32>(j) / res};
            m.vertices.push_back(v);
        }
    }
    const u32 stride = res + 1;
    m.indices.reserve(static_cast<usize>(res) * res * 6);
    for (u32 j = 0; j < res; ++j) {
        for (u32 i = 0; i < res; ++i) {
            const u32 i0 = j * stride + i, i1 = i0 + 1, i2 = i0 + stride, i3 = i2 + 1;
            m.indices.insert(m.indices.end(), {i0, i2, i3, i0, i3, i1});
        }
    }
    return m;
}

u32 HashU32(u32 n) {
    n = (n ^ 61u) ^ (n >> 16);
    n *= 9u;
    n ^= n >> 4;
    n *= 0x27d4eb2du;
    n ^= n >> 15;
    return n;
}
} // namespace

void AddRipple(Scene& scene, const glm::vec3& worldPos, f32 strength) {
    auto& rip = scene.WaterRipples();
    if (rip.size() >= rhi::kMaxRipples) rip.erase(rip.begin()); // evict the oldest
    rip.emplace_back(worldPos.x, worldPos.z, 0.0f, strength);   // .z = age (0 = fresh)
}

void Update(Scene& scene, Renderer& renderer, f32 dt) {
    // The scene animation clock drives BOTH the GPU wave/ripple/sky time (via MakeView) and
    // CPU water buoyancy - advance it here, once, before render + physics read it this frame.
    scene.AdvanceTime(dt);
    auto& reg = scene.Registry();

    // 1. Build/regenerate grid meshes on change; attach the water MeshInstance.
    bool hasWater = false;
    for (const entt::entity e : reg.view<WaterComponent, Transform>()) {
        hasWater = true;
        WaterComponent& w = reg.get<WaterComponent>(e);
        w.resolution = std::clamp(w.resolution, 16u, kMaxRes);
        if (w.dirty || !w.mesh.IsValid()) {
            MeshData md = BuildGrid(w.size, w.resolution);
            if (w.mesh.IsValid()) {
                // Update the geometry IN PLACE. The buffer was reserved at MAX resolution,
                // and neither a size nor a resolution change (clamped <= kMaxRes) exceeds
                // that reservation, so this never grows - which matters because the RHI has
                // no mesh-destroy, so re-reserving on every edit would orphan the old GPU
                // mesh's VRAM for the process lifetime.
                renderer.UpdateMesh(w.mesh, md);
            } else {
                // First build: reserve at MAX resolution so every later edit updates in place.
                const u32 capV = (kMaxRes + 1u) * (kMaxRes + 1u);
                const u32 capI = kMaxRes * kMaxRes * 6u;
                w.mesh = renderer.UploadMeshReserved(md, capV, capI);
            }
            w.dirty = false;

            MeshInstance mi;
            mi.mesh = w.mesh;
            mi.materialFlags = rhi::MaterialFlag_Water | rhi::MaterialFlag_Transparent |
                               rhi::MaterialFlag_NoShadow;
            reg.emplace_or_replace<MeshInstance>(e, mi);

            // Local bounds: the flat grid + a vertical margin for the wave displacement.
            // NOTE: the frustum culler tests the MESH bounds (this flat grid, y=0), so the
            // margin only feeds the shadow scene-bounds fit, not culling - a crest can pop at
            // the frustum edge in the rare view where the flat plane itself is off-screen
            // (accepted: the plane's large XZ extent keeps it frustum-intersecting almost
            // always).
            f32 amp = 0.0f;
            for (int k = 0; k < 4; ++k) amp += std::abs(w.waveAmplitude[k]);
            const f32 half = w.size * 0.5f;
            reg.emplace_or_replace<AABB>(
                e, AABB{glm::vec3(-half, -amp - 1.0f, -half), glm::vec3(half, amp + 1.0f, half)});
        }
    }

    // 2. Age + cull the interactive ripple ring buffer (swap-erase, order-independent).
    auto& rip = scene.WaterRipples();
    for (usize i = 0; i < rip.size();) {
        rip[i].z += dt;
        if (rip[i].z > kRippleLife) {
            rip[i] = rip.back();
            rip.pop_back();
        } else {
            ++i;
        }
    }

    // 3. Rain splash rings: while raining, drop a few big impact rings near the camera (the
    // dense micro-ripples are the shader's procedural overlay; these are the visible splashes
    // and also exercise the ring path). Rate scales with rain intensity.
    const SceneEnvironment& env = scene.Environment();
    if (hasWater && env.precipType == 1u && env.precipIntensity > 0.01f) {
        static f32 s_timer = 0.0f;
        static u32 s_ctr = 0;
        s_timer += dt;
        const f32 interval = 0.30f / std::max(env.precipIntensity, 0.1f);
        const glm::vec3 cam = renderer.GetCamera().Position();
        int guard = 0;
        while (s_timer >= interval && guard++ < 8) {
            s_timer -= interval;
            const u32 h = HashU32(s_ctr++);
            const f32 ox = ((h & 0xffffu) / 65535.0f - 0.5f) * 50.0f;
            const f32 oz = (((h >> 16) & 0xffffu) / 65535.0f - 0.5f) * 50.0f;
            AddRipple(scene, glm::vec3(cam.x + ox, 0.0f, cam.z + oz), 0.5f);
        }
    }
}

f32 SurfaceHeightAt(Scene& scene, f32 x, f32 z) {
    auto& reg = scene.Registry();
    const f32 t = scene.Time();
    for (const entt::entity e : reg.view<WaterComponent, Transform>()) {
        const WaterComponent& w = reg.get<WaterComponent>(e);
        const glm::mat4 wm = scene.WorldMatrix(e);
        // The grid spans [-size/2, size/2] in local XZ; scale to world by the basis lengths.
        // (CPU buoyancy treats water as axis-aligned - a rotated plane is not sampled here.)
        const f32 halfX = (w.size * 0.5f) * glm::length(glm::vec3(wm[0]));
        const f32 halfZ = (w.size * 0.5f) * glm::length(glm::vec3(wm[2]));
        if (std::abs(x - wm[3].x) > halfX || std::abs(z - wm[3].z) > halfZ) continue; // outside
        const f32 baseY = wm[3].y;
        f32 h = 0.0f;
        for (int i = 0; i < 4; ++i) {
            if (w.waveLength[i] < 0.01f) continue;
            const f32 a = glm::radians(w.waveAngle[i]);
            const glm::vec2 d =
                glm::normalize(glm::vec2(std::cos(a), std::sin(a)) + glm::vec2(1e-5f, 0.0f));
            const f32 k = 6.2831853f / w.waveLength[i];
            h += w.waveAmplitude[i] * std::sin(k * (d.x * x + d.y * z) + t * w.waveSpeed[i] * k);
        }
        return baseY + h; // first water entity in-bounds = the scene water surface
    }
    return -1.0e9f; // no water under this point: nothing floats
}

void ApplyBuoyancy(Scene& scene, PhysicsWorld& physics, f32 dt) {
    auto& reg = scene.Registry();
    auto view = reg.view<WaterComponent, Transform>();
    if (view.begin() == view.end()) return; // no water
    const entt::entity we = *view.begin();
    const WaterComponent& w = reg.get<WaterComponent>(we);
    const glm::mat4 wm = scene.WorldMatrix(we);
    const f32 baseY = wm[3].y;
    const f32 t = scene.Time();

    // World-space XZ footprint of this (axis-aligned) water plane. Bodies outside it get the
    // no-water sentinel so they aren't buoyed by a finite grid as though it were infinite.
    const glm::vec2 center(wm[3].x, wm[3].z);
    const f32 halfX = (w.size * 0.5f) * glm::length(glm::vec3(wm[0]));
    const f32 halfZ = (w.size * 0.5f) * glm::length(glm::vec3(wm[2]));

    // Snapshot the active waves so the per-body height lambda costs no registry walk. This is
    // the SAME Gerstner sum the shader uses (with the SAME clock), so bodies float exactly on
    // the rendered surface.
    struct WaveP { glm::vec2 d; f32 amp, k, phase; };
    std::array<WaveP, 4> waves{};
    int nw = 0;
    for (int i = 0; i < 4; ++i) {
        if (w.waveLength[i] < 0.01f) continue;
        const f32 a = glm::radians(w.waveAngle[i]);
        const glm::vec2 d =
            glm::normalize(glm::vec2(std::cos(a), std::sin(a)) + glm::vec2(1e-5f, 0.0f));
        const f32 k = 6.2831853f / w.waveLength[i];
        waves[nw++] = {d, w.waveAmplitude[i], k, t * w.waveSpeed[i] * k};
    }
    const auto heightAt = [waves, nw, baseY, center, halfX, halfZ](f32 x, f32 z) -> f32 {
        if (std::abs(x - center.x) > halfX || std::abs(z - center.y) > halfZ)
            return -1.0e9f; // outside the water footprint - PhysicsWorld skips it
        f32 h = 0.0f;
        for (int i = 0; i < nw; ++i)
            h += waves[i].amp *
                 std::sin(waves[i].k * (waves[i].d.x * x + waves[i].d.y * z) + waves[i].phase);
        return baseY + h;
    };
    physics.ApplyBuoyancy(scene, dt, heightAt, std::max(w.buoyancy, 0.0f), 0.7f, 0.3f);
}

} // namespace hbe::water
