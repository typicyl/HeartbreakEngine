// Scene/ParticleGpuSim.cpp - see the header for the design and its two documented
// divergences from the CPU pool.
#include "Scene/ParticleGpuSim.h"

#include "Assets/AssetLoader.h"
#include "Core/Log.h"
#include "Renderer/Renderer.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Vfx/VfxStack.h"
#include "Vfx/VfxTypes.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace hbe::particle {

namespace {

using MT = vfx::ModuleType;

// --------------------------------------------------------------------------
// Sizing constants. Every one of these is a CEILING the CPU checks, not a cost:
// the record buffer is one allocation, the other three are per-frame rings whose
// written prefix is proportional to the emitters actually present.
// --------------------------------------------------------------------------

// 16 MB / 64 B. At the 25%-slack ring sizing that is ~200k simultaneous GPU
// particles across the whole scene - well past the point where the CPU path's 6 MB
// vertex ring silently truncated (26 214 particles).
constexpr u32 kRecordElements = 262144;
constexpr u32 kMaxEmitters = 512;
constexpr u32 kMaxModules = 16;   // the v1 catalog is 12 opcodes; a stack cannot exceed this
constexpr u32 kMaxJobs = 16384;   // thread groups per frame across BOTH dispatches
constexpr u32 kGroupSize = 64;    // must equal VfxSim.hlsl's [numthreads(64,1,1)]
constexpr u32 kEmitterVec4 = 12;  // 192 B: 128 B render header + 64 B control block
constexpr u32 kModuleVec4 = 4;    // 64 B: opcode + ModuleParams::f[12]

// Each emitter's block starts on a 256-byte boundary (4 elements). The batch base
// reaches Vulkan as a DYNAMIC STORAGE-BUFFER OFFSET, which must be a multiple of
// minStorageBufferOffsetAlignment - 32 on this project's dev GPU but 256 on several
// shipping parts. Aligning here costs at most 3 wasted slots per emitter and makes
// the placement legal on every desktop limit rather than on the one it was tested on.
constexpr u32 kBlockAlign = rhi::kGpuParticleBlockAlign;

// VfxSim.hlsl's phase selector.
constexpr u32 kPhaseUpdate = 0;
constexpr u32 kPhaseSpawn = 1;

struct SimCB {
    u32 phase = 0;
    u32 jobBase = 0;
    u32 jobCount = 0;
    f32 dt = 0.0f;
};
static_assert(sizeof(SimCB) == 16, "VfxSim.hlsl's cbuffer is four words.");

inline u32 AsU32(f32 v) {
    u32 u = 0;
    std::memcpy(&u, &v, sizeof(u));
    return u;
}

// True when this emitter wants the curl-noise force. It is the only OPTIONAL module
// in the GPU stack, so it is the only thing besides the ring size that can change the
// program's shape - hence the only thing besides those in the signature below.
inline bool WantsCurl(const ParticleEmitter& em) {
    return em.useCurlNoise ? (em.curlStrength != 0.0f) : (em.turbulence > 0.0f);
}

// Structural hash. Only these change the SHAPE of the program or the ring; every
// other authored field is re-stamped in place every frame, so dragging a slider never
// recompiles and never re-places the emitter (which would restart the effect).
u64 GpuStackSignature(const ParticleEmitter& em) {
    u64 h = 0x51ED270B3A17C9E1ull; // distinct from the CPU path's seed so toggling
    const auto mix = [&h](u64 v) { // gpuSim always forces a recompile
        h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    };
    mix(GpuRingCapacity(em));
    mix(WantsCurl(em) ? 1ull : 0ull);
    return h ? h : 1ull;
}

} // namespace

