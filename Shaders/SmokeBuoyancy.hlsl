// SmokeBuoyancy.hlsl - Fedkiw two-term body force added to velocity IN PLACE (== CPU ApplyBuoyancy):
// f = (beta*(T - Tamb) - alpha*density) * up; vel += f*dt.
#include "SmokeCommon.hlsli"

[[vk::binding(0, 0)]] cbuffer BuoyCB : register(b0) {
    int4   gDim;          // xyz = dim
    float4 gUp_beta;      // xyz = up, w = beta
    float4 gAlpha_amb_dt; // x = alpha, y = ambientTemp, z = dt
};
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> gVel : register(u0);
[[vk::binding(2, 0)]] StructuredBuffer<float4>   gScl : register(t0);

[numthreads(8, 8, 8)]
void CSMain(uint3 gid : SV_DispatchThreadID) {
    int3 dim = gDim.xyz;
    if (any((int3)gid >= dim)) return;
    uint i = SmokeIdx((int3)gid, dim);
    float4 vel = gVel[i];
    float4 scl = gScl[i];
    float force = gUp_beta.w * (scl.y - gAlpha_amb_dt.y) - gAlpha_amb_dt.x * scl.x;
    vel.xyz += gUp_beta.xyz * (force * gAlpha_amb_dt.z);
    gVel[i] = vel;
}
