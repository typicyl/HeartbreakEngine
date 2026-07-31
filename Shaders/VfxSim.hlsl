// Shaders/VfxSim.hlsl - GPU PARTICLE SIMULATION: one kernel, one PSO, an INTERPRETER.
//
// WHY AN INTERPRETER AND NOT GENERATED CODE. Shaders in this engine compile at
// *cmake* time with DXC and are loaded from packs - there is no runtime shader
// compilation and no shipped DXC (Vfx/VfxTypes.h says so at the top, and it is the
// same constraint that forced GpuParticle to be a fixed 64-byte record instead of a
// per-emitter attribute set). A Niagara-style "one generated kernel per stack" is
// therefore not expressible here at all. So the module stack is uploaded as a
// PROGRAM - a (opcode, 12 floats) pair per module - and this kernel switches on the
// opcode.
//
// The switch is CHEAP, and that is a property of the data layout rather than luck:
// the opcode is read from a buffer indexed by (emitter, module), and every lane in a
// wave is working on the same emitter's same module, so the opcode is WAVE-UNIFORM.
// The hardware takes a scalar branch, not a divergent one; the cost is the same as
// an `if` on a constant. The only genuinely per-lane branches in the whole kernel are
// the dead-particle early-out and CollidePlane's signed-distance test, both of which
// the CPU kernels have too.
//
// PARITY. Every opcode below is a transliteration of the matching K_* kernel in
// Source/Vfx/VfxStack.cpp, including its RNG draw ORDER (which is load-bearing:
// K_SpawnInitState splits its draws into separate statements precisely because C++
// leaves the operands of `*` unsequenced, and this file must draw in that same
// order or the two paths produce different particles from the same seed). The RNG
// itself is the bit-exact port already in VfxCommon.hlsli. `--test-vfxsim` runs the
// CPU stack and this kernel over the same emitter and compares particle for
// particle; it is what stops this file from being delivered blind.
//
// WHAT IS *NOT* HERE. Opcodes 12..15 (Legacy.InitState / Forces / DragLinear /
// Integrate) are CPU-ONLY BY CONSTRUCTION - they read ParticleView::user (a 3x3
// basis plus ~25 scalars that does not fit ModuleParams) and ParticleView::emitterRng
// (a serial stream). They are the compatibility bridge for pre-module-stack emitters
// and they are what --test-vfxcompat pins bit-exactly. An emitter opting into GPU
// simulation compiles to the v1 modules instead; the CPU builder refuses to hand a
// legacy stack to this kernel.
//
// SLOT MODEL. The pool here is a RING of fixed slots, not the CPU's swap-pop array:
// a compacting pool needs either a prefix sum or an atomic append, and both would
// make the live count a GPU-side value that the draw would then have to read back or
// go indirect for. Instead the CPU owns the ring cursor (it already owns the spawn
// scheduler - f64 accumulator and all), a dead particle keeps its slot until the
// cursor laps it, and "dead" is expressed to the renderer as a ZERO SIZE, which
// makes the quad degenerate and costs no fill. Consequence, stated plainly: pool
// ORDER differs from the CPU path, and an undersized ring recycles a still-live
// particle instead of dropping the spawn. Both are documented divergences, not bugs.
#include "VfxCommon.hlsli"

// ---------------------------------------------------------------------------
// Bindings - the ComputePipelineDesc convention in RHI.h
//   constants : b0     [[vk::binding(0, 0)]]
//   uav i     : u<i>   [[vk::binding(1 + i, 0)]]
//   srv i     : t<i>   [[vk::binding(1 + uavCount + i, 0)]]
// uavCount = 1, srvCount = 3.
// ---------------------------------------------------------------------------

[[vk::binding(0, 0)]]
cbuffer VfxSimCB : register(b0) {
    uint  gPhase;    // kVfxPhaseUpdate | kVfxPhaseSpawn
    uint  gJobBase;  // first job index this dispatch owns
    uint  gJobCount; // jobs == thread groups
    float gDt;
};

