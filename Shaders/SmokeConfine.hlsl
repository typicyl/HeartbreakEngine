// SmokeConfine.hlsl - vorticity confinement force added to velocity IN PLACE (== CPU
// ApplyConfinement): f = eps*h*(N x curl), N = normalize(grad|curl|). Neighbours clamped.
#include "SmokeCommon.hlsli"

[[vk::binding(0, 0)]] cbuffer ConfCB : register(b0) {
    int4   gDim;       // xyz = dim
    float4 gInvVoxel;  // xyz = invVoxel
    float4 gEps_h_dt;  // x = eps, y = h, z = dt
};
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> gVel  : register(u0);
[[vk::binding(2, 0)]] StructuredBuffer<float4>   gCurl : register(t0);

#define MC(xx, yy, zz) gCurl[SmokeIdx(clamp(int3(xx, yy, zz), int3(0, 0, 0), dim - 1), dim)].w

[numthreads(8, 8, 8)]
void CSMain(uint3 gid : SV_DispatchThreadID) {
    int3 dim = gDim.xyz;
    if (any((int3)gid >= dim)) return;
    int3 p = (int3)gid;
    int x = p.x, y = p.y, z = p.z;
    uint i = SmokeIdx(p, dim);
    float3 iv = gInvVoxel.xyz;
    float3 grad = float3((MC(x + 1, y, z) - MC(x - 1, y, z)) * 0.5 * iv.x,
                         (MC(x, y + 1, z) - MC(x, y - 1, z)) * 0.5 * iv.y,
                         (MC(x, y, z + 1) - MC(x, y, z - 1)) * 0.5 * iv.z);
    float len = length(grad);
    if (len < 1e-5) return;
    float3 N = grad / len;
    float3 force = gEps_h_dt.x * gEps_h_dt.y * cross(N, gCurl[i].xyz);
    float4 vel = gVel[i];
    vel.xyz += force * gEps_h_dt.z;
    gVel[i] = vel;
}
