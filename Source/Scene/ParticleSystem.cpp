// Scene/ParticleSystem.cpp
#include "Scene/ParticleSystem.h"

#include "Assets/AssetLoader.h"
#include "Renderer/Renderer.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

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

} // namespace

void Update(Scene& scene, f32 dt, bool emitting) {
    if (dt <= 0.0f) return;
    auto& reg = scene.Registry();

    for (const entt::entity e : reg.view<ParticleEmitter>()) {
        ParticleEmitter& em = reg.get<ParticleEmitter>(e);

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
            if (em.drag > 0.0f) p.vel *= glm::max(0.0f, 1.0f - em.drag * dt);
            p.pos += p.vel * dt;
            p.rot += em.spin * dt;
            ++i;
        }

        if (!emitting || !em.emitting || em.rate <= 0.0f) continue;

        // Spawn origin + orientation come from the emitter's world transform.
        const glm::mat4 world = scene.WorldMatrix(e);
        const glm::vec3 origin(world[3]);
        const glm::mat3 basis(world); // emitter rotation/scale for the base direction
        const glm::vec3 baseDir =
            glm::dot(em.direction, em.direction) > 1e-6f
                ? glm::normalize(basis * em.direction)
                : glm::vec3(0.0f, 1.0f, 0.0f);

        em.spawnAccum += em.rate * dt;
        int toSpawn = static_cast<int>(em.spawnAccum);
        em.spawnAccum -= static_cast<f32>(toSpawn);
        for (; toSpawn > 0 && em.pool.size() < em.maxParticles; --toSpawn) {
            ParticleEmitter::Particle p;
            p.pos = origin + RandUnit(em.rngState) * (em.emitRadius * Rand01(em.rngState));
            const glm::vec3 dir =
                glm::normalize(glm::mix(baseDir, RandUnit(em.rngState),
                                        glm::clamp(em.spread, 0.0f, 1.0f)) +
                               glm::vec3(1e-5f));
            const f32 speed = em.startSpeed *
                              (1.0f + RandSigned(em.rngState) * em.speedVariance);
            p.vel = dir * speed;
            p.life = glm::max(0.05f, em.lifetime *
                                         (1.0f + RandSigned(em.rngState) * em.lifetimeVariance));
            p.age = 0.0f;
            p.rot = Rand01(em.rngState) * 6.2831853f;
            p.seed = Rand01(em.rngState);
            em.pool.push_back(p);
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

        for (const ParticleEmitter::Particle& p : em.pool) {
            const f32 t = glm::clamp(p.age / glm::max(p.life, 1e-4f), 0.0f, 1.0f);
            const f32 size = glm::mix(em.startSize, em.endSize, t);
            const glm::vec4 col = glm::mix(em.startColor, em.endColor, t);
            // Billboard basis, rotated by the particle's spin around the view axis.
            const f32 c = std::cos(p.rot), s = std::sin(p.rot);
            const glm::vec3 r = (camRight * c + camUp * s) * (size * 0.5f);
            const glm::vec3 u = (camUp * c - camRight * s) * (size * 0.5f);
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
            const rhi::ParticleVertex a = V(p0, 0, 0), b = V(p1, 1, 0),
                                      cc = V(p2, 1, 1), d = V(p3, 0, 1);
            out.push_back(a); out.push_back(b); out.push_back(cc);
            out.push_back(a); out.push_back(cc); out.push_back(d);
        }
    }
}

} // namespace hbe::particle
