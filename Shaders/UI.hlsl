// Shaders/UI.hlsl - in-game UI overlay (screen-space textured triangles).
//
// Vertices arrive in NDC (canvas scaling happens on the CPU) with a bindless
// texture index per vertex: index 0 is the engine's 1x1 white texture, so
// solid quads and textured quads (images, the font atlas) share one pipeline.
// Colors are straight-alpha and blended over the lit scene.

// Bindless table (same registers/sets as Common.hlsli's).
[[vk::binding(0, 1)]] SamplerState gUISampler  : register(s0, space0);
[[vk::binding(1, 1)]] Texture2D    gUITextures[] : register(t0, space0);

struct VSInput
{
    float2 positionNDC : POSITION;
    float2 uv          : TEXCOORD0;
    float4 color       : COLOR0;
    uint   texIndex    : TEXCOORD1;
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
    o.positionCS = float4(input.positionNDC, 0.0f, 1.0f);
    o.uv = input.uv;
    o.color = input.color;
    o.texIndex = input.texIndex;
    return o;
}

float4 PSMain(VSOutput input) : SV_Target
{
    float4 tex = gUITextures[NonUniformResourceIndex(input.texIndex)]
                     .Sample(gUISampler, input.uv);
    return input.color * tex;
}