// The authored module list for a GPU emitter: the v1 catalog, in the order an artist
// would build it. Deliberately NOT the Legacy.* spine - those four modules read
// ParticleView::user and ParticleView::emitterRng and cannot exist in a shader (see
// the note on ParticleEmitter::gpuSim).
//
// Size is ALWAYS simulated, and that is structural rather than a preference: the ring
// signals a dead slot to the renderer by zeroing sizeX, so the vertex shader has to be
// reading the per-particle size rather than the start/end ramp or a corpse would draw
// at full size until the cursor lapped it.
vfx::StackDesc BuildGpuDesc(const ParticleEmitter& em) {
    vfx::StackDesc d;
    d.maxParticles = GpuRingCapacity(em);
    d.seed = em.gpuSeed ? em.gpuSeed : 0x9E3779B9u;
    d.spawnRate = em.rate;
    d.burst = em.burst;
    d.loop = em.loop;
    d.duration = em.duration;
    // Exact64, not the CPU path's Legacy32. The compatibility argument for the f32
    // carry is that it keeps ALREADY-AUTHORED emitters spawning on the same frames;
    // a GPU emitter is a new configuration by construction, so it gets the exact
    // accumulator (vfx::SelfTest check (e) measures why that matters at high rates).
    d.accum = vfx::SpawnAccumMode::Exact64;
    d.modules.reserve(kMaxModules);

    d.modules.emplace_back(MT::SpawnInitState, vfx::ModuleParams{});
    d.modules.emplace_back(MT::SpawnInitColor, vfx::ModuleParams{});
    d.modules.emplace_back(MT::SpawnInitSize, vfx::ModuleParams{});
    d.modules.emplace_back(MT::SpawnInitRotation, vfx::ModuleParams{});

    d.modules.emplace_back(MT::ConstantForce, vfx::ModuleParams{});
    if (WantsCurl(em)) d.modules.emplace_back(MT::CurlNoiseForce, vfx::ModuleParams{});
    d.modules.emplace_back(MT::Drag, vfx::ModuleParams{});
    d.modules.emplace_back(MT::Integrate, vfx::ModuleParams{});
    // After the integrator, exactly as the CPU path orders them: they touch neither
    // position nor velocity and must see THIS frame's age.
    d.modules.emplace_back(MT::ColorOverLife, vfx::ModuleParams{});
    d.modules.emplace_back(MT::SizeOverLife, vfx::ModuleParams{});
    d.modules.emplace_back(MT::RotationRate, vfx::ModuleParams{});
    return d;
}

// This frame's authored values into the compiled modules. O(modules), no allocation.
// The world matrix is folded in here (origin + rotated direction) because a v1 spawn
// module takes plain numbers - it has no basis to sample a shape with.
void StampGpuParams(ParticleEmitter& em, const glm::mat4& world) {
    const glm::vec3 origin(world[3]);
    const glm::mat3 basis(world);
    const glm::vec3 dir = glm::dot(em.direction, em.direction) > 1e-6f
                              ? glm::normalize(basis * em.direction)
                              : glm::vec3(0.0f, 1.0f, 0.0f);
    const f32 spawnEnv = em.fadeIn > 0.0f ? 0.0f : 1.0f;

    for (u32 s = 0; s < vfx::kStageCount; ++s) {
        for (vfx::CompiledModule& m : em.stack.stages[s]) {
            vfx::ModuleParams& p = m.params;
            switch (m.type) {
                case MT::SpawnInitState:
                    p.f[0] = origin.x; p.f[1] = origin.y; p.f[2] = origin.z;
                    p.f[3] = em.emitRadius;
                    p.f[4] = dir.x; p.f[5] = dir.y; p.f[6] = dir.z;
                    p.f[7] = em.startSpeed;
                    p.f[8] = em.speedVariance;
                    p.f[9] = em.lifetime;
                    p.f[10] = em.lifetimeVariance;
                    p.f[11] = em.spread;
                    break;
                case MT::SpawnInitColor:
                    p.f[0] = em.startColor.r; p.f[1] = em.startColor.g; p.f[2] = em.startColor.b;
                    // The envelope at t == 0, so a particle's first frame matches its
                    // second (the update stage never sees the particles born this frame).
                    p.f[3] = em.startColor.a * spawnEnv;
                    p.f[4] = em.colorVariance;
                    break;
                case MT::SpawnInitSize:
                    p.f[0] = em.startSize; p.f[1] = em.startSize; p.f[2] = em.sizeVariance;
                    break;
                case MT::SpawnInitRotation:
                    p.f[0] = 6.2831853f;  // initial angle anywhere on the circle
                    p.f[1] = em.spin;     // min == max: the legacy `rot += spin*dt`
                    p.f[2] = em.spin;
                    break;
                case MT::ConstantForce:
                    p.f[0] = em.gravity.x; p.f[1] = em.gravity.y; p.f[2] = em.gravity.z;
                    break;
                case MT::CurlNoiseForce:
                    // `turbulence` maps onto curl noise when the artist has not
                    // explicitly enabled the curl module. It is not the same field -
                    // the legacy one has non-zero divergence and clumps - but it is
                    // the nearest v1 expression of "make it swirl this much".
                    p.f[0] = em.useCurlNoise ? em.curlStrength : em.turbulence;
                    p.f[1] = em.useCurlNoise ? em.curlFrequency : em.turbulenceScale;
                    break;
                case MT::Drag:
                    p.f[0] = em.drag;
                    break;
                case MT::ColorOverLife:
                    p.f[0] = em.startColor.r; p.f[1] = em.startColor.g;
                    p.f[2] = em.startColor.b; p.f[3] = em.startColor.a;
                    p.f[4] = em.endColor.r; p.f[5] = em.endColor.g;
                    p.f[6] = em.endColor.b; p.f[7] = em.endColor.a;
                    p.f[8] = em.fadeIn; p.f[9] = em.fadeOut; p.f[10] = em.colorVariance;
                    break;
                case MT::SizeOverLife:
                    p.f[0] = em.startSize; p.f[1] = em.startSize;
                    p.f[2] = em.endSize; p.f[3] = em.endSize;
                    p.f[4] = em.sizeVariance;
                    break;
                default:
                    break; // Integrate / RotationRate take no operands
            }
        }
    }
}