// The particle store. A RWByteAddressBuffer rather than RWStructuredBuffer<GpuParticle>
// for the same reason ParticleGpu.hlsl reads a ByteAddressBuffer: ONE buffer carries
// both the 128-byte emitter record and the 64-byte particle records, and a structured
// buffer can only have one element type.
[[vk::binding(1, 0)]] RWByteAddressBuffer gVfxRecords : register(u0);

// One entry per THREAD GROUP: (emitterIndex, localBase, -, -). Built on the CPU, so a
// group never has to search for its emitter and the whole group is guaranteed to be
// working on ONE emitter - which is what makes the opcode wave-uniform.
[[vk::binding(2, 0)]] StructuredBuffer<uint4> gVfxJobs : register(t0);

// 12 uint4 (192 B) per emitter: [0..7] the 128-byte GpuEmitter render header this
// kernel copies into the record buffer, [8..11] the control words below.
[[vk::binding(3, 0)]] StructuredBuffer<uint4> gVfxEmitters : register(t1);

// The compiled program: 4 uint4 (64 B) per module - opcode + ModuleParams::f[12].
[[vk::binding(4, 0)]] StructuredBuffer<uint4> gVfxProgram : register(t2);

static const uint kVfxPhaseUpdate = 0u; // ParticleUpdate then ParticleCollide, [0, used)
static const uint kVfxPhaseSpawn  = 1u; // ParticleSpawn over this frame's new slots

static const uint kVfxEmitterStrideVec4 = 12u; // 192 B
static const uint kVfxModuleStrideVec4  = 4u;  // 64 B

// ModuleType (Source/Vfx/VfxStack.h). APPEND-ONLY - the value IS the catalog row
// index and authored assets store it, so these numbers are asset state.
static const uint kOpSpawnInitState    = 0u;
static const uint kOpSpawnInitColor    = 1u;
static const uint kOpSpawnInitSize     = 2u;
static const uint kOpSpawnInitRotation = 3u;
static const uint kOpConstantForce     = 4u;
static const uint kOpDrag              = 5u;
static const uint kOpCurlNoiseForce    = 6u;
static const uint kOpColorOverLife     = 7u;
static const uint kOpSizeOverLife      = 8u;
static const uint kOpRotationRate      = 9u;
static const uint kOpIntegrate         = 10u;
static const uint kOpCollidePlane      = 11u;
// 12..15 = Legacy.* - CPU-only, never reach this kernel (see the header note).

// Per-module RNG salts. Verbatim from VfxStack.cpp's anonymous namespace: without
// them two spawn modules initialising particle i would draw from the same stream
// position and their outputs would correlate.
static const uint kSaltState    = 0x5BD1E995u;
static const uint kSaltColor    = 0x27D4EB2Fu;
static const uint kSaltSize     = 0x165667B1u;
static const uint kSaltRotation = 0x9E3779B1u;

// ---------------------------------------------------------------------------
// Decoded per-emitter control block (gVfxEmitters[e*12 + 8 .. +11])
// ---------------------------------------------------------------------------
struct VfxEmitterCtl {
    uint  recordBase;   // element index of this emitter's 128-byte header
    uint  capacity;     // ring slots
    uint  used;         // high-water: the ParticleUpdate range
    uint  moduleCount;  // total modules in the program (update + collide + spawn)
    uint  programBase;  // uint4 index of module 0
    uint  updateCount;  // program[0 .. updateCount)                 = ParticleUpdate
    uint  collideCount; // program[updateCount .. +collideCount)     = ParticleCollide
    uint  spawnCount;   // program[.. +spawnCount)                   = ParticleSpawn
    uint  ringCursor;   // first ring slot this frame's spawns occupy
    uint  spawnN;       // particles spawned this frame (the `n` SpawnInitState needs)
    uint  spawnBase;    // EmitterState::spawnCounter - the RNG stream key
    uint  seed;         // CompiledStack::seed
    float time;         // EmitterState::emitterTime (curl-noise phase)
};

