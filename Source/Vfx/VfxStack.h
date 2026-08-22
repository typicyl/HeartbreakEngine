// Vfx/VfxStack.h - the module stack: a compiled, validated list of small kernels.
//
// An emitter is not a struct of 60 tuning knobs any more. It is an ORDERED LIST of
// modules, each one a tiny function over a contiguous slice of the pool, each one
// declaring which attributes it reads and which it writes. Module order IS the
// semantics - Drag before Gravity is a different effect than Gravity before Drag,
// and that is the point.
//
// WHY STAGES, AND WHY Collide IS ITS OWN STAGE
// A flat list plus read/write masks is NOT enough to keep a stack correct. A
// collision module reads Position and the integrator writes Position, so a
// read/write validator happily accepts "collide, then integrate" - the particle is
// tested against the world at LAST frame's position and then teleported through the
// wall. Read-before-write is not the same property as read-before-INTEGRATE.
// So collision gets its own stage, ordered after ParticleUpdate by construction:
// at runtime the broken order is UNREPRESENTABLE. The compiler additionally
// REJECTS an authored order that implies collide-before-integrate, so an artist who
// drags a collision module above the integrator gets a named error instead of a
// silent reorder that makes the stack behave differently from how it reads.
//
// WHY THE KERNEL IS PER-CHUNK, NOT PER-PARTICLE
// `Kernel` runs one module over particles [begin, end). It is called once per chunk
// per module, never once per particle, so the inner loop is a tight single-stream
// pass over an SoA array that the compiler can vectorise. That is what makes a
// 12-module stack cost about the same as one hand-written fused loop, and it is the
// reason the whole design is affordable at all.
//
// SCOPE - AND THIS IS LIVE CODE, NOT A SANDBOX. The CPU core here (model, catalog,
// compiler, validator, runner) is what every ParticleEmitter in every scene actually
// runs: Scene/ParticleSystem.cpp compiles each emitter's component fields into a stack
// of these modules, the opt-in flags are serialized, the editor exposes them, and
// particle::Update -> vfx::RunFrame is called from the frame loop every frame. Editing
// a kernel below changes shipped emitters. What is NOT here yet is the GPU path:
// Shaders/VfxCommon.hlsli + VfxSim.hlsl, the emitter-stage modules, and the .hbvfx
// asset all land in Phase 2 (Compile() rejects an emitter-stage module today rather
// than binning one that the runner would never execute).
//
// The compatibility contract is the load-bearing part: an untouched emitter compiles
// to the four Legacy.* modules, which are verbatim copies of the pre-stack fixed loop,
// and Scene/ParticleSystem.cpp's CompatSelfTest (--test-vfxcompat) proves bit-identity
// against a frozen oracle on every run. Do not "clean up" a Legacy.* kernel.
#pragma once

#include "Core/Types.h"
#include "Vfx/VfxTypes.h"

#include <string>
#include <string_view>
#include <vector>

