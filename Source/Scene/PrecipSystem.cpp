// Scene/PrecipSystem.cpp - see PrecipSystem.h.
#include "Scene/PrecipSystem.h"

#include "Scene/Scene.h"

#include <cmath>

namespace hbe::precip {

namespace {
// Cheap integer hash -> [0,1). Deterministic per index so the field seeds the same way
// every run (no <random>, no per-frame allocation).
f32 Hash01(u32 n) {
    n = (n ^ 61u) ^ (n >> 16);
    n *= 9u;
    n ^= n >> 4;
    n *= 0x27d4eb2du;
    n ^= n >> 15;
    return static_cast<f32>(n & 0x00ffffffu) / 16777216.0f;
}
} // namespace

void UpdateAndBuild(PrecipField& f, Scene& scene, const glm::vec3& camPos,
                    const glm::vec3& camRight, const glm::vec3& camUp, f32 dt,
                    std::vector<rhi::ParticleVertex>& alphaOut) {
    const SceneEnvironment& env = scene.Environment();
    const u32 type = env.precipType;
    const f32 intensity = glm::clamp(env.precipIntensity, 0.0f, 1.0f);

    // Off or clear -> release the pool so it costs nothing when the weather is dry.
    if (type == 0u || intensity <= 0.001f) {
        if (!f.off.empty()) { f.off.clear(); f.seed.clear(); }
        f.curType = 0u;
        return;
    }
    f.time += dt;

    const bool rain = (type == 1u);
    const f32 H = rain ? 22.0f : 20.0f;              // camera box half-extent (m)
    const u32 maxCount = rain ? 6000u : 3500u;       // at full intensity
    const u32 target = glm::max(1u, static_cast<u32>(static_cast<f32>(maxCount) * intensity));
    const f32 fall = rain ? 18.0f : 1.1f;            // fall speed (m/s)

    // World wind (m/s) from the scene's cloud wind direction/speed, precip-scaled. Snow
    // catches less wind than rain (lower terminal velocity is already its own signature).
    const f32 wrad = glm::radians(env.windAngle);
    const f32 wmag = glm::clamp(env.windSpeed * 120.0f, 0.0f, 15.0f) * (rain ? 1.0f : 0.5f);
    const glm::vec3 wind(std::cos(wrad) * wmag, 0.0f, std::sin(wrad) * wmag);

    // (Re)seed on a type change; otherwise grow/shrink smoothly to the intensity target
    // without reshuffling the particles that are already falling.
    if (f.curType != type) { f.off.clear(); f.seed.clear(); f.curType = type; }
    if (f.off.size() > target) { f.off.resize(target); f.seed.resize(target); }
    while (f.off.size() < target) {
        const u32 i = static_cast<u32>(f.off.size());
        f.off.emplace_back((Hash01(i * 3u + 0u) * 2.0f - 1.0f) * H,
                           (Hash01(i * 3u + 1u) * 2.0f - 1.0f) * H,
                           (Hash01(i * 3u + 2u) * 2.0f - 1.0f) * H);
        f.seed.push_back(Hash01(i * 7u + 5u));
    }

    // Draw budget: never append past the backends' COMBINED CPU particle-vertex cap
    // (6 verts/particle). Weather is appended last, so without this it is the first
    // casualty of the silent tail-drop when authored VFX fill the shared buffer; here it
    // simulates the whole field but only emits what fits, yielding gracefully instead.
    const usize freeVerts = (rhi::kMaxCpuParticleVertices > alphaOut.size())
                                ? (rhi::kMaxCpuParticleVertices - alphaOut.size())
                                : 0u;
    usize drawBudget = freeVerts / 6u;
    alphaOut.reserve(alphaOut.size() + glm::min(f.off.size(), drawBudget) * 6u);
    const f32 twoH = 2.0f * H;

    for (usize i = 0; i < f.off.size() && drawBudget > 0u; ++i) {
        const f32 sd = f.seed[i];
        glm::vec3 v;
        if (rain) {
            v = wind + glm::vec3(0.0f, -fall * (0.85f + 0.3f * sd), 0.0f);
        } else {
            // Snow: a gentle swirling drift on top of the wind + slow fall.
            v = wind + glm::vec3(std::sin(f.time * 0.7f + sd * 6.3f) * 0.5f,
                                 -fall * (0.7f + 0.6f * sd),
                                 std::cos(f.time * 0.6f + sd * 5.1f) * 0.5f);
        }
        glm::vec3 o = f.off[i] + v * dt;
        // Wrap each axis into [-H, H) so the field stays centred on the moving camera.
        o.x -= twoH * std::floor((o.x + H) / twoH);
        o.y -= twoH * std::floor((o.y + H) / twoH);
        o.z -= twoH * std::floor((o.z + H) / twoH);
        f.off[i] = o;

        // Fade near the box extremes (hides the toroidal wrap on ALL three axes) and
        // right on the lens. The x/z term matters because the wind gives every particle
        // a lateral velocity, so drops wrap sideways too - without it they pop at the
        // lateral box edges (worst for wind-cranked rain and slow, opaque snow).
        f32 fade = glm::smoothstep(H, H * 0.75f, std::fabs(o.y)) *
                   glm::smoothstep(H, H * 0.85f, glm::max(std::fabs(o.x), std::fabs(o.z))) *
                   glm::smoothstep(1.0f, 2.5f, glm::length(o));
        if (fade <= 0.001f) continue;

        const glm::vec3 wp = camPos + o;
        glm::vec3 r, u;
        f32 cr, cg, cb, ca;
        if (rain) {
            // WORLD-SPACE velocity-stretched streak: the streak axis is the real fall
            // direction (wind + gravity); the quad only rotates AROUND that axis to stay
            // edge-on to the camera. The old math projected the velocity onto the camera
            // plane, so looking ALONG the fall direction (straight up/down) collapsed the
            // streak to a dot and the rain "stopped working" - this stays a proper streak.
            const glm::vec3 toCam = camPos - wp;      // = -o; from the drop toward the camera
            const glm::vec3 vdir = glm::normalize(v); // world fall direction (wind + gravity)
            glm::vec3 side = glm::cross(vdir, toCam); // perpendicular to both fall and view
            const f32 slen = glm::length(side);
            const f32 width = 0.012f;
            const f32 len = 0.55f + 0.02f * glm::length(v); // faster fall = longer streak
            if (slen > 1e-4f) {
                side /= slen;
                u = vdir * (len * 0.5f);   // along the WORLD fall direction
                r = side * (width * 0.5f); // thin, edge-on to the camera
            } else {
                // Viewing almost exactly along the rain: a small round dot is the honest look.
                r = camRight * (width * 0.5f);
                u = camUp * (width * 0.5f);
            }
            cr = 0.62f; cg = 0.70f; cb = 0.85f; ca = 0.35f * fade;
        } else {
            const f32 size = 0.04f + 0.06f * sd;
            r = camRight * (size * 0.5f);
            u = camUp * (size * 0.5f);
            cr = 0.95f; cg = 0.96f; cb = 1.0f; ca = 0.85f * fade;
        }

        const glm::vec3 p0 = wp - r + u, p1 = wp + r + u, p2 = wp + r - u, p3 = wp - r - u;
        const auto V = [&](const glm::vec3& P, f32 uu, f32 vv) {
            rhi::ParticleVertex pv;
            pv.x = P.x; pv.y = P.y; pv.z = P.z;
            pv.u = uu; pv.v = vv;
            pv.r = cr; pv.g = cg; pv.b = cb; pv.a = ca;
            pv.texIndex = 0; // procedural soft dot
            return pv;
        };
        const rhi::ParticleVertex a = V(p0, 0.0f, 0.0f), b = V(p1, 1.0f, 0.0f),
                                  cc = V(p2, 1.0f, 1.0f), d = V(p3, 0.0f, 1.0f);
        alphaOut.push_back(a); alphaOut.push_back(b); alphaOut.push_back(cc);
        alphaOut.push_back(a); alphaOut.push_back(cc); alphaOut.push_back(d);
        --drawBudget;
    }
}

} // namespace hbe::precip