VfxEmitterCtl VfxLoadCtl(uint e) {
    const uint b = e * kVfxEmitterStrideVec4;
    const uint4 c0 = gVfxEmitters[b + 8u];
    const uint4 c1 = gVfxEmitters[b + 9u];
    const uint4 c2 = gVfxEmitters[b + 10u];
    const uint4 c3 = gVfxEmitters[b + 11u];
    VfxEmitterCtl k;
    k.recordBase = c0.x; k.capacity = c0.y; k.used = c0.z; k.moduleCount = c0.w;
    k.programBase = c1.x; k.updateCount = c1.y; k.collideCount = c1.z; k.spawnCount = c1.w;
    k.ringCursor = c2.x; k.spawnN = c2.y; k.spawnBase = c2.z; k.seed = c2.w;
    k.time = asfloat(c3.x);
    return k;
}

// ---------------------------------------------------------------------------
// Module operands
// ---------------------------------------------------------------------------
//
// One module = 4 uint4: (opcode, f0, f1, f2)(f3..f6)(f7..f10)(f11, -, -, -).
// `time`, `seed` and `spawnBase` are NOT per module - VfxStack.cpp's Stamp() writes
// the same three values into every module's ModuleParams every frame, so they live
// once in the emitter control block instead of 12 times in the program.
struct VfxModule {
    uint  op;
    float f[12];
};

VfxModule VfxLoadModule(uint base, uint index) {
    const uint o = base + index * kVfxModuleStrideVec4;
    const uint4 w0 = gVfxProgram[o + 0u];
    const uint4 w1 = gVfxProgram[o + 1u];
    const uint4 w2 = gVfxProgram[o + 2u];
    const uint4 w3 = gVfxProgram[o + 3u];
    VfxModule m;
    m.op = w0.x;
    m.f[0] = asfloat(w0.y); m.f[1] = asfloat(w0.z); m.f[2] = asfloat(w0.w);
    m.f[3] = asfloat(w1.x); m.f[4] = asfloat(w1.y); m.f[5] = asfloat(w1.z);
    m.f[6] = asfloat(w1.w);
    m.f[7] = asfloat(w2.x); m.f[8] = asfloat(w2.y); m.f[9] = asfloat(w2.z);
    m.f[10] = asfloat(w2.w);
    m.f[11] = asfloat(w3.x);
    return m;
}

// ---------------------------------------------------------------------------
// Record load / store
// ---------------------------------------------------------------------------

uint VfxSlotOffset(uint recordBase, uint slot) {
    // The emitter header occupies the first two 64-byte elements of the block, so
    // particle `slot` sits at (recordBase + 2 + slot) * 64 - the exact address
    // ParticleGpu.hlsl's VS reads when the batch base points at recordBase.
    return (recordBase + 2u + slot) * kVfxParticleBytes;
}

GpuParticle VfxLoadRecord(uint off) {
    const uint4 w0 = gVfxRecords.Load4(off + 0u);
    const uint4 w1 = gVfxRecords.Load4(off + 16u);
    const uint4 w2 = gVfxRecords.Load4(off + 32u);
    const uint4 w3 = gVfxRecords.Load4(off + 48u);
    GpuParticle p;
    p.position = asfloat(w0.xyz); p.age = asfloat(w0.w);
    p.velocity = asfloat(w1.xyz); p.lifetime = asfloat(w1.w);
    p.colorRG = w2.x; p.colorBA = w2.y;
    p.sizeX = asfloat(w2.z); p.sizeY = asfloat(w2.w);
    p.rotation = asfloat(w3.x); p.rotationRate = asfloat(w3.y);
    p.subImage = asfloat(w3.z); p.seed = w3.w;
    return p;
}

