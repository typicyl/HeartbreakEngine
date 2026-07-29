// Vfx/VfxStack.cpp
#include "Vfx/VfxStack.h"

#include "Core/Log.h"
#include "Vfx/VfxLegacy.h"

#include <cmath>
#include <cstddef>
#include <format>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace hbe::vfx {

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

const char* AttrName(Attr a) {
    switch (a) {
        case Attr::Position: return "Position";
        case Attr::Velocity: return "Velocity";
        case Attr::Age: return "Age";
        case Attr::Lifetime: return "Lifetime";
        case Attr::Color: return "Color";
        case Attr::Size: return "Size";
        case Attr::Rotation: return "Rotation";
        case Attr::RotationRate: return "RotationRate";
        case Attr::SubImage: return "SubImage";
        case Attr::Seed: return "Seed";
        case Attr::Flags: return "Flags";
        case Attr::Count: break;
    }
    return "<invalid>";
}

void AppendMaskNames(AttributeMask mask, std::string& out) {
    bool first = true;
    for (u32 i = 0; i < static_cast<u32>(Attr::Count); ++i) {
        if ((mask & (1u << i)) == 0u) continue;
        if (!first) out += '|';
        out += AttrName(static_cast<Attr>(i));
        first = false;
    }
    if (first) out += "<none>";
}

const char* StageName(ModuleStage s) {
    switch (s) {
        case ModuleStage::EmitterSpawn: return "EmitterSpawn";
        case ModuleStage::EmitterUpdate: return "EmitterUpdate";
        case ModuleStage::ParticleSpawn: return "ParticleSpawn";
        case ModuleStage::ParticleUpdate: return "ParticleUpdate";
        case ModuleStage::ParticleCollide: return "ParticleCollide";
        case ModuleStage::Count: break;
    }
    return "<invalid>";
}

