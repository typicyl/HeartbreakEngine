// Shaders/PostCommon.hlsli - shared declarations for the post-process stack.
//
// Every post pass is a fullscreen triangle reading its inputs through the
// bindless texture table (indices in PostConstants). UVs derive from
// SV_Position so the passes are viewport-flip agnostic (identical on D3D12
// and Vulkan regardless of the negative-height viewport trick).
#ifndef HBE_POST_COMMON_HLSLI
#define HBE_POST_COMMON_HLSLI

#include "Common.hlsli"

// Per-pass constants. Bound at the per-object slot (b1 / set 0 binding 1) so
// post passes reuse the existing constant-arena plumbing in both backends.
cbuffer PostConstants : register(b1)
{
    uint   gInput0;       // bindless texture indices (0 = 1x1 white)
    uint   gInput1;
    uint   gInput2;
    uint   gInput3;
    float2 gOutTexel;     // 1 / output target size
    float2 gInTexel;      // 1 / primary input size
    float4 gPostParams0;  // pass-specific
    float4 gPostParams1;
    float4 gPostParams2;  // painterly: (kuwaharaRadius, warmCool, reserved, reserved)
};

// Linear-clamp sampler for post sampling (the bindless s0 sampler wraps).
// D3D12: static sampler s1. Vulkan: immutable sampler at set 0 binding 2.
[[vk::binding(2, 0)]] SamplerState gClampSampler : register(s1, space0);

float4 SamplePost(uint index, float2 uv)
{
    return gTextures[NonUniformResourceIndex(index)].SampleLevel(gClampSampler, uv, 0.0f);
}

struct FSOutput
{
    float4 positionCS : SV_Position;
};

// Oversized fullscreen triangle: (-1,-1), (3,-1), (-1,3).
FSOutput VSMain(uint vertexId : SV_VertexID)
{
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    FSOutput o;
    o.positionCS = float4(uv * 2.0f - 1.0f, 0.0f, 1.0f);
    return o;
}

float Luma(float3 c)
{
    return dot(c, float3(0.299f, 0.587f, 0.114f));
}

#endif // HBE_POST_COMMON_HLSLI
