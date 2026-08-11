// SmokePressureClear.hlsl - zero both pressure ping-pong buffers before the Jacobi loop
// (== CPU std::fill(pressure_, 0) before the solve; clearing both is harmless and parity-safe).
#include "SmokeCommon.hlsli"

[[vk::binding(0, 0)]] cbuffer ClearCB : register(b0) { int4 gDim; };
[[vk::binding(1, 0)]] RWStructuredBuffer<float> gPa : register(u0);
[[vk::binding(2, 0)]] RWStructuredBuffer<float> gPb : register(u1);

[numthreads(8, 8, 8)]
void CSMain(uint3 gid : SV_DispatchThreadID) {
    int3 dim = gDim.xyz;
    if (any((int3)gid >= dim)) return;
    uint i = SmokeIdx((int3)gid, dim);
    gPa[i] = 0.0;
    gPb[i] = 0.0;
}