// ---------------------------------------------------------------------------
// Kernels
// ---------------------------------------------------------------------------
//
// Every kernel is a pure pass over [begin, end) of one emitter's SoA streams. No
// kernel reads a global, a wall clock, or the scene - the only per-frame state it
// can see is what the runner stamped into ModuleParams (time / seed / spawnBase).
// That restriction is the whole determinism story: reproduce the dt sequence and
// the emitter seed and you reproduce the simulation exactly.
namespace {

// Per-module RNG salts. Without these, two spawn modules initialising particle i
// would draw from the SAME stream position and their outputs would correlate (a
// particle that spawned fast would always spawn large). Salting decorrelates the
// streams while keeping every draw a pure function of (seed, spawnIndex).
constexpr u32 kSaltState = 0x5BD1E995u;
constexpr u32 kSaltColor = 0x27D4EB2Fu;
constexpr u32 kSaltSize = 0x165667B1u;
constexpr u32 kSaltRotation = 0x9E3779B1u;

inline Rng SpawnRng(const ModuleParams& k, u32 local, u32 salt) {
    return Rng{HashSeed(k.seed ^ salt, k.spawnBase + local)};
}

// --- ParticleSpawn ---------------------------------------------------------

// SpawnInitState: the mandatory base initialiser.
//   f[0..2] origin (world)   f[3] emitRadius
//   f[4..6] direction        f[7] speed        f[8] speedVariance
//   f[9] lifetime            f[10] lifetimeVariance                f[11] spread
//
// SUB-FRAME BIRTH: particle `local` of `n` is born at fraction (local+0.5)/n through
// the frame, so it is advanced by the REMAINDER of the frame here. Without this,
// every particle spawned in a frame appears at exactly one point and a moving
// emitter lays down visible "beads on a string". It is also what lets the update
// stage safely run over [0, oldCount) only.
void K_SpawnInitState(ParticleView& p, const ModuleParams& k, u32 begin, u32 end, f32 dt) {
    if (!p.position || !p.velocity || !p.age || !p.lifetime || end <= begin) return;
    const u32 n = end - begin;
    const glm::vec3 origin(k.f[0], k.f[1], k.f[2]);
    const f32 radius = k.f[3];
    glm::vec3 dir(k.f[4], k.f[5], k.f[6]);
    dir = (glm::dot(dir, dir) > 1e-12f) ? glm::normalize(dir) : glm::vec3(0.0f, 1.0f, 0.0f);
    const f32 speed = k.f[7];
    const f32 speedVar = k.f[8];
    const f32 life = k.f[9];
    const f32 lifeVar = k.f[10];
    const f32 spread = glm::clamp(k.f[11], 0.0f, 1.0f);
    const f32 invN = 1.0f / static_cast<f32>(n);

    for (u32 i = begin; i < end; ++i) {
        const u32 local = i - begin;
        Rng rng = SpawnRng(k, local, kSaltState);

        const f32 spawnT = (static_cast<f32>(local) + 0.5f) * invN;
        const f32 subDt = dt * (1.0f - spawnT);

        // SPLIT ON PURPOSE - do not fold these back into one expression. Both
        // `rng.NextUnit()` and `radius * rng.Next01()` advance the SAME rng, and the
        // operands of `*` are UNSEQUENCED even when the operator is overloaded
        // (C++17 [over.match.oper]/2 gives an overloaded operator the built-in
        // operator's sequencing). Written as one statement the draw ORDER is chosen
        // by the compiler, so this kernel's determinism would be a property of the
        // build rather than of the source - the exact landmine K_LegacyInitState
        // documents. Two statements make the order explicit and portable.
        const glm::vec3 offsetDir = rng.NextUnit();
        const f32 offsetLen = radius * rng.Next01();
        const glm::vec3 offset = offsetDir * offsetLen;
        const glm::vec3 spreadDir = rng.NextUnit();
        glm::vec3 v = glm::mix(dir, spreadDir, spread);
        const f32 vlen = glm::length(v);
        v = (vlen > 1e-5f) ? (v / vlen) : dir;
        v *= speed * (1.0f + rng.NextSigned() * speedVar);

        p.position[i] = origin + offset + v * subDt;
        p.velocity[i] = v;
        p.age[i] = subDt;
        p.lifetime[i] = glm::max(0.05f, life * (1.0f + rng.NextSigned() * lifeVar));
        if (p.seed) p.seed[i] = rng.state;
        if (p.flags) p.flags[i] = kFlagAlive;
    }
}

// SpawnInitColor: f[0..3] rgba, f[4] per-particle brightness variance.
//
// The variance is hashed from the particle's STORED seed, not drawn from the spawn
// stream, so Update.ColorOverLife can reproduce the identical scale on every later
// frame. A spawn-order draw could not be reproduced, and the over-life module - which
// rewrites Color from the ramp every frame - would erase the variation one frame
// after birth. Same reasoning for size.
void K_SpawnInitColor(ParticleView& p, const ModuleParams& k, u32 begin, u32 end, f32 /*dt*/) {
    if (!p.color) return;
    const glm::vec4 base(k.f[0], k.f[1], k.f[2], k.f[3]);
    const f32 variance = k.f[4];
    for (u32 i = begin; i < end; ++i) {
        const f32 s = p.seed ? VarianceScale(p.seed[i], kSaltColor, variance) : 1.0f;
        p.color[i] = glm::vec4(base.x * s, base.y * s, base.z * s, base.w);
    }
}

// SpawnInitSize: f[0] x, f[1] y, f[2] variance (uniform scale on both axes).
void K_SpawnInitSize(ParticleView& p, const ModuleParams& k, u32 begin, u32 end, f32 /*dt*/) {
    if (!p.sizeX || !p.sizeY) return;
    const f32 sx = k.f[0];
    const f32 sy = k.f[1];
    const f32 variance = k.f[2];
    for (u32 i = begin; i < end; ++i) {
        const f32 s =
            glm::max(0.0f, p.seed ? VarianceScale(p.seed[i], kSaltSize, variance) : 1.0f);
        p.sizeX[i] = sx * s;
        p.sizeY[i] = sy * s;
    }
}

// SpawnInitRotation: f[0] initial-angle range (radians), f[1] rate min, f[2] rate max.
void K_SpawnInitRotation(ParticleView& p, const ModuleParams& k, u32 begin, u32 end, f32 /*dt*/) {
    if (!p.rotation || !p.rotationRate) return;
    const f32 range = k.f[0];
    const f32 rateMin = k.f[1];
    const f32 rateMax = k.f[2];
    for (u32 i = begin; i < end; ++i) {
        Rng rng = SpawnRng(k, i - begin, kSaltRotation);
        p.rotation[i] = rng.Next01() * range;
        p.rotationRate[i] = rateMin + rng.Next01() * (rateMax - rateMin);
    }
}

// --- ParticleUpdate --------------------------------------------------------

// ConstantForce (gravity, wind, any uniform acceleration): f[0..2] acceleration.
void K_ConstantForce(ParticleView& p, const ModuleParams& k, u32 begin, u32 end, f32 dt) {
    if (!p.velocity) return;
    const glm::vec3 dv(k.f[0] * dt, k.f[1] * dt, k.f[2] * dt);
    for (u32 i = begin; i < end; ++i) p.velocity[i] += dv;
}

// Drag: f[0] coefficient (1/sec).
//
// EXPONENTIAL, not the legacy `vel *= max(0, 1 - drag*dt)`. The linear form is
// framerate-DEPENDENT - halving dt changes the terminal look - and because it is
// clamped it SATURATES: once drag*dt >= 1, reachable at drag 8 on a hitching frame,
// it stops the particle dead in one step. exp(-drag*dt) is the exact solution of
// dv/dt = -drag*v, so the result is identical at 30, 60 and 144 Hz, it approaches
// zero smoothly, and it never overshoots through zero either.
void K_Drag(ParticleView& p, const ModuleParams& k, u32 begin, u32 end, f32 dt) {
    if (!p.velocity) return;
    const f32 s = std::exp(-glm::max(0.0f, k.f[0]) * dt);
    for (u32 i = begin; i < end; ++i) p.velocity[i] *= s;
}

// CurlNoiseForce: f[0] strength, f[1] spatial frequency.
//
// This is the CURL of a sinusoidal vector potential, so it is EXACTLY
// divergence-free (div of any curl is identically zero) - particles swirl and never
// pile up in sinks. The legacy "turbulence" field was a raw sin/cos triple with
// non-zero divergence, which is why it visibly clumped. Analytic, three cosines,
// no texture fetch and no gradient sampling.
//
//   psi(p) = ( sin(f*p.y + t), sin(f*p.z + 1.3t), sin(f*p.x + 0.7t) )
//   curl   = ( -f*cos(f*p.z + 1.3t), -f*cos(f*p.x + 0.7t), -f*cos(f*p.y + t) )
void K_CurlNoiseForce(ParticleView& p, const ModuleParams& k, u32 begin, u32 end, f32 dt) {
    if (!p.position || !p.velocity) return;
    const f32 strength = k.f[0];
    const f32 freq = k.f[1];
    const f32 t = k.time;
    const f32 scale = strength * freq * dt;
    for (u32 i = begin; i < end; ++i) {
        const glm::vec3 q = p.position[i] * freq;
        const glm::vec3 curl(-std::cos(q.z + 1.3f * t), -std::cos(q.x + 0.7f * t),
                             -std::cos(q.y + t));
        p.velocity[i] += curl * scale;
    }
}

// ColorOverLife: f[0..3] start rgba, f[4..7] end rgba, f[8] fadeIn, f[9] fadeOut,
// f[10] per-particle brightness variance (hashed from the stored seed - see
// K_SpawnInitColor for why it is not a spawn-stream draw).
// The fade envelope multiplies alpha, so a two-key colour ramp plus in/out fades is
// ONE pass instead of the legacy code's three interleaved branches.
void K_ColorOverLife(ParticleView& p, const ModuleParams& k, u32 begin, u32 end, f32 /*dt*/) {
    if (!p.color || !p.age || !p.lifetime) return;
    const glm::vec4 c0(k.f[0], k.f[1], k.f[2], k.f[3]);
    const glm::vec4 c1(k.f[4], k.f[5], k.f[6], k.f[7]);
    const f32 fadeIn = k.f[8];
    const f32 fadeOut = k.f[9];
    const f32 variance = k.f[10];
    for (u32 i = begin; i < end; ++i) {
        const f32 life = (p.lifetime[i] > 1e-6f) ? p.lifetime[i] : 1e-6f;
        const f32 t = glm::clamp(p.age[i] / life, 0.0f, 1.0f);
        glm::vec4 c = glm::mix(c0, c1, t);
        f32 env = 1.0f;
        if (fadeIn > 0.0f && t < fadeIn) env *= t / fadeIn;
        if (fadeOut > 0.0f && t > 1.0f - fadeOut) env *= (1.0f - t) / fadeOut;
        c.w *= glm::clamp(env, 0.0f, 1.0f);
        const f32 s = p.seed ? VarianceScale(p.seed[i], kSaltColor, variance) : 1.0f;
        p.color[i] = glm::vec4(c.x * s, c.y * s, c.z * s, c.w);
    }
}

// SizeOverLife: f[0] startX, f[1] startY, f[2] endX, f[3] endY, f[4] variance.
void K_SizeOverLife(ParticleView& p, const ModuleParams& k, u32 begin, u32 end, f32 /*dt*/) {
    if (!p.sizeX || !p.sizeY || !p.age || !p.lifetime) return;
    const f32 x0 = k.f[0], y0 = k.f[1], x1 = k.f[2], y1 = k.f[3];
    const f32 variance = k.f[4];
    for (u32 i = begin; i < end; ++i) {
        const f32 life = (p.lifetime[i] > 1e-6f) ? p.lifetime[i] : 1e-6f;
        const f32 t = glm::clamp(p.age[i] / life, 0.0f, 1.0f);
        const f32 s =
            glm::max(0.0f, p.seed ? VarianceScale(p.seed[i], kSaltSize, variance) : 1.0f);
        // glm::mix, not x0 + (x1-x0)*t: the two differ in the low bits, and the
        // renderer's size ramp (Scene/ParticleSystem.cpp BuildVertices) uses glm::mix.
        // Matching it is what makes "simulated size with variance 0" bit-identical to
        // the render-time curve it replaces.
        p.sizeX[i] = glm::mix(x0, x1, t) * s;
        p.sizeY[i] = glm::mix(y0, y1, t) * s;
    }
}

// RotationRate: integrates Rotation from the per-particle rate. Separate from the
// position integrator so an effect that does not spin never pays for it.
void K_RotationRate(ParticleView& p, const ModuleParams& /*k*/, u32 begin, u32 end, f32 dt) {
    if (!p.rotation || !p.rotationRate) return;
    for (u32 i = begin; i < end; ++i) p.rotation[i] += p.rotationRate[i] * dt;
}

// Integrate: the Euler integrator. Position from velocity, and Age from dt.
//
// It is a real module in the stack (not a hidden step) precisely so that the stack
// reads the way it executes: every force module above it accumulates into Velocity,
// this consumes Velocity, and anything below it is looking at the NEW position. The
// compiler appends it automatically when the author has not placed one.
void K_Integrate(ParticleView& p, const ModuleParams& /*k*/, u32 begin, u32 end, f32 dt) {
    if (!p.position || !p.velocity || !p.age) return;
    for (u32 i = begin; i < end; ++i) {
        p.position[i] += p.velocity[i] * dt;
        p.age[i] += dt;
    }
}

// --- ParticleCollide -------------------------------------------------------

// CollidePlane: f[0..2] plane normal, f[3] plane offset d (plane is dot(n,p)+d = 0),
// f[4] restitution, f[5] tangential friction.
//
// Runs in the ParticleCollide stage, which the stage enum guarantees is AFTER
// integration. If it ran before, it would resolve against last frame's position and
// the particle would tunnel - see the note in VfxStack.h.
void K_CollidePlane(ParticleView& p, const ModuleParams& k, u32 begin, u32 end, f32 /*dt*/) {
    if (!p.position || !p.velocity) return;
    glm::vec3 n(k.f[0], k.f[1], k.f[2]);
    const f32 nlen = glm::length(n);
    if (nlen < 1e-6f) return;
    n /= nlen;
    const f32 d = k.f[3] / nlen;
    const f32 restitution = glm::clamp(k.f[4], 0.0f, 1.0f);
    const f32 friction = glm::clamp(k.f[5], 0.0f, 1.0f);

    for (u32 i = begin; i < end; ++i) {
        const f32 sd = glm::dot(n, p.position[i]) + d;
        if (sd >= 0.0f) continue;
        p.position[i] -= n * sd; // project back onto the plane
        const f32 vn = glm::dot(n, p.velocity[i]);
        if (vn < 0.0f) {
            const glm::vec3 normalPart = n * vn;
            const glm::vec3 tangentPart = p.velocity[i] - normalPart;
            p.velocity[i] = tangentPart * (1.0f - friction) - normalPart * restitution;
        }
        if (p.flags) p.flags[i] |= kFlagCollided;
    }
}

// --- The catalog -----------------------------------------------------------

constexpr AttributeMask kPos = AttrBit(Attr::Position);
constexpr AttributeMask kVel = AttrBit(Attr::Velocity);
constexpr AttributeMask kAge = AttrBit(Attr::Age);
constexpr AttributeMask kLife = AttrBit(Attr::Lifetime);
constexpr AttributeMask kCol = AttrBit(Attr::Color);
constexpr AttributeMask kSize = AttrBit(Attr::Size);
constexpr AttributeMask kRot = AttrBit(Attr::Rotation);
constexpr AttributeMask kRate = AttrBit(Attr::RotationRate);
constexpr AttributeMask kSeed = AttrBit(Attr::Seed);
constexpr AttributeMask kFlags = AttrBit(Attr::Flags);

// Indexed by ModuleType. The static_assert below is what keeps this table in
// lockstep with the enum - the exact 4-site drift that has bitten the schematic
// catalog and the particle template list before.
const ModuleDesc kCatalog[] = {
    {"Spawn.InitState", ModuleStage::ParticleSpawn, 0,
     kPos | kVel | kAge | kLife | kSeed | kFlags, &K_SpawnInitState},
    {"Spawn.InitColor", ModuleStage::ParticleSpawn, kSeed, kCol, &K_SpawnInitColor},
    {"Spawn.InitSize", ModuleStage::ParticleSpawn, kSeed, kSize, &K_SpawnInitSize},
    {"Spawn.InitRotation", ModuleStage::ParticleSpawn, 0, kRot | kRate, &K_SpawnInitRotation},

    {"Update.ConstantForce", ModuleStage::ParticleUpdate, kVel, kVel, &K_ConstantForce},
    {"Update.Drag", ModuleStage::ParticleUpdate, kVel, kVel, &K_Drag},
    {"Update.CurlNoiseForce", ModuleStage::ParticleUpdate, kPos | kVel, kVel, &K_CurlNoiseForce},
    {"Update.ColorOverLife", ModuleStage::ParticleUpdate, kAge | kLife | kSeed, kCol,
     &K_ColorOverLife},
    {"Update.SizeOverLife", ModuleStage::ParticleUpdate, kAge | kLife | kSeed, kSize,
     &K_SizeOverLife},
    {"Update.RotationRate", ModuleStage::ParticleUpdate, kRot | kRate, kRot, &K_RotationRate},
    {"Update.Integrate", ModuleStage::ParticleUpdate, kPos | kVel | kAge, kPos | kAge,
     &K_Integrate, true},

    // Flags is in READS as well as writes because K_CollidePlane does |= on it, which
    // is a read-modify-write. Declaring only the write would let the validator accept a
    // stack in which nothing initialises Flags: the stream would then be allocated
    // (Collide.Plane's write mask puts it in usedAttrs) but never cleared, and
    // ParticleSoA::SwapPop recycles slots - so a freshly spawned particle would inherit
    // its predecessor's kFlagCollided bit. SizeStream only zeroes on a size CHANGE, so
    // a recompile at the same capacity would not save it either.
    {"Collide.Plane", ModuleStage::ParticleCollide, kPos | kVel | kFlags,
     kPos | kVel | kFlags, &K_CollidePlane},

    // Compatibility bridge (Vfx/VfxLegacy.cpp). Legacy.Forces advances Age because
    // the old loop did it before the buoyancy roll read it, so Legacy.Integrate does
    // NOT - splitting them the other way would shift the roll by a frame.
    // kFlags is in the write mask because K_LegacyInitState stamps kFlagAlive. The
    // kernel guards the pointer, so omitting it did not crash - it just made the
    // declaration a lie, and a Legacy.InitState + Collide.Plane stack would then have
    // had an allocated-but-uninitialised Flags stream. Declare what the kernel touches.
    {"Legacy.InitState", ModuleStage::ParticleSpawn, 0,
     kPos | kVel | kAge | kLife | kRot | kSeed | kFlags, &K_LegacyInitState},
    {"Legacy.Forces", ModuleStage::ParticleUpdate, kPos | kVel | kAge | kLife, kVel | kAge,
     &K_LegacyForces},
    {"Legacy.DragLinear", ModuleStage::ParticleUpdate, kVel, kVel, &K_LegacyDragLinear},
    {"Legacy.Integrate", ModuleStage::ParticleUpdate, kPos | kVel | kRot, kPos | kRot,
     &K_LegacyIntegrate, true},
};

static_assert(sizeof(kCatalog) / sizeof(kCatalog[0]) == static_cast<usize>(ModuleType::Count),
              "kCatalog and ModuleType drifted apart. They are one list: every enumerator "
              "must have exactly one row, in the same order.");

const ModuleDesc kInvalidDesc{"<invalid>", ModuleStage::ParticleUpdate, 0, 0, nullptr};

} // namespace

