// Vfx/VfxLegacy.cpp
#include "Vfx/VfxLegacy.h"

#include <cmath>

namespace hbe::vfx {

namespace {

// The pre-stack PRNG, moved here unchanged. Note it is NOT vfx::Rng: it returns
// (s & 0xFFFFFF) / 0x1000000 from a xorshift13/17/5 stream and the two differ in the
// low bits, so substituting one for the other would change every authored effect.
f32 Rand01(u32& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return static_cast<f32>(s & 0xFFFFFFu) / static_cast<f32>(0x1000000);
}
f32 RandSigned(u32& s) { return Rand01(s) * 2.0f - 1.0f; }

glm::vec3 RandUnit(u32& s) {
    const f32 z = RandSigned(s);
    const f32 a = Rand01(s) * 6.2831853f;
    const f32 r = std::sqrt(glm::max(0.0f, 1.0f - z * z));
    return {r * std::cos(a), r * std::sin(a), z};
}

// Spawn offset in the emitter's LOCAL space (the caller applies origin + basis).
// The per-shape DRAW COUNT is part of the compatibility contract: Point takes 0
// draws, Box takes 3, Disc/Cone take 2, Sphere/Hemisphere take 3. Changing any of
// them shifts every subsequent particle's stream.
glm::vec3 SampleSpawnLocal(const LegacyParams& L, u32& s) {
    switch (L.shape) {
        case LegacyShape::Point:
            return glm::vec3(0.0f);
        case LegacyShape::Hemisphere: {
            glm::vec3 d = RandUnit(s);
            d.y = std::abs(d.y);
            return d * (L.emitRadius * Rand01(s));
        }
        case LegacyShape::Box:
            return {RandSigned(s) * L.boxHalfExtents.x, RandSigned(s) * L.boxHalfExtents.y,
                    RandSigned(s) * L.boxHalfExtents.z};
        case LegacyShape::Disc:
        case LegacyShape::Cone: { // both spawn on a base disc (XZ)
            const f32 a = Rand01(s) * 6.2831853f;
            const f32 r = L.emitRadius * std::sqrt(Rand01(s));
            return {r * std::cos(a), 0.0f, r * std::sin(a)};
        }
        case LegacyShape::Sphere:
        default: {
            // SPLIT, AND THE ORDER IS THE PIN - see K_LegacyInitState's note. The
            // original read `RandUnit(s) * (emitRadius * Rand01(s))`, whose operands
            // are UNSEQUENCED, so the draw order was the compiler's choice and not the
            // source's. Splitting it into two statements here (radius scalar first,
            // which is what MSVC picks for glm's templated operator*) makes the order a
            // property of the engine. The ORACLE in Scene/ParticleSystem.cpp keeps the
            // original one-liner deliberately unsplit, so --test-vfxcompat checks this
            // pin against the compiler on every run instead of agreeing with itself.
            // Sphere is the component DEFAULT shape - getting this wrong silently moves
            // every authored sphere emitter in every shipped scene.
            const f32 len = L.emitRadius * Rand01(s); // FIRST
            const glm::vec3 dir = RandUnit(s);        // then
            return dir * len;
        }
    }
}

// Initial velocity direction (world space).
glm::vec3 SampleDir(const LegacyParams& L, u32& s, const glm::vec3& baseDir) {
    if (L.shape == LegacyShape::Cone) {
        const f32 cosMax = std::cos(glm::radians(glm::clamp(L.coneAngle, 0.0f, 179.0f)));
        const f32 cosT = glm::mix(1.0f, cosMax, Rand01(s));
        const f32 sinT = std::sqrt(glm::max(0.0f, 1.0f - cosT * cosT));
        const f32 phi = Rand01(s) * 6.2831853f;
        const glm::vec3 up = std::abs(baseDir.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
        const glm::vec3 tx = glm::normalize(glm::cross(up, baseDir));
        const glm::vec3 ty = glm::cross(baseDir, tx);
        return glm::normalize(baseDir * cosT + (tx * std::cos(phi) + ty * std::sin(phi)) * sinT);
    }
    return glm::normalize(glm::mix(baseDir, RandUnit(s), glm::clamp(L.spread, 0.0f, 1.0f)) +
                          glm::vec3(1e-5f));
}

} // namespace

// Legacy.InitState - the old spawnOne(), one particle per slot.
//
// PINNED EVALUATION ORDER - and this is not a formality, it was a live landmine.
// The statement being reproduced was
//     p.vel = SampleDir(em, baseDir) * (startSpeed * (1 + RandSigned(rng) * speedVar));
// Both operands of `*` draw from the same sequential emitter stream, and in C++ the
// operands of `*` are UNSEQUENCED - so which one drew first was never specified by
// the source at all; it was whatever the compiler picked. MSVC picks the RIGHT-hand
// operand, so the SPEED multiplier consumes the stream before the direction does, and
// that is what is pinned below. Writing it the way it reads (direction first) produces
// a completely different, equally "correct-looking" emitter - CompatSelfTest caught
// exactly that on the first run, with matching positions and lifetimes and wrong
// velocities. The oracle in Scene/ParticleSystem.cpp keeps the original unsequenced
// statement verbatim, so this pin is CHECKED against the compiler rather than assumed,
// and from here on the draw order is a property of the engine, not of the build.
void K_LegacyInitState(ParticleView& p, const ModuleParams& /*k*/, u32 begin, u32 end,
                       f32 /*dt*/) {
    if (!p.position || !p.velocity || !p.age || !p.lifetime || !p.rotation) return;
    if (!p.emitterRng || !p.user) return;
    const LegacyParams& L = *static_cast<const LegacyParams*>(p.user);
    u32& s = *p.emitterRng;

    for (u32 i = begin; i < end; ++i) {
        p.position[i] = L.origin + L.basis * SampleSpawnLocal(L, s);
        const f32 speed = L.startSpeed * (1.0f + RandSigned(s) * L.speedVariance); // FIRST
        const glm::vec3 dir = SampleDir(L, s, L.baseDir);                          // then
        p.velocity[i] = dir * speed;
        p.lifetime[i] = glm::max(0.05f, L.lifetime * (1.0f + RandSigned(s) * L.lifetimeVariance));
        // NO sub-frame birth offset: the old system stamped age 0 and the exact
        // spawn position. Spawn.InitState (the modern module) advances new particles
        // by the remainder of the frame, which is better and is exactly why it cannot
        // be used here.
        p.age[i] = 0.0f;
        p.rotation[i] = Rand01(s) * 6.2831853f;
        // The old Particle carried an f32 `seed` that nothing ever read. The DRAW
        // still has to happen - dropping it would shift every later particle's stream
        // - so it is made useful instead: the post-draw state becomes the particle's
        // u32 seed, which is what the variance-capable modules hash.
        Rand01(s);
        if (p.seed) p.seed[i] = s;
        if (p.flags) p.flags[i] = kFlagAlive;
    }
}

// Legacy.Forces - age, then every velocity term the old loop applied, in order.
//
// Age is incremented HERE rather than in the integrator because the buoyancy/vortex
// roll reads it: the old loop did `age += dt` first and computed `heat` from the
// already-advanced age. Splitting age out into Legacy.Integrate would shift the whole
// roll by one frame.
//
// One deliberate difference with no observable effect: the old loop retired a
// particle the instant its age crossed its lifetime and therefore never applied
// forces to it. Here forces run over the whole [0, oldCount) range and retirement
// happens afterwards in RunFrame. Every particle that SURVIVES is treated
// identically, and the swap-pop retirement visits the same indices in the same order,
// so both the surviving set and its array order are unchanged - only work done on
// particles that are about to be discarded differs.
void K_LegacyForces(ParticleView& p, const ModuleParams& k, u32 begin, u32 end, f32 dt) {
    if (!p.position || !p.velocity || !p.age || !p.lifetime || !p.user) return;
    const LegacyParams& L = *static_cast<const LegacyParams*>(p.user);
    const bool hasRoll = L.buoyancy != 0.0f || L.vortex != 0.0f;

    for (u32 i = begin; i < end; ++i) {
        p.age[i] += dt;
        p.velocity[i] += L.gravity * dt;

        if (L.turbulence > 0.0f) { // swirly divergence-light noise force
            const glm::vec3 q = p.position[i] * L.turbulenceScale + glm::vec3(k.time * 0.5f);
            const glm::vec3 f(std::sin(q.y) - std::cos(q.z), std::sin(q.z) - std::cos(q.x),
                              std::sin(q.x) - std::cos(q.y));
            p.velocity[i] += f * (L.turbulence * dt);
        }
        if (hasRoll) {
            // Heat = 1 at birth, 0 at death. Buoyancy lifts hot gas; the vortex ring
            // rolls the front outward and over (the mushroom cap).
            const f32 heat =
                1.0f - glm::clamp(p.age[i] / glm::max(p.lifetime[i], 1e-4f), 0.0f, 1.0f);
            if (L.buoyancy != 0.0f) p.velocity[i].y += L.buoyancy * heat * dt;
            if (L.vortex != 0.0f) {
                const glm::vec3 rel = p.position[i] - L.origin;
                const glm::vec2 rxz(rel.x, rel.z);
                const f32 rlen = glm::length(rxz);
                if (rlen > 1e-3f) {
                    const glm::vec2 out = rxz / rlen;
                    const f32 up = glm::max(0.0f, rel.y);
                    p.velocity[i].x += out.x * L.vortex * (0.5f + up * 0.35f) * dt;
                    p.velocity[i].z += out.y * L.vortex * (0.5f + up * 0.35f) * dt;
                    p.velocity[i].y -= L.vortex * rlen * 0.35f * dt;
                }
            }
        }
    }
}

// Legacy.DragLinear - `vel *= max(0, 1 - drag*dt)`, kept WRONG on purpose.
//
// Be precise about WHICH way it is wrong, because it is easy to misremember: the
// shipped form is CLAMPED, so it cannot flip the velocity sign. Once drag*dt >= 1 -
// which the editor's 0..8 drag range reaches on a 130 ms hitch - the particle is
// brought to a dead stop in a single frame instead of overshooting through zero. The
// defect is that the damping is framerate-DEPENDENT (the terminal look changes with
// dt) and saturates: 8 small steps and 1 big step of the same total duration settle
// differently, so an effect looks different at 30, 60 and 144 Hz. Update.Drag -
// exp(-drag*dt), the exact solution of dv/dt = -drag*v - is dt-invariant and never
// saturates. But the fix changes how every existing emitter settles, so it is opt-in
// per emitter ("Exponential drag") and this is what an untouched emitter keeps using.
void K_LegacyDragLinear(ParticleView& p, const ModuleParams& /*k*/, u32 begin, u32 end, f32 dt) {
    if (!p.velocity || !p.user) return;
    const LegacyParams& L = *static_cast<const LegacyParams*>(p.user);
    if (L.drag <= 0.0f) return; // the old loop skipped the multiply entirely
    const f32 s = glm::max(0.0f, 1.0f - L.drag * dt);
    for (u32 i = begin; i < end; ++i) p.velocity[i] *= s;
}

// Legacy.Integrate - explicit Euler on position, plus the emitter-uniform spin.
// Age is NOT advanced here; Legacy.Forces already did it (see the note above).
void K_LegacyIntegrate(ParticleView& p, const ModuleParams& /*k*/, u32 begin, u32 end, f32 dt) {
    if (!p.position || !p.velocity || !p.rotation || !p.user) return;
    const LegacyParams& L = *static_cast<const LegacyParams*>(p.user);
    const f32 dRot = L.spin * dt;
    for (u32 i = begin; i < end; ++i) {
        p.position[i] += p.velocity[i] * dt;
        p.rotation[i] += dRot;
    }
}

} // namespace hbe::vfx
