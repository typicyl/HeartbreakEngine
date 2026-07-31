// Scene/ParticleGpuSim.h - GPU particle SIMULATION (the compute half of the VFX path).
//
// Phase I2 moved the billboard EXPANSION to the vertex shader; the simulation stayed
// on the CPU and the CPU still gathered a 64-byte record per particle every frame.
// This removes that too: an emitter with ParticleEmitter::gpuSim keeps its particles
// in a device-local ring inside one shared buffer, a compute shader
// (Shaders/VfxSim.hlsl) advances them, and the SAME buffer is what ParticleGpu.hlsl's
// vertex shader reads. Per frame the CPU touches O(emitters) bytes, not O(particles).
//
// WHAT STAYS ON THE CPU, AND WHY IT IS NOT A COMPROMISE
//   * The spawn SCHEDULER. It owns an f64 accumulator whose exactness is measured by
//     vfx::SelfTest check (e), and moving it to the GPU would make the per-frame spawn
//     count a GPU-side value - which is precisely what forces indirect draw, a
//     D3D12 CommandSignature, and the two Vulkan features (multiDrawIndirect,
//     drawIndirectFirstInstance) this engine does not enable. Keeping it here costs
//     ~20 CPU instructions per emitter per frame and buys a plain
//     DrawInstanced(6*N,1,0,0) on both backends.
//   * The ring CURSOR and high-water mark, for the same reason: they are what make N
//     a CPU-known number.
// Everything per-PARTICLE - forces, curl noise, drag, the colour/size ramps,
// rotation, integration, collision, retirement, and the spawn initialisers with their
// RNG - runs in the compute shader.
//
// THE RING, AND ITS TWO HONEST DIVERGENCES FROM THE CPU POOL
// The CPU pool is a swap-pop array: retiring particle i moves the last live particle
// into its slot, so [0, count) is always dense. A compute shader cannot do that
// without a prefix sum or an atomic append, and either one makes the live count a GPU
// value again. So a GPU emitter gets a fixed-size RING sized from its authored rate
// and lifetime, a dead particle keeps its slot until the cursor laps it, and "dead"
// reaches the renderer as a ZERO SIZE (a zero-area quad - vertex work, no fill).
// Therefore:
//   1. POOL ORDER DIFFERS. Nothing downstream may depend on pool order anyway
//      (ParticleSoA::SwapPop already scrambles it, and the particle draw has never
//      sorted), but a hash of the pool will not match between the two paths.
//   2. OVERFLOW RECYCLES INSTEAD OF DROPPING. The CPU pool refuses a spawn when full;
//      the ring overwrites the oldest slot. The ring is sized at
//      burst + rate*maxLifetime + 25% + 8 (GpuRingCapacity below) precisely so that
//      the slot the cursor reaches is already dead in steady state.
#pragma once

#include "Core/Types.h"
#include "RHI/RHI.h"
#include "Vfx/VfxStack.h"

#include <glm/glm.hpp>

#include <filesystem>
#include <vector>