const ModuleDesc& Describe(ModuleType t) {
    const u32 i = static_cast<u32>(t);
    return (i < ModuleCount()) ? kCatalog[i] : kInvalidDesc;
}

const ModuleDesc* FindByName(std::string_view name) {
    for (u32 i = 0; i < ModuleCount(); ++i)
        if (name == kCatalog[i].name) return &kCatalog[i];
    return nullptr;
}

// ---------------------------------------------------------------------------
// Compile + validate
// ---------------------------------------------------------------------------

namespace {

void AddError(std::string& errors, const std::string& line) {
    if (!errors.empty()) errors += '\n';
    errors += line;
}

// Reports every attribute a module reads that nothing before it produced. Returns
// false if any were missing.
bool CheckReads(const CompiledModule& m, AttributeMask available, std::string& errors) {
    const AttributeMask missing = m.reads & ~available;
    if (missing == 0u) return true;
    std::string names;
    AppendMaskNames(missing, names);
    AddError(errors, std::string(Describe(m.type).name) + " reads " + names +
                         " but no earlier module writes it and no spawn module "
                         "initialises it.");
    return false;
}

} // namespace

bool Compile(const StackDesc& def, CompiledStack& out, std::string* errors) {
    out = CompiledStack{};
    out.maxParticles = def.maxParticles;
    out.seed = def.seed;
    out.spawnRate = def.spawnRate;
    out.burst = def.burst;
    out.loop = def.loop;
    out.duration = def.duration;
    out.accum = def.accum;

    std::string err;

    // Pass 1 - resolve, and check the AUTHORED (flat) order.
    //
    // Stage binning makes collide-after-integrate true at runtime no matter what the
    // asset says. But an artist who drags a collision module above the integrator has
    // written a stack that does not do what it reads, so silently reordering it is
    // worse than refusing it. Reject with a named error instead.
    i32 firstIntegrate = -1;
    i32 firstCollide = -1;
    i32 lastUpdate = -1;
    i32 emitterStageAt = -1;
    u32 integrateCount = 0;
    bool ok = true;

    for (usize i = 0; i < def.modules.size(); ++i) {
        const StackModule& m = def.modules[i];
        if (static_cast<u32>(m.type) >= ModuleCount()) {
            AddError(err, "module #" + std::to_string(i) + ": unknown module type " +
                              std::to_string(static_cast<u32>(m.type)) + ".");
            ok = false;
            continue;
        }
        if (!m.enabled) continue;
        const ModuleDesc& d = Describe(m.type);
        if (d.integrates) {
            if (firstIntegrate < 0) firstIntegrate = static_cast<i32>(i);
            ++integrateCount;
        }
        if (d.stage == ModuleStage::ParticleUpdate) lastUpdate = static_cast<i32>(i);
        if (d.stage == ModuleStage::ParticleCollide && firstCollide < 0)
            firstCollide = static_cast<i32>(i);
        if ((d.stage == ModuleStage::EmitterSpawn || d.stage == ModuleStage::EmitterUpdate) &&
            emitterStageAt < 0)
            emitterStageAt = static_cast<i32>(i);
    }

    if (integrateCount > 1) {
        AddError(err, "the stack has " + std::to_string(integrateCount) +
                          " integrators. The integrator is singular - forces accumulate "
                          "into Velocity and exactly one module consumes it.");
        ok = false;
    }
    if (firstCollide >= 0 && firstIntegrate >= 0 && firstCollide < firstIntegrate) {
        AddError(err, std::string(Describe(def.modules[static_cast<usize>(firstCollide)].type).name) +
                          " (ParticleCollide) is authored BEFORE the integrator. Collision "
                          "must resolve against the position the integrator just produced; "
                          "before it, the particle is tested at last frame's position and "
                          "tunnels. Move it below the integrator.");
        ok = false;
    }
    // The OTHER direction of the same trap, and it is just as silent. Stage binning
    // moves every collide after every update, so an authored
    //   [..., Integrate, Collide.Plane, Update.Drag]
    // reads "bounce, then damp the bounce" and executes "damp, then bounce": the
    // restitution/friction velocity the collide produced is never damped, and the drag
    // is applied to the pre-impact velocity instead. Rejecting collide-before-integrate
    // but silently reordering update-after-collide would only cover half the promise
    // this compiler makes in VfxStack.h ("a stack that does not do what it reads").
    if (firstCollide >= 0 && lastUpdate > firstCollide) {
        AddError(err, std::string(Describe(def.modules[static_cast<usize>(lastUpdate)].type).name) +
                          " (ParticleUpdate) is authored AFTER " +
                          Describe(def.modules[static_cast<usize>(firstCollide)].type).name +
                          " (ParticleCollide), but the stage order runs every update module "
                          "BEFORE every collide module - so it would execute in the opposite "
                          "order to the way it reads, on the pre-collision velocity. Move it "
                          "above the collision module.");
        ok = false;
    }
    // RunFrame only dispatches ParticleSpawn/Update/Collide. No catalog row uses the
    // two emitter stages yet (they land in Phase 2), so a module in one would compile
    // clean, fold its mask into usedAttrs, and never execute - a silent no-op. Name it
    // instead of waiting for the first Phase-2 module to be mysteriously inert.
    if (emitterStageAt >= 0) {
        const ModuleDesc& d = Describe(def.modules[static_cast<usize>(emitterStageAt)].type);
        AddError(err, std::string(d.name) + " is in the " + StageName(d.stage) +
                          " stage, which the runner does not execute yet. Emitter-stage "
                          "dispatch lands with the Phase-2 emitter modules; until RunFrame "
                          "runs those stages a module placed in one would silently never run.");
        ok = false;
    }

    if (!ok) {
        if (errors) *errors = err;
        return false;
    }

    // Pass 2 - bin into stages, preserving authored order within each stage.
    for (const StackModule& m : def.modules) {
        if (!m.enabled) continue;
        const ModuleDesc& d = Describe(m.type);
        CompiledModule cm;
        cm.type = m.type;
        cm.kernel = d.kernel;
        cm.params = m.params;
        cm.reads = d.reads;
        cm.writes = d.writes;
        out.stages[static_cast<u32>(d.stage)].push_back(cm);
    }

    // Pass 3 - append the implicit integrator when the author omitted it. Placing it
    // last in ParticleUpdate is the only correct spot: every force module above has
    // finished accumulating into Velocity by then.
    auto& updateStage = out.stages[static_cast<u32>(ModuleStage::ParticleUpdate)];
    if (integrateCount == 0) {
        const ModuleDesc& d = Describe(ModuleType::Integrate);
        CompiledModule cm;
        cm.type = ModuleType::Integrate;
        cm.kernel = d.kernel;
        cm.reads = d.reads;
        cm.writes = d.writes;
        updateStage.push_back(cm);
    }

    // Pass 4 - read/write validation, in execution order.
    //
    // `available` starts as everything the spawn stage initialises, then grows as
    // update modules write. This is the payoff of the masks: "RotationRate reads
    // Rotation but nothing initialises it" is a named compile error rather than a
    // silent read of a stream that was never even allocated.
    AttributeMask available = 0;
    for (const CompiledModule& m : out.stages[static_cast<u32>(ModuleStage::ParticleSpawn)])
        available |= m.writes;

    // A spawn module may itself read what an earlier spawn module wrote.
    {
        AttributeMask spawnAvail = 0;
        for (const CompiledModule& m : out.stages[static_cast<u32>(ModuleStage::ParticleSpawn)]) {
            if (!CheckReads(m, spawnAvail, err)) ok = false;
            spawnAvail |= m.writes;
        }
    }
    for (const CompiledModule& m : updateStage) {
        if (!CheckReads(m, available, err)) ok = false;
        available |= m.writes;
    }
    for (const CompiledModule& m : out.stages[static_cast<u32>(ModuleStage::ParticleCollide)]) {
        if (!CheckReads(m, available, err)) ok = false;
        available |= m.writes;
    }

    // Retirement needs Age and Lifetime; without them nothing ever dies.
    if ((available & (kAge | kLife)) != (kAge | kLife)) {
        AddError(err, "stack never initialises Age and Lifetime - particles would never "
                      "retire. Add Spawn.InitState.");
        ok = false;
    }

    // Pass 5 - the union mask. THIS is dead-stream elimination: ParticleSoA only
    // allocates the streams in it, so a stack with no rotation module holds zero
    // bytes of rotation.
    for (u32 s = 0; s < kStageCount; ++s)
        for (const CompiledModule& m : out.stages[s]) {
            out.readMask |= m.reads;
            out.writeMask |= m.writes;
        }
    out.usedAttrs = out.readMask | out.writeMask;

    out.valid = ok;
    if (errors) *errors = err;
    return ok;
}

