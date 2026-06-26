// Scene/ParticleSystem.h - CPU-simulated, GPU-billboarded particles.
//
// Each ParticleEmitter pools its particles (no per-frame allocation). Update()
// integrates + spawns; BuildVertices() emits camera-facing billboard quads into
// two batched lists (alpha-blended + additive) drawn in one pass each by the
// renderer's particle pass. Efficient by construction: pooled, one buffer.
#pragma once

#include "Core/Types.h"
#include "RHI/RHI.h"

#include <glm/glm.hpp>

#include <filesystem>
#include <vector>

namespace hbe {

class Scene;
class Renderer;

namespace particle {

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

} // namespace particle
} // namespace hbe
