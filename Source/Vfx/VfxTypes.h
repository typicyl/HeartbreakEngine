// Vfx/VfxTypes.h - the VFX attribute model: what a particle IS, everywhere.
//
// The whole module-stack system rests on one decision: the per-particle attribute
// set is a FIXED superset, not a per-emitter dynamic allocation. Niagara can
// allocate attributes per emitter because it JITs a VM; this engine cannot.
// Shaders are compiled at *cmake* time by DXC and loaded from packs - there is no
// runtime shader compilation - so a layout that varies per asset would require
// shipping a compiler. Instead there is ONE `GpuParticle` record, exactly 64 bytes,
// and modules declare a read/write MASK over that fixed set.
//
// The mask is not decoration. It drives three real things:
//   * dead-stream elimination on the CPU - a stack with no rotation module never
//     allocates the rotation streams at all (see ParticleSoA::Reserve),
//   * compile-time validation - "Color is read by SizeOverLife but never
//     initialised" is a named error instead of a silent read of garbage,
//   * the editor's stack UI, which can grey out modules whose inputs are missing.
//
// TWO LAYOUTS, ON PURPOSE:
//   CPU sim uses structure-of-arrays. A module's inner loop then walks one
//   contiguous stream, which autovectorises and lets unused streams cost zero.
//   GPU sim uses array-of-structs (`GpuParticle`), because a 64-byte record is
//   exactly one cache line, so a 32-lane wave touches 32 whole lines with no
//   straddle and no gather.
//
// RULE 2 (three-site constant lockstep) APPLIES TO `GpuParticle`. Its C++
// definition here, the HLSL mirror in Shaders/VfxCommon.hlsli, and the upload path
// are ONE layout. The static_assert below is the tripwire that keeps them honest;
// the HLSL side lands in Phase 2 and MUST be written against these exact offsets.
//
// APPEND-ONLY: `Attr`'s order IS the bit order of AttributeMask and the field order
// of GpuParticle. Authored .hbvfx assets store module read/write masks, so
// reordering the enum silently reinterprets every shipped effect. Add at the end,
// before Count, forever.
#pragma once

#include "Core/Types.h"

