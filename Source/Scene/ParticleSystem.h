// Scene/ParticleSystem.h - CPU-simulated, GPU-billboarded particles.
//
// Each ParticleEmitter pools its particles (no per-frame allocation). Update()
// integrates + spawns; BuildVertices() emits camera-facing billboard quads into
// two batched lists (alpha-blended + additive) drawn in one pass each by the
// renderer's particle pass. Efficient by construction: pooled, one buffer.
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
    VolFire, VolSmoke, VolExplosion, // true raymarched volumetric (volumetric flag on)
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
void BuildVertices(Scene& scene, Renderer& renderer,
                   const std::filesystem::path& assetsDir, const glm::vec3& camRight,
                   const glm::vec3& camUp, std::vector<rhi::ParticleVertex>& alphaOut,
                   std::vector<rhi::ParticleVertex>& addOut);

// Collects density/temperature blobs from every `volumetric`-flagged emitter (one
// blob per live particle) for the raymarched volumetric pass. Fills `blobsOut`
// (cleared first, capped at rhi::kMaxVolumeBlobs) and `paramsOut` (world-space AABB
// enclosing all blobs + the tuning knobs taken from the first volumetric emitter).
// Returns true if any volumetric blobs were produced (params valid); false means no
// volumetric emitters are active and the caller should clear the volume.
bool BuildVolumetricBlobs(Scene& scene, std::vector<rhi::VolumeBlob>& blobsOut,
                          rhi::VolumeParams& paramsOut);

} // namespace particle
} // namespace hbe