// --------------------------------------------------------------------------
// Ring sizing
// --------------------------------------------------------------------------

u32 GpuRingCapacity(const ParticleEmitter& em) {
    const f32 lifeVar = glm::clamp(em.lifetimeVariance, 0.0f, 1.0f);
    const f32 maxLife = glm::max(0.05f, em.lifetime * (1.0f + lifeVar));
    // Steady state is rate * lifetime; the burst is on top of it because a one-shot
    // fires its whole count in a single frame.
    const f64 steady = static_cast<f64>(glm::max(0.0f, em.rate)) * static_cast<f64>(maxLife);
    // 25% slack + 8. The slack is what makes "the cursor laps a live particle" a
    // pathology of a badly authored emitter rather than the normal case: without it
    // the ring is exactly full at steady state and jitter in dt would recycle live
    // particles every few frames.
    const f64 want = (steady + static_cast<f64>(em.burst)) * 1.25 + 8.0;
    const f64 capped = glm::min(want, static_cast<f64>(em.maxParticles));
    // One emitter's ring is bound as ONE descriptor range on Vulkan, so it cannot
    // exceed the record buffer's declared bind window. Clamping here (rather than
    // letting the draw clamp it) keeps `gpuUsed` honest: the simulation never fills
    // slots the vertex shader would then be unable to see.
    constexpr u32 kMaxRing =
        rhi::kMaxGpuParticleBatchElements - rhi::kGpuParticleEmitterElements;
    return glm::clamp(static_cast<u32>(capped), 1u, kMaxRing);
}

bool AnyGpuSim(Scene& scene) {
    auto& reg = scene.Registry();
    for (const entt::entity e : reg.view<ParticleEmitter>()) {
        if (reg.get<ParticleEmitter>(e).gpuSim) return true;
    }
    return false;
}

// --------------------------------------------------------------------------
// The CPU half of one emitter's frame
// --------------------------------------------------------------------------

