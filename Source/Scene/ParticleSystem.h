// Scene/ParticleSystem.h - CPU-simulated, GPU-billboarded particles.
//
// Update() steps every ParticleEmitter through the VFX MODULE STACK (Source/Vfx):
// the component's authored fields are compiled into an ordered list of small kernels
// over a structure-of-arrays pool, once, and re-stamped in place every frame.
// BuildVertices() then emits camera-facing billboard quads into two batched lists
// (alpha-blended + additive), one draw each.
//
// BACKWARDS COMPATIBILITY IS THE POINT OF THE DEFAULT STACK. An emitter that nobody
// has touched compiles to the four Legacy.* compatibility modules, which are verbatim
// copies of the fixed loop this system used to run - same arithmetic, same order,
// same RNG draws, same f32 spawn accumulator. So an already-authored scene simulates
// bit-identically; only an artist ticking one of the new module flags changes
// anything. CompatSelfTest() proves that against a frozen copy of the old loop.
//
// Allocation: the pool is a structure-of-arrays sized at stack-compile time and grown
// geometrically at SPAWN time up to the authored maxParticles - never by per-particle
// push_back the way the old pool was, and never inside an update kernel. Steady state
// is zero allocations per frame, which is what the header used to claim and did not do.
// The authored cap is a ceiling, not a commitment: an emitter authored at 200 000 max
// that only ever holds 50 particles pays for 50.
#pragma once

#include "Core/Types.h"
#include "RHI/RHI.h"
#include "Scene/Components.h" // ParticleEmitter (templates return one by value)

#include <glm/glm.hpp>

#include <filesystem>
#include <vector>

namespace hbe {

class Scene;
class Renderer;

namespace particle {

// Built-in emitter presets. MakeTemplate fills a fully-tuned ParticleEmitter
// (procedural soft-dot sprite, so they work with no texture assets).
enum class Template : u32 {
    Fire = 0, Smoke, Dust, Rain, Leaves, Explosion, Sparks, Magic,
    Count
};
ParticleEmitter MakeTemplate(Template t);
const char* TemplateName(Template t);

// Steps every ParticleEmitter: integrate live particles (gravity/drag/spin),
// recycle expired, and spawn new ones when `emitting` (play mode / runtime).
// Existing particles always finish their life even when emitting is false.
void Update(Scene& scene, f32 dt, bool emitting);

// Builds camera-facing billboards for all emitters. `camRight`/`camUp` are the
// camera basis (world). Alpha-blended emitters append to `alphaOut`, additive to
// `addOut`. Both are cleared first.
//
// Emitters with ParticleEmitter::gpuExpand are SKIPPED here - their quads are built
// in the vertex shader from the records BuildGpuRecords writes. That is the whole
// opt-in seam: an emitter is expanded by exactly one of the two paths, never both.
void BuildVertices(Scene& scene, Renderer& renderer,
                   const std::filesystem::path& assetsDir, const glm::vec3& camRight,
                   const glm::vec3& camUp, std::vector<rhi::ParticleVertex>& alphaOut,
                   std::vector<rhi::ParticleVertex>& addOut);

// Mesh particles: for every emitter with render == Render::Mesh, resolve its `.uaf` mesh (cached)
// and append one rhi::DrawItem per live particle (transform + particle-colour tint) to `out`. The
// renderer's run-builder auto-instances the identical-material ones. A no-op unless an emitter opts
// into Mesh mode, so existing scenes are unaffected. Call before the renderer collects draw items.
void CollectMeshParticles(Scene& scene, Renderer& renderer, const std::filesystem::path& assetsDir,
                          std::vector<rhi::DrawItem>& out);

// True if any emitter in the scene opted into GPU vertex expansion. Cheap; the
// caller uses it to avoid allocating the record buffer in projects that never do.
bool AnyGpuExpand(Scene& scene);

// Total live particles across all CPU-SIMULATED emitters (gpuSim emitters have no CPU
// pool and are counted by particle::GpuSim::Stats instead). Exists so a benchmark can
// state the population it measured rather than assume the authored target was reached
// - the stress rig fills over ~lifetime seconds of WALL time, so a run that ends
// before it saturates would otherwise report a per-particle cost against a particle
// count it never had.
u32 LiveCount(Scene& scene);

// GPU VERTEX EXPANSION upload. Writes, for every `gpuExpand` emitter with live
// particles, a 128-byte vfx::GpuEmitter record followed by one 64-byte
// vfx::GpuParticle per particle, into `dst` (a mapped rhi CpuWrite|ShaderRead
// buffer of 64-byte elements), and appends one rhi::GpuParticleBatch per emitter to
// `batchesOut` (cleared first).
//
// This replaces 240 bytes of CPU-built world-space vertices per particle with 64
// bytes of raw state - the vertex shader does the billboarding. `capacityElements`
// is the buffer size in 64-byte elements; emitters that do not fit are truncated or
// dropped (explicitly, at the end, rather than silently clipped mid-draw the way the
// fixed 6 MB vertex ring does). Returns the number of elements written.
//
// `dst` is write-combined upload memory: this function only ever writes it.
u32 BuildGpuRecords(Scene& scene, Renderer& renderer, const std::filesystem::path& assetsDir,
                    const glm::vec3& camRight, const glm::vec3& camUp, void* dst,
                    u32 capacityElements, std::vector<rhi::GpuParticleBatch>& batchesOut);

// Headless, GPU-free A/B proof that moving onto the module stack changed nothing.
//
// It runs a FROZEN copy of the pre-stack simulation loop (the oracle) and the live
// module-stack path side by side over the same dt sequence, and compares position,
// velocity, age, lifetime and rotation BIT-EXACTLY, particle for particle, for every
// built-in template plus a parameter fuzz that exercises all six emit shapes, both
// non-billboard render modes, bursts, non-looping windows, buoyancy/vortex roll and
// emission on/off edges. Anything less than bit-exact is not a proof: a particle
// system is chaotic, so a one-ulp difference in frame 1 is a visibly different effect
// a second later.
//
// It also checks that the opt-in modules are genuinely opt-in (enabling none leaves
// the hashes untouched) and that enabling one actually changes the result - a
// compatibility test that passes because nothing is wired up would be worthless.
// Returns true on pass; logs each sub-result. Wired to --test-vfxcompat.
bool CompatSelfTest();

} // namespace particle
} // namespace hbe