void VfxStoreRecord(uint off, GpuParticle p) {
    gVfxRecords.Store4(off + 0u,  uint4(asuint(p.position), asuint(p.age)));
    gVfxRecords.Store4(off + 16u, uint4(asuint(p.velocity), asuint(p.lifetime)));
    gVfxRecords.Store4(off + 32u, uint4(p.colorRG, p.colorBA, asuint(p.sizeX), asuint(p.sizeY)));
    gVfxRecords.Store4(off + 48u,
                       uint4(asuint(p.rotation), asuint(p.rotationRate), asuint(p.subImage),
                             p.seed));
}

// half4 pack - the GPU twin of glm::packHalf2x16 at the CPU gather site.
void VfxPackColor(inout GpuParticle p, float4 c) {
    p.colorRG = (f32tof16(c.r) & 0xFFFFu) | (f32tof16(c.g) << 16);
    p.colorBA = (f32tof16(c.b) & 0xFFFFu) | (f32tof16(c.a) << 16);
}

// ---------------------------------------------------------------------------
// RNG helpers (bit-exact twins of VfxStack.cpp / VfxTypes.h)
// ---------------------------------------------------------------------------

// vfx::SpawnRng - the spawn stream for particle `local` of this frame's batch.
uint VfxSpawnRng(uint seed, uint spawnBase, uint local, uint salt) {
    return VfxHashSeed(seed ^ salt, spawnBase + local);
}

// vfx::VarianceScale - derived from the particle's STORED seed, not the spawn stream,
// so an over-life module reproduces the identical scale on every later frame.
float VfxVarianceScale(uint particleSeed, uint salt, float variance) {
    if (variance == 0.0f) return 1.0f; // exactly 1.0 - never perturb the default path
    uint s = VfxHashSeed(particleSeed ^ salt, 0u);
    return 1.0f + VfxRngNextSigned(s) * variance;
}

// ---------------------------------------------------------------------------
// The kernels
// ---------------------------------------------------------------------------

// K_SpawnInitState. f[0..2] origin, f[3] emitRadius, f[4..6] direction, f[7] speed,
// f[8] speedVariance, f[9] lifetime, f[10] lifetimeVariance, f[11] spread.
//
// SUB-FRAME BIRTH: particle `local` of `n` is born (local+0.5)/n through the frame
// and is advanced by the REMAINDER here. That is what lets the update stage run over
// the pre-spawn range only, and what removes the "beads on a string" artefact when
// the emitter is moving.
void VfxSpawnInitState(inout GpuParticle p, VfxModule m, uint seed, uint spawnBase, uint local,
                       uint n, float dt) {
    const float3 origin = float3(m.f[0], m.f[1], m.f[2]);
    const float radius = m.f[3];
    float3 dir = float3(m.f[4], m.f[5], m.f[6]);
    dir = (dot(dir, dir) > 1e-12f) ? normalize(dir) : float3(0.0f, 1.0f, 0.0f);
    const float speed = m.f[7];
    const float speedVar = m.f[8];
    const float life = m.f[9];
    const float lifeVar = m.f[10];
    const float spread = clamp(m.f[11], 0.0f, 1.0f);
    const float invN = 1.0f / (float)max(1u, n);

    uint rng = VfxSpawnRng(seed, spawnBase, local, kSaltState);

    const float spawnT = ((float)local + 0.5f) * invN;
    const float subDt = dt * (1.0f - spawnT);

    // DRAW ORDER IS THE CONTRACT. K_SpawnInitState splits these into separate
    // statements because C++ leaves the operands of `*` unsequenced even for an
    // overloaded operator; reordering here would silently desynchronise the two
    // paths for the same seed. Same five draws, same order: offsetDir, offsetLen,
    // spreadDir, speed, lifetime.
    const float3 offsetDir = VfxRngNextUnit(rng);
    const float offsetLen = radius * VfxRngNext01(rng);
    const float3 offset = offsetDir * offsetLen;
    const float3 spreadDir = VfxRngNextUnit(rng);
    float3 v = lerp(dir, spreadDir, spread);
    const float vlen = length(v);
    v = (vlen > 1e-5f) ? (v / vlen) : dir;
    v *= speed * (1.0f + VfxRngNextSigned(rng) * speedVar);

    p.position = origin + offset + v * subDt;
    p.velocity = v;
    p.age = subDt;
    p.lifetime = max(0.05f, life * (1.0f + VfxRngNextSigned(rng) * lifeVar));
    // The FULL 32-bit stream state, not VfxPackSeedFlags. GpuParticle documents that
    // Seed and Flags alias on the GPU (flags in the high 8 bits), but no v1 module
    // READS a flag - kFlagAlive is implied by age < lifetime and kFlagCollided is
    // write-only - whereas VarianceScale hashes the seed and must see all 32 bits to
    // agree with the CPU. Fidelity where it is observable, aliasing where it is not.
    p.seed = rng;
}