void StepGpuEmitter(ParticleEmitter& em, const glm::mat4& world, f32 dt, bool emitting,
                    u32 entityId) {
    if (em.gpuSeed == 0) em.gpuSeed = vfx::HashSeed(0x5FE2C1D3u, entityId);

    const u64 sig = GpuStackSignature(em);
    if (sig != em.stackSignature) {
        std::string errs;
        if (!vfx::Compile(BuildGpuDesc(em), em.stack, &errs)) {
            HBE_WARN("[particle] GPU emitter stack failed to compile: {}", errs);
        }
        // The GPU owns the particles: hold ZERO CPU pool memory. Reserve(0,0) also
        // frees whatever the CPU path had allocated if this emitter just switched.
        em.pool.Reserve(0, 0);
        em.stackSignature = sig;
        // Force a re-place: the ring size is part of the signature, so it may have
        // changed, and the slots' contents are meaningless across a resize anyway.
        em.gpuCapacity = 0;
        em.gpuUsed = em.gpuCursor = em.gpuTotalSpawned = 0;
        em.state = vfx::EmitterState{};
        em.state.rngState = vfx::HashSeed(em.gpuSeed, 0u);
        em.state.emitting = false; // the first active frame is a rising edge
    }

    // Per-frame authored constants the runner owns.
    em.stack.seed = em.gpuSeed;
    em.stack.spawnRate = em.rate;
    em.stack.burst = em.burst;
    em.stack.loop = em.loop;
    em.stack.duration = em.duration;
    StampGpuParams(em, world);

    em.state.spawnThisFrame = 0;
    if (dt <= 0.0f || !em.stack.valid) return;

    // Clocks. emitterTime is the noise phase and NEVER resets; emitterAge is relative
    // to the current emission window. Same two lines vfx::RunFrame opens with.
    em.state.emitterTime += dt;
    em.state.emitterAge += dt;

    const bool active = emitting && em.emitting;
    if (active && !em.state.wasEmitting) {
        em.state.emitterAge = 0.0f;
        em.state.burstFired = false;
    }
    em.state.emitting = active;
    em.state.wasEmitting = active;

    // The spawn scheduler, a transliteration of vfx::RunFrame step 5. This is the
    // ONE thing that deliberately stays on the CPU: it makes the frame's particle
    // count a CPU-known number, which is what keeps the draw a plain
    // DrawInstanced(6*N,1,0,0) on both backends instead of an indirect draw.
    u32 want = 0;
    if (active) {
        if (!em.state.burstFired && em.stack.burst > 0) {
            want += em.stack.burst;
            em.state.burstFired = true;
        }
        const bool windowOpen = em.stack.loop || em.state.emitterAge <= em.stack.duration;
        if (windowOpen && em.stack.spawnRate > 0.0f) {
            em.state.spawnAccum +=
                static_cast<f64>(em.stack.spawnRate) * static_cast<f64>(dt);
            const f64 whole = std::floor(em.state.spawnAccum);
            em.state.spawnAccum -= whole;
            const f64 clamped = glm::min(whole, static_cast<f64>(em.stack.maxParticles));
            want += static_cast<u32>(clamped);
        }
    }
    em.state.spawnThisFrame = want; // GpuSim::Update clamps it to the resident ring
}

// --------------------------------------------------------------------------
// GpuSim
// --------------------------------------------------------------------------