// ---------------------------------------------------------------------------
// Runner
// ---------------------------------------------------------------------------

void ReservePool(const CompiledStack& stack, ParticleSoA& pool) {
    pool.Reserve(stack.maxParticles, stack.usedAttrs);
}

namespace {

// Copies the authored constants and stamps the per-frame emitter state. 64 bytes
// per module per frame, which is nothing next to a pass over thousands of particles,
// and it is what keeps the kernel signature free of scene access.
inline ModuleParams Stamp(const CompiledModule& m, const CompiledStack& stack,
                          const EmitterState& em) {
    ModuleParams p = m.params;
    p.time = em.emitterTime;
    p.seed = stack.seed;
    p.spawnBase = em.spawnCounter;
    return p;
}

// `allocated` is the pool's real stream mask. In a debug build a module whose
// declared reads|writes is not a subset of it is reported by NAME, once.
//
// This exists because the guards at the top of every kernel ("if (!p.position)
// return;") mean a mask violation does NOT crash - it silently skips the module for
// the whole frame, which is the harder failure to diagnose, not the easier one. The
// compile-time validator is the real guard; this is the runtime backstop that makes
// a violation loud instead of invisible. It is per module per frame, not per
// particle, and compiled out entirely in Release.
void RunStage(const CompiledStack& stack, const EmitterState& em, ModuleStage stage,
              ParticleView& view, u32 begin, u32 end, f32 dt,
              [[maybe_unused]] AttributeMask allocated) {
    if (end <= begin) return;
    for (const CompiledModule& m : stack.stages[static_cast<u32>(stage)]) {
        if (!m.kernel) continue;
#if !defined(NDEBUG)
        if (const AttributeMask missing = (m.reads | m.writes) & ~allocated; missing != 0u) {
            static bool reported = false;
            if (!reported) {
                reported = true;
                std::string names;
                AppendMaskNames(missing, names);
                HBE_ERROR("[vfx] {} declares {} but the pool never allocated it - the kernel "
                          "will no-op on those streams. Compile() should have rejected this "
                          "stack; the catalog row and the kernel have drifted apart.",
                          Describe(m.type).name, names);
            }
        }
#endif
        const ModuleParams p = Stamp(m, stack, em);
        m.kernel(view, p, begin, end, dt);
    }
}

} // namespace

