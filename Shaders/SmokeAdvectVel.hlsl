// SmokeAdvectVel.hlsl - semi-Lagrangian self-advection of velocity (== CPU AdvectVelocity).
// Manual 8-tap trilinear (no sampler on the compute path); lerp order X->Y->Z matches SampleVel.
#include "SmokeCommon.hlsli"

[[vk::binding(0, 0)]] cbuffer AdvCB : register(b0) {
    int4   gDim;         // xyz = dim
    float4 gInvVoxel_dt; // xyz = invVoxel, w = dt
};
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> gVelOut : register(u0);
[[vk::binding(2, 0)]] StructuredBuffer<float4>   gVelIn  : register(t0);

[numthreads(8, 8, 8)]
void CSMain(uint3 gid : SV_DispatchThreadID) {
    int3 dim = gDim.xyz;
    if (any((int3)gid >= dim)) return;
    int3 c = (int3)gid;
    uint i = SmokeIdx(c, dim);

    float3 v = gVelIn[i].xyz;
    float3 gp = float3(c) - (v * gInvVoxel_dt.w) * gInvVoxel_dt.xyz; // backtrace in grid space

    int3 b0, b1; float3 f;
    SmokeTrilinearSetup(gp, dim, b0, b1, f);
    float3 c000 = gVelIn[SmokeIdx(int3(b0.x, b0.y, b0.z), dim)].xyz;
    float3 c100 = gVelIn[SmokeIdx(int3(b1.x, b0.y, b0.z), dim)].xyz;
    float3 c010 = gVelIn[SmokeIdx(int3(b0.x, b1.y, b0.z), dim)].xyz;
    float3 c110 = gVelIn[SmokeIdx(int3(b1.x, b1.y, b0.z), dim)].xyz;
    float3 c001 = gVelIn[SmokeIdx(int3(b0.x, b0.y, b1.z), dim)].xyz;
    float3 c101 = gVelIn[SmokeIdx(int3(b1.x, b0.y, b1.z), dim)].xyz;
    float3 c011 = gVelIn[SmokeIdx(int3(b0.x, b1.y, b1.z), dim)].xyz;
    float3 c111 = gVelIn[SmokeIdx(int3(b1.x, b1.y, b1.z), dim)].xyz;
    float3 x00 = c000 + (c100 - c000) * f.x;
    float3 x10 = c010 + (c110 - c010) * f.x;
    float3 x01 = c001 + (c101 - c001) * f.x;
    float3 x11 = c011 + (c111 - c011) * f.x;
    float3 y0 = x00 + (x10 - x00) * f.y;
    float3 y1 = x01 + (x11 - x01) * f.y;
    float3 adv = y0 + (y1 - y0) * f.z;

    gVelOut[i] = float4(adv, gVelIn[i].w); // .w = this cell's solid mask (0 on GPU)
}