// K_SpawnInitColor. f[0..3] rgba, f[4] brightness variance (hashed from the stored
// seed so Update.ColorOverLife reproduces the identical scale later).
void VfxSpawnInitColor(inout GpuParticle p, VfxModule m) {
    const float s = VfxVarianceScale(p.seed, kSaltColor, m.f[4]);
    VfxPackColor(p, float4(m.f[0] * s, m.f[1] * s, m.f[2] * s, m.f[3]));
}

// K_SpawnInitSize. f[0] x, f[1] y, f[2] variance (uniform on both axes).
void VfxSpawnInitSize(inout GpuParticle p, VfxModule m) {
    const float s = max(0.0f, VfxVarianceScale(p.seed, kSaltSize, m.f[2]));
    p.sizeX = m.f[0] * s;
    p.sizeY = m.f[1] * s;
}

// K_SpawnInitRotation. f[0] initial-angle range, f[1] rate min, f[2] rate max.
void VfxSpawnInitRotation(inout GpuParticle p, VfxModule m, uint seed, uint spawnBase,
                          uint local) {
    uint rng = VfxSpawnRng(seed, spawnBase, local, kSaltRotation);
    p.rotation = VfxRngNext01(rng) * m.f[0];
    p.rotationRate = m.f[1] + VfxRngNext01(rng) * (m.f[2] - m.f[1]);
}

// K_ConstantForce. f[0..2] acceleration.
void VfxConstantForce(inout GpuParticle p, VfxModule m, float dt) {
    p.velocity += float3(m.f[0] * dt, m.f[1] * dt, m.f[2] * dt);
}

// K_Drag. f[0] coefficient. EXPONENTIAL - exp(-drag*dt) is the exact solution of
// dv/dt = -drag*v, so it is framerate-independent and never overshoots through zero
// (the legacy `1 - drag*dt` form does both).
void VfxDrag(inout GpuParticle p, VfxModule m, float dt) {
    p.velocity *= exp(-max(0.0f, m.f[0]) * dt);
}

// K_CurlNoiseForce. f[0] strength, f[1] frequency. The CURL of a sinusoidal vector
// potential, so it is exactly divergence-free and particles swirl instead of piling
// into sinks.
void VfxCurlNoiseForce(inout GpuParticle p, VfxModule m, float t, float dt) {
    const float strength = m.f[0];
    const float freq = m.f[1];
    const float3 q = p.position * freq;
    const float3 curl = float3(-cos(q.z + 1.3f * t), -cos(q.x + 0.7f * t), -cos(q.y + t));
    p.velocity += curl * (strength * freq * dt);
}

// K_ColorOverLife. f[0..3] start rgba, f[4..7] end rgba, f[8] fadeIn, f[9] fadeOut,
// f[10] brightness variance.
void VfxColorOverLife(inout GpuParticle p, VfxModule m) {
    const float4 c0 = float4(m.f[0], m.f[1], m.f[2], m.f[3]);
    const float4 c1 = float4(m.f[4], m.f[5], m.f[6], m.f[7]);
    const float fadeIn = m.f[8], fadeOut = m.f[9];
    const float life = (p.lifetime > 1e-6f) ? p.lifetime : 1e-6f;
    const float t = clamp(p.age / life, 0.0f, 1.0f);
    float4 c = lerp(c0, c1, t);
    float env = 1.0f;
    if (fadeIn > 0.0f && t < fadeIn) env *= t / fadeIn;
    if (fadeOut > 0.0f && t > 1.0f - fadeOut) env *= (1.0f - t) / fadeOut;
    c.w *= clamp(env, 0.0f, 1.0f);
    const float s = VfxVarianceScale(p.seed, kSaltColor, m.f[10]);
    VfxPackColor(p, float4(c.x * s, c.y * s, c.z * s, c.w));
}

