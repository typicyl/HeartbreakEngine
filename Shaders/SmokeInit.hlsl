// SmokeInit.hlsl - one-time state init for the GPU Eulerian solver (Reset). Matches the CPU
// Reset(): velocity 0 (+ solid mask 0 in .w; no GPU obstacles), scalars density 0 / temperature
// ambient / fuel 0.
#include "SmokeCommon.hlsli"

[[vk::binding(0, 0)]] cbuffer InitCB : register(b0) {
    int4   gDim; // xyz = dim
    float4 gAmb; // x = ambientTemperature
};
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> gVel : register(u0);
[[vk::binding(2, 0)]] RWStructuredBuffer<float4> gScl : register(u1);

[numthreads(8, 8, 8)]
void CSMain(uint3 gid : SV_DispatchThreadID) {
    int3 dim = gDim.xyz;
    if (any((int3)gid >= dim)) return;
    uint i = SmokeIdx((int3)gid, dim);
    gVel[i] = float4(0, 0, 0, 0);
    gScl[i] = float4(0, gAmb.x, 0, 0);
}