RunStats RunFrame(const CompiledStack& stack, EmitterState& em, ParticleSoA& pool, f32 dt,
                  const void* user) {
    RunStats st;
    st.live = pool.count;
    if (!stack.valid || dt <= 0.0f) return st;

    // 1. Emitter clocks. emitterTime is the noise/sub-UV phase and NEVER resets;
    //    emitterAge is relative to the current loop.
    em.emitterTime += dt;
    em.emitterAge += dt;

    // The two non-stream members are attached here rather than inside MakeView so
    // MakeView stays a pure function of the pool; only the runner knows the emitter.
    const auto bindView = [&]() {
        ParticleView v = MakeView(pool);
        v.emitterRng = &em.rngState;
        v.user = user;
        return v;
    };

    const u32 oldCount = pool.count;
    ParticleView view = bindView();

    // 2/3. Update, then collide - both over [0, oldCount) ONLY. Particles spawned
    //      below in step 5 must not be integrated in the same frame they are born;
    //      their sub-frame catch-up is the spawn kernel's job.
    RunStage(stack, em, ModuleStage::ParticleUpdate, view, 0, oldCount, dt, pool.used);
    RunStage(stack, em, ModuleStage::ParticleCollide, view, 0, oldCount, dt, pool.used);

    // 4. Retire. Swap-pop, so the survivor moved into slot i is examined next
    //    iteration (hence no ++i on the kill branch).
    if (pool.Has(Attr::Age) && pool.Has(Attr::Lifetime)) {
        for (u32 i = 0; i < pool.count;) {
            if (pool.age[i] >= pool.lifetime[i]) {
                pool.SwapPop(i);
                ++st.killed;
            } else {
                ++i;
            }
        }
    }

    // 5. Spawn.
    u32 want = 0;
    if (em.emitting) {
        if (!em.burstFired && stack.burst > 0) {
            want += stack.burst;
            em.burstFired = true;
        }
        const bool windowOpen = stack.loop || em.emitterAge <= stack.duration;
        if (windowOpen && stack.spawnRate > 0.0f) {
            f64 whole = 0.0;
            if (stack.accum == SpawnAccumMode::Legacy32) {
                // Byte-for-byte the pre-module-stack scheduler, including the
                // truncating cast. See SpawnAccumMode for why it is preserved.
                em.spawnAccum32 += stack.spawnRate * dt;
                const i32 n = static_cast<i32>(em.spawnAccum32);
                em.spawnAccum32 -= static_cast<f32>(n);
                whole = (n > 0) ? static_cast<f64>(n) : 0.0;
            } else {
                // f64 accumulator - see EmitterState::spawnAccum for why not f32.
                em.spawnAccum += static_cast<f64>(stack.spawnRate) * static_cast<f64>(dt);
                whole = std::floor(em.spawnAccum);
                em.spawnAccum -= whole;
            }
            const f64 clamped = (whole > static_cast<f64>(stack.maxParticles))
                                    ? static_cast<f64>(stack.maxParticles)
                                    : whole;
            want += static_cast<u32>(clamped);
        }
    }
    em.wasEmitting = em.emitting;

    if (want > 0) {
        u32 first = 0;
        const u32 granted = pool.Grant(want, first);
        st.dropped = want - granted; // OVERFLOW POLICY: drop, never recycle
        st.spawned = granted;
        if (granted > 0) {
            // MANDATORY rebind: Grant may have grown the committed capacity, which
            // reallocates every live stream and invalidates every pointer in `view`.
            view = bindView();
            RunStage(stack, em, ModuleStage::ParticleSpawn, view, first, first + granted, dt,
                     pool.used);
            em.spawnCounter += granted; // monotonic - the RNG stream key
        }
    }

    em.spawnThisFrame = st.spawned;
    st.live = pool.count;
    return st;
}