bool GpuSim::EnsureResources(Renderer& renderer) {
    if (failed_) return false;
    if (records_.IsValid()) return true;
    if (!renderer.SupportsGpuCompute()) {
        HBE_WARN("Particles: backend has no GPU compute; gpuSim emitters will not simulate "
                 "or draw (CPU emitters are unaffected).");
        failed_ = true;
        return false;
    }

    rhi::GpuBufferDesc rd{};
    rd.elementCount = kRecordElements;
    rd.elementStride = sizeof(vfx::GpuParticle);
    // ShaderWrite for the compute pass, ShaderRead for the vertex shader that draws
    // the very same bytes. That combination is the whole point of the I1 seam.
    rd.usage = rhi::GpuBufferUsage::ShaderWrite | rhi::GpuBufferUsage::ShaderRead;
    // One emitter's ring is one bind, so this is the Vulkan descriptor range AND the
    // per-emitter ceiling both backends clamp to (GpuRingCapacity clamps to match).
    rd.maxBindElements = rhi::kMaxGpuParticleBatchElements;
    rd.debugName = "VfxSimRecords";
    records_ = renderer.CreateGpuBuffer(rd);

    rhi::GpuBufferDesc ed{};
    ed.elementCount = kMaxEmitters * kEmitterVec4;
    ed.elementStride = 16;
    ed.usage = rhi::GpuBufferUsage::ShaderRead | rhi::GpuBufferUsage::CpuWrite;
    ed.debugName = "VfxSimEmitters";
    emitters_ = renderer.CreateGpuBuffer(ed);

    rhi::GpuBufferDesc jd{};
    jd.elementCount = kMaxJobs;
    jd.elementStride = 16;
    jd.usage = rhi::GpuBufferUsage::ShaderRead | rhi::GpuBufferUsage::CpuWrite;
    jd.debugName = "VfxSimJobs";
    jobs_ = renderer.CreateGpuBuffer(jd);

    rhi::GpuBufferDesc pd{};
    pd.elementCount = kMaxEmitters * kMaxModules * kModuleVec4;
    pd.elementStride = 16;
    pd.usage = rhi::GpuBufferUsage::ShaderRead | rhi::GpuBufferUsage::CpuWrite;
    pd.debugName = "VfxSimProgram";
    program_ = renderer.CreateGpuBuffer(pd);

    rhi::ComputePipelineDesc cd{};
    cd.shaderName = "VfxSim";
    cd.entryPoint = "CSMain";
    cd.constantBytes = sizeof(SimCB);
    cd.uavCount = 1;
    cd.srvCount = 3;
    cd.debugName = "VfxSim";
    pipeline_ = renderer.CreateComputePipeline(cd);

    if (!records_.IsValid() || !emitters_.IsValid() || !jobs_.IsValid() ||
        !program_.IsValid() || !pipeline_.IsValid()) {
        HBE_ERROR("Particles: GPU simulation resources unavailable; gpuSim emitters are "
                  "disabled this session.");
        Shutdown(renderer);
        failed_ = true;
        return false;
    }
    HBE_INFO("Particles: GPU simulation ready ({} record slots, {} MB).", kRecordElements,
             (static_cast<u64>(kRecordElements) * sizeof(vfx::GpuParticle)) >> 20);
    return true;
}

void GpuSim::Shutdown(Renderer& renderer) {
    if (records_.IsValid()) renderer.DestroyGpuBuffer(records_);
    if (emitters_.IsValid()) renderer.DestroyGpuBuffer(emitters_);
    if (jobs_.IsValid()) renderer.DestroyGpuBuffer(jobs_);
    if (program_.IsValid()) renderer.DestroyGpuBuffer(program_);
    records_ = {};
    emitters_ = {};
    jobs_ = {};
    program_ = {};
    pipeline_ = {};
    batches_.clear();
}

void GpuSim::Reallocate(Scene& scene) {
    // Full compaction. Rare by construction (only when the incremental bump allocator
    // runs out of room), and it RESETS every GPU emitter's particles - which is why
    // the incremental path exists at all: a new emitter appearing must not restart
    // every other effect in the scene.
    ++epoch_;
    u32 cursor = 0;
    u32 placed = 0;
    auto& reg = scene.Registry();
    for (const entt::entity e : reg.view<ParticleEmitter>()) {
        ParticleEmitter& em = reg.get<ParticleEmitter>(e);
        if (!em.gpuSim || !em.stack.valid) continue;
        em.gpuEpoch = epoch_;
        em.gpuUsed = em.gpuCursor = em.gpuTotalSpawned = 0;
        em.gpuCapacity = 0;
        if (placed >= kMaxEmitters) continue;
        const u32 cap = GpuRingCapacity(em);
        const u32 base = (cursor + kBlockAlign - 1u) & ~(kBlockAlign - 1u);
        const u32 need = rhi::kGpuParticleEmitterElements + cap;
        if (static_cast<u64>(base) + need > kRecordElements) continue;
        em.gpuSlotBase = base;
        em.gpuCapacity = cap;
        cursor = base + need;
        ++placed;
    }
    allocCursor_ = cursor;
    residentCount_ = placed;
}

