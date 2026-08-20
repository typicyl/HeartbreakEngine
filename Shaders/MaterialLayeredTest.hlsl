// MaterialLayeredTest.hlsl - compile gate for the material-layer GPU library.
//
// Registering this kernel in cmake/ShaderCompile.cmake is what proves MaterialLayered.hlsli +
// Triplanar.hlsli compile to BOTH DXIL and SPIR-V (the silent-dormancy precedent: an unregistered
// shader simply does not exist). It exercises every public helper so a compile error in any of them
// fails the build. It is not dispatched at runtime; it exists for the shader-compile verification.
//
// Binding convention (ComputePipelineDesc, RHI.h): cb=b0/binding(0,0), uav i=u<i>/binding(1+i,0).
#include "MaterialLayered.hlsli"

[[vk::binding(0, 0)]]
cbuffer TestCB : register(b0) {
    uint gCount;
    uint gPad0;
    uint gPad1;
    uint gPad2;
};

[[vk::binding(1, 0)]] RWStructuredBuffer<float> gOut : register(u0);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint i = id.x;
    if (i >= gCount) return;

    const float3 n = normalize(float3(0.2f, 1.0f, 0.15f));
    const float3 worldPos = float3(1.5f, 0.3f, 3.0f);
    const float3 objPos = float3(0.25f, 0.0f, 0.75f);

    // Projection modes (size-independent tiling).
    float2 uvW = LayeredProjectUV(HBE_PROJ_WORLD, worldPos, objPos, float2(0.5f, 0.5f),
                                  float2(0.0f, 0.0f), n, float3(2.0f, 2.0f, 2.0f));
    float2 uvT = LayeredProjectUV(HBE_PROJ_TRIPLANAR, worldPos, objPos, float2(0.5f, 0.5f),
                                  float2(0.0f, 0.0f), n, float3(1.0f, 1.0f, 1.0f));

    // Triplanar weights + per-plane UVs + world tangent frame.
    float3 tw = TriplanarWeights(n);
    float2 ux, uy, uz;
    TriplanarUVs(worldPos, float3(1.0f, 1.0f, 1.0f), ux, uy, uz);
    float3 tanT, tanB;
    WorldTangentFrame(n, tanT, tanB);

    // Normal + height blend operators.
    float3 nb = LayeredBlendNormalRNM(float3(0, 0, 1), normalize(float3(0.1f, 0.2f, 1.0f)), 0.7f);
    float hw = LayeredHeightWeight(0.5f, 0.9f, 0.5f, 1.0f, 0.3f, 0.2f);

    // Full accumulate path.
    LayerSample acc;
    acc.albedo = float3(0.8f, 0.8f, 0.8f);
    acc.metallic = 0.0f;
    acc.roughness = 0.5f;
    acc.normalTS = float3(0, 0, 1);
    acc.height = 0.5f;
    LayerSample lyr;
    lyr.albedo = float3(0.1f, 0.05f, 0.02f);
    lyr.metallic = 0.0f;
    lyr.roughness = 0.9f;
    lyr.normalTS = normalize(float3(0.0f, 0.3f, 1.0f));
    lyr.height = 0.8f;
    LayeredAccumulate(acc, lyr, hw, true, true);

    gOut[i] = uvW.x + uvT.y + tw.y + ux.x + uy.y + uz.x + tanT.x + tanB.z + nb.z + acc.albedo.r +
              acc.roughness + acc.normalTS.z + acc.height;
}
