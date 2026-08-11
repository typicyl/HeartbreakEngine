// Source/Volume/VolumeRasterize.h - the ONE place shapes are voxelized, so every solver stamps
// emitters + obstacles identically. Two entry points: a per-point coverage sampler (called inside a
// solver's emission loop) and a whole-grid mask rasterizer (used to voxelize static obstacles once
// at Reset()). Pure CPU + deterministic; no solver / RHI / asset dependency.
#pragma once

#include "Volume/VolumeFrame.h"     // VolumeBounds
#include "Volume/VolumeSimConfig.h" // VolumeShape

#include <glm/glm.hpp>

#include <vector>

namespace hbe::volume {

// Fractional coverage in [0,1] of world point `worldPos` inside `shape`: 1 fully inside, 0 outside,
// with a soft band of width shape.edgeSoftness across the boundary. This is what an emission pass
// multiplies its per-second rates by, per voxel.
f32 ShapeCoverage(const VolumeShape& shape, const glm::vec3& worldPos);

// Rasterize `shape` into a full per-voxel coverage grid over `bounds` (sized bounds.voxelCount()).
// If `additive` is false (default) each voxel is max-combined with any existing value (union of
// shapes); if true, coverage is summed and clamped to 1. `outMask` is resized/zeroed on first use
// only when its size does not already match (so it can accumulate several shapes across calls).
void RasterizeShape(const VolumeBounds& bounds, const VolumeShape& shape,
                    std::vector<f32>& outMask, bool additive = false);

} // namespace hbe::volume