namespace hbe {

class Scene;
class Renderer;
struct ParticleEmitter;

namespace particle {

// Slots the ring needs to hold this emitter's steady state without lapping a live
// particle. A pure function of the authored fields, so the CPU spawn clamp and the
// suballocator agree without either of them owning the number.
u32 GpuRingCapacity(const ParticleEmitter& em);

// True if any emitter in the scene opted into GPU simulation. Cheap; the caller uses
// it to avoid creating a 16 MB buffer in projects that never do.
bool AnyGpuSim(Scene& scene);

// One frame of the CPU half for ONE gpuSim emitter: compile/stamp the v1 module
// stack and run the spawn scheduler. Called from particle::Update instead of
// vfx::RunFrame - the per-particle work is the compute shader's. `entityId` seeds the
// emitter's RNG key so two identical emitters in a scene do not produce the same
// particles at different origins.
void StepGpuEmitter(ParticleEmitter& em, const glm::mat4& world, f32 dt, bool emitting,
                    u32 entityId);

// The v1 module list a gpuSim emitter compiles to, and this frame's operands for it.
// Public for ONE reason: --test-vfxsim drives a CPU reference through vfx::RunFrame
// with the EXACT same stack and the EXACT same ModuleParams and compares it to what
// the compute shader produced. A parity test that rebuilt the stack by hand would be
// testing its own copy of the mapping, not the mapping.
vfx::StackDesc BuildGpuDesc(const ParticleEmitter& em);
void StampGpuParams(ParticleEmitter& em, const glm::mat4& world);

// Owns the shared GPU-simulation resources for one Engine. Everything is created
// lazily on the first frame a `gpuSim` emitter exists and never recreated.
class GpuSim {
public:
    struct Stats {
        u32 emitters = 0;   // emitters simulated this frame
        u32 slots = 0;      // ring slots dispatched over (the update range)
        u32 spawned = 0;    // particles born this frame
        u32 groups = 0;     // thread groups across both dispatches
        u32 dropped = 0;    // emitters that did not fit the record buffer
    };

    // One frame. Stamps each GPU emitter's program + control block, queues the two
    // dispatches (ParticleUpdate+Collide, then ParticleSpawn), and fills the draw
    // batches. Call BEFORE Renderer::RenderScene - both backends execute the compute
    // queue in their BeginFrame, because Vulkan cannot record compute inside a render
    // pass. Returns true when there is anything to draw.
    // `mapWaitMsOut`, when given, receives the time spent inside MapGpuBuffer. That
    // is GPU BACK-PRESSURE, not simulation work: Vulkan's MapGpuBuffer waits on this
    // ring slot's fence (D3D12 already waited at the end of the previous EndFrame), so
    // a phase timer that includes it reports the GPU stalling the CPU as CPU cost and
    // the two backends stop being comparable. Same exclusion the gpuExpand upload makes.
    bool Update(Scene& scene, Renderer& renderer, const std::filesystem::path& assetsDir,
                const glm::vec3& camRight, const glm::vec3& camUp, f32 dt,
                f64* mapWaitMsOut = nullptr);

    // This frame's draw payload (valid until the next Update).
    rhi::GpuBufferHandle Records() const { return records_; }
    const std::vector<rhi::GpuParticleBatch>& Batches() const { return batches_; }

    const Stats& GetStats() const { return stats_; }
    bool Failed() const { return failed_; }

    // Releases every buffer + the pipeline. Waits for GPU idle inside
    // DestroyGpuBuffer, so this is a shutdown call.
    void Shutdown(Renderer& renderer);

private:
    bool EnsureResources(Renderer& renderer);
    // Re-places every resident emitter's ring in the record buffer from scratch and
    // bumps the epoch (which resets their particles). Rare: scene load, a capacity
    // change, an emitter appearing or disappearing.
    void Reallocate(Scene& scene);

    rhi::GpuBufferHandle records_{};  // device-local: compute writes, the VS reads
    rhi::GpuBufferHandle emitters_{}; // CpuWrite: 192 B header+control per emitter
    rhi::GpuBufferHandle jobs_{};     // CpuWrite: one uint4 per thread group
    rhi::GpuBufferHandle program_{};  // CpuWrite: 64 B per module
    rhi::ComputePipelineHandle pipeline_{};
    std::vector<rhi::GpuParticleBatch> batches_;
    u32 epoch_ = 1;         // suballocation generation (0 means "never placed")
    u32 allocCursor_ = 0;   // bump allocator head, in 64-byte record elements
    u32 residentCount_ = 0; // emitters currently holding a block
    bool failed_ = false;   // creation failed once; do not retry every frame
    bool dropWarned_ = false; // one-shot: a scene-wide VFX outage must not be silent
    Stats stats_{};
};

} // namespace particle
} // namespace hbe
