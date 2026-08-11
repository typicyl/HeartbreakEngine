// SmokeVorticity.hlsl - curl of velocity + its magnitude (== CPU ComputeVorticity). Neighbours are
// clamped to the domain (not zeroed) - this is about the velocity field's structure. curl.w = |curl|.
#include "SmokeCommon.hlsli"

[[vk::binding(0, 0)]] cbuffer VortCB : register(b0) {
    int4   gDim;      // xyz = dim
    float4 gInvVoxel; // xyz = invVoxel
};
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> gCurl : register(u0);
[[vk::binding(2, 0)]] StructuredBuffer<float4>   gVel  : register(t0);

#define VC(xx, yy, zz) gVel[SmokeIdx(clamp(int3(xx, yy, zz), int3(0, 0, 0), dim - 1), dim)].xyz

[numthreads(8, 8, 8)]
void CSMain(uint3 gid : SV_DispatchThreadID) {
    int3 dim = gDim.xyz;
    if (any((int3)gid >= dim)) return;
    int3 p = (int3)gid;
    int x = p.x, y = p.y, z = p.z;
    float3 iv = gInvVoxel.xyz;
    float3 c;
    c.x = (VC(x, y + 1, z).z - VC(x, y - 1, z).z) * 0.5 * iv.y -
          (VC(x, y, z + 1).y - VC(x, y, z - 1).y) * 0.5 * iv.z;
    c.y = (VC(x, y, z + 1).x - VC(x, y, z - 1).x) * 0.5 * iv.z -
          (VC(x + 1, y, z).z - VC(x - 1, y, z).z) * 0.5 * iv.x;
    c.z = (VC(x + 1, y, z).y - VC(x - 1, y, z).y) * 0.5 * iv.x -
          (VC(x, y + 1, z).x - VC(x, y - 1, z).x) * 0.5 * iv.y;
    gCurl[SmokeIdx(p, dim)] = float4(c, length(c));
}