namespace hbe {
namespace vfx {

// ---------------------------------------------------------------------------
// Stages
// ---------------------------------------------------------------------------

// Execution order is the enum order, and it is fixed. A module cannot choose its
// stage at author time - its stage is a property of what it does (ModuleDesc), so
// an illegal ordering cannot be expressed in an asset.
enum class ModuleStage : u8 {
    EmitterSpawn = 0,  // once, when the emitter (re)starts
    EmitterUpdate,     // once per frame, per emitter - decides spawn counts
    ParticleSpawn,     // over [oldCount, newCount) - initialises new particles
    ParticleUpdate,    // over [0, oldCount)        - forces, curves, integration
    ParticleCollide,   // over [0, oldCount), STRICTLY AFTER integration
    Count
};

constexpr u32 kStageCount = static_cast<u32>(ModuleStage::Count);

const char* StageName(ModuleStage s);

// ---------------------------------------------------------------------------
// Module parameters
// ---------------------------------------------------------------------------

// A 64-byte POD blob per module INSTANCE, resolved from the authored parameters at
// compile time. One fixed size means the whole stack's constants are one
// contiguous upload on the GPU path, and no kernel ever chases a pointer.
//
// The last three fields are STAMPED BY THE RUNNER each frame, not authored. They
// carry the only per-frame state a particle kernel is allowed to see, which is what
// keeps kernels pure: no kernel may read a wall clock, a global, or the scene.
struct ModuleParams {
    f32 f[12]{};       // module-specific constants; meaning is per ModuleType
    f32 time = 0.0f;   // stamped: EmitterState::emitterTime (noise/sub-UV phase)
    u32 seed = 0;      // stamped: the emitter's RNG key
    u32 spawnBase = 0; // stamped: EmitterState::spawnCounter at this spawn
    u32 pad = 0;
};
static_assert(sizeof(ModuleParams) == 64,
              "ModuleParams is uploaded as one 64-byte constant block per module - "
              "keep it at 64 so a 32-module stack is exactly 2 KB.");

// Runs ONE module over particles [begin, end) of ONE emitter.
// CONTRACT: a kernel may dereference exactly the streams named in its own
// ModuleDesc reads|writes. Compile() guarantees those streams are allocated; every
// other pointer in the view is null.
using Kernel = void (*)(ParticleView& p, const ModuleParams& k, u32 begin, u32 end, f32 dt);

// ---------------------------------------------------------------------------
// The module catalog
// ---------------------------------------------------------------------------

// APPEND-ONLY, like Attr: authored assets will store module identity. New modules
// go at the end, before Count.
enum class ModuleType : u16 {
    // ParticleSpawn
    SpawnInitState = 0, // position/velocity/age/lifetime/seed/flags - the base init
    SpawnInitColor,
    SpawnInitSize,
    SpawnInitRotation,
    // ParticleUpdate
    ConstantForce, // gravity and any other uniform acceleration
    Drag,
    CurlNoiseForce,
    ColorOverLife,
    SizeOverLife,
    RotationRate, // integrates Rotation from RotationRate
    Integrate,    // the Euler integrator - position and age
    // ParticleCollide
    CollidePlane,
    // Compatibility bridge for emitters authored before the module stack. These are
    // compound and CPU-only; see Vfx/VfxLegacy.h for why they are verbatim copies
    // rather than re-expressions in terms of the modules above.
    LegacyInitState,
    LegacyForces,
    LegacyDragLinear,
    LegacyIntegrate,
    Count
};

struct ModuleDesc {
    const char* name = "";
    ModuleStage stage = ModuleStage::ParticleUpdate;
    AttributeMask reads = 0;
    AttributeMask writes = 0;
    Kernel kernel = nullptr;
    // True for the module that consumes Velocity into Position. It is a PROPERTY of
    // the module rather than an identity test against ModuleType::Integrate, because
    // the legacy bridge has its own integrator: without this, Compile would append a
    // second one and every legacy particle would move at double speed.
    bool integrates = false;
};

// The catalog is the single source of truth for stage and masks. Out-of-range
// returns a null-kernel descriptor named "<invalid>" rather than reading past the
// table, so a corrupt asset degrades instead of crashing.
const ModuleDesc& Describe(ModuleType t);
const ModuleDesc* FindByName(std::string_view name);
constexpr u32 ModuleCount() { return static_cast<u32>(ModuleType::Count); }

// ---------------------------------------------------------------------------
// Authored stack -> compiled stack
// ---------------------------------------------------------------------------

// One authored entry. The authored list is FLAT and ordered exactly as the artist
// sees it in the stack UI; the compiler bins entries into stages and validates that
// the flat order does not contradict the stage order.
struct StackModule {
    ModuleType type = ModuleType::Count;
    ModuleParams params{};
    bool enabled = true;

    StackModule() = default;
    StackModule(ModuleType t, const ModuleParams& p) : type(t), params(p) {}
};

// How the spawn scheduler carries its fractional particle between frames.
//
// Exact64 is the default and the right answer (see EmitterState::spawnAccum for the
// measurement that justifies it). Legacy32 exists for one reason: emitters authored
// before the module stack accumulated in f32, and the two accumulators do not merely
// differ in total - they put spawns on DIFFERENT FRAMES, because the residues round
// differently. For an existing effect that is a visible change, so compatibility
// stacks keep the old arithmetic and only new stacks get the exact one.
enum class SpawnAccumMode : u8 { Exact64 = 0, Legacy32 };

// The authored emitter: the module list plus the handful of constants the runner
// itself owns (spawn scheduling is not a particle kernel - it decides how many
// particles exist, so it cannot be expressed as a pass over particles).
struct StackDesc {
    std::vector<StackModule> modules;
    u32 maxParticles = 1024;
    u32 seed = 0x9E3779B9u; // authored determinism seed
    f32 spawnRate = 32.0f;  // particles/second
    u32 burst = 0;          // one-shot burst at emitter start
    bool loop = true;
    f32 duration = 1.0f; // emission window when !loop
    SpawnAccumMode accum = SpawnAccumMode::Exact64;
};

struct CompiledModule {
    ModuleType type = ModuleType::Count;
    Kernel kernel = nullptr;
    ModuleParams params{};
    AttributeMask reads = 0;
    AttributeMask writes = 0;
};

// The compiled result. `usedAttrs` is the union of every enabled module's
// reads|writes and is exactly what ParticleSoA::Reserve is handed - that union IS
// the dead-stream elimination.
struct CompiledStack {
    std::vector<CompiledModule> stages[kStageCount];
    AttributeMask usedAttrs = 0;
    AttributeMask readMask = 0;
    AttributeMask writeMask = 0;

