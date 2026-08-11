// SmokeCommon.hlsli - shared helpers for the GPU Eulerian smoke solver kernels.
//
// These MUST stay bit-faithful to the CPU oracle (EulerianSmokeSimulation.cpp / VolumeRasterize.cpp)
// so --test-gpusolver's golden-diff is meaningful. Grid indexing == VolumeFrame::VoxelIndex; the
// domain is a CLOSED BOX (a neighbour outside [0,dim) is "solid": zero velocity for divergence,
// dropped from the Poisson stencil, mirrored for the gradient). Interior obstacles are NOT modelled
// on the GPU (they fall back to the CPU solver), so there is no per-cell solid mask here.
#ifndef HBE_SMOKE_COMMON_HLSLI
#define HBE_SMOKE_COMMON_HLSLI

// Row-major flat index, X fastest then Y then Z (== VoxelIndex in VolumeFrame.h).
uint SmokeIdx(int3 c, int3 dim) {
    return (uint)((c.z * dim.y + c.y) * dim.x + c.x);
}

// A neighbour coordinate outside the domain is a closed-box wall ("solid").
bool SmokeOOB(int3 c, int3 dim) {
    return c.x < 0 || c.y < 0 || c.z < 0 || c.x >= dim.x || c.y >= dim.y || c.z >= dim.z;
}

// Rotate v by quaternion q=(x,y,z,w) - identical formula to glm::quat * vec3.
float3 SmokeQuatRotate(float4 q, float3 v) {
    float3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

// Signed-distance -> coverage in [0,1] (== VolumeRasterize.cpp CoverageFromSdf): 1 at sdf<=-band,
// 0 at sdf>=+band, smoothstep across.
float SmokeCoverageFromSdf(float sdf, float band) {
    band = max(band, 1e-5);
    float t = saturate((sdf + band) / (2.0 * band));
    float s = t * t * (3.0 - 2.0 * t);
    return 1.0 - s;
}

// Fractional coverage of world point wp inside an emitter shape (== VolumeRasterize.cpp
// ShapeCoverage). kind: 0=Sphere, 1=Box, 2=Cone, 3=MeshVoxelized(treated as its AABB).
float SmokeShapeCoverage(int kind, float3 center, float3 halfExtents, float4 rot, float coneHeight,
                         float edgeSoftness, float3 wp) {
    // Into local space: undo translation then rotation (conjugate == inverse for a unit quat).
    float4 qConj = float4(-rot.xyz, rot.w);
    float3 local = SmokeQuatRotate(qConj, wp - center);

    if (kind == 0) { // Sphere
        float radius = max(halfExtents.x, 1e-5);
        float band = edgeSoftness * radius;
        float sdf = length(local) - radius;
        return SmokeCoverageFromSdf(sdf, band);
    } else if (kind == 2) { // Cone (local +Y, base radius at y=0 -> point at coneHeight)
        float h = max(coneHeight, 1e-5);
        if (local.y < 0.0 || local.y > h) return 0.0;
        float baseR = max(halfExtents.x, 1e-5);
        float allowedR = baseR * (1.0 - local.y / h);
        float r = length(float2(local.x, local.z));
        float band = max(edgeSoftness * baseR, 1e-5);
        return SmokeCoverageFromSdf(r - allowedR, band);
    } else { // Box / MeshVoxelized(AABB)
        float3 he = max(halfExtents, float3(1e-5, 1e-5, 1e-5));
        float3 q = abs(local) - he;
        float outside = length(max(q, float3(0, 0, 0)));
        float inside = min(max(q.x, max(q.y, q.z)), 0.0);
        float sdf = outside + inside;
        float band = edgeSoftness * min(he.x, min(he.y, he.z));
        return SmokeCoverageFromSdf(sdf, band);
    }
}

// Trilinear addressing in GRID space (integer coords = cell centres), clamp-addressed to
// [0,dim-1] - matches EulerianSmokeSimulation.cpp SampleScalar/SampleVel. The caller does the 8
// corner loads (HLSL can't pass a StructuredBuffer to a helper pre-SM6.6) and lerps X then Y then Z.
void SmokeTrilinearSetup(float3 gp, int3 dim, out int3 b0, out int3 b1, out float3 f) {
    float3 g = clamp(gp, float3(0, 0, 0), float3(dim - 1));
    b0 = (int3)floor(g);
    b1 = min(b0 + 1, dim - 1);
    f = g - (float3)b0;
}

#endif // HBE_SMOKE_COMMON_HLSLI
