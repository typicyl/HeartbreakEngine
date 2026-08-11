// SmokeAdvectScalars.hlsl - semi-Lagrangian advection of density/temperature by the PROJECTED
// velocity, with exp dissipation/cooling folded in (== CPU AdvectScalars). Manual 8-tap trilinear;
// temperature relaxes toward ambient. Fuel (.z) is carried (0 in the current solver).
#include "SmokeCommon.hlsli"

[[vk::binding(0, 0)]] cbuffer AdvScalCB : register(b0) {
    int4   gDim;         // xyz = dim
    float4 gInvVoxel_dt; // xyz = invVoxel, w = dt
    float4 gFades;       // x = densFade, y = tempFade, z = ambientTemp
};
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> gSclOut : register(u0);
[[vk::binding(2, 0)]] StructuredBuffer<float4>   gSclIn  : register(t0);
[[vk::binding(3, 0)]] StructuredBuffer<float4>   gVel    : register(t1);

[numthreads(8, 8, 8)]
void CSMain(uint3 gid : SV_DispatchThreadID) {
    int3 dim = gDim.xyz;
    if (any((int3)gid >= dim)) return;
    int3 c = (int3)gid;
    uint i = SmokeIdx(c, dim);

    float3 v = gVel[i].xyz;
    float3 gp = float3(c) - (v * gInvVoxel_dt.w) * gInvVoxel_dt.xyz;

    int3 b0, b1; float3 f;
    SmokeTrilinearSetup(gp, dim, b0, b1, f);
    float4 c000 = gSclIn[SmokeIdx(int3(b0.x, b0.y, b0.z), dim)];
    float4 c100 = gSclIn[SmokeIdx(int3(b1.x, b0.y, b0.z), dim)];
    float4 c010 = gSclIn[SmokeIdx(int3(b0.x, b1.y, b0.z), dim)];
    float4 c110 = gSclIn[SmokeIdx(int3(b1.x, b1.y, b0.z), dim)];
    float4 c001 = gSclIn[SmokeIdx(int3(b0.x, b0.y, b1.z), dim)];
    float4 c101 = gSclIn[SmokeIdx(int3(b1.x, b0.y, b1.z), dim)];
    float4 c011 = gSclIn[SmokeIdx(int3(b0.x, b1.y, b1.z), dim)];
    float4 c111 = gSclIn[SmokeIdx(int3(b1.x, b1.y, b1.z), dim)];
    float4 x00 = c000 + (c100 - c000) * f.x;
    float4 x10 = c010 + (c110 - c010) * f.x;
    float4 x01 = c001 + (c101 - c001) * f.x;
    float4 x11 = c011 + (c111 - c011) * f.x;
    float4 y0 = x00 + (x10 - x00) * f.y;
    float4 y1 = x01 + (x11 - x01) * f.y;
    float4 s = y0 + (y1 - y0) * f.z;

    float amb = gFades.z;
    float d = s.x * gFades.x;                    // density * densFade
    float t = amb + (s.y - amb) * gFades.y;       // temperature toward ambient
    gSclOut[i] = float4(d, t, s.z, 0.0);
}
