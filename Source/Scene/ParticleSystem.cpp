// Scene/ParticleSystem.cpp
#include "Scene/ParticleSystem.h"

#include "Assets/AssetLoader.h"
#include "Renderer/Renderer.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <limits>

namespace hbe::particle {

namespace {

// Cheap per-emitter xorshift PRNG in [0,1).
f32 Rand01(u32& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return static_cast<f32>(s & 0xFFFFFFu) / static_cast<f32>(0x1000000);
}
f32 RandSigned(u32& s) { return Rand01(s) * 2.0f - 1.0f; }

// A roughly uniform direction on the unit sphere.
glm::vec3 RandUnit(u32& s) {
    const f32 z = RandSigned(s);
    const f32 a = Rand01(s) * 6.2831853f;
    const f32 r = std::sqrt(glm::max(0.0f, 1.0f - z * z));
    return {r * std::cos(a), r * std::sin(a), z};
}

// Spawn offset in the emitter's LOCAL space (caller applies origin + basis).
glm::vec3 SampleSpawnLocal(ParticleEmitter& em) {
    u32& s = em.rngState;
    switch (em.shape) {
        case ParticleEmitter::Shape::Point:
            return glm::vec3(0.0f);
        case ParticleEmitter::Shape::Hemisphere: {
            glm::vec3 d = RandUnit(s);
            d.y = std::abs(d.y);
            return d * (em.emitRadius * Rand01(s));
        }
        case ParticleEmitter::Shape::Box:
            return {RandSigned(s) * em.boxHalfExtents.x,
                    RandSigned(s) * em.boxHalfExtents.y,
                    RandSigned(s) * em.boxHalfExtents.z};
        case ParticleEmitter::Shape::Disc:
        case ParticleEmitter::Shape::Cone: { // both spawn on a base disc (XZ)
            const f32 a = Rand01(s) * 6.2831853f;
            const f32 r = em.emitRadius * std::sqrt(Rand01(s));
            return {r * std::cos(a), 0.0f, r * std::sin(a)};
        }
        case ParticleEmitter::Shape::Sphere:
        default:
            return RandUnit(s) * (em.emitRadius * Rand01(s));
    }
}

// Initial velocity direction (world space). Cone confines to `coneAngle` about
// baseDir; every other shape uses the legacy direction+spread blend.
glm::vec3 SampleDir(ParticleEmitter& em, const glm::vec3& baseDir) {
    u32& s = em.rngState;
    if (em.shape == ParticleEmitter::Shape::Cone) {
        const f32 cosMax = std::cos(glm::radians(glm::clamp(em.coneAngle, 0.0f, 179.0f)));
        const f32 cosT = glm::mix(1.0f, cosMax, Rand01(s));
        const f32 sinT = std::sqrt(glm::max(0.0f, 1.0f - cosT * cosT));
        const f32 phi = Rand01(s) * 6.2831853f;
        const glm::vec3 up =
            std::abs(baseDir.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
        const glm::vec3 tx = glm::normalize(glm::cross(up, baseDir));
        const glm::vec3 ty = glm::cross(baseDir, tx);
        return glm::normalize(baseDir * cosT + (tx * std::cos(phi) + ty * std::sin(phi)) * sinT);
    }
    return glm::normalize(glm::mix(baseDir, RandUnit(s),
                                   glm::clamp(em.spread, 0.0f, 1.0f)) +
                          glm::vec3(1e-5f));
}

} // namespace

void Update(Scene& scene, f32 dt, bool emitting) {
    if (dt <= 0.0f) return;
    auto& reg = scene.Registry();

    for (const entt::entity e : reg.view<ParticleEmitter>()) {
        ParticleEmitter& em = reg.get<ParticleEmitter>(e);
        em.simTime += dt; // ever-accumulating (turbulence phase)

        // Emitter world transform (origin doubles as the vortex axis for the roll).
        const glm::mat4 world = scene.WorldMatrix(e);
        const glm::vec3 origin(world[3]);
        const glm::mat3 basis(world); // emitter rotation/scale for shape + direction
        const bool hasRoll = em.buoyancy != 0.0f || em.vortex != 0.0f;

        // Integrate live particles; swap-and-pop the expired ones.
        for (usize i = 0; i < em.pool.size();) {
            ParticleEmitter::Particle& p = em.pool[i];
            p.age += dt;
            if (p.age >= p.life) {
                em.pool[i] = em.pool.back();
                em.pool.pop_back();
                continue;
            }
            p.vel += em.gravity * dt;
            if (em.turbulence > 0.0f) { // swirly divergence-light noise force
                const glm::vec3 q = p.pos * em.turbulenceScale + glm::vec3(em.simTime * 0.5f);
                const glm::vec3 f(std::sin(q.y) - std::cos(q.z), std::sin(q.z) - std::cos(q.x),
                                  std::sin(q.x) - std::cos(q.y));
                p.vel += f * (em.turbulence * dt);
            }
            if (hasRoll) {
                // Heat = 1 at birth, 0 at death. Buoyancy lifts hot gas (rising fireball
                // that stalls into a cap); the vortex ring rolls the front outward + over.
                const f32 heat = 1.0f - glm::clamp(p.age / glm::max(p.life, 1e-4f), 0.0f, 1.0f);
                if (em.buoyancy != 0.0f) p.vel.y += em.buoyancy * heat * dt;
                if (em.vortex != 0.0f) {
                    const glm::vec3 rel = p.pos - origin;
                    const glm::vec2 rxz(rel.x, rel.z);
                    const f32 rlen = glm::length(rxz);
                    if (rlen > 1e-3f) {
                        const glm::vec2 out = rxz / rlen; // outward radial in the XZ plane
                        // Poloidal roll of a rising vortex ring: push outward (stronger up
                        // high) and curl the outer edge downward -> the mushroom-cap overhang.
                        const f32 up = glm::max(0.0f, rel.y);
                        p.vel.x += out.x * em.vortex * (0.5f + up * 0.35f) * dt;
                        p.vel.z += out.y * em.vortex * (0.5f + up * 0.35f) * dt;
                        p.vel.y -= em.vortex * rlen * 0.35f * dt;
                    }
                }
            }
            if (em.drag > 0.0f) p.vel *= glm::max(0.0f, 1.0f - em.drag * dt);
            p.pos += p.vel * dt;
            p.rot += em.spin * dt;
            ++i;
        }

        // Emission activation edge: reset the burst + duration window on restart.
        const bool active = emitting && em.emitting;
        if (active && !em.wasEmitting) { em.activeAge = 0.0f; em.bursted = false; }
        em.wasEmitting = active;
        if (!active) continue;
        em.activeAge += dt;

        // Spawn direction comes from the emitter's world orientation (origin/basis above).
        const glm::vec3 baseDir =
            glm::dot(em.direction, em.direction) > 1e-6f
                ? glm::normalize(basis * em.direction)
                : glm::vec3(0.0f, 1.0f, 0.0f);

        const auto spawnOne = [&]() {
            ParticleEmitter::Particle p;
            p.pos = origin + basis * SampleSpawnLocal(em);
            p.vel = SampleDir(em, baseDir) *
                    (em.startSpeed * (1.0f + RandSigned(em.rngState) * em.speedVariance));
            p.life = glm::max(0.05f, em.lifetime *
                                         (1.0f + RandSigned(em.rngState) * em.lifetimeVariance));
            p.age = 0.0f;
            p.rot = Rand01(em.rngState) * 6.2831853f;
            p.seed = Rand01(em.rngState);
            em.pool.push_back(p);
        };

        // One-shot burst (explosions) when emission (re)starts.
        if (!em.bursted && em.burst > 0) {
            em.bursted = true;
            for (u32 k = 0; k < em.burst && em.pool.size() < em.maxParticles; ++k) spawnOne();
        }
        // Continuous emission, gated by the duration window when not looping.
        const bool windowOpen = em.loop || em.activeAge <= em.duration;
        if (windowOpen && em.rate > 0.0f) {
            em.spawnAccum += em.rate * dt;
            int toSpawn = static_cast<int>(em.spawnAccum);
            em.spawnAccum -= static_cast<f32>(toSpawn);
            for (; toSpawn > 0 && em.pool.size() < em.maxParticles; --toSpawn) spawnOne();
        }
    }
}

void BuildVertices(Scene& scene, Renderer& renderer, const std::filesystem::path& assetsDir,
                   const glm::vec3& camRight, const glm::vec3& camUp,
                   std::vector<rhi::ParticleVertex>& alphaOut,
                   std::vector<rhi::ParticleVertex>& addOut) {
    alphaOut.clear();
    addOut.clear();
    auto& reg = scene.Registry();

    for (const entt::entity e : reg.view<ParticleEmitter>()) {
        ParticleEmitter& em = reg.get<ParticleEmitter>(e);
        if (em.pool.empty()) continue;

        // Resolve the sprite once (cached). 0 = procedural soft dot in the shader.
        if (!em.textureResolved) {
            em.textureResolved = true;
            em.textureCache = 0;
            if (!em.texture.empty() && !assetsDir.empty()) {
                em.textureCache = assets::LoadTexture(renderer, assetsDir / em.texture).index;
            }
        }
        const u32 tex = em.textureCache;
        std::vector<rhi::ParticleVertex>& out = em.additive ? addOut : alphaOut;
        out.reserve(out.size() + em.pool.size() * 6);

        const u32 cols = glm::max(1u, em.subUVCols), rows = glm::max(1u, em.subUVRows);
        for (const ParticleEmitter::Particle& p : em.pool) {
            const f32 t = glm::clamp(p.age / glm::max(p.life, 1e-4f), 0.0f, 1.0f);
            const f32 size = glm::mix(em.startSize, em.endSize, t);
            glm::vec4 col = glm::mix(em.startColor, em.endColor, t);
            // Alpha fade-in/out envelope (fractions of life).
            f32 env = 1.0f;
            if (em.fadeIn > 0.0f && t < em.fadeIn) env *= t / em.fadeIn;
            if (em.fadeOut > 0.0f && t > 1.0f - em.fadeOut) env *= (1.0f - t) / em.fadeOut;
            col.a *= glm::clamp(env, 0.0f, 1.0f);

            // Sub-UV (sprite-sheet) cell -> local UV rect.
            f32 u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
            if (cols > 1 || rows > 1) {
                const u32 cells = cols * rows;
                const u32 frame = (em.subUVFps > 0.0f)
                                      ? static_cast<u32>(p.age * em.subUVFps) % cells
                                      : glm::min(static_cast<u32>(t * static_cast<f32>(cells)),
                                                 cells - 1);
                const u32 fx = frame % cols, fy = frame / cols;
                const f32 du = 1.0f / static_cast<f32>(cols), dv = 1.0f / static_cast<f32>(rows);
                u0 = static_cast<f32>(fx) * du; v0 = static_cast<f32>(fy) * dv;
                u1 = u0 + du; v1 = v0 + dv;
            }

            // Quad axes per render mode.
            const f32 c = std::cos(p.rot), s = std::sin(p.rot);
            glm::vec3 r, u;
            if (em.render == ParticleEmitter::Render::Stretched) {
                glm::vec2 vs(glm::dot(p.vel, camRight), glm::dot(p.vel, camUp));
                const f32 vlen = glm::length(vs);
                if (vlen > 1e-4f) {
                    vs /= vlen;
                    const glm::vec3 lng = camRight * vs.x + camUp * vs.y;   // along velocity
                    const glm::vec3 prp = camRight * (-vs.y) + camUp * vs.x; // perpendicular
                    r = prp * (size * 0.5f);
                    u = lng * (size * 0.5f * glm::max(1.0f, em.stretch));
                } else {
                    r = (camRight * c + camUp * s) * (size * 0.5f);
                    u = (camUp * c - camRight * s) * (size * 0.5f);
                }
            } else if (em.render == ParticleEmitter::Render::Horizontal) {
                r = glm::vec3(c, 0.0f, s) * (size * 0.5f);   // lies flat in world XZ
                u = glm::vec3(-s, 0.0f, c) * (size * 0.5f);
            } else { // Billboard: camera-facing, spun around the view axis
                r = (camRight * c + camUp * s) * (size * 0.5f);
                u = (camUp * c - camRight * s) * (size * 0.5f);
            }
            const glm::vec3 p0 = p.pos - r + u; // top-left
            const glm::vec3 p1 = p.pos + r + u; // top-right
            const glm::vec3 p2 = p.pos + r - u; // bottom-right
            const glm::vec3 p3 = p.pos - r - u; // bottom-left
            const auto V = [&](const glm::vec3& wp, f32 uu, f32 vv) {
                rhi::ParticleVertex v;
                v.x = wp.x; v.y = wp.y; v.z = wp.z;
                v.u = uu; v.v = vv;
                v.r = col.r; v.g = col.g; v.b = col.b; v.a = col.a;
                v.texIndex = tex;
                return v;
            };
            const rhi::ParticleVertex a = V(p0, u0, v0), b = V(p1, u1, v0),
                                      cc = V(p2, u1, v1), d = V(p3, u0, v1);
            out.push_back(a); out.push_back(b); out.push_back(cc);
            out.push_back(a); out.push_back(cc); out.push_back(d);
        }
    }
}

bool BuildVolumetricBlobs(Scene& scene, std::vector<rhi::VolumeBlob>& blobsOut,
                          rhi::VolumeParams& paramsOut) {
    blobsOut.clear();
    auto& reg = scene.Registry();

    glm::vec3 bmin(std::numeric_limits<f32>::max());
    glm::vec3 bmax(std::numeric_limits<f32>::lowest());
    f32 radiusSum = 0.0f; // -> average blob radius = the STABLE world noise scale
    bool haveParams = false; // first volumetric emitter seeds the tuning knobs

    for (const entt::entity e : reg.view<ParticleEmitter>()) {
        ParticleEmitter& em = reg.get<ParticleEmitter>(e);
        if (!em.volumetric || em.pool.empty()) continue;

        const f32 radScale = glm::max(0.01f, em.volRadiusScale);
        for (const ParticleEmitter::Particle& p : em.pool) {
            if (blobsOut.size() >= rhi::kMaxVolumeBlobs) break;
            const f32 t = glm::clamp(p.age / glm::max(p.life, 1e-4f), 0.0f, 1.0f);
            const f32 size = glm::mix(em.startSize, em.endSize, t);

            // Density fades in/out with the same envelope as the billboard alpha so
            // blobs appear and dissipate smoothly instead of popping.
            f32 env = 1.0f;
            if (em.fadeIn > 0.0f && t < em.fadeIn) env *= t / em.fadeIn;
            if (em.fadeOut > 0.0f && t > 1.0f - em.fadeOut) env *= (1.0f - t) / em.fadeOut;
            env = glm::clamp(env, 0.0f, 1.0f);

            rhi::VolumeBlob b;
            b.pos = p.pos;
            b.radius = glm::max(0.01f, size * radScale);
            b.density = glm::max(0.0f, em.volDensity) * env;
            // Fire cools as it rises: hot at birth, fading to smoke over life.
            b.temperature = glm::clamp(em.volTemperature * (1.0f - t), 0.0f, 1.0f);
            if (b.density <= 1e-4f) continue;
            blobsOut.push_back(b);
            radiusSum += b.radius;

            // Tuning knobs come from the first emitter that actually CONTRIBUTES a blob
            // (not merely the first with a non-empty pool), so the emitter that lights
            // the shared volume is one that put density into it.
            if (!haveParams) {
                haveParams = true;
                paramsOut.densityScale = 1.0f;
                paramsOut.emission = glm::max(0.0f, em.volEmission);
                paramsOut.extinction = glm::max(1e-3f, em.volExtinction);
                paramsOut.stepCount = static_cast<u32>(glm::clamp(em.volSteps, 4, 256));
                paramsOut.resolution = static_cast<u32>(glm::clamp(em.volResolution, 32, 192));
                paramsOut.noiseDetail = glm::clamp(em.volDetail, 0.0f, 1.0f);
            }

            bmin = glm::min(bmin, b.pos - glm::vec3(b.radius));
            bmax = glm::max(bmax, b.pos + glm::vec3(b.radius));
        }
    }

    if (blobsOut.empty()) return false;

    // Pad the AABB slightly so the raymarch fully clips soft blob edges.
    const glm::vec3 pad(0.25f);
    paramsOut.boundsMin = bmin - pad;
    paramsOut.boundsMax = bmax + pad;
    paramsOut.blobCount = static_cast<u32>(blobsOut.size());
    // Average blob radius = the world-space turbulence scale. Derived from particle
    // size (stable frame-to-frame), NOT the AABB extent (which jumps when the emitter
    // moves fast) - so the procedural noise stays world-anchored and doesn't crawl/pop.
    paramsOut.noiseScale = glm::max(0.05f, radiusSum / static_cast<f32>(blobsOut.size()));
    return true;
}

const char* TemplateName(Template t) {
    switch (t) {
        case Template::Fire: return "Fire";
        case Template::Smoke: return "Smoke";
        case Template::Dust: return "Dust";
        case Template::Rain: return "Rain";
        case Template::Leaves: return "Leaves";
        case Template::Explosion: return "Explosion";
        case Template::Sparks: return "Sparks";
        case Template::Magic: return "Magic";
        case Template::VolFire: return "Volumetric Fire";
        case Template::VolSmoke: return "Volumetric Smoke";
        case Template::VolExplosion: return "Volumetric Explosion (mushroom)";
        default: return "?";
    }
}

ParticleEmitter MakeTemplate(Template t) {
    using S = ParticleEmitter::Shape;
    using R = ParticleEmitter::Render;
    ParticleEmitter e; // procedural soft dot (no texture) for all presets
    e.texture.clear();
    switch (t) {
        case Template::Fire:
            e.additive = true; e.shape = S::Cone; e.coneAngle = 16.0f; e.emitRadius = 0.15f;
            e.direction = {0, 1, 0}; e.startSpeed = 1.6f; e.speedVariance = 0.4f;
            e.gravity = {0, 0.6f, 0}; e.drag = 1.0f; e.rate = 70.0f; e.maxParticles = 400;
            e.lifetime = 1.1f; e.lifetimeVariance = 0.3f;
            e.startColor = {1.0f, 0.8f, 0.35f, 1.0f}; e.endColor = {1.0f, 0.25f, 0.05f, 0.0f};
            e.startSize = 0.5f; e.endSize = 0.06f; e.fadeOut = 0.35f;
            e.turbulence = 0.9f; e.turbulenceScale = 1.6f;
            break;
        case Template::Smoke:
            e.additive = false; e.shape = S::Sphere; e.emitRadius = 0.2f;
            e.direction = {0, 1, 0}; e.startSpeed = 0.5f; e.speedVariance = 0.4f;
            e.gravity = {0, 0.3f, 0}; e.drag = 0.8f; e.rate = 18.0f; e.maxParticles = 300;
            e.lifetime = 3.5f; e.lifetimeVariance = 0.3f; e.spin = 0.3f;
            e.startColor = {0.5f, 0.5f, 0.5f, 0.5f}; e.endColor = {0.3f, 0.3f, 0.3f, 0.0f};
            e.startSize = 0.5f; e.endSize = 2.2f; e.fadeIn = 0.15f; e.fadeOut = 0.5f;
            e.turbulence = 0.5f; e.turbulenceScale = 0.8f;
            break;
        case Template::Dust:
            e.additive = false; e.shape = S::Box; e.boxHalfExtents = {1.5f, 0.1f, 1.5f};
            e.direction = {0, 1, 0}; e.startSpeed = 0.2f; e.speedVariance = 0.6f;
            e.gravity = {0, 0.05f, 0}; e.drag = 0.5f; e.rate = 26.0f; e.maxParticles = 300;
            e.lifetime = 5.0f; e.lifetimeVariance = 0.5f;
            e.startColor = {0.7f, 0.65f, 0.5f, 0.35f}; e.endColor = {0.7f, 0.65f, 0.5f, 0.0f};
            e.startSize = 0.2f; e.endSize = 0.6f; e.fadeIn = 0.2f; e.fadeOut = 0.4f;
            e.turbulence = 0.3f; e.turbulenceScale = 0.5f;
            break;
        case Template::Rain:
            e.additive = false; e.render = R::Stretched; e.stretch = 8.0f;
            e.shape = S::Box; e.boxHalfExtents = {6.0f, 0.1f, 6.0f};
            e.direction = {0, -1, 0}; e.startSpeed = 12.0f; e.speedVariance = 0.1f; e.spread = 0.0f;
            e.gravity = {0, -9.0f, 0}; e.drag = 0.0f; e.rate = 400.0f; e.maxParticles = 2000;
            e.lifetime = 1.4f; e.lifetimeVariance = 0.2f;
            e.startColor = {0.6f, 0.7f, 0.9f, 0.5f}; e.endColor = {0.6f, 0.7f, 0.9f, 0.3f};
            e.startSize = 0.04f; e.endSize = 0.04f;
            break;
        case Template::Leaves:
            e.additive = false; e.shape = S::Box; e.boxHalfExtents = {3.0f, 0.1f, 3.0f};
            e.direction = {0, -1, 0}; e.startSpeed = 0.6f; e.speedVariance = 0.5f;
            e.gravity = {0, -0.5f, 0}; e.drag = 0.8f; e.rate = 8.0f; e.maxParticles = 120;
            e.lifetime = 6.0f; e.lifetimeVariance = 0.4f; e.spin = 1.5f;
            e.startColor = {0.5f, 0.6f, 0.2f, 1.0f}; e.endColor = {0.4f, 0.3f, 0.1f, 0.0f};
            e.startSize = 0.2f; e.endSize = 0.2f; e.fadeOut = 0.2f;
            e.turbulence = 1.2f; e.turbulenceScale = 0.6f;
            break;
        case Template::Explosion:
            e.additive = true; e.shape = S::Sphere; e.emitRadius = 0.1f;
            e.burst = 200; e.loop = false; e.duration = 0.05f; e.rate = 0.0f; e.maxParticles = 400;
            e.direction = {0, 1, 0}; e.startSpeed = 6.0f; e.speedVariance = 0.5f; e.spread = 1.0f;
            e.gravity = {0, -2.0f, 0}; e.drag = 2.5f; e.lifetime = 0.8f; e.lifetimeVariance = 0.3f;
            e.startColor = {1.0f, 0.9f, 0.6f, 1.0f}; e.endColor = {1.0f, 0.3f, 0.05f, 0.0f};
            e.startSize = 0.5f; e.endSize = 1.2f; e.fadeOut = 0.5f; e.turbulence = 1.0f;
            break;
        case Template::Sparks:
            e.additive = true; e.render = R::Stretched; e.stretch = 5.0f; e.shape = S::Point;
            e.burst = 40; e.loop = false; e.duration = 0.1f; e.rate = 0.0f; e.maxParticles = 200;
            e.direction = {0, 1, 0}; e.startSpeed = 5.0f; e.speedVariance = 0.6f; e.spread = 0.8f;
            e.gravity = {0, -9.0f, 0}; e.drag = 0.5f; e.lifetime = 1.2f; e.lifetimeVariance = 0.4f;
            e.startColor = {1.0f, 0.85f, 0.4f, 1.0f}; e.endColor = {1.0f, 0.4f, 0.1f, 0.0f};
            e.startSize = 0.06f; e.endSize = 0.02f; e.fadeOut = 0.3f;
            break;
        case Template::Magic:
            e.additive = true; e.shape = S::Sphere; e.emitRadius = 0.3f;
            e.direction = {0, 1, 0}; e.startSpeed = 0.6f; e.speedVariance = 0.5f;
            e.gravity = {0, 0.2f, 0}; e.drag = 1.0f; e.rate = 40.0f; e.maxParticles = 300;
            e.lifetime = 2.5f; e.lifetimeVariance = 0.4f; e.spin = 1.0f;
            e.startColor = {0.5f, 0.7f, 1.0f, 1.0f}; e.endColor = {0.8f, 0.4f, 1.0f, 0.0f};
            e.startSize = 0.15f; e.endSize = 0.05f; e.fadeIn = 0.1f; e.fadeOut = 0.4f;
            e.turbulence = 1.5f; e.turbulenceScale = 1.2f;
            break;
        case Template::VolFire:
            // Real raymarched fire: particles are invisible carriers (billboard alpha
            // ~0), the 3D volume does the rendering. Hot base cools as it rises.
            e.additive = false; e.shape = S::Cone; e.coneAngle = 18.0f; e.emitRadius = 0.25f;
            e.direction = {0, 1, 0}; e.startSpeed = 1.4f; e.speedVariance = 0.4f;
            e.gravity = {0, 0.9f, 0}; e.drag = 0.9f; e.rate = 90.0f; e.maxParticles = 500;
            e.lifetime = 1.6f; e.lifetimeVariance = 0.3f;
            e.startColor = {1, 1, 1, 0.0f}; e.endColor = {1, 1, 1, 0.0f}; // volume renders it
            e.startSize = 0.5f; e.endSize = 0.9f; e.fadeIn = 0.1f; e.fadeOut = 0.4f;
            e.turbulence = 1.1f; e.turbulenceScale = 1.4f;
            e.volumetric = true; e.volDensity = 0.6f; e.volRadiusScale = 2.2f;
            e.volTemperature = 1.0f; e.volEmission = 3.5f; e.volExtinction = 1.4f;
            e.volSteps = 56; e.volResolution = 96;
            break;
        case Template::VolSmoke:
            e.additive = false; e.shape = S::Sphere; e.emitRadius = 0.3f;
            e.direction = {0, 1, 0}; e.startSpeed = 0.6f; e.speedVariance = 0.4f;
            e.gravity = {0, 0.4f, 0}; e.drag = 0.7f; e.rate = 45.0f; e.maxParticles = 400;
            e.lifetime = 3.5f; e.lifetimeVariance = 0.3f; e.spin = 0.2f;
            e.startColor = {1, 1, 1, 0.0f}; e.endColor = {1, 1, 1, 0.0f}; // volume renders it
            e.startSize = 0.6f; e.endSize = 2.0f; e.fadeIn = 0.15f; e.fadeOut = 0.5f;
            e.turbulence = 0.6f; e.turbulenceScale = 0.7f;
            e.volumetric = true; e.volDensity = 0.9f; e.volRadiusScale = 2.6f;
            e.volTemperature = 0.0f; e.volEmission = 0.0f; e.volExtinction = 1.8f;
            e.volSteps = 48; e.volResolution = 96;
            break;
        case Template::VolExplosion:
            // Nuclear-style mushroom cloud. A NARROW, sustained upward column feeds the
            // stem; buoyancy rockets the hot front up; the vortex ring pushes outward
            // MORE the higher a particle has risen (see particle::Update) so the risen
            // material blooms into a cap while fresh particles stay in the stem -> the
            // classic funnel. Billboard alpha 0: the volume renders it.
            e.additive = false; e.shape = S::Sphere; e.emitRadius = 0.3f;
            e.burst = 80; e.loop = false; e.duration = 1.5f; e.rate = 260.0f;
            e.maxParticles = 1300;
            e.direction = {0, 1, 0}; e.startSpeed = 7.0f; e.speedVariance = 0.4f; e.spread = 0.25f;
            e.gravity = {0, 0.2f, 0}; e.drag = 1.2f;
            e.lifetime = 5.0f; e.lifetimeVariance = 0.3f;
            e.startColor = {1, 1, 1, 0.0f}; e.endColor = {1, 1, 1, 0.0f};
            e.startSize = 0.6f; e.endSize = 2.4f; e.fadeIn = 0.05f; e.fadeOut = 0.5f;
            e.turbulence = 1.6f; e.turbulenceScale = 0.7f;
            e.buoyancy = 10.0f; // strong rise -> the cap climbs high and piles up
            e.vortex = 4.5f;    // height-scaled toroidal roll -> the mushroom overhang
            e.volumetric = true; e.volDensity = 0.85f; e.volRadiusScale = 2.3f;
            e.volTemperature = 1.0f; e.volEmission = 4.5f; e.volExtinction = 1.5f;
            e.volSteps = 64; e.volDetail = 0.75f; // extra billowing for the hero cloud
            e.volResolution = 128; // hero effect: bigger volume (lower for low-end)
            break;
        default:
            break;
    }
    return e;
}

} // namespace hbe::particle