    // These five are re-stampable in place: the runner reads them every frame, so an
    // artist dragging Rate or Duration never triggers a recompile or an allocation.
    // Only the MODULE SET and maxParticles are structural.
    u32 maxParticles = 0;
    f32 spawnRate = 0.0f;
    u32 burst = 0;
    bool loop = true;
    f32 duration = 1.0f;

    u32 seed = 0;
    SpawnAccumMode accum = SpawnAccumMode::Exact64;
    bool valid = false;

    u32 ModuleCountTotal() const {
        u32 n = 0;
        for (u32 s = 0; s < kStageCount; ++s) n += static_cast<u32>(stages[s].size());
        return n;
    }
};

// Compiles and VALIDATES. Returns false and fills `errors` (newline-joined, one per
// problem, each naming the module and the attribute) on any of:
//   * an unknown / out-of-range ModuleType,
//   * a ParticleCollide-stage module authored BEFORE the integrator (see the header
//     note - this is the ordering trap that read/write masks alone cannot catch),
//   * a ParticleUpdate-stage module authored AFTER a ParticleCollide one, which is the
//     same trap in the other direction: stage binning would silently run it on the
//     PRE-collision velocity, so the stack would execute the opposite of how it reads,
//   * more than one integrator,
//   * an EmitterSpawn/EmitterUpdate-stage module, which the runner does not dispatch
//     yet and would therefore silently never execute,
//   * a module that READS an attribute no earlier module wrote and no ParticleSpawn
//     module initialises.
// On success, an implicit Integrate is appended to ParticleUpdate if the author did
// not place one, and `out` is ready to hand to Reserve()/RunFrame().
bool Compile(const StackDesc& def, CompiledStack& out, std::string* errors);

// ---------------------------------------------------------------------------
// Reference runner
// ---------------------------------------------------------------------------

struct RunStats {
    u32 spawned = 0;
    u32 killed = 0;
    u32 live = 0;
    u32 dropped = 0; // spawns refused because the pool was full
};

// One emitter, one frame. Order matters and is deliberate:
//   1. advance the emitter clocks
//   2. ParticleUpdate over [0, oldCount)   <- NOT the whole pool
//   3. ParticleCollide over [0, oldCount)
//   4. retire (swap-pop) everything with age >= lifetime
//   5. spawn: grant slots, run ParticleSpawn over the NEW range only
// Step 2 running over [0, oldCount) rather than [0, count) is the reason particles
// spawned this frame are not integrated twice. Their sub-frame catch-up is done by
// the spawn kernel instead (it advances each new particle by the fraction of the
// frame that remained when it was born), which also removes the "beads on a string"
// artefact when the emitter is moving.
//
// `user` is forwarded to every kernel as ParticleView::user and is only meaningful
// for the Legacy.* compatibility modules (Vfx/VfxLegacy.h); pass nullptr otherwise.
// `deathsOut`, if non-null, receives the world position of every particle retired this frame (before
// swap-pop overwrites it) - the seam for sub-emitters ("spawn a child effect where a particle died").
// Additive + default-null, so it costs nothing and changes nothing for callers that do not want it.
RunStats RunFrame(const CompiledStack& stack, EmitterState& em, ParticleSoA& pool, f32 dt,
                  const void* user = nullptr, std::vector<glm::vec3>* deathsOut = nullptr);

// Sizes `pool` for `stack` and RESETS it to zero live particles. Call once after
// Compile, never per frame. It commits an initial slice rather than the whole authored
// maxParticles (see ParticleSoA); Grant grows the commitment on demand.
void ReservePool(const CompiledStack& stack, ParticleSoA& pool);

// Order-independent content hash of the LIVE simulation state (the SoA streams in
// usedAttrs, up to count). Used by the determinism proof and, later, by the movie
// renderer's --vfxdeterminism check. Hashes SIM STATE, never render records: those
// carry bindless texture indices that depend on asset load order and half-precision
// colour, neither of which is a property of the simulation.
u64 HashPool(const ParticleSoA& pool);

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------

// Headless, GPU-free proof of the four properties this core has to have:
//   (a) sizeof(GpuParticle) == 64, at the exact field offsets the HLSL mirror reads,
//   (b) dead-stream elimination really skips unused streams (null view pointers AND
//       zero bytes held),
//   (c) validation REJECTS both directions of the ordering trap (a collide module
//       authored before integration, and an update module authored after a collide)
//       plus a read of an attribute nothing initialises, while accepting the legal
//       order - a validator that rejects everything would pass a one-sided test,
//   (d) determinism: same seed -> bit-identical pool after N frames; different seed
//       -> different pool. Plus the f64 spawn accumulator actually mattering.
// Returns true on pass; logs each sub-result. Wired to --test-vfxstack.
bool SelfTest();

} // namespace vfx
} // namespace hbe
