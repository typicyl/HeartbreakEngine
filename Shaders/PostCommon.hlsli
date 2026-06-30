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
    float4 gPostParams3;  // brush-stroke area mask (minX, minY, maxX, maxY in screen UV)
    // World-anchored painterly censors (3D sphere test). Per censor:
    // gCensors[i] = (worldCenter.xyz, worldRadius); strength/feather packed per .x..w.
    float4 gCensors[4];
    float4 gCensorStrength;   // per-censor strength (.x=censor0 ... .w=censor3)
    float4 gCensorFeather;    // per-censor feather fraction (.x..w)
    uint4  gCensorCount;      // .x = active censor count
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

// Decode the 2-bit painterly mask packed into the forward HDR alpha by MeshPBR:
// .x = dynamic-exempt (restore crisp), .y = censored (a CensorComponent target).
// Values are {0, 1/3, 2/3, 1} sampled 1:1 so they read back exactly.
float2 DecodeMaskBits(float a)
{
    const float bits = a * 3.0f;                       // -> {0,1,2,3}
    const float censored = step(1.5f, bits);           // 2 or 3
    const float exempt = (abs(bits - 1.0f) < 0.5f || bits > 2.5f) ? 1.0f : 0.0f; // 1 or 3
    return float2(exempt, censored);
}

// Unproject a screen UV + depth back to a world position (gInvViewProj from b0).
float3 WorldFromDepthPost(float2 uv, float depth)
{
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 w = mul(gInvViewProj, float4(ndc, depth, 1.0f));
    return w.xyz / w.w;
}

// World-space painterly-censor weight at a world position: 1 fully inside a
// censor sphere, feathering to 0 at its radius (max over all active censors). A
// true 3D test - camera-angle independent, hits static + dynamic geometry alike.
float CensorWeightWS(float3 worldPos)
{
    float weight = 0.0f;
    [unroll] for (uint i = 0u; i < 4u; ++i)
    {
        if (i >= gCensorCount.x) break;
        const float3 center = gCensors[i].xyz;
        const float  radius = gCensors[i].w;
        const float  inner = radius * (1.0f - gCensorFeather[i]); // full strength within
        const float  d = distance(worldPos, center);
        weight = max(weight, (1.0f - smoothstep(inner, radius, d)) * gCensorStrength[i]);
    }
    return weight;
}

#endif // HBE_POST_COMMON_HLSLI