bool GpuSim::Update(Scene& scene, Renderer& renderer, const std::filesystem::path& assetsDir,
                    const glm::vec3& camRight, const glm::vec3& camUp, f32 dt,
                    f64* mapWaitMsOut) {
    batches_.clear();
    stats_ = Stats{};
    auto& reg = scene.Registry();

    bool any = false;
    for (const entt::entity e : reg.view<ParticleEmitter>()) {
        const ParticleEmitter& em = reg.get<ParticleEmitter>(e);
        if (em.gpuSim && em.stack.valid) { any = true; break; }
    }
    if (!any) return false;
    if (!EnsureResources(renderer)) return false;

    // --- Placement -------------------------------------------------------
    // Incremental: an emitter that is new or whose ring size changed gets the next
    // free block. Only when the bump allocator is exhausted does everything move.
    for (const entt::entity e : reg.view<ParticleEmitter>()) {
        ParticleEmitter& em = reg.get<ParticleEmitter>(e);
        if (!em.gpuSim || !em.stack.valid) continue;
        const u32 cap = GpuRingCapacity(em);
        // gpuCapacity == 0 with a current epoch means "Reallocate already considered
        // me this epoch and could not place me". That state must be TERMINAL for the
        // epoch: retrying re-enters Reallocate, which bumps the epoch and zeroes
        // gpuUsed/gpuCursor on EVERY GPU emitter in the scene. Without this the first
        // oversubscribed frame becomes a permanent livelock in which nothing ever
        // accumulates - every GPU effect in the level collapses to a flicker of
        // newborn particles, with no signal but a `DROPPED` suffix on a perf line.
        if (em.gpuEpoch == epoch_ && (em.gpuCapacity == cap || em.gpuCapacity == 0)) continue;
        const u32 base = (allocCursor_ + kBlockAlign - 1u) & ~(kBlockAlign - 1u);
        const u32 need = rhi::kGpuParticleEmitterElements + cap;
        if (residentCount_ >= kMaxEmitters ||
            static_cast<u64>(base) + need > kRecordElements) {
            Reallocate(scene);
            break;
        }
        em.gpuSlotBase = base;
        em.gpuCapacity = cap;
        em.gpuEpoch = epoch_;
        em.gpuUsed = em.gpuCursor = em.gpuTotalSpawned = 0;
        allocCursor_ = base + need;
        ++residentCount_;
    }

    // See the mapWaitMsOut contract in the header: this is fence wait, not work.
    const auto mapStart = std::chrono::high_resolution_clock::now();
    u8* eDst = static_cast<u8*>(renderer.MapGpuBuffer(emitters_));
    u8* jDst = static_cast<u8*>(renderer.MapGpuBuffer(jobs_));
    u8* pDst = static_cast<u8*>(renderer.MapGpuBuffer(program_));
    if (mapWaitMsOut) {
        *mapWaitMsOut += std::chrono::duration<f64, std::milli>(
                             std::chrono::high_resolution_clock::now() - mapStart)
                             .count();
    }
    if (!eDst || !jDst || !pDst) return false;

    // Update-phase jobs go straight into the buffer; spawn-phase jobs are staged here
    // and appended, so each dispatch owns ONE contiguous job range and the shader can
    // address it with a base + a group id.
    static std::vector<u32> spawnJobs; // (emitter, localBase) pairs; reused every frame
    spawnJobs.clear();

    u32 emitterIndex = 0;
    u32 programCursor = 0; // in uint4 elements
    u32 jobCount = 0;

    for (const entt::entity e : reg.view<ParticleEmitter>()) {
        ParticleEmitter& em = reg.get<ParticleEmitter>(e);
        if (!em.gpuSim || !em.stack.valid || em.gpuCapacity == 0) {
            if (em.gpuSim) {
                ++stats_.dropped;
                if (!dropWarned_) {
                    dropWarned_ = true;
                    HBE_WARN("[particle] GPU simulation record buffer is oversubscribed: at "
                             "least one gpuSim emitter got no ring and will not simulate or "
                             "draw. Reduce Max/Rate/Lifetime on the GPU emitters, or the "
                             "number of them ({} elements total).",
                             kRecordElements);
                }
            }
            continue;
        }
        if (emitterIndex >= kMaxEmitters) { ++stats_.dropped; continue; }

        const u32 modules = em.stack.ModuleCountTotal();
        if (modules > kMaxModules || programCursor + modules * kModuleVec4 >
                                         kMaxEmitters * kMaxModules * kModuleVec4) {
            ++stats_.dropped;
            continue;
        }

        // Same lazy, cached sprite resolve both CPU paths do (it touches the asset
        // loader and the filesystem, so it can never move into a shader).
        if (!em.textureResolved) {
            em.textureResolved = true;
            em.textureCache = 0;
            if (!em.texture.empty() && !assetsDir.empty()) {
                em.textureCache = assets::LoadTexture(renderer, assetsDir / em.texture).index;
            }
        }

        const u32 cap = em.gpuCapacity;
        const u32 usedBefore = em.gpuUsed;
        // The dt guard is not paranoia. particle::Update early-returns on dt <= 0, so
        // StepGpuEmitter does not run and spawnThisFrame still holds the PREVIOUS
        // frame's count - consuming it again would spawn the same particles twice on
        // the first paused frame. The update dispatch still runs (dt 0 is a no-op) so
        // the render header stays fresh and a paused scene keeps drawing.
        const u32 spawnN = (dt > 0.0f) ? glm::min(em.state.spawnThisFrame, cap) : 0u;

        // --- the 128-byte render header the compute shader copies into the ring ---
        vfx::GpuEmitter rec;
        rec.startColor = em.startColor;
        rec.endColor = em.endColor;
        rec.sizeFade = glm::vec4(em.startSize, em.endSize, em.fadeIn, em.fadeOut);
        rec.texIndex = em.textureCache;
        rec.subUV = glm::max(1u, em.subUVCols) | (glm::max(1u, em.subUVRows) << 16);
        rec.subUVFps = em.subUVFps;
        // Every stream EXISTS in a 64-byte GPU record, so all four presence bits are
        // set unconditionally - dead-stream elimination is a property of the CPU SoA
        // pool, which a gpuSim emitter does not have. SimSize in particular is
        // load-bearing: it is what makes a retired slot's zeroed size collapse the
        // quad instead of the ramp drawing it at full size.
        rec.flags = vfx::GpuEmitterFlag::SimColor | vfx::GpuEmitterFlag::SimSize |
                    vfx::GpuEmitterFlag::HasRot | vfx::GpuEmitterFlag::HasVel;
        rec.camRight = camRight;
        rec.stretch = em.stretch;
        rec.camUp = camUp;
        rec.renderMode = static_cast<u32>(em.render);

        u8* edst = eDst + static_cast<usize>(emitterIndex) * kEmitterVec4 * 16u;
        std::memcpy(edst, &rec, sizeof(rec));

        // --- the control block (VfxEmitterCtl in VfxSim.hlsl) ---
        u32* c = reinterpret_cast<u32*>(edst + sizeof(rec));
        const auto& upd = em.stack.stages[static_cast<u32>(vfx::ModuleStage::ParticleUpdate)];
        const auto& col = em.stack.stages[static_cast<u32>(vfx::ModuleStage::ParticleCollide)];
        const auto& spw = em.stack.stages[static_cast<u32>(vfx::ModuleStage::ParticleSpawn)];
        c[0] = em.gpuSlotBase;
        c[1] = cap;
        c[2] = usedBefore;
        c[3] = modules;
        c[4] = programCursor;
        c[5] = static_cast<u32>(upd.size());
        c[6] = static_cast<u32>(col.size());
        c[7] = static_cast<u32>(spw.size());
        c[8] = em.gpuCursor;
        c[9] = spawnN;
        c[10] = em.state.spawnCounter;
        c[11] = em.stack.seed;
        c[12] = AsU32(em.state.emitterTime);
        c[13] = c[14] = c[15] = 0;

        // --- the program: update modules, then collide, then spawn ---
        // That order IS the stage order, so the shader runs [0,upd) then
        // [upd,+col) for a live particle and [.. ,+spw) for a new one, and the
        // "collide strictly after the integrator" guarantee survives as layout.
        u32 mi = 0;
        const auto emit = [&](const std::vector<vfx::CompiledModule>& stage) {
            for (const vfx::CompiledModule& m : stage) {
                u32* w = reinterpret_cast<u32*>(
                    pDst + static_cast<usize>(programCursor + mi * kModuleVec4) * 16u);
                w[0] = static_cast<u32>(m.type);
                for (u32 k = 0; k < 12; ++k) w[1 + k] = AsU32(m.params.f[k]);
                w[13] = w[14] = w[15] = 0;
                ++mi;
            }
        };
        emit(upd);
        emit(col);
        emit(spw);
        programCursor += modules * kModuleVec4;

        // --- jobs ---
        // At least one update group even at usedBefore == 0: its thread 0 is what
        // writes the render header, and a brand-new emitter must have one before its
        // first spawned particles are drawn.
        const u32 updGroups = glm::max(1u, (usedBefore + kGroupSize - 1u) / kGroupSize);
        const u32 spwGroups = (spawnN + kGroupSize - 1u) / kGroupSize;
        if (jobCount + updGroups + spwGroups + static_cast<u32>(spawnJobs.size() / 2) >
            kMaxJobs) {
            ++stats_.dropped;
            continue;
        }
        for (u32 g = 0; g < updGroups; ++g) {
            u32* j = reinterpret_cast<u32*>(jDst + static_cast<usize>(jobCount + g) * 16u);
            j[0] = emitterIndex;
            j[1] = g * kGroupSize;
            j[2] = j[3] = 0;
        }
        jobCount += updGroups;
        for (u32 g = 0; g < spwGroups; ++g) {
            spawnJobs.push_back(emitterIndex);
            spawnJobs.push_back(g * kGroupSize);
        }

        // --- CPU bookkeeping: the ring cursor and the high-water mark ---
        em.state.spawnCounter += spawnN; // monotonic - the per-particle RNG stream key
        em.gpuTotalSpawned += spawnN;
        em.gpuCursor = (em.gpuCursor + spawnN) % cap;
        em.gpuUsed = glm::min(cap, em.gpuTotalSpawned);

        if (em.gpuUsed > 0) {
            rhi::GpuParticleBatch b;
            b.recordFirst = em.gpuSlotBase;
            b.count = em.gpuUsed; // includes the slots this frame's spawn dispatch fills
            b.additive = em.additive ? 1u : 0u;
            batches_.push_back(b);
        }
        stats_.slots += usedBefore;
        stats_.spawned += spawnN;
        ++stats_.emitters;
        ++emitterIndex;
    }

    if (emitterIndex == 0) return false;

    const u32 updateJobs = jobCount;
    const u32 spawnJobBase = jobCount;
    const u32 spawnJobCount = static_cast<u32>(spawnJobs.size() / 2);
    for (u32 i = 0; i < spawnJobCount; ++i) {
        u32* j = reinterpret_cast<u32*>(jDst + static_cast<usize>(spawnJobBase + i) * 16u);
        j[0] = spawnJobs[i * 2 + 0];
        j[1] = spawnJobs[i * 2 + 1];
        j[2] = j[3] = 0;
    }

    // Two dispatches of ONE pipeline. Both backends put a barrier between queued
    // dispatches (D3D12 a UAV barrier, Vulkan a VkMemoryBarrier), which is exactly
    // what makes the ordering safe: after the ring has lapped, a spawn slot can lie
    // inside the update range, so the spawn pass must not begin until the update
    // pass has finished reading it.
    rhi::ComputeDispatch d{};
    d.pipeline = pipeline_;
    d.uavs[0] = records_;
    d.uavCount = 1;
    d.srvs[0] = jobs_;
    d.srvs[1] = emitters_;
    d.srvs[2] = program_;
    d.srvCount = 3;

    if (updateJobs > 0) {
        const SimCB cb{kPhaseUpdate, 0, updateJobs, dt};
        d.constants = &cb;
        d.constantBytes = sizeof(cb);
        d.groupsX = updateJobs;
        renderer.QueueCompute(d);
        stats_.groups += updateJobs;
    }
    if (spawnJobCount > 0) {
        const SimCB cb{kPhaseSpawn, spawnJobBase, spawnJobCount, dt};
        d.constants = &cb;
        d.constantBytes = sizeof(cb);
        d.groupsX = spawnJobCount;
        renderer.QueueCompute(d);
        stats_.groups += spawnJobCount;
    }
    return !batches_.empty();
}

} // namespace hbe::particle