// K_SizeOverLife. f[0] startX, f[1] startY, f[2] endX, f[3] endY, f[4] variance.
void VfxSizeOverLife(inout GpuParticle p, VfxModule m) {
    const float life = (p.lifetime > 1e-6f) ? p.lifetime : 1e-6f;
    const float t = clamp(p.age / life, 0.0f, 1.0f);
    const float s = max(0.0f, VfxVarianceScale(p.seed, kSaltSize, m.f[4]));
    p.sizeX = lerp(m.f[0], m.f[2], t) * s;
    p.sizeY = lerp(m.f[1], m.f[3], t) * s;
}

// K_RotationRate.
void VfxRotationRate(inout GpuParticle p, float dt) { p.rotation += p.rotationRate * dt; }

// K_Integrate - the Euler integrator. Position from velocity, Age from dt. It is a
// real module rather than a hidden step so the stack reads the way it executes.
void VfxIntegrate(inout GpuParticle p, float dt) {
    p.position += p.velocity * dt;
    p.age += dt;
}

// K_CollidePlane. f[0..2] normal, f[3] offset d, f[4] restitution, f[5] friction.
// Runs in the ParticleCollide stage, which the program layout guarantees is AFTER
// integration - before it, the particle is tested at last frame's position and
// tunnels.
void VfxCollidePlane(inout GpuParticle p, VfxModule m) {
    float3 n = float3(m.f[0], m.f[1], m.f[2]);
    const float nlen = length(n);
    if (nlen < 1e-6f) return;
    n /= nlen;
    const float d = m.f[3] / nlen;
    const float restitution = clamp(m.f[4], 0.0f, 1.0f);
    const float friction = clamp(m.f[5], 0.0f, 1.0f);

    const float sd = dot(n, p.position) + d;
    if (sd >= 0.0f) return;
    p.position -= n * sd; // project back onto the plane
    const float vn = dot(n, p.velocity);
    if (vn < 0.0f) {
        const float3 normalPart = n * vn;
        const float3 tangentPart = p.velocity - normalPart;
        p.velocity = tangentPart * (1.0f - friction) - normalPart * restitution;
    }
}

// ---------------------------------------------------------------------------
// The interpreter
// ---------------------------------------------------------------------------

// Runs program[first, first+count) over ONE particle. The opcode is wave-uniform
// (same emitter, same module index, every lane), so this switch is a scalar branch.
void VfxRunModules(inout GpuParticle p, VfxEmitterCtl k, uint first, uint count, uint local,
                   float dt) {
    for (uint i = 0u; i < count; ++i) {
        const VfxModule m = VfxLoadModule(k.programBase, first + i);
        switch (m.op) {
            case kOpSpawnInitState:
                VfxSpawnInitState(p, m, k.seed, k.spawnBase, local, k.spawnN, dt);
                break;
            case kOpSpawnInitColor:    VfxSpawnInitColor(p, m); break;
            case kOpSpawnInitSize:     VfxSpawnInitSize(p, m); break;
            case kOpSpawnInitRotation: VfxSpawnInitRotation(p, m, k.seed, k.spawnBase, local); break;
            case kOpConstantForce:     VfxConstantForce(p, m, dt); break;
            case kOpDrag:              VfxDrag(p, m, dt); break;
            case kOpCurlNoiseForce:    VfxCurlNoiseForce(p, m, k.time, dt); break;
            case kOpColorOverLife:     VfxColorOverLife(p, m); break;
            case kOpSizeOverLife:      VfxSizeOverLife(p, m); break;
            case kOpRotationRate:      VfxRotationRate(p, dt); break;
            case kOpIntegrate:         VfxIntegrate(p, dt); break;
            case kOpCollidePlane:      VfxCollidePlane(p, m); break;
            default: break; // Legacy.* and anything a newer asset invented: no-op.
        }
    }
}

