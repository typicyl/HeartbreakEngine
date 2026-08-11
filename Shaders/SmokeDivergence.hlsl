// SmokeDivergence.hlsl - divergence of the post-force velocity (== CPU Project step 1). A solid /
// out-of-domain neighbour contributes 0 velocity (closed-box wall).
#include "SmokeCommon.hlsli"

[[vk::binding(0, 0)]] cbuffer DivCB : register(b0) {
    int4   gDim;      // xyz = dim
    float4 gInvVoxel; // xyz = invVoxel
};
[[vk::binding(1, 0)]] RWStructuredBuffer<float>  gDiv : register(u0);
[[vk::binding(2, 0)]] StructuredBuffer<float4>   gVel : register(t0);

[numthreads(8, 8, 8)]
void CSMain(uint3 gid : SV_DispatchThreadID) {
    int3 dim = gDim.xyz;
    if (any((int3)gid >= dim)) return;
    int3 p = (int3)gid;
    int x = p.x, y = p.y, z = p.z;
    float vxp = SmokeOOB(int3(x + 1, y, z), dim) ? 0.0 : gVel[SmokeIdx(int3(x + 1, y, z), dim)].x;
    float vxm = SmokeOOB(int3(x - 1, y, z), dim) ? 0.0 : gVel[SmokeIdx(int3(x - 1, y, z), dim)].x;
    float vyp = SmokeOOB(int3(x, y + 1, z), dim) ? 0.0 : gVel[SmokeIdx(int3(x, y + 1, z), dim)].y;
    float vym = SmokeOOB(int3(x, y - 1, z), dim) ? 0.0 : gVel[SmokeIdx(int3(x, y - 1, z), dim)].y;
    float vzp = SmokeOOB(int3(x, y, z + 1), dim) ? 0.0 : gVel[SmokeIdx(int3(x, y, z + 1), dim)].z;
    float vzm = SmokeOOB(int3(x, y, z - 1), dim) ? 0.0 : gVel[SmokeIdx(int3(x, y, z - 1), dim)].z;
    float3 iv = gInvVoxel.xyz;
    gDiv[SmokeIdx(p, dim)] = 0.5 * ((vxp - vxm) * iv.x + (vyp - vym) * iv.y + (vzp - vzm) * iv.z);
}
