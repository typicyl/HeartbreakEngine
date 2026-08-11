// Source/Volume/VolumeRasterize.cpp - see the header.
#include "Volume/VolumeRasterize.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>

namespace hbe::volume {
namespace {

f32 Saturate(f32 x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

// smoothstep-based coverage from a SIGNED distance (negative inside): 1 at sdf <= -band,
// 0 at sdf >= +band, smooth across. `band` is the half-width of the soft edge in world units.
f32 CoverageFromSdf(f32 sdf, f32 band) {
    band = glm::max(band, 1e-5f);
    const f32 t = Saturate((sdf + band) / (2.0f * band)); // 0 inside -> 1 outside
    const f32 s = t * t * (3.0f - 2.0f * t);              // smoothstep
    return 1.0f - s;
}

} // namespace

f32 ShapeCoverage(const VolumeShape& shape, const glm::vec3& worldPos) {
    // Into local space: undo translation then rotation (conjugate == inverse for a unit quat).
    const glm::vec3 local = glm::conjugate(shape.rotation) * (worldPos - shape.center);

    switch (shape.kind) {
    case VolumeShapeKind::Sphere: {
        const f32 radius = glm::max(shape.halfExtents.x, 1e-5f);
        const f32 band   = shape.edgeSoftness * radius;
        const f32 sdf    = glm::length(local) - radius;
        return CoverageFromSdf(sdf, band);
    }
    case VolumeShapeKind::Box:
    case VolumeShapeKind::MeshVoxelized: { // MeshVoxelized placeholder: treat as its AABB for now.
        const glm::vec3 he = glm::max(shape.halfExtents, glm::vec3(1e-5f));
        const glm::vec3 q  = glm::abs(local) - he;
        // Standard box SDF (negative inside).
        const f32 outside = glm::length(glm::max(q, glm::vec3(0.0f)));
        const f32 inside  = glm::min(glm::max(q.x, glm::max(q.y, q.z)), 0.0f);
        const f32 sdf     = outside + inside;
        const f32 band    = shape.edgeSoftness * glm::min(he.x, glm::min(he.y, he.z));
        return CoverageFromSdf(sdf, band);
    }
    case VolumeShapeKind::Cone: {
        // Cone along local +Y: full base radius at y=0, narrowing linearly to a point at y=coneHeight.
        const f32 h = glm::max(shape.coneHeight, 1e-5f);
        if (local.y < 0.0f || local.y > h) {
            // Outside the height band: distance to the nearer cap plane drives a soft cutoff.
            const f32 dy = local.y < 0.0f ? -local.y : (local.y - h);
            const f32 band = glm::max(shape.edgeSoftness * h, 1e-5f);
            return CoverageFromSdf(dy, band) * 0.0f; // hard cap in Y (no coverage beyond the ends)
        }
        const f32 baseR   = glm::max(shape.halfExtents.x, 1e-5f);
        const f32 allowedR = baseR * (1.0f - local.y / h);
        const f32 r       = glm::length(glm::vec2(local.x, local.z));
        const f32 band    = glm::max(shape.edgeSoftness * baseR, 1e-5f);
        return CoverageFromSdf(r - allowedR, band);
    }
    }
    return 0.0f;
}

void RasterizeShape(const VolumeBounds& bounds, const VolumeShape& shape,
                    std::vector<f32>& outMask, bool additive) {
    const usize count = bounds.voxelCount();
    if (outMask.size() != count) outMask.assign(count, 0.0f);

    for (int z = 0; z < bounds.dim.z; ++z)
        for (int y = 0; y < bounds.dim.y; ++y)
            for (int x = 0; x < bounds.dim.x; ++x) {
                const glm::vec3 wp = bounds.voxelCenter(x, y, z);
                const f32 cov = ShapeCoverage(shape, wp);
                if (cov <= 0.0f) continue;
                const usize idx = VoxelIndex(bounds, x, y, z);
                outMask[idx] = additive ? glm::min(1.0f, outMask[idx] + cov)
                                        : glm::max(outMask[idx], cov);
            }
}

} // namespace hbe::volume