u64 HashPool(const ParticleSoA& pool) {
    u64 h = 1469598103934665603ull; // FNV-1a 64 offset basis
    const auto mix = [&h](const void* data, usize bytes) {
        const u8* p = static_cast<const u8*>(data);
        for (usize i = 0; i < bytes; ++i) {
            h ^= static_cast<u64>(p[i]);
            h *= 1099511628211ull;
        }
    };
    const u32 n = pool.count;
    mix(&n, sizeof(n));
    mix(&pool.used, sizeof(pool.used));
    if (n == 0) return h;
    if (pool.Has(Attr::Position)) mix(pool.position.data(), n * sizeof(glm::vec3));
    if (pool.Has(Attr::Velocity)) mix(pool.velocity.data(), n * sizeof(glm::vec3));
    if (pool.Has(Attr::Age)) mix(pool.age.data(), n * sizeof(f32));
    if (pool.Has(Attr::Lifetime)) mix(pool.lifetime.data(), n * sizeof(f32));
    if (pool.Has(Attr::Color)) mix(pool.color.data(), n * sizeof(glm::vec4));
    if (pool.Has(Attr::Size)) {
        mix(pool.sizeX.data(), n * sizeof(f32));
        mix(pool.sizeY.data(), n * sizeof(f32));
    }
    if (pool.Has(Attr::Rotation)) mix(pool.rotation.data(), n * sizeof(f32));
    if (pool.Has(Attr::RotationRate)) mix(pool.rotationRate.data(), n * sizeof(f32));
    if (pool.Has(Attr::SubImage)) mix(pool.subImage.data(), n * sizeof(f32));
    if (pool.Has(Attr::Seed)) mix(pool.seed.data(), n * sizeof(u32));
    if (pool.Has(Attr::Flags)) mix(pool.flags.data(), n * sizeof(u32));
    return h;
}

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------

namespace {

ModuleParams P(std::initializer_list<f32> vals) {
    ModuleParams p;
    u32 i = 0;
    for (const f32 v : vals) {
        if (i >= 12) break;
        p.f[i++] = v;
    }
    return p;
}

// The reference "fire" stack: every v1 module, in the order an artist would build it.
StackDesc MakeFullStack(u32 seed) {
    StackDesc d;
    d.maxParticles = 4096;
    d.seed = seed;
    d.spawnRate = 400.0f;
    d.loop = true;
    d.modules = {
        // origin, radius, direction, speed, speedVar, life, lifeVar, spread
        {ModuleType::SpawnInitState, P({0, 0, 0, 0.25f, 0, 1, 0, 2.5f, 0.4f, 1.5f, 0.3f, 0.35f})},
        {ModuleType::SpawnInitColor, P({1.0f, 0.7f, 0.25f, 1.0f, 0.2f})},
        {ModuleType::SpawnInitSize, P({0.3f, 0.3f, 0.25f})},
        {ModuleType::SpawnInitRotation, P({6.2831853f, -2.0f, 2.0f})},
        {ModuleType::ConstantForce, P({0.0f, -1.2f, 0.0f})},
        {ModuleType::CurlNoiseForce, P({1.6f, 0.9f})},
        {ModuleType::Drag, P({0.8f})},
        {ModuleType::ColorOverLife, P({1.0f, 0.8f, 0.3f, 1.0f, 0.8f, 0.15f, 0.05f, 0.0f, 0.1f,
                                       0.35f})},
        {ModuleType::SizeOverLife, P({0.3f, 0.3f, 0.05f, 0.05f})},
        {ModuleType::RotationRate, P({})},
        {ModuleType::Integrate, P({})},
        {ModuleType::CollidePlane, P({0.0f, 1.0f, 0.0f, 0.0f, 0.35f, 0.2f})},
    };
    return d;
}

// The lean stack: no colour, no size, no rotation. Its whole point is that the pool
// must not allocate those streams at all.
StackDesc MakeLeanStack(u32 seed) {
    StackDesc d;
    d.maxParticles = 4096;
    d.seed = seed;
    d.spawnRate = 400.0f;
    d.modules = {
        {ModuleType::SpawnInitState, P({0, 0, 0, 0.25f, 0, 1, 0, 2.5f, 0.4f, 1.5f, 0.3f, 0.35f})},
        {ModuleType::ConstantForce, P({0.0f, -9.81f, 0.0f})},
        {ModuleType::Integrate, P({})},
    };
    return d;
}

u64 SimulateAndHash(const StackDesc& desc, u32 frames, f32 dt) {
    CompiledStack stack;
    std::string errs;
    if (!Compile(desc, stack, &errs)) return 0;
    ParticleSoA pool;
    ReservePool(stack, pool);
    EmitterState em;
    em.Reset(desc.seed);
    for (u32 i = 0; i < frames; ++i) RunFrame(stack, em, pool, dt);
    return HashPool(pool);
}

} // namespace

