// Shaders/Particle.hlsl - camera-facing particle billboards.
//
// Vertices arrive in WORLD space (the CPU billboards them against the camera
// basis) with a per-vertex colour + bindless sprite index. The VS just projects
// by the frame view-proj; the PS tints the sprite, or draws a procedural soft
// round dot when no sprite is set (texIndex 0). Drawn in the HDR scene pass with
// depth test (no write), so additive sparks bloom downstream.
#include "Common.hlsli"

struct VSInput
{
    float3 positionWS : POSITION;
    float2 uv         : TEXCOORD0;
    float4 color      : COLOR0;
    uint   texIndex   : TEXCOORD1;
};

struct VSOutput
{
    float4 positionCS : SV_Position;
    float2 uv         : TEXCOORD0;
    float4 color      : COLOR0;
    nointerpolation uint texIndex : TEXCOORD1;
};

VSOutput VSMain(VSInput input)
{
    VSOutput o;
    o.positionCS = mul(gViewProj, float4(input.positionWS, 1.0f));
    o.uv = input.uv;
    o.color = input.color;
    o.texIndex = input.texIndex;
    return o;
}

float4 PSMain(VSOutput input) : SV_Target
{
    float4 c = input.color;
    if (input.texIndex != 0u)
    {
        c *= gTextures[NonUniformResourceIndex(input.texIndex)].Sample(gBindlessSampler, input.uv);
    }
    else
    {
        // Procedural soft round dot: radial falloff from the quad centre, so a
        // texture-less emitter still looks like a soft particle, not a square.
        float2 d = input.uv * 2.0f - 1.0f;
        float r = saturate(1.0f - dot(d, d));
        c.a *= r * r;
    }
    return c;
}