#include <glm/glm.hpp>

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace hbe {
namespace vfx {

// ---------------------------------------------------------------------------
// Attributes
// ---------------------------------------------------------------------------

// Every per-particle attribute the v1 module set can touch.
enum class Attr : u32 {
    Position = 0, // float3, world space
    Velocity,     // float3, world units/sec
    Age,          // float, seconds since spawn
    Lifetime,     // float, seconds; Age >= Lifetime -> dead
    Color,        // float4 linear HDR (stored as half4 on the GPU)
    Size,         // float2 world half-extents (x == y for round sprites)
    Rotation,     // float radians; ALSO the alignment angle in Stretched render mode
    RotationRate, // float rad/sec
    SubImage,     // float, FRACTIONAL sub-UV frame index (so frames can cross-blend)
    Seed,         // uint, per-particle RNG state (deterministic; see HashSeed)
    Flags,        // uint bitfield: Alive | Collided | KillPending | Custom0..
    Count
};

// Deliberately ABSENT, and why:
//   * PreviousPosition - exactly (Position - Velocity*dt) for the Euler integrator
//     this system uses, so storing it is pure waste.
//   * Mass - a per-emitter constant. No v1 module needs it per particle.
//   * MeshOrientation / RibbonID - Phase 4 renderers. They get a separate "wide"
//     128-byte buffer variant rather than bloating the common case to pay for a
//     feature 95% of effects never use.

using AttributeMask = u32;
static_assert(static_cast<u32>(Attr::Count) <= 32,
              "AttributeMask is a u32 - Attr::Count outgrew it. Widen the alias and "
              "every mask literal together, or split into two masks.");

constexpr AttributeMask AttrBit(Attr a) { return 1u << static_cast<u32>(a); }

// Handy composites.
constexpr AttributeMask kAttrNone = 0u;
constexpr AttributeMask kAttrAll = (1u << static_cast<u32>(Attr::Count)) - 1u;

// Stable debug/error-message name. Index-safe for any value < Attr::Count.
const char* AttrName(Attr a);

// Renders a mask as "Position|Velocity|Age" into a caller-owned string. Used by
// the compiler's validation diagnostics and the editor stack UI.
void AppendMaskNames(AttributeMask mask, std::string& out);

// ---------------------------------------------------------------------------
// Per-particle flags (the Attr::Flags bitfield)
// ---------------------------------------------------------------------------
constexpr u32 kFlagAlive = 1u << 0;
constexpr u32 kFlagCollided = 1u << 1; // set by a ParticleCollide-stage module
constexpr u32 kFlagKillPending = 1u << 2;

// ---------------------------------------------------------------------------
// GpuParticle - the 64-byte GPU record
// ---------------------------------------------------------------------------

// EXACTLY 64 bytes = one cache line. Mirrored byte-for-byte by `struct GpuParticle`
// in Shaders/VfxCommon.hlsli (Phase 2). glm::vec3 is 12 bytes with 4-byte alignment
// under this project's GLM configuration (GLM_FORCE_DEFAULT_ALIGNED_GENTYPES is NOT
// defined), which is what makes the C++ packing match HLSL's.
//
// `Flags` is packed into the HIGH 8 BITS of `seed` on the GPU: the xorshift stream
// only needs 24 bits of state to stay well distributed here, and re-hashing on read
// restores the avalanche. That is what keeps the record at 64 bytes instead of 80.
// Consequence, documented so nobody trips on it: for the GPU path AttrBit(Seed) and
// AttrBit(Flags) ALIAS. The CPU SoA path below keeps them as separate streams, so
// dead-stream elimination can still drop one without the other.
struct GpuParticle {
    glm::vec3 position;
    f32 age; //  0..15
    glm::vec3 velocity;
    f32 lifetime;         // 16..31
    u32 colorRG, colorBA; // 32..39  half4 linear HDR
    f32 sizeX, sizeY;     // 40..47
    f32 rotation, rotationRate; // 48..55
    f32 subImage;               // 56..59
    u32 seed;                   // 60..63  (flags in the high 8 bits - see above)
};

static_assert(sizeof(GpuParticle) == 64,
              "GpuParticle is mirrored byte-for-byte by `struct GpuParticle` in "
              "Shaders/VfxCommon.hlsli and consumed by Shaders/VfxSim.hlsl. RHI rule 2 "
              "applies: change this struct, the HLSL mirror, and the upload path "
              "together or not at all.");
static_assert(alignof(GpuParticle) == 4,
              "GpuParticle must have no tail padding - HLSL packs it at 4-byte "
              "granularity (mirror: Shaders/VfxCommon.hlsli).");
static_assert(offsetof(GpuParticle, position) == 0 && offsetof(GpuParticle, age) == 12 &&
                  offsetof(GpuParticle, velocity) == 16 && offsetof(GpuParticle, lifetime) == 28 &&
                  offsetof(GpuParticle, colorRG) == 32 && offsetof(GpuParticle, colorBA) == 36 &&
                  offsetof(GpuParticle, sizeX) == 40 && offsetof(GpuParticle, sizeY) == 44 &&
                  offsetof(GpuParticle, rotation) == 48 &&
                  offsetof(GpuParticle, rotationRate) == 52 &&
                  offsetof(GpuParticle, subImage) == 56 && offsetof(GpuParticle, seed) == 60,
              "GpuParticle field offsets drifted - the HLSL mirror in "
              "Shaders/VfxCommon.hlsli reads these exact byte offsets.");

// ---------------------------------------------------------------------------
// GpuEmitter - the 128-byte per-emitter render record
// ---------------------------------------------------------------------------

// GPU VERTEX EXPANSION needs every value that BuildVertices reads as a LOOP
// INVARIANT (the ramps, the fade envelope, the sub-UV grid, the render mode, the
// sprite index and the four mask bits). On the CPU those are just fields of the
// component the loop is sitting inside; in a vertex shader they have to be
// fetched, so they become a record.
//
// EXACTLY 128 bytes = 2 GpuParticle elements, and that is deliberate: the whole
// frame lives in ONE structured buffer with ONE 64-byte stride, laid out as
//   [emitter record][its particles][emitter record][its particles]...
// so a batch is bound by pointing the buffer's base at its emitter record. The VS
// then reads the emitter at byte 0 and particle i at byte 128 + i*64 - no
// per-particle emitter index, no second binding, and the base rides in the BUFFER
// OFFSET (D3D12 root-SRV GPU VA / Vulkan dynamic offset) rather than a
// firstInstance, so the SV_InstanceID vs gl_InstanceIndex divergence cannot arise.
//
// camRight/camUp live HERE rather than in FrameConstants on purpose. The basis the
// CPU path uses is roll-free and world-up derived (Engine.cpp builds it from
// Camera::Forward, deliberately ignoring Camera::up_), so reconstructing it from
// the view matrix in the shader would silently change how a rolled cinematic
// camera renders particles. Carrying it per emitter reproduces the CPU basis
// exactly and costs no three-site FrameCB/FrameUBO/FrameConstants lockstep change.
//
// Mirrored byte-for-byte by `struct GpuEmitter` in Shaders/VfxCommon.hlsli.
struct GpuEmitter {
    glm::vec4 startColor{1.0f};          //   0
    glm::vec4 endColor{1.0f};            //  16
    glm::vec4 sizeFade{0.0f};            //  32  x=startSize y=endSize z=fadeIn w=fadeOut
    u32 texIndex = 0;                    //  48  bindless sprite (0 = procedural soft dot)
    u32 subUV = 1u | (1u << 16);         //  52  cols | rows<<16
    f32 subUVFps = 0.0f;                 //  56  >0 loops the sheet; 0 plays it over life
    u32 flags = 0;                       //  60  see GpuEmitterFlag
    glm::vec3 camRight{1.0f, 0.0f, 0.0f}; //  64
    f32 stretch = 1.0f;                  //  76  Stretched aspect (>= 1)
    glm::vec3 camUp{0.0f, 1.0f, 0.0f};   //  80
    u32 renderMode = 0;                  //  92  ParticleEmitter::Render
    glm::vec4 _pad0{0.0f};               //  96
    glm::vec4 _pad1{0.0f};               // 112
};

// Which per-particle STREAMS actually exist for this emitter. These are not value
// selectors - dead-stream elimination makes an unused stream's memory not exist at
// all, so the CPU gather substitutes a default and the VS must know which case it
// is looking at (identical to the simColor/simSize/hasRot/hasVel booleans
// BuildVertices reads off ParticleSoA::Has).
namespace GpuEmitterFlag {
enum : u32 {
    SimColor = 1u << 0, // per-particle colour attribute (else the start/end ramp)
    SimSize = 1u << 1,  // per-particle size attribute (else the start/end ramp)
    HasRot = 1u << 2,   // rotation stream exists (else no spin)
    HasVel = 1u << 3,   // velocity stream exists (else Stretched degrades to Billboard)
};
}

static_assert(sizeof(GpuEmitter) == 128,
              "GpuEmitter must be exactly 2 GpuParticle elements - the render buffer is "
              "one stride and the VS reads particle i at 128 + i*64. Mirror: "
              "Shaders/VfxCommon.hlsli.");
static_assert(alignof(GpuEmitter) == 4,
              "GpuEmitter must have no implicit padding (glm vec types are packed under "
              "this project's GLM config; see the GpuParticle note above).");
static_assert(offsetof(GpuEmitter, startColor) == 0 && offsetof(GpuEmitter, endColor) == 16 &&
                  offsetof(GpuEmitter, sizeFade) == 32 && offsetof(GpuEmitter, texIndex) == 48 &&
                  offsetof(GpuEmitter, subUV) == 52 && offsetof(GpuEmitter, subUVFps) == 56 &&
                  offsetof(GpuEmitter, flags) == 60 && offsetof(GpuEmitter, camRight) == 64 &&
                  offsetof(GpuEmitter, stretch) == 76 && offsetof(GpuEmitter, camUp) == 80 &&
                  offsetof(GpuEmitter, renderMode) == 92,
              "GpuEmitter field offsets drifted - Shaders/VfxCommon.hlsli decodes these exact "
              "byte offsets out of a ByteAddressBuffer.");

// Flags <-> seed packing helpers (GPU aliasing; see the note on GpuParticle).
constexpr u32 SeedOf(u32 packed) { return packed & 0x00FFFFFFu; }
constexpr u32 FlagsOf(u32 packed) { return packed >> 24; }
constexpr u32 PackSeedFlags(u32 seed, u32 flags) {
    return (seed & 0x00FFFFFFu) | ((flags & 0xFFu) << 24);
}

// ---------------------------------------------------------------------------
// Deterministic RNG
// ---------------------------------------------------------------------------

// PCG-style hash. Every particle's stream is derived PURELY from (emitter seed,
// spawn index) - never a global counter, never a pointer, never a wall clock. That
// is what makes a movie capture reproducible: each emitter owns its own stream, so
// emitter ITERATION ORDER cannot influence any particle's values.
constexpr u32 HashSeed(u32 emitterSeed, u32 spawnIndex) {
    u32 s = emitterSeed ^ (spawnIndex * 0x9E3779B9u);
    s ^= s >> 16;
    s *= 0x7FEB352Du;
    s ^= s >> 15;
    s *= 0x846CA68Bu;
    s ^= s >> 16;
    return s | 1u; // never 0: xorshift's fixed point
}

// xorshift32 stream. Trivially copyable, lives on the stack inside a kernel loop.
struct Rng {
    u32 state = 1u;

    u32 NextU32() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
    // [0,1). 24 bits of mantissa - exactly what an f32 can represent losslessly.
    f32 Next01() { return static_cast<f32>(NextU32() & 0x00FFFFFFu) / 16777216.0f; }
    // [-1,1].
    f32 NextSigned() { return Next01() * 2.0f - 1.0f; }
    // Uniform direction on the unit sphere (inverse-CDF on z, so no rejection loop -
    // a rejection loop would make cost data-dependent and hurt vectorisation).
    glm::vec3 NextUnit() {
        const f32 z = NextSigned();
        const f32 a = Next01() * 6.28318530718f;
        const f32 r = std::sqrt(glm::max(0.0f, 1.0f - z * z));
        return glm::vec3(r * std::cos(a), r * std::sin(a), z);
    }
};

// ---------------------------------------------------------------------------
// ParticleSoA - the CPU pool
// ---------------------------------------------------------------------------

// Structure-of-arrays pool for ONE emitter instance. Streams are allocated only for
// attributes in the compiled stack's union mask, so a stack with no rotation module
// never pays for rotation/rotationRate at all - that is the dead-stream elimination
// the read/write masks exist to enable.
//
// COMMITTED CAPACITY vs AUTHORED CAP. `maxParticles` is a CEILING an artist sets, not
// a prediction: the editor allows 200 000, far more than the 6 MB particle vertex
// buffer can even draw, and an emitter authored at rate 24 / lifetime 2 will never
// hold more than ~50 of them. Committing the ceiling up front would have such an
// emitter value-initialise ~8 MB of pages on scene load for a working set of a few
// kilobytes - a real regression against already-authored content, because the
// pre-stack pool was push_back-grown and tracked the LIVE count.
//
// So Reserve() commits min(cap, kInitialCommit) and Grant() doubles on demand up to
// the ceiling. The property that actually matters is preserved exactly: growth is
// geometric (O(log) reallocations over an emitter's whole life, none in steady state)
// and it happens ONLY in Grant, i.e. between the update stage and the spawn stage,
// never underneath a running kernel. The runner re-binds its ParticleView straight
// after Grant for precisely this reason.
struct ParticleSoA {
    std::vector<glm::vec3> position, velocity;
    std::vector<f32> age, lifetime, sizeX, sizeY, rotation, rotationRate, subImage;
    std::vector<glm::vec4> color;
    std::vector<u32> seed, flags;

    u32 count = 0;          // live particles
    u32 capacity = 0;       // slots actually committed; streams are sized to this
    u32 maxCapacity = 0;    // the authored ceiling (stack.maxParticles)
    AttributeMask used = 0; // which streams are actually backed by memory

    // First commit. Large enough that the overwhelming majority of emitters never
    // grow at all (the component default cap is 512), small enough that an absurd
    // authored ceiling costs nothing until particles actually exist.
    static constexpr u32 kInitialCommit = 1024;

    bool Has(Attr a) const { return (used & AttrBit(a)) != 0u; }

    // Sizes the live streams and frees every dead one. Safe to call again with a
    // different mask/capacity (a recompile after an edit); it resets `count`.
    void Reserve(u32 cap, AttributeMask usedMask) {
        used = usedMask;
        maxCapacity = cap;
        count = 0;
        Commit(cap < kInitialCommit ? cap : kInitialCommit, false);
    }

    // Hands out `request` contiguous slots at the end, growing the commitment
    // geometrically if there is headroom under the authored ceiling. Returns how many
    // were actually granted; `firstOut` is the first index.
    // OVERFLOW POLICY IS DROP, and it is explicit here rather than buried at two
    // separate spawn sites the way the old system had it.
    u32 Grant(u32 request, u32& firstOut) {
        firstOut = count;
        if (request > capacity - count && capacity < maxCapacity) {
            u64 want = static_cast<u64>(count) + static_cast<u64>(request);
            u64 grow = capacity ? static_cast<u64>(capacity) * 2u : 64u;
            if (grow < want) grow = want;
            if (grow > maxCapacity) grow = maxCapacity;
            Commit(static_cast<u32>(grow), true); // preserve the live particles
        }
        const u32 room = capacity - count;
        const u32 n = request < room ? request : room;
        count += n;
        return n;
    }

    // Retire particle `i` in O(1) by moving the last live particle into its slot.
    // Order-destructive by design: nothing downstream may depend on pool order (the
    // renderer sorts by view depth when it needs to).
    void SwapPop(u32 i) {
        if (count == 0) return;
        const u32 last = count - 1;
        if (i != last) {
            if (Has(Attr::Position)) position[i] = position[last];
            if (Has(Attr::Velocity)) velocity[i] = velocity[last];
            if (Has(Attr::Age)) age[i] = age[last];
            if (Has(Attr::Lifetime)) lifetime[i] = lifetime[last];
            if (Has(Attr::Color)) color[i] = color[last];
            if (Has(Attr::Size)) {
                sizeX[i] = sizeX[last];
                sizeY[i] = sizeY[last];
            }
            if (Has(Attr::Rotation)) rotation[i] = rotation[last];
            if (Has(Attr::RotationRate)) rotationRate[i] = rotationRate[last];
            if (Has(Attr::SubImage)) subImage[i] = subImage[last];
            if (Has(Attr::Seed)) seed[i] = seed[last];
            if (Has(Attr::Flags)) flags[i] = flags[last];
        }
        count = last;
    }

    void Clear() { count = 0; }

    // Total heap bytes actually held by the streams. This is what makes dead-stream
    // elimination MEASURABLE rather than merely asserted.
    usize BytesAllocated() const {
        return Bytes(position) + Bytes(velocity) + Bytes(age) + Bytes(lifetime) + Bytes(color) +
               Bytes(sizeX) + Bytes(sizeY) + Bytes(rotation) + Bytes(rotationRate) +
               Bytes(subImage) + Bytes(seed) + Bytes(flags);
    }

private:
    // `preserve` is the difference between a recompile (throw the pool away) and a
    // growth (keep every live particle where it is). Both value-initialise the new
    // tail, so a freshly committed slot is never read as stale data.
    void Commit(u32 cap, bool preserve) {
        capacity = cap;
        SizeStream(position, cap, Has(Attr::Position), preserve);
        SizeStream(velocity, cap, Has(Attr::Velocity), preserve);
        SizeStream(age, cap, Has(Attr::Age), preserve);
        SizeStream(lifetime, cap, Has(Attr::Lifetime), preserve);
        SizeStream(color, cap, Has(Attr::Color), preserve);
        SizeStream(sizeX, cap, Has(Attr::Size), preserve);
        SizeStream(sizeY, cap, Has(Attr::Size), preserve);
        SizeStream(rotation, cap, Has(Attr::Rotation), preserve);
        SizeStream(rotationRate, cap, Has(Attr::RotationRate), preserve);
        SizeStream(subImage, cap, Has(Attr::SubImage), preserve);
        SizeStream(seed, cap, Has(Attr::Seed), preserve);
        SizeStream(flags, cap, Has(Attr::Flags), preserve);
    }

    template <class T>
    static void SizeStream(std::vector<T>& v, u32 cap, bool on, bool preserve) {
        if (on) {
            if (v.size() != cap) {
                if (!preserve) v.clear();
                v.resize(cap); // value-initialised
            }
        } else if (!v.empty() || v.capacity() != 0) {
            v.clear();
            v.shrink_to_fit(); // an unused stream must hold ZERO bytes
        }
    }
    template <class T>
    static usize Bytes(const std::vector<T>& v) {
        return v.capacity() * sizeof(T);
    }
};

// ---------------------------------------------------------------------------
// ParticleView - what a kernel actually sees
// ---------------------------------------------------------------------------

// Raw pointers into the SoA so a kernel is a plain function pointer with zero
// indirection per particle. A stream the compiled stack does not use is NULL, so a
// kernel can never read garbage from an attribute it did not declare.
//
// It does NOT fault, and it is worth being precise about that: every kernel opens
// with a defensive early return on its own pointers, so a module that reaches outside
// its declared reads|writes silently NO-OPS for the whole frame - the harder failure
// to diagnose, not the easier one. Two things make it loud instead. Compile()'s
// read/write validation is the primary guard (it refuses the stack before it ever
// runs), and RunStage carries a debug-only assertion that a module's declared mask is
// a subset of what the pool actually allocated, reported by module name.
//
// The last two members are NOT per-particle streams and exist for ONE reason: the
// Legacy.* compatibility modules (Vfx/VfxLegacy.h), which have to reproduce the
// pre-module-stack emitter BIT-EXACTLY so that already-authored scenes keep
// rendering the way they render today.
//   * `emitterRng` is the emitter's single sequential xorshift stream. The legacy
//     spawn path drew from it in a fixed order with a per-shape variable number of
//     draws; that stream cannot be reconstructed from a per-particle hash, so the
//     kernel must be allowed to advance it. Consequence: any module that touches
//     it is SERIAL BY CONSTRUCTION and must never be handed to jobs::ParallelFor.
//   * `user` is an opaque pointer to the emitter's own parameter block. A legacy
//     emitter needs a 3x3 basis plus ~25 scalars to sample its spawn shape, which
//     does not fit in the 64-byte, GPU-shaped ModuleParams - and never needs to,
//     because the legacy modules are a CPU-only migration bridge. Real modules use
//     ModuleParams and stay uploadable.
// Both are null for every stack that contains no legacy module, and every legacy
// kernel null-checks them, so the normal path is unaffected.
struct ParticleView {
    glm::vec3* position = nullptr;
    glm::vec3* velocity = nullptr;
    f32* age = nullptr;
    f32* lifetime = nullptr;
    f32* sizeX = nullptr;
    f32* sizeY = nullptr;
    f32* rotation = nullptr;
    f32* rotationRate = nullptr;
    f32* subImage = nullptr;
    glm::vec4* color = nullptr;
    u32* seed = nullptr;
    u32* flags = nullptr;
    u32 count = 0;

    u32* emitterRng = nullptr;   // emitter-level RNG stream (legacy compat only)
    const void* user = nullptr;  // emitter parameter block (legacy compat only)
};

// Per-particle deterministic variance, derived from the particle's OWN stored seed
// rather than from a spawn-order stream. That distinction matters: a spawn-order
// draw can only be made once (at birth), so an over-life module could not reproduce
// it and would wipe the variation on the particle's second frame. Hashing the stored
// seed gives the same scale at spawn and on every subsequent frame, which is what
// lets Spawn.InitSize and Update.SizeOverLife agree.
inline f32 VarianceScale(u32 particleSeed, u32 salt, f32 variance) {
    if (variance == 0.0f) return 1.0f; // exactly 1.0 - never perturb the default path
    Rng r{HashSeed(particleSeed ^ salt, 0u)};
    return 1.0f + r.NextSigned() * variance;
}

inline ParticleView MakeView(ParticleSoA& pool) {
    ParticleView v;
    // Keyed on the mask, NOT on vector::data(), because data() on an empty vector is
    // allowed to return a non-null pointer.
    if (pool.Has(Attr::Position)) v.position = pool.position.data();
    if (pool.Has(Attr::Velocity)) v.velocity = pool.velocity.data();
    if (pool.Has(Attr::Age)) v.age = pool.age.data();
    if (pool.Has(Attr::Lifetime)) v.lifetime = pool.lifetime.data();
    if (pool.Has(Attr::Color)) v.color = pool.color.data();
    if (pool.Has(Attr::Size)) {
        v.sizeX = pool.sizeX.data();
        v.sizeY = pool.sizeY.data();
    }
    if (pool.Has(Attr::Rotation)) v.rotation = pool.rotation.data();
    if (pool.Has(Attr::RotationRate)) v.rotationRate = pool.rotationRate.data();
    if (pool.Has(Attr::SubImage)) v.subImage = pool.subImage.data();
    if (pool.Has(Attr::Seed)) v.seed = pool.seed.data();
    if (pool.Has(Attr::Flags)) v.flags = pool.flags.data();
    v.count = pool.count;
    return v;
}

// ---------------------------------------------------------------------------
// EmitterState - per-emitter simulation state
// ---------------------------------------------------------------------------

// Emitter-stage modules read/write this; particle-stage modules see it as
// constants stamped into their ModuleParams.
struct EmitterState {
    // f64, NOT f32 - but for a narrower reason than the usual telling, and the
    // difference is worth stating because the usual telling is wrong here.
    //
    // The often-repeated argument is "f32 loses integer precision past 2^24". That
    // does NOT apply to this accumulator: the residue is reduced by floor() every
    // frame, so it lives in [0,1) and never approaches 2^24. VfxStack's SelfTest
    // check (e) measures it - at rate 1234/s over a 60-minute fixed-dt capture the
    // f32 and f64 accumulators emit the SAME total, to the particle.
    //
    // What f32 actually costs is quantisation of the residue to the ulp of
    // (residue + rate*dt), which grows with the rate. MEASURED by that same check:
    // f32 and f64 agree exactly up to rate 1e4/s, and first disagree at rate 1e5/s -
    // an ordinary GPU-sim emitter rate - where a 60-minute capture comes out one
    // particle apart. One particle is enough: a movie re-render no longer matches
    // the previous take. f64 makes the scheduler exact independently of rate and
    // capture length for 8 bytes per emitter. It is the right default; it is just
    // not a 2^24 fix.
    f64 spawnAccum = 0.0;

    // The f32 twin, used ONLY by SpawnAccumMode::Legacy32. It is not a fallback and
    // not a perf option: emitters authored before the module stack had an f32
    // accumulator, and swapping in the f64 one changes WHICH FRAME a spawn lands on
    // (the residues round differently), which is a visible change to an existing
    // effect. Compatibility stacks keep the old arithmetic; every new stack gets the
    // exact one. See VfxStack.h SpawnAccumMode.
    f32 spawnAccum32 = 0.0f;

    f32 emitterAge = 0.0f;  // seconds since the current loop started
    f32 emitterTime = 0.0f; // never resets - the noise/sub-UV phase clock

    // Declared for the emitter-stage modules that land in Phase 2. Phase 1's
    // reference runner does not restart loops, so it stays 0 - do not read it yet.
    u32 loopCount = 0;

    u32 spawnCounter = 0; // MONOTONIC. The RNG stream key; see HashSeed.
    u32 rngState = 0;     // emitter-level stream (spawn counts, bursts)
    u32 spawnThisFrame = 0;

    bool emitting = true;
    bool wasEmitting = false;
    bool burstFired = false;

    glm::mat4 world{1.0f};
    glm::vec3 prevOrigin{0.0f};
    glm::vec3 originVelocity{0.0f}; // for spawn-time velocity inheritance

    // Resets everything derived, keeping authored fields. Used by the editor when an
    // emitter restarts and by the determinism self-test.
    void Reset(u32 seed) {
        spawnAccum = 0.0;
        spawnAccum32 = 0.0f;
        emitterAge = 0.0f;
        emitterTime = 0.0f;
        loopCount = 0;
        spawnCounter = 0;
        rngState = HashSeed(seed, 0u);
        spawnThisFrame = 0;
        emitting = true;
        wasEmitting = false;
        burstFired = false;
        world = glm::mat4(1.0f);
        prevOrigin = glm::vec3(0.0f);
        originVelocity = glm::vec3(0.0f);
    }
};

} // namespace vfx
} // namespace hbe
