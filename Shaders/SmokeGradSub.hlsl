// SmokeGradSub.hlsl - subtract the pressure gradient to make velocity divergence-free IN PLACE
// (== CPU Project step 3). Neumann (mirror centre pressure) at solids/walls.
#include "SmokeCommon.hlsli"

[[vk::binding(0, 0)]] cbuffer GradCB : register(b0) {
    int4   gDim;      // xyz = dim
    float4 gInvVoxel; // xyz = invVoxel
};
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> gVel   : register(u0);
[[vk::binding(2, 0)]] StructuredBuffer<float>    gPress : register(t0);

[numthreads(8, 8, 8)]
void CSMain(uint3 gid : SV_DispatchThreadID) {
    int3 dim = gDim.xyz;
    if (any((int3)gid >= dim)) return;
    int3 p = (int3)gid;
    int x = p.x, y = p.y, z = p.z;
    uint i = SmokeIdx(p, dim);
    float pc = gPress[i];
    float pxp = SmokeOOB(int3(x + 1, y, z), dim) ? pc : gPress[SmokeIdx(int3(x + 1, y, z), dim)];
    float pxm = SmokeOOB(int3(x - 1, y, z), dim) ? pc : gPress[SmokeIdx(int3(x - 1, y, z), dim)];
    float pyp = SmokeOOB(int3(x, y + 1, z), dim) ? pc : gPress[SmokeIdx(int3(x, y + 1, z), dim)];
    float pym = SmokeOOB(int3(x, y - 1, z), dim) ? pc : gPress[SmokeIdx(int3(x, y - 1, z), dim)];
    float pzp = SmokeOOB(int3(x, y, z + 1), dim) ? pc : gPress[SmokeIdx(int3(x, y, z + 1), dim)];
    float pzm = SmokeOOB(int3(x, y, z - 1), dim) ? pc : gPress[SmokeIdx(int3(x, y, z - 1), dim)];
    float3 iv = gInvVoxel.xyz;
    float4 vel = gVel[i];
    vel.x -= 0.5 * (pxp - pxm) * iv.x;
    vel.y -= 0.5 * (pyp - pym) * iv.y;
    vel.z -= 0.5 * (pzp - pzm) * iv.z;
    gVel[i] = vel;
}
