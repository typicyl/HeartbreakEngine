// Scene/ParticleSystem.cpp
#include "Scene/ParticleSystem.h"

#include "Assets/AssetLoader.h"
#include "Core/Log.h"
#include "Renderer/Renderer.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Vfx/VfxLegacy.h"
#include "Vfx/VfxStack.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace hbe::particle {

// vfx::LegacyShape is a copy of ParticleEmitter::Shape kept in Source/Vfx so the VFX
// core does not depend on Scene. This is the lockstep guard: the two enums are one
// list, and a reorder in either would silently re-point every authored emit shape.
static_assert(static_cast<u32>(ParticleEmitter::Shape::Point) ==
                      static_cast<u32>(vfx::LegacyShape::Point) &&
                  static_cast<u32>(ParticleEmitter::Shape::Sphere) ==
                      static_cast<u32>(vfx::LegacyShape::Sphere) &&
                  static_cast<u32>(ParticleEmitter::Shape::Hemisphere) ==
                      static_cast<u32>(vfx::LegacyShape::Hemisphere) &&
                  static_cast<u32>(ParticleEmitter::Shape::Box) ==
                      static_cast<u32>(vfx::LegacyShape::Box) &&
                  static_cast<u32>(ParticleEmitter::Shape::Disc) ==
                      static_cast<u32>(vfx::LegacyShape::Disc) &&
                  static_cast<u32>(ParticleEmitter::Shape::Cone) ==
                      static_cast<u32>(vfx::LegacyShape::Cone),
              "ParticleEmitter::Shape and vfx::LegacyShape drifted apart. They mirror each "
              "other value-for-value; fix both or neither.");

namespace {

using MT = vfx::ModuleType;

// --------------------------------------------------------------------------
// Component -> compiled stack
// --------------------------------------------------------------------------

// Only these change the SHAPE of the stack. Everything else (rate, burst, gravity,
// colours, ...) is re-stamped in place every frame, so dragging a slider in the
// inspector never recompiles and never allocates - which matters because the
// inspector edits these on every frame the mouse is down.
u64 StackSignature(const ParticleEmitter& em) {
    u64 h = 0x9E3779B97F4A7C15ull; // seeded nonzero: 0 means "never compiled"
    const auto mix = [&h](u64 v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); };
    mix(em.maxParticles);
    mix(em.useCurlNoise ? 1ull : 0ull);
    mix(em.expDrag ? 1ull : 0ull);
    mix(em.simulateColor ? 1ull : 0ull);
    mix(em.simulateSize ? 1ull : 0ull);
    return h ? h : 1ull;
}

// The authored module list for one emitter.
//
// The spine is the four Legacy.* modules in their original order. Opt-in modules are
// spliced in at the point the legacy code would have run the equivalent step, so
// enabling one changes that step and nothing else:
//   * curl noise lands after the legacy forces and BEFORE drag, exactly where the
//     legacy turbulence sat, so the swirl is damped like every other force;
//   * exponential drag REPLACES Legacy.DragLinear rather than stacking with it;
//   * the colour/size curves run after the integrator - they touch neither position
//     nor velocity, and putting them last means they see this frame's final age.
vfx::StackDesc BuildDesc(const ParticleEmitter& em) {
    vfx::StackDesc d;
    // NOT clamped to >= 1. The editor already clamps the slider, but a hand-written
    // scene with maxParticles 0 emitted nothing under the old system, and a silent
    // clamp here would give it one particle - a small difference, but the guarantee is
    // bit-identity, and Reserve(0)/Grant handle a zero-capacity pool correctly.
    d.maxParticles = em.maxParticles;
    d.seed = 0;                                 // the legacy stream lives in EmitterState
    d.accum = vfx::SpawnAccumMode::Legacy32;    // see SpawnAccumMode
    d.modules.reserve(8);

    d.modules.emplace_back(MT::LegacyInitState, vfx::ModuleParams{});
    if (em.simulateColor) d.modules.emplace_back(MT::SpawnInitColor, vfx::ModuleParams{});
    if (em.simulateSize) d.modules.emplace_back(MT::SpawnInitSize, vfx::ModuleParams{});

    d.modules.emplace_back(MT::LegacyForces, vfx::ModuleParams{});
    if (em.useCurlNoise) d.modules.emplace_back(MT::CurlNoiseForce, vfx::ModuleParams{});
    d.modules.emplace_back(em.expDrag ? MT::Drag : MT::LegacyDragLinear, vfx::ModuleParams{});
    d.modules.emplace_back(MT::LegacyIntegrate, vfx::ModuleParams{});

    if (em.simulateColor) d.modules.emplace_back(MT::ColorOverLife, vfx::ModuleParams{});
    if (em.simulateSize) d.modules.emplace_back(MT::SizeOverLife, vfx::ModuleParams{});
    return d;
}

// The fade envelope at t == 0, i.e. what the renderer-side ramp produces for a
// brand-new particle. Spawn.InitColor is stamped with it so a particle's first frame
// matches its second - the update stage runs over [0, oldCount) and therefore never
// sees the particles born this frame.
f32 SpawnEnvelope(const ParticleEmitter& em) { return em.fadeIn > 0.0f ? 0.0f : 1.0f; }