bool SelfTest() {
    bool pass = true;

    // ---- (a) the 64-byte record ------------------------------------------
    {
        // The real guard is the static_assert in VfxTypes.h - this cannot fail at
        // runtime, which is exactly the property being reported. `if constexpr` keeps
        // it honest (and silences C4127) while the log line still states the numbers
        // whoever writes Shaders/VfxCommon.hlsli has to match.
        constexpr bool offsetsOk =
            offsetof(GpuParticle, position) == 0 && offsetof(GpuParticle, age) == 12 &&
            offsetof(GpuParticle, velocity) == 16 && offsetof(GpuParticle, lifetime) == 28 &&
            offsetof(GpuParticle, colorRG) == 32 && offsetof(GpuParticle, sizeX) == 40 &&
            offsetof(GpuParticle, rotation) == 48 && offsetof(GpuParticle, subImage) == 56 &&
            offsetof(GpuParticle, seed) == 60;
        if constexpr (sizeof(GpuParticle) == 64 && offsetsOk && sizeof(ModuleParams) == 64) {
            HBE_INFO("[vfx] (a) layout PASS - sizeof(GpuParticle)={} at the offsets "
                     "Shaders/VfxCommon.hlsli mirrors; sizeof(ModuleParams)={}.",
                     sizeof(GpuParticle), sizeof(ModuleParams));
        } else {
            HBE_ERROR("[vfx] (a) layout FAIL - sizeof(GpuParticle)={} (want 64), "
                      "sizeof(ModuleParams)={} (want 64).",
                      sizeof(GpuParticle), sizeof(ModuleParams));
            pass = false;
        }
    }

    // ---- (b) dead-stream elimination -------------------------------------
    {
        CompiledStack lean, full;
        std::string e1, e2;
        const bool cl = Compile(MakeLeanStack(0xC0FFEEu), lean, &e1);
        const bool cf = Compile(MakeFullStack(0xC0FFEEu), full, &e2);

        ParticleSoA leanPool, fullPool;
        if (cl) ReservePool(lean, leanPool);
        if (cf) ReservePool(full, fullPool);

        const ParticleView lv = MakeView(leanPool);
        const AttributeMask dead =
            AttrBit(Attr::Color) | AttrBit(Attr::Size) | AttrBit(Attr::Rotation) |
            AttrBit(Attr::RotationRate) | AttrBit(Attr::SubImage);

        const bool maskClean = cl && (lean.usedAttrs & dead) == 0u;
        const bool viewNull = lv.color == nullptr && lv.sizeX == nullptr && lv.sizeY == nullptr &&
                              lv.rotation == nullptr && lv.rotationRate == nullptr &&
                              lv.subImage == nullptr;
        const bool zeroBytes = leanPool.color.capacity() == 0 && leanPool.sizeX.capacity() == 0 &&
                               leanPool.sizeY.capacity() == 0 &&
                               leanPool.rotation.capacity() == 0 &&
                               leanPool.rotationRate.capacity() == 0 &&
                               leanPool.subImage.capacity() == 0;
        const usize leanBytes = leanPool.BytesAllocated();
        const usize fullBytes = fullPool.BytesAllocated();

        // And it must actually RUN with those streams absent. This is a smoke test of
        // the lean configuration, NOT a probe of mask discipline: the lean stack
        // contains no module that could violate its mask, and a violating kernel would
        // early-return rather than fault. Mask violations are caught by Compile()'s
        // validator (check (c)) and by RunStage's debug-only subset assertion.
        EmitterState em;
        em.Reset(0xC0FFEEu);
        u32 live = 0;
        if (cl)
            for (u32 i = 0; i < 60; ++i) live = RunFrame(lean, em, leanPool, 1.0f / 60.0f).live;

        if (cl && cf && maskClean && viewNull && zeroBytes && leanBytes < fullBytes && live > 0) {
            HBE_INFO("[vfx] (b) dead-stream PASS - lean stack holds {} B vs full {} B at a "
                     "committed capacity of {} (cap {}); {} unused streams hold zero bytes "
                     "and view null; ran 60 frames, {} live.",
                     leanBytes, fullBytes, leanPool.capacity, lean.maxParticles, 6, live);
        } else {
            HBE_ERROR("[vfx] (b) dead-stream FAIL - compiled({},{}) maskClean={} viewNull={} "
                      "zeroBytes={} leanBytes={} fullBytes={} live={} err1='{}' err2='{}'.",
                      cl, cf, maskClean, viewNull, zeroBytes, leanBytes, fullBytes, live, e1, e2);
            pass = false;
        }
    }

    // ---- (c) validation rejects collide-before-integrate ------------------
    {
        StackDesc bad;
        bad.maxParticles = 256;
        bad.modules = {
            {ModuleType::SpawnInitState, P({0, 0, 0, 0.25f, 0, 1, 0, 2.0f, 0.2f, 1.0f, 0.2f, 0.3f})},
            {ModuleType::CollidePlane, P({0.0f, 1.0f, 0.0f, 0.0f, 0.4f, 0.1f})},
            {ModuleType::Integrate, P({})},
        };
        std::string badErr;
        CompiledStack badStack;
        const bool rejected = !Compile(bad, badStack, &badErr) && !badStack.valid;

        // Positive control: the same modules in the legal order must compile.
        StackDesc good = bad;
        std::swap(good.modules[1], good.modules[2]);
        std::string goodErr;
        CompiledStack goodStack;
        const bool accepted = Compile(good, goodStack, &goodErr);

        // And the read/write validator must catch an uninitialised attribute.
        StackDesc missing;
        missing.maxParticles = 256;
        missing.modules = {
            {ModuleType::SpawnInitState, P({0, 0, 0, 0.25f, 0, 1, 0, 2.0f, 0.2f, 1.0f, 0.2f, 0.3f})},
            {ModuleType::RotationRate, P({})}, // reads Rotation - nothing initialises it
            {ModuleType::Integrate, P({})},
        };
        std::string missErr;
        CompiledStack missStack;
        const bool missingRejected = !Compile(missing, missStack, &missErr);

        // The OTHER half of the ordering trap: an update module authored below a
        // collide module. This one is nastier than collide-before-integrate because it
        // is not obviously wrong on the page - it reads "bounce off the plane, then
        // damp the post-bounce velocity" - while stage binning would execute it as
        // "damp, then bounce", applying the drag to the pre-impact velocity and never
        // damping the restitution the collision just produced.
        StackDesc afterCollide;
        afterCollide.maxParticles = 256;
        afterCollide.modules = {
            {ModuleType::SpawnInitState, P({0, 0, 0, 0.25f, 0, 1, 0, 2.0f, 0.2f, 1.0f, 0.2f, 0.3f})},
            {ModuleType::Integrate, P({})},
            {ModuleType::CollidePlane, P({0.0f, 1.0f, 0.0f, 0.0f, 0.4f, 0.1f})},
            {ModuleType::Drag, P({2.0f})}, // reads as post-bounce; would run pre-bounce
        };
        std::string acErr;
        CompiledStack acStack;
        const bool afterCollideRejected = !Compile(afterCollide, acStack, &acErr);

        if (rejected && accepted && missingRejected && afterCollideRejected) {
            HBE_INFO("[vfx] (c) validation PASS - collide-before-integrate rejected "
                     "(\"{}\"); update-after-collide rejected (\"{}\"); legal order "
                     "accepted ({} modules); uninitialised read rejected (\"{}\").",
                     badErr, acErr, goodStack.ModuleCountTotal(), missErr);
        } else {
            HBE_ERROR("[vfx] (c) validation FAIL - rejected={} accepted={} missingRejected={} "
                      "afterCollideRejected={} badErr='{}' goodErr='{}' missErr='{}' "
                      "acErr='{}'.",
                      rejected, accepted, missingRejected, afterCollideRejected, badErr, goodErr,
                      missErr, acErr);
            pass = false;
        }
    }

    // ---- (d) determinism --------------------------------------------------
    {
        constexpr u32 kFrames = 240;
        constexpr f32 kDt = 1.0f / 60.0f;
        const u64 h1 = SimulateAndHash(MakeFullStack(0xC0FFEEu), kFrames, kDt);
        const u64 h2 = SimulateAndHash(MakeFullStack(0xC0FFEEu), kFrames, kDt);
        const u64 h3 = SimulateAndHash(MakeFullStack(0xDEADBEEFu), kFrames, kDt);

        // A hash of an empty pool would make equality meaningless - check we really
        // simulated something.
        CompiledStack probe;
        std::string probeErr;
        u32 liveCount = 0;
        if (Compile(MakeFullStack(0xC0FFEEu), probe, &probeErr)) {
            ParticleSoA pool;
            ReservePool(probe, pool);
            EmitterState em;
            em.Reset(0xC0FFEEu);
            for (u32 i = 0; i < kFrames; ++i) liveCount = RunFrame(probe, em, pool, kDt).live;
        }

        const bool same = (h1 == h2) && h1 != 0;
        const bool differs = (h1 != h3);
        if (same && differs && liveCount > 0) {
            HBE_INFO("[vfx] (d) determinism PASS - {} frames x {} live particles: seed "
                     "0xC0FFEE -> {:#x} twice; seed 0xDEADBEEF -> {:#x}.",
                     kFrames, liveCount, h1, h3);
        } else {
            HBE_ERROR("[vfx] (d) determinism FAIL - h1={:#x} h2={:#x} h3={:#x} live={}.", h1, h2,
                      h3, liveCount);
            pass = false;
        }
    }

    // ---- (e) spawn scheduling is exact over a feature-length capture ------
    //
    // The asserted property is the one that actually matters: over a 60-minute
    // fixed-dt capture the scheduler must emit floor(rate * duration) particles to
    // within one, with no systematic drift. Anything else and a movie re-render
    // does not match the previous take.
    //
    // The f32/f64 sweep below is MEASUREMENT, not assertion, and it corrects a piece
    // of received wisdom. The "f32 loses integer precision past 2^24" argument does
    // NOT apply to this accumulator: the residue is reduced by floor() every frame,
    // so it lives in [0,1) and never reaches 2^24. What f32 actually costs is the
    // quantisation of that residue to the ulp of (residue + rate*dt), which scales
    // with the rate. The sweep locates the boundary: identical totals up to 1e4/s,
    // first divergence at 1e5/s - an ordinary GPU-sim emitter rate - where a
    // 60-minute capture lands one particle apart, which is already enough to break a
    // movie re-render. So f64 is right, for parameter-independence, not overflow.
    {
        constexpr u32 kFrames = 216000; // 3600 s at 60 Hz
        constexpr f32 kDt = 1.0f / 60.0f;

        const auto run = [](f32 rate, u64& out64, u64& out32) {
            f64 acc64 = 0.0;
            f32 acc32 = 0.0f;
            out64 = 0;
            out32 = 0;
            for (u32 i = 0; i < kFrames; ++i) {
                acc64 += static_cast<f64>(rate) * static_cast<f64>(kDt);
                const f64 w64 = std::floor(acc64);
                acc64 -= w64;
                out64 += static_cast<u64>(w64);

                acc32 += rate * kDt;
                const f32 w32 = std::floor(acc32);
                acc32 -= w32;
                out32 += static_cast<u64>(w32);
            }
        };

        constexpr f32 kSpawnRate = 1234.567f; // an ordinary heavy emitter
        u64 total64 = 0, total32 = 0;
        run(kSpawnRate, total64, total32);
        const f64 expected = static_cast<f64>(kSpawnRate) * static_cast<f64>(kDt) * kFrames;
        const f64 err64 = std::abs(static_cast<f64>(total64) - expected);

        // Where does f32 actually start to differ?
        const f32 sweep[] = {1.0e3f, 1.0e4f, 1.0e5f, 1.0e6f, 1.0e7f};
        f32 firstDivergentRate = 0.0f;
        u64 divergence = 0;
        for (const f32 r : sweep) {
            u64 a = 0, b = 0;
            run(r, a, b);
            if (a != b) {
                firstDivergentRate = r;
                divergence = (a > b) ? (a - b) : (b - a);
                break;
            }
        }

        if (err64 <= 1.0) {
            HBE_INFO("[vfx] (e) spawn scheduling PASS - {} frames at rate {}: f64 emitted {} "
                     "vs {:.2f} exact (err {:.2f}). f32/f64 sweep: {} .",
                     kFrames, kSpawnRate, total64, expected, err64,
                     firstDivergentRate > 0.0f
                         ? std::format("first divergence at rate {} ({} particles apart)",
                                       firstDivergentRate, divergence)
                         : std::string("identical up to rate 1e7 - the 2^24 folklore does not "
                                       "apply to a floor-reduced residue"));
        } else {
            HBE_ERROR("[vfx] (e) spawn scheduling FAIL - f64 emitted {} vs {:.2f} exact "
                      "(err {:.2f}).",
                      total64, expected, err64);
            pass = false;
        }
    }

    if (pass) HBE_INFO("[vfx] SelfTest PASS - attribute model + module stack core verified.");
    else HBE_ERROR("[vfx] SelfTest FAIL.");
    return pass;
}

} // namespace hbe::vfx
