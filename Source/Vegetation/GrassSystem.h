// Vegetation/GrassSystem.h - the SEPARATE grass path (not "tiny trees").
//
// Grass is a distinct, optimized layer: a dense PATCH of blade tufts is meshed ONCE and
// instanced across the terrain (one instance per ground cell), so thousands of patches ×
// tens of tufts render as GPU-instanced draws of a single shared mesh. This is the
// CPU-placed / GPU-instanced first cut; the design's GPU-COMPUTE blade generation (for
// 10M-blade density + per-blade GPU wind + trample interaction) is the scale upgrade that
// slots in behind the same IVegetationRenderer seam. See docs/Design-Vegetation.md §16.
#pragma once

#include "Vegetation/VegetationTypes.h"
#include "Assets/Mesh.h"

#include <glm/glm.hpp>

namespace hbe {

class Scene;
class Renderer;

namespace veg {

struct GrassParams {
    f32 patchSize     = 2.0f;   // world size of one patch (m)
    u32 tuftsPerPatch = 40;     // blade tufts per patch
    f32 bladeHeight   = 0.42f;  // tuft height (m), varied per tuft
    f32 bladeWidth    = 0.05f;  // tuft width (m)
    f32 spacing       = 1.8f;   // patch placement grid (m)
    f32 slopeLimitDeg = 28.0f;  // no grass on steeper ground
    f32 jitter        = 0.6f;   // patch position jitter (fraction of spacing)
    glm::vec4 color{0.30f, 0.52f, 0.16f, 1.0f};
};

// Build ONE grass patch mesh: `tuftsPerPatch` crossed-quad blade tufts scattered in a
// patchSize×patchSize area, deterministic from `seed`. Optimized (no LODs; grass fades by
// distance culling of whole patches).
MeshData BuildGrassPatch(u64 seed, const GrassParams& p = {});

// Scatter grass patches over [aabbMin,aabbMax] on a grid, place each on the ground via the
// surface query (skipping steep/off-terrain/underwater cells), and spawn one instance of
// the shared patch mesh per cell. Returns the number of patches spawned.
u32 PopulateGrass(Scene& scene, Renderer& renderer, const SurfaceQueryFn& surface,
                  const glm::vec2& aabbMin, const glm::vec2& aabbMax, u64 seed,
                  const GrassParams& p = {});

} // namespace veg
} // namespace hbe