// Copies this frame's authored values into the compiled modules. O(modules), no
// allocation, no recompile. The Legacy.* modules take nothing here - they read the
// whole emitter through ParticleView::user (see FillLegacy).
void StampParams(ParticleEmitter& em) {
    for (u32 s = 0; s < vfx::kStageCount; ++s) {
        for (vfx::CompiledModule& m : em.stack.stages[s]) {
            vfx::ModuleParams& p = m.params;
            switch (m.type) {
                case MT::SpawnInitColor: {
                    const f32 env = SpawnEnvelope(em);
                    p.f[0] = em.startColor.r;
                    p.f[1] = em.startColor.g;
                    p.f[2] = em.startColor.b;
                    p.f[3] = em.startColor.a * env;
                    p.f[4] = em.colorVariance;
                    break;
                }
                case MT::SpawnInitSize:
                    p.f[0] = em.startSize;
                    p.f[1] = em.startSize;
                    p.f[2] = em.sizeVariance;
                    break;
                case MT::CurlNoiseForce:
                    p.f[0] = em.curlStrength;
                    p.f[1] = em.curlFrequency;
                    break;
                case MT::Drag:
                    p.f[0] = em.drag;
                    break;
                case MT::ColorOverLife:
                    p.f[0] = em.startColor.r;
                    p.f[1] = em.startColor.g;
                    p.f[2] = em.startColor.b;
                    p.f[3] = em.startColor.a;
                    p.f[4] = em.endColor.r;
                    p.f[5] = em.endColor.g;
                    p.f[6] = em.endColor.b;
                    p.f[7] = em.endColor.a;
                    p.f[8] = em.fadeIn;
                    p.f[9] = em.fadeOut;
                    p.f[10] = em.colorVariance;
                    break;
                case MT::SizeOverLife:
                    p.f[0] = em.startSize;
                    p.f[1] = em.startSize;
                    p.f[2] = em.endSize;
                    p.f[3] = em.endSize;
                    p.f[4] = em.sizeVariance;
                    break;
                default:
                    break; // Legacy.* modules read em.legacy
            }
        }
    }
}

// Rebuilds the legacy parameter block from the component + this frame's world matrix.
void FillLegacy(const ParticleEmitter& em, const glm::mat4& world, vfx::LegacyParams& L) {
    L.basis = glm::mat3(world);  // emitter rotation/scale for shape + direction
    L.origin = glm::vec3(world[3]); // also the vortex axis for the mushroom roll
    L.baseDir = glm::dot(em.direction, em.direction) > 1e-6f
                    ? glm::normalize(L.basis * em.direction)
                    : glm::vec3(0.0f, 1.0f, 0.0f);
    L.boxHalfExtents = em.boxHalfExtents;
    L.shape = static_cast<vfx::LegacyShape>(em.shape);
    L.emitRadius = em.emitRadius;
    L.coneAngle = em.coneAngle;
    L.startSpeed = em.startSpeed;
    L.speedVariance = em.speedVariance;
    L.lifetime = em.lifetime;
    L.lifetimeVariance = em.lifetimeVariance;
    L.spread = em.spread;
    L.gravity = em.gravity;
    L.turbulence = em.turbulence;
    L.turbulenceScale = em.turbulenceScale;
    L.buoyancy = em.buoyancy;
    L.vortex = em.vortex;
    L.drag = em.drag;
    L.spin = em.spin;
}

// Compiles if the structure changed, then advances one frame. Split out of Update()
// so CompatSelfTest can drive a single emitter without a Scene.
void StepEmitter(ParticleEmitter& em, const glm::mat4& world, f32 dt, bool emitting) {
    const u64 sig = StackSignature(em);
    if (sig != em.stackSignature) {
        const bool first = (em.stackSignature == 0);
        std::string errs;
        if (!vfx::Compile(BuildDesc(em), em.stack, &errs))
            HBE_WARN("[particle] emitter stack failed to compile: {}", errs);
        // Sizes every live stream. This also RESETS the pool to zero live particles, so
        // an artist raising Max or ticking a module flag mid-play restarts the effect.
        vfx::ReservePool(em.stack, em.pool);
        em.stackSignature = sig;
        if (first) {
            em.state = vfx::EmitterState{};
            em.state.rngState = vfx::kLegacyRngSeed; // the authored stream start
            em.state.emitting = false;               // first active frame is a rising edge
        }
        // RE-ARM, on every structural recompile and not just the first. The pool was
        // just emptied, so leaving the emission edge latched would not "restart" the
        // effect - it would DELETE it. A one-shot emitter (Explosion, Sparks, or any
        // non-looping preset whose duration window has expired) has burstFired == true
        // and wasEmitting == true, and with the particles gone and no rising edge to
        // clear either flag, nothing would ever spawn again until the artist manually
        // unticked and re-ticked Emitting. Clearing them here makes the next active
        // frame a rising edge, which re-fires the burst and reopens the window.
        em.state.emitterAge = 0.0f;
        em.state.burstFired = false;
        em.state.wasEmitting = false;
    }

    // Per-frame authored constants the runner owns. Cheap enough to write blind.
    em.stack.spawnRate = em.rate;
    em.stack.burst = em.burst;
    em.stack.loop = em.loop;
    em.stack.duration = em.duration;
    StampParams(em);
    FillLegacy(em, world, em.legacy);

    // Emission activation edge: reset the burst + duration window on restart. This
    // sits here rather than in RunFrame because "the emitter restarted" is a property
    // of the component's `emitting` flag, which the runner cannot see.
    const bool active = emitting && em.emitting;
    if (active && !em.state.wasEmitting) {
        em.state.emitterAge = 0.0f;
        em.state.burstFired = false;
    }
    em.state.emitting = active;
    vfx::RunFrame(em.stack, em.state, em.pool, dt, &em.legacy);
}

} // namespace

