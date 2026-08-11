// SmokeEmit.hlsl - inject ONE emitter's grid source terms IN PLACE (== CPU EmitSources for one
// emitter). One dispatch is queued per active emitter (emitter data rides the constant block, the
// only race-free per-dispatch CPU channel under deferred execution - a CpuWrite ring would desync).
#include "SmokeCommon.hlsli"

[[vk::binding(0, 0)]] cbuffer EmitCB : register(b0) {
    int4   gDim;           // xyz = dim
    float4 gWorldMin_dt;   // xyz = worldMin, w = dt
    float4 gVoxel_amb;     // xyz = voxelSize, w = ambientTemp (unused here)
    float4 gCenter_cone;   // xyz = shape.center, w = coneHeight
    float4 gHalf_soft;     // xyz = shape.halfExtents, w = edgeSoftness
    float4 gRot;           // shape.rotation quat (x,y,z,w)
    float4 gVel_dens;      // xyz = inflow velocity, w = densityRate
    float4 gRates;         // x = temperatureRate, y = temperatureTarget, z = fuelRate, w = kind
};
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> gScl : register(u0);
[[vk::binding(2, 0)]] RWStructuredBuffer<float4> gVel : register(u1);

[numthreads(8, 8, 8)]
void CSMain(uint3 gid : SV_DispatchThreadID) {
    int3 dim = gDim.xyz;
    if (any((int3)gid >= dim)) return;
    int3 c = (int3)gid;
    uint i = SmokeIdx(c, dim);

    float3 wp = gWorldMin_dt.xyz + (float3(c) + 0.5) * gVoxel_amb.xyz; // voxelCenter
    int kind = (int)(gRates.w + 0.5);
    float cov = SmokeShapeCoverage(kind, gCenter_cone.xyz, gHalf_soft.xyz, gRot, gCenter_cone.w,
                                   gHalf_soft.w, wp);
    if (cov <= 0.0) return;

    float dt = gWorldMin_dt.w;
    float4 scl = gScl[i];
    scl.x += gVel_dens.w * cov * dt;          // density
    float k = saturate(gRates.x * cov * dt);   // temperature relaxation toward target
    scl.y += (gRates.y - scl.y) * k;
    scl.z += gRates.z * cov * dt;              // fuel
    gScl[i] = scl;

    float4 vel = gVel[i];
    vel.xyz += gVel_dens.xyz * (cov * dt);     // inflow
    gVel[i] = vel;
}
