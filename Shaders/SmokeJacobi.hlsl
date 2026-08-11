// SmokeJacobi.hlsl - one Jacobi sweep of the pressure Poisson solve (== CPU Project step 2).
// Anisotropic coefficients c_a = 1/h_a^2; Neumann at solids/walls (out-of-domain neighbours are
// dropped from BOTH numerator and denominator). Ping-pong: reads gPin, writes gPout.
#include "SmokeCommon.hlsli"

[[vk::binding(0, 0)]] cbuffer JacobiCB : register(b0) {
    int4   gDim;   // xyz = dim
    float4 gCoeff; // x = cx, y = cy, z = cz
};
[[vk::binding(1, 0)]] RWStructuredBuffer<float> gPout : register(u0);
[[vk::binding(2, 0)]] StructuredBuffer<float>   gPin  : register(t0);
[[vk::binding(3, 0)]] StructuredBuffer<float>   gDiv  : register(t1);

[numthreads(8, 8, 8)]
void CSMain(uint3 gid : SV_DispatchThreadID) {
    int3 dim = gDim.xyz;
    if (any((int3)gid >= dim)) return;
    int3 p = (int3)gid;
    int x = p.x, y = p.y, z = p.z;
    uint i = SmokeIdx(p, dim);
    float cx = gCoeff.x, cy = gCoeff.y, cz = gCoeff.z;
    float num = -gDiv[i];
    float den = 0.0;
    if (!SmokeOOB(int3(x + 1, y, z), dim)) { num += cx * gPin[SmokeIdx(int3(x + 1, y, z), dim)]; den += cx; }
    if (!SmokeOOB(int3(x - 1, y, z), dim)) { num += cx * gPin[SmokeIdx(int3(x - 1, y, z), dim)]; den += cx; }
    if (!SmokeOOB(int3(x, y + 1, z), dim)) { num += cy * gPin[SmokeIdx(int3(x, y + 1, z), dim)]; den += cy; }
    if (!SmokeOOB(int3(x, y - 1, z), dim)) { num += cy * gPin[SmokeIdx(int3(x, y - 1, z), dim)]; den += cy; }
    if (!SmokeOOB(int3(x, y, z + 1), dim)) { num += cz * gPin[SmokeIdx(int3(x, y, z + 1), dim)]; den += cz; }
    if (!SmokeOOB(int3(x, y, z - 1), dim)) { num += cz * gPin[SmokeIdx(int3(x, y, z - 1), dim)]; den += cz; }
    gPout[i] = den > 1e-8 ? num / den : 0.0;
}