[numthreads(64, 1, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    const uint job = gJobBase + gid.x;
    if (gid.x >= gJobCount) return;
    const uint2 j = gVfxJobs[job].xy;
    const uint emitter = j.x;
    const uint localBase = j.y;
    const VfxEmitterCtl k = VfxLoadCtl(emitter);
    const uint lane = localBase + tid.x;

    if (gPhase == kVfxPhaseUpdate) {
        // The emitter's render header (camera basis, ramps, sub-UV grid, sprite
        // index, flags) is CPU data that changes every frame, and it has to live
        // INSIDE the record buffer because the batch is selected by pointing the
        // buffer base at it. One thread copies the 128 bytes; the CPU never touches
        // this device-local buffer at all. Every active emitter is guaranteed at
        // least one update group (even at used == 0) so this always runs.
        if (localBase == 0u && tid.x == 0u) {
            const uint hb = emitter * kVfxEmitterStrideVec4;
            const uint dst = k.recordBase * kVfxParticleBytes;
            [unroll] for (uint w = 0u; w < 8u; ++w) {
                gVfxRecords.Store4(dst + w * 16u, gVfxEmitters[hb + w]);
            }
        }
        if (lane >= k.used) return;

        const uint off = VfxSlotOffset(k.recordBase, lane);
        GpuParticle p = VfxLoadRecord(off);

        // Dead slot: hold it degenerate (zero size = zero-area quad, no fill) until
        // the ring cursor laps it. Re-running the modules on a corpse would keep
        // integrating its position toward infinity for nothing.
        if (p.age >= p.lifetime) {
            gVfxRecords.Store2(off + 40u, uint2(0u, 0u)); // sizeX, sizeY
            return;
        }

        // ParticleUpdate, then ParticleCollide. Both are per-particle passes over the
        // SAME lane, so one thread running them back to back reproduces the CPU
        // runner's two stages exactly - and the stage split (collide strictly after
        // the integrator) survives as the program's module ORDER.
        VfxRunModules(p, k, 0u, k.updateCount, 0u, gDt);
        VfxRunModules(p, k, k.updateCount, k.collideCount, 0u, gDt);

        // Retirement, expressed as geometry. The CPU pool swap-pops here; this ring
        // cannot, so a particle that just crossed its lifetime is collapsed now
        // rather than drawing one oversized frame before the next update catches it.
        if (p.age >= p.lifetime) { p.sizeX = 0.0f; p.sizeY = 0.0f; }
        VfxStoreRecord(off, p);
        return;
    }

    // --- Spawn ------------------------------------------------------------
    if (lane >= k.spawnN || k.capacity == 0u) return;
    const uint slot = (k.ringCursor + lane) % k.capacity;
    const uint off = VfxSlotOffset(k.recordBase, slot);

    // Zero first: the slot may hold a lapped corpse, and a spawn module only writes
    // the attributes it declares. The CPU path gets this for free because
    // ParticleSoA::Commit value-initialises every newly committed slot.
    GpuParticle p;
    p.position = float3(0.0f, 0.0f, 0.0f); p.age = 0.0f;
    p.velocity = float3(0.0f, 0.0f, 0.0f); p.lifetime = 1.0f;
    p.colorRG = 0u; p.colorBA = 0u;
    p.sizeX = 0.0f; p.sizeY = 0.0f;
    p.rotation = 0.0f; p.rotationRate = 0.0f;
    p.subImage = 0.0f; p.seed = 1u;

    const uint spawnFirst = k.updateCount + k.collideCount;
    VfxRunModules(p, k, spawnFirst, k.spawnCount, lane, gDt);
    VfxStoreRecord(off, p);
}