void Update(Scene& scene, f32 dt, bool emitting) {
    if (dt <= 0.0f) return;
    auto& reg = scene.Registry();
    for (const entt::entity e : reg.view<ParticleEmitter>()) {
        ParticleEmitter& em = reg.get<ParticleEmitter>(e);
        StepEmitter(em, scene.WorldMatrix(e), dt, emitting);
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
        const u32 count = em.pool.count;
        if (count == 0) continue;

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
        out.reserve(out.size() + static_cast<usize>(count) * 6);

        // Colour and size are evaluated HERE from age, exactly as they always were -
        // unless the emitter opted into the simulated attributes, in which case the
        // module stack already wrote per-particle values and this reads them instead.
        // With variance 0 the two paths produce the same numbers; the streams exist so
        // that variance (and, later, GPU simulation) is expressible at all.
        const bool simColor = em.pool.Has(vfx::Attr::Color);
        const bool simSize = em.pool.Has(vfx::Attr::Size);
        // Position/Age/Lifetime are guaranteed by Compile (it refuses a stack that
        // never initialises Age and Lifetime, and every integrator writes Position).
        // Rotation and Velocity are not: a future stack with no spin and no stretched
        // rendering would legitimately drop them, and dead-stream elimination makes
        // those pointers NULL rather than zeroed. Read them through the mask.
        const bool hasRot = em.pool.Has(vfx::Attr::Rotation);
        const bool hasVel = em.pool.Has(vfx::Attr::Velocity);

        const u32 cols = glm::max(1u, em.subUVCols), rows = glm::max(1u, em.subUVRows);
        for (u32 i = 0; i < count; ++i) {
            const glm::vec3 pos = em.pool.position[i];
            const f32 age = em.pool.age[i];
            const f32 t = glm::clamp(age / glm::max(em.pool.lifetime[i], 1e-4f), 0.0f, 1.0f);
            const f32 size = simSize ? em.pool.sizeX[i] : glm::mix(em.startSize, em.endSize, t);
            glm::vec4 col;
            if (simColor) {
                col = em.pool.color[i];
            } else {
                col = glm::mix(em.startColor, em.endColor, t);
                // Alpha fade-in/out envelope (fractions of life).
                f32 env = 1.0f;
                if (em.fadeIn > 0.0f && t < em.fadeIn) env *= t / em.fadeIn;
                if (em.fadeOut > 0.0f && t > 1.0f - em.fadeOut) env *= (1.0f - t) / em.fadeOut;
                col.a *= glm::clamp(env, 0.0f, 1.0f);
            }

            // Sub-UV (sprite-sheet) cell -> local UV rect.
            f32 u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
            if (cols > 1 || rows > 1) {
                const u32 cells = cols * rows;
                const u32 frame = (em.subUVFps > 0.0f)
                                      ? static_cast<u32>(age * em.subUVFps) % cells
                                      : glm::min(static_cast<u32>(t * static_cast<f32>(cells)),
                                                 cells - 1);
                const u32 fx = frame % cols, fy = frame / cols;
                const f32 du = 1.0f / static_cast<f32>(cols), dv = 1.0f / static_cast<f32>(rows);
                u0 = static_cast<f32>(fx) * du; v0 = static_cast<f32>(fy) * dv;
                u1 = u0 + du; v1 = v0 + dv;
            }

            // Quad axes per render mode.
            const f32 rot = hasRot ? em.pool.rotation[i] : 0.0f;
            const f32 c = std::cos(rot), s = std::sin(rot);
            glm::vec3 r, u;
            if (em.render == ParticleEmitter::Render::Stretched && hasVel) {
                const glm::vec3 vel = em.pool.velocity[i];
                glm::vec2 vs(glm::dot(vel, camRight), glm::dot(vel, camUp));
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
            const glm::vec3 p0 = pos - r + u; // top-left
            const glm::vec3 p1 = pos + r + u; // top-right
            const glm::vec3 p2 = pos + r - u; // bottom-right
            const glm::vec3 p3 = pos - r - u; // bottom-left
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
        if (!em.volumetric || em.pool.count == 0) continue;

        const f32 radScale = glm::max(0.01f, em.volRadiusScale);
        const bool simSize = em.pool.Has(vfx::Attr::Size);
        for (u32 i = 0; i < em.pool.count; ++i) {
            if (blobsOut.size() >= rhi::kMaxVolumeBlobs) break;
            const f32 t = glm::clamp(em.pool.age[i] / glm::max(em.pool.lifetime[i], 1e-4f),
                                     0.0f, 1.0f);
            const f32 size = simSize ? em.pool.sizeX[i] : glm::mix(em.startSize, em.endSize, t);

            // Density fades in/out with the same envelope as the billboard alpha so
            // blobs appear and dissipate smoothly instead of popping. Deliberately the
            // fade envelope and NOT the simulated colour's alpha: the colour ramp also
            // carries the start/end alpha keys, which the volumetric presets set to 0.
            f32 env = 1.0f;
            if (em.fadeIn > 0.0f && t < em.fadeIn) env *= t / em.fadeIn;
            if (em.fadeOut > 0.0f && t > 1.0f - em.fadeOut) env *= (1.0f - t) / em.fadeOut;
            env = glm::clamp(env, 0.0f, 1.0f);

            rhi::VolumeBlob b;
            b.pos = em.pool.position[i];
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

// ---------------------------------------------------------------------------
// CompatSelfTest
// ---------------------------------------------------------------------------

namespace {

// THE ORACLE. What follows is the pre-module-stack simulation, copied unchanged from
// the revision this file replaced. It is not "reference-quality" code and it is not
// meant to be: its only job is to be exactly what shipped, so that the module-stack
// path can be diffed against it.
//
// DO NOT tidy it, do not share helpers with the production kernels, and do not fix
// the things it gets wrong (the framerate-dependent drag, the divergent turbulence,
// the f32 spawn carry). Sharing code with the thing under test would make the
// comparison vacuous; fixing bugs here would make the comparison fail for the right
// reason at the wrong time.
struct RefParticle {
    glm::vec3 pos{0.0f};
    glm::vec3 vel{0.0f};
    f32 age = 0.0f;
    f32 life = 1.0f;
    f32 rot = 0.0f;
    f32 seed = 0.0f;
};

struct RefState {
    std::vector<RefParticle> pool;
    f32 spawnAccum = 0.0f;
    u32 rngState = 0x1234567u;
    f32 simTime = 0.0f;
    f32 activeAge = 0.0f;
    bool bursted = false;
    bool wasEmitting = false;
};

f32 RefRand01(u32& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return static_cast<f32>(s & 0xFFFFFFu) / static_cast<f32>(0x1000000);
}
f32 RefRandSigned(u32& s) { return RefRand01(s) * 2.0f - 1.0f; }

glm::vec3 RefRandUnit(u32& s) {
    const f32 z = RefRandSigned(s);
    const f32 a = RefRand01(s) * 6.2831853f;
    const f32 r = std::sqrt(glm::max(0.0f, 1.0f - z * z));
    return {r * std::cos(a), r * std::sin(a), z};
}

glm::vec3 RefSampleSpawnLocal(const ParticleEmitter& em, u32& s) {
    switch (em.shape) {
        case ParticleEmitter::Shape::Point:
            return glm::vec3(0.0f);
        case ParticleEmitter::Shape::Hemisphere: {
            glm::vec3 d = RefRandUnit(s);
            d.y = std::abs(d.y);
            return d * (em.emitRadius * RefRand01(s));
        }
        case ParticleEmitter::Shape::Box:
            return {RefRandSigned(s) * em.boxHalfExtents.x, RefRandSigned(s) * em.boxHalfExtents.y,
                    RefRandSigned(s) * em.boxHalfExtents.z};
        case ParticleEmitter::Shape::Disc:
        case ParticleEmitter::Shape::Cone: {
            const f32 a = RefRand01(s) * 6.2831853f;
            const f32 r = em.emitRadius * std::sqrt(RefRand01(s));
            return {r * std::cos(a), 0.0f, r * std::sin(a)};
        }
        case ParticleEmitter::Shape::Sphere:
        default:
            // DELIBERATELY LEFT UNSPLIT, like the velocity statement in spawnOne below.
            // Its operands are unsequenced, so which one draws first is the compiler's
            // choice - and that choice is exactly what VfxLegacy.cpp's Sphere case pins
            // in two explicit statements. Keeping the original one-liner on this side is
            // what makes the comparison a real check of the pin rather than two copies
            // of the same ambiguity agreeing with each other.
            return RefRandUnit(s) * (em.emitRadius * RefRand01(s));
    }
}

glm::vec3 RefSampleDir(const ParticleEmitter& em, u32& s, const glm::vec3& baseDir) {
    if (em.shape == ParticleEmitter::Shape::Cone) {
        const f32 cosMax = std::cos(glm::radians(glm::clamp(em.coneAngle, 0.0f, 179.0f)));
        const f32 cosT = glm::mix(1.0f, cosMax, RefRand01(s));
        const f32 sinT = std::sqrt(glm::max(0.0f, 1.0f - cosT * cosT));
        const f32 phi = RefRand01(s) * 6.2831853f;
        const glm::vec3 up = std::abs(baseDir.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
        const glm::vec3 tx = glm::normalize(glm::cross(up, baseDir));
        const glm::vec3 ty = glm::cross(baseDir, tx);
        return glm::normalize(baseDir * cosT + (tx * std::cos(phi) + ty * std::sin(phi)) * sinT);
    }
    return glm::normalize(glm::mix(baseDir, RefRandUnit(s), glm::clamp(em.spread, 0.0f, 1.0f)) +
                          glm::vec3(1e-5f));
}

void RefUpdate(const ParticleEmitter& em, RefState& S, const glm::mat4& world, f32 dt,
               bool emitting) {
    if (dt <= 0.0f) return;
    S.simTime += dt;
    const glm::vec3 origin(world[3]);
    const glm::mat3 basis(world);
    const bool hasRoll = em.buoyancy != 0.0f || em.vortex != 0.0f;

    for (usize i = 0; i < S.pool.size();) {
        RefParticle& p = S.pool[i];
        p.age += dt;
        if (p.age >= p.life) {
            S.pool[i] = S.pool.back();
            S.pool.pop_back();
            continue;
        }
        p.vel += em.gravity * dt;
        if (em.turbulence > 0.0f) {
            const glm::vec3 q = p.pos * em.turbulenceScale + glm::vec3(S.simTime * 0.5f);
            const glm::vec3 f(std::sin(q.y) - std::cos(q.z), std::sin(q.z) - std::cos(q.x),
                              std::sin(q.x) - std::cos(q.y));
            p.vel += f * (em.turbulence * dt);
        }
        if (hasRoll) {
            const f32 heat = 1.0f - glm::clamp(p.age / glm::max(p.life, 1e-4f), 0.0f, 1.0f);
            if (em.buoyancy != 0.0f) p.vel.y += em.buoyancy * heat * dt;
            if (em.vortex != 0.0f) {
                const glm::vec3 rel = p.pos - origin;
                const glm::vec2 rxz(rel.x, rel.z);
                const f32 rlen = glm::length(rxz);
                if (rlen > 1e-3f) {
                    const glm::vec2 out = rxz / rlen;
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

    const bool active = emitting && em.emitting;
    if (active && !S.wasEmitting) {
        S.activeAge = 0.0f;
        S.bursted = false;
    }
    S.wasEmitting = active;
    if (!active) return;
    S.activeAge += dt;

    const glm::vec3 baseDir = glm::dot(em.direction, em.direction) > 1e-6f
                                  ? glm::normalize(basis * em.direction)
                                  : glm::vec3(0.0f, 1.0f, 0.0f);

    const auto spawnOne = [&]() {
        RefParticle p;
        p.pos = origin + basis * RefSampleSpawnLocal(em, S.rngState);
        // The statement whose operand order K_LegacyInitState pins. Left exactly as
        // it was written so the pin is checked against the compiler, not assumed.
        p.vel = RefSampleDir(em, S.rngState, baseDir) *
                (em.startSpeed * (1.0f + RefRandSigned(S.rngState) * em.speedVariance));
        p.life = glm::max(0.05f,
                          em.lifetime * (1.0f + RefRandSigned(S.rngState) * em.lifetimeVariance));
        p.age = 0.0f;
        p.rot = RefRand01(S.rngState) * 6.2831853f;
        p.seed = RefRand01(S.rngState);
        S.pool.push_back(p);
    };

    if (!S.bursted && em.burst > 0) {
        S.bursted = true;
        for (u32 k = 0; k < em.burst && S.pool.size() < em.maxParticles; ++k) spawnOne();
    }
    const bool windowOpen = em.loop || S.activeAge <= em.duration;
    if (windowOpen && em.rate > 0.0f) {
        S.spawnAccum += em.rate * dt;
        int toSpawn = static_cast<int>(S.spawnAccum);
        S.spawnAccum -= static_cast<f32>(toSpawn);
        for (; toSpawn > 0 && S.pool.size() < em.maxParticles; --toSpawn) spawnOne();
    }
}

// --- comparison harness ----------------------------------------------------

// Bit equality, not ==: a particle sim can legitimately reach inf/NaN (normalize of a
// degenerate direction, drag*dt > 1 on a hitch), and == would silently call two NaNs
// "different" and hide a real match, or call +0/-0 "same" and hide a real difference.
bool BitEq(f32 a, f32 b) {
    u32 ua = 0, ub = 0;
    std::memcpy(&ua, &a, sizeof(ua));
    std::memcpy(&ub, &b, sizeof(ub));
    return ua == ub;
}
bool BitEq(const glm::vec3& a, const glm::vec3& b) {
    return BitEq(a.x, b.x) && BitEq(a.y, b.y) && BitEq(a.z, b.z);
}

// The dt sequence deliberately is NOT fixed: it mixes 60/144/30 Hz with a 50 ms
// hitch, because several of the behaviours being preserved (the linear drag, the f32
// spawn carry, the age-then-retire order) only differ from their modern replacements
// when dt varies.
f32 TestDt(u32 frame) {
    static const f32 kDts[] = {1.0f / 60.0f, 1.0f / 60.0f, 1.0f / 144.0f, 0.05f, 1.0f / 30.0f};
    return kDts[frame % 5];
}

// A non-trivial emitter transform: translation, a rotation about all three axes, and
// a non-uniform scale, so `basis` is neither identity nor orthonormal. An identity
// world matrix would hide any mistake in how the shape sampler is transformed.
glm::mat4 TestWorld() {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(3.5f, 1.25f, -2.0f));
    m = glm::rotate(m, 0.7f, glm::normalize(glm::vec3(0.3f, 1.0f, 0.2f)));
    m = glm::scale(m, glm::vec3(1.3f, 0.8f, 1.1f));
    return m;
}

// Runs both paths over `frames` and returns the frame index of the first divergence,
// or -1 when they stayed bit-identical the whole way. `emitting` is toggled off for a
// stretch in the middle so the burst/duration restart edge is exercised too.
i32 FirstDivergence(const ParticleEmitter& authored, u32 frames, std::string& detail) {
    ParticleEmitter live = authored;
    RefState ref;
    const glm::mat4 world = TestWorld();

    for (u32 f = 0; f < frames; ++f) {
        const f32 dt = TestDt(f);
        const bool emitting = !(f >= frames / 3 && f < frames / 3 + 12);
        RefUpdate(authored, ref, world, dt, emitting);
        StepEmitter(live, world, dt, emitting);

        if (live.pool.count != static_cast<u32>(ref.pool.size())) {
            detail = "live " + std::to_string(live.pool.count) + " particles vs oracle " +
                     std::to_string(ref.pool.size());
            return static_cast<i32>(f);
        }
        for (u32 i = 0; i < live.pool.count; ++i) {
            const RefParticle& r = ref.pool[i];
            // Name the FIELD, not just "they differ": the fields fail for very
            // different reasons (velocity -> the spawn draw order, age -> the retire
            // ordering, rotation -> the spin term), and a bit-level diff that only
            // prints six decimal places says nothing at all.
            const char* field = nullptr;
            if (!BitEq(live.pool.position[i], r.pos)) field = "position";
            else if (!BitEq(live.pool.velocity[i], r.vel)) field = "velocity";
            else if (!BitEq(live.pool.age[i], r.age)) field = "age";
            else if (!BitEq(live.pool.lifetime[i], r.life)) field = "lifetime";
            else if (!BitEq(live.pool.rotation[i], r.rot)) field = "rotation";
            if (!field) continue;
            const glm::vec3 lv = live.pool.velocity[i];
            detail = std::string(field) + " differs on particle " + std::to_string(i) +
                     " - live vel(" + std::to_string(lv.x) + "," + std::to_string(lv.y) + "," +
                     std::to_string(lv.z) + ") vs oracle (" + std::to_string(r.vel.x) + "," +
                     std::to_string(r.vel.y) + "," + std::to_string(r.vel.z) + "), life " +
                     std::to_string(live.pool.lifetime[i]) + " vs " + std::to_string(r.life);
            return static_cast<i32>(f);
        }
    }
    return -1;
}

// Deterministic parameter fuzz. Sweeps every emit shape and render mode and stirs in
// the awkward combinations: zero-radius shapes, spread 0 and 1, drag high enough that
// drag*dt > 1 on the 50 ms frame, bursts with rate 0, non-looping windows that expire
// mid-run, and the buoyancy/vortex roll.
ParticleEmitter FuzzEmitter(u32 i) {
    u32 s = vfx::HashSeed(0xF0FFEEu, i);
    const auto next = [&s]() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return static_cast<f32>(s & 0xFFFFFFu) / static_cast<f32>(0x1000000);
    };
    ParticleEmitter e;
    e.shape = static_cast<ParticleEmitter::Shape>(i % 6);
    e.render = static_cast<ParticleEmitter::Render>((i / 6) % 3);
    e.maxParticles = 64u + static_cast<u32>(next() * 400.0f);
    e.rate = (i % 7 == 0) ? 0.0f : next() * 300.0f;
    e.burst = (i % 3 == 0) ? static_cast<u32>(next() * 60.0f) : 0u;
    e.loop = (i % 4) != 0;
    e.duration = 0.2f + next() * 1.5f;
    e.lifetime = 0.1f + next() * 3.0f;
    e.lifetimeVariance = next();
    e.emitRadius = (i % 5 == 0) ? 0.0f : next() * 2.0f;
    e.boxHalfExtents = glm::vec3(next() * 3.0f, next() * 0.5f, next() * 3.0f);
    e.coneAngle = next() * 90.0f;
    e.direction = glm::vec3(next() * 2.0f - 1.0f, next() * 2.0f - 1.0f, next() * 2.0f - 1.0f);
    if (i % 11 == 0) e.direction = glm::vec3(0.0f); // exercises the degenerate fallback
    e.startSpeed = next() * 12.0f;
    e.speedVariance = next();
    e.spread = (i % 9 == 0) ? 0.0f : ((i % 9 == 1) ? 1.0f : next());
    e.gravity = glm::vec3(next() * 2.0f - 1.0f, -next() * 10.0f, next() * 2.0f - 1.0f);
    // 7.5 * 0.05 > 1 on the 50 ms hitch frame -> the clamp-to-zero branch of the legacy
    // linear drag (max(0, 1 - drag*dt)), which is the one regime where the linear and
    // exponential forms are furthest apart. It does NOT flip the sign - the clamp is
    // there precisely to stop that - so this covers saturation, not inversion.
    e.drag = (i % 6 == 0) ? 7.5f : next() * 3.0f;
    e.turbulence = (i % 2 == 0) ? next() * 3.0f : 0.0f;
    e.turbulenceScale = 0.2f + next() * 2.0f;
    e.buoyancy = (i % 5 == 1) ? next() * 15.0f : 0.0f;
    e.vortex = (i % 5 == 1) ? next() * 6.0f : 0.0f;
    e.spin = next() * 4.0f - 2.0f;
    e.startSize = next();
    e.endSize = next() * 2.0f;
    e.fadeIn = next() * 0.4f;
    e.fadeOut = next() * 0.5f;
    e.stretch = 1.0f + next() * 8.0f;
    return e;
}

// Simulates `frames` and hashes the resulting pool, so "did this flag change
// anything?" is one comparable number.
u64 RunAndHash(const ParticleEmitter& authored, u32 frames) {
    ParticleEmitter live = authored;
    const glm::mat4 world = TestWorld();
    for (u32 f = 0; f < frames; ++f) StepEmitter(live, world, TestDt(f), true);
    return vfx::HashPool(live.pool);
}

} // namespace

bool CompatSelfTest() {
    bool pass = true;
    constexpr u32 kFrames = 360;

    // ---- (a) every built-in template is bit-identical ----------------------
    {
        u32 checked = 0, totalParticles = 0;
        i32 firstBad = -1;
        std::string badName, detail;
        for (u32 t = 0; t < static_cast<u32>(Template::Count); ++t) {
            const ParticleEmitter e = MakeTemplate(static_cast<Template>(t));
            std::string d;
            const i32 f = FirstDivergence(e, kFrames, d);
            if (f >= 0 && firstBad < 0) {
                firstBad = f;
                badName = TemplateName(static_cast<Template>(t));
                detail = d;
            }
            ++checked;
            ParticleEmitter live = e; // report a live count so an empty pass is visible
            const glm::mat4 w = TestWorld();
            for (u32 i = 0; i < 120; ++i) StepEmitter(live, w, TestDt(i), true);
            totalParticles += live.pool.count;
        }
        if (firstBad < 0 && totalParticles > 0) {
            HBE_INFO("[particle] (a) templates PASS - all {} presets bit-identical to the "
                     "pre-stack loop over {} frames of mixed dt ({} live particles across "
                     "them).",
                     checked, kFrames, totalParticles);
        } else {
            HBE_ERROR("[particle] (a) templates FAIL - '{}' diverged at frame {} ({}); "
                      "totalParticles={}.",
                      badName, firstBad, detail, totalParticles);
            pass = false;
        }
    }

    // ---- (b) parameter fuzz -----------------------------------------------
    {
        constexpr u32 kCases = 66; // 6 shapes x 3 render modes x ~4 passes
        i32 firstBad = -1;
        u32 badCase = 0;
        std::string detail;
        u32 live = 0;
        for (u32 i = 0; i < kCases; ++i) {
            const ParticleEmitter e = FuzzEmitter(i);
            std::string d;
            const i32 f = FirstDivergence(e, 200, d);
            if (f >= 0 && firstBad < 0) {
                firstBad = f;
                badCase = i;
                detail = d;
            }
            ParticleEmitter probe = e;
            const glm::mat4 w = TestWorld();
            for (u32 k = 0; k < 60; ++k) StepEmitter(probe, w, TestDt(k), true);
            live += probe.pool.count;
        }
        if (firstBad < 0 && live > 0) {
            HBE_INFO("[particle] (b) fuzz PASS - {} randomised emitters (all 6 shapes, all 3 "
                     "render modes, bursts, expiring windows, drag*dt>1, vortex roll) "
                     "bit-identical; {} particles simulated in the probe pass.",
                     kCases, live);
        } else {
            HBE_ERROR("[particle] (b) fuzz FAIL - case {} diverged at frame {} ({}); live={}.",
                      badCase, firstBad, detail, live);
            pass = false;
        }
    }

    // ---- (c) the new modules are opt-in, and they actually do something ----
    //
    // Half of this test is the point of the whole task: with every flag off, nothing
    // changed. The other half guards against the failure mode where a compatibility
    // test passes because the new code is not wired up at all - so each flag must
    // move the hash.
    {
        const ParticleEmitter base = MakeTemplate(Template::Smoke);
        const u64 h0 = RunAndHash(base, 180);

        ParticleEmitter curl = base;
        curl.useCurlNoise = true;
        curl.curlStrength = 2.0f;
        ParticleEmitter drag = base;
        drag.expDrag = true;
        ParticleEmitter col = base;
        col.simulateColor = true;
        col.colorVariance = 0.5f;
        ParticleEmitter siz = base;
        siz.simulateSize = true;
        siz.sizeVariance = 0.5f;

        const u64 hCurl = RunAndHash(curl, 180);
        const u64 hDrag = RunAndHash(drag, 180);
        const u64 hCol = RunAndHash(col, 180);
        const u64 hSiz = RunAndHash(siz, 180);

        // Colour/size only add STREAMS, they do not move a particle, so their pool
        // hash must differ (new streams are hashed) while position stays identical.
        ParticleEmitter colLive = col;
        ParticleEmitter baseLive = base;
        const glm::mat4 w = TestWorld();
        for (u32 f = 0; f < 180; ++f) {
            StepEmitter(colLive, w, TestDt(f), true);
            StepEmitter(baseLive, w, TestDt(f), true);
        }
        bool positionsMatch = colLive.pool.count == baseLive.pool.count;
        for (u32 i = 0; positionsMatch && i < colLive.pool.count; ++i)
            positionsMatch = BitEq(colLive.pool.position[i], baseLive.pool.position[i]);

        // And with variance 0 the simulated colour must equal the render-time ramp it
        // replaces, or "opt in and it looks the same" would be a lie.
        ParticleEmitter zeroVar = base;
        zeroVar.simulateColor = true;
        zeroVar.simulateSize = true;
        ParticleEmitter zv = zeroVar;
        for (u32 f = 0; f < 180; ++f) StepEmitter(zv, w, TestDt(f), true);
        bool rampMatch = zv.pool.count > 0;
        for (u32 i = 0; i < zv.pool.count; ++i) {
            const f32 t =
                glm::clamp(zv.pool.age[i] / glm::max(zv.pool.lifetime[i], 1e-4f), 0.0f, 1.0f);
            glm::vec4 want = glm::mix(zv.startColor, zv.endColor, t);
            f32 env = 1.0f;
            if (zv.fadeIn > 0.0f && t < zv.fadeIn) env *= t / zv.fadeIn;
            if (zv.fadeOut > 0.0f && t > 1.0f - zv.fadeOut) env *= (1.0f - t) / zv.fadeOut;
            want.a *= glm::clamp(env, 0.0f, 1.0f);
            const glm::vec4 got = zv.pool.color[i];
            if (!(BitEq(got.x, want.x) && BitEq(got.y, want.y) && BitEq(got.z, want.z) &&
                  BitEq(got.w, want.w)) ||
                !BitEq(zv.pool.sizeX[i], glm::mix(zv.startSize, zv.endSize, t))) {
                rampMatch = false;
                break;
            }
        }

        const bool allDiffer = hCurl != h0 && hDrag != h0 && hCol != h0 && hSiz != h0;
        if (allDiffer && positionsMatch && rampMatch) {
            HBE_INFO("[particle] (c) opt-in PASS - base {:#x}; curl {:#x}, expDrag {:#x}, "
                     "simColor {:#x}, simSize {:#x} all differ. Colour/size leave every "
                     "position bit-identical, and at variance 0 they reproduce the "
                     "render-time ramp exactly.",
                     h0, hCurl, hDrag, hCol, hSiz);
        } else {
            HBE_ERROR("[particle] (c) opt-in FAIL - allDiffer={} positionsMatch={} "
                      "rampMatch={} (h0={:#x} curl={:#x} drag={:#x} col={:#x} siz={:#x}).",
                      allDiffer, positionsMatch, rampMatch, h0, hCurl, hDrag, hCol, hSiz);
            pass = false;
        }
    }

    // ---- (d) allocation: lazy commit, geometric growth, quiet steady state ---
    //
    // The property is NOT "one allocation, ever". `maxParticles` is a CEILING an artist
    // sets (the editor allows 200 000, far above what the 6 MB particle vertex buffer
    // can draw), so committing it up front would have an emitter authored at the
    // ceiling touch ~8 MB of pages on scene load for a working set of a few kilobytes -
    // a regression against the pre-stack pool, which push_back-grew and tracked the
    // LIVE count. What must hold is what the old system could not deliver: the
    // commitment tracks demand, growth is geometric rather than per-spawn, and steady
    // state is silent.
    {
        ParticleEmitter e = MakeTemplate(Template::Rain); // 2000 max, rate 400
        const glm::mat4 w = TestWorld();
        usize bytes = 0;
        u32 growths = 0, peakLive = 0;
        for (u32 f = 0; f < 400; ++f) {
            StepEmitter(e, w, TestDt(f), true);
            const usize b = e.pool.BytesAllocated();
            if (b != bytes) {
                ++growths;
                bytes = b;
            }
            peakLive = glm::max(peakLive, e.pool.count);
        }
        // Steady state must then be completely silent - not "few allocations", none.
        const usize settled = bytes;
        u32 tailGrowths = 0;
        for (u32 f = 400; f < 700; ++f) {
            StepEmitter(e, w, TestDt(f), true);
            if (e.pool.BytesAllocated() != settled) ++tailGrowths;
        }
        // Bytes per COMMITTED SLOT for the legacy stack: position+velocity (24) plus
        // age, lifetime, rotation, seed and flags (20). The old pool held 40 B/particle
        // for the same six values plus a `seed` nothing read; the extra 4 is the Flags
        // stream that Legacy.InitState declares and stamps.
        const usize perSlot = settled / glm::max(1u, e.pool.capacity);
        // Growth from kInitialCommit to the ceiling is log2-bounded (1024 -> 2000 here
        // is a single step), so anything past a handful means it is growing per spawn.
        const bool boundedGrowth = growths <= 6;
        const bool commitFitsDemand = e.pool.capacity >= peakLive;

        // And the lazy commit itself: an absurd authored ceiling must cost nothing
        // until the particles actually exist. This is the case the old push_back pool
        // handled for free and a naive full commit would regress.
        ParticleEmitter big = MakeTemplate(Template::Fire);
        big.maxParticles = 200000; // the editor's ceiling
        for (u32 f = 0; f < 200; ++f) StepEmitter(big, w, TestDt(f), true);
        const usize bigBytes = big.pool.BytesAllocated();
        const usize ceilingBytes = static_cast<usize>(big.maxParticles) * perSlot;
        const bool lazy = big.pool.capacity < big.maxParticles && bigBytes * 16 < ceilingBytes;

        if (boundedGrowth && tailGrowths == 0 && commitFitsDemand && perSlot == 44 &&
            peakLive > 0 && lazy) {
            HBE_INFO("[particle] (d) allocation PASS - {} B committed ({} slots x {} B) for a "
                     "peak of {} live after {} growth step(s); zero reallocation over the "
                     "next 300 frames. A 200000-max emitter holding {} particles commits {} B, "
                     "not the {} B its ceiling would.",
                     settled, e.pool.capacity, perSlot, peakLive, growths, big.pool.count,
                     bigBytes, ceilingBytes);
        } else {
            HBE_ERROR("[particle] (d) allocation FAIL - settled={} capacity={} perSlot={} "
                      "peakLive={} growths={} tailGrowths={} lazy={} bigBytes={} bigCap={}.",
                      settled, e.pool.capacity, perSlot, peakLive, growths, tailGrowths, lazy,
                      bigBytes, big.pool.capacity);
            pass = false;
        }
    }

    if (pass)
        HBE_INFO("[particle] CompatSelfTest PASS - the module stack reproduces the pre-stack "
                 "simulation bit-for-bit.");
    else HBE_ERROR("[particle] CompatSelfTest FAIL.");
    return pass;
}

} // namespace hbe::particle
