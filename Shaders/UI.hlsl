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
    // NDC clip rect (xy = min, zw = max). The CPU emits the open sentinel
    // (-2..2) for unclipped content; ScrollViews emit their view rect.
    float4 clipRect    : TEXCOORD2;
    uint   fx          : TEXCOORD3; // per-element effect id (0 = normal)
};

struct VSOutput
{
    float4 positionCS : SV_Position;
    float2 uv         : TEXCOORD0;
    float4 color      : COLOR0;
    nointerpolation uint   texIndex : TEXCOORD1;
    nointerpolation float4 clipRect : TEXCOORD2;
    float2 ndcPos     : TEXCOORD3; // interpolated NDC position for the clip test
    nointerpolation uint   fx       : TEXCOORD4;
};

VSOutput VSMain(VSInput input)
{
    VSOutput o;
    o.positionCS = float4(input.positionNDC, 0.0f, 1.0f);
    o.uv = input.uv;
    o.color = input.color;
    o.texIndex = input.texIndex;
    o.clipRect = input.clipRect;
    o.ndcPos = input.positionNDC;
    o.fx = input.fx;
    return o;
}

float4 PSMain(VSOutput input) : SV_Target
{
    // Per-vertex clip rect (ScrollView content): reject fragments outside it.
    if (input.ndcPos.x < input.clipRect.x || input.ndcPos.y < input.clipRect.y ||
        input.ndcPos.x > input.clipRect.z || input.ndcPos.y > input.clipRect.w)
        discard;
    float4 tex = gUITextures[NonUniformResourceIndex(input.texIndex)]
                     .Sample(gUISampler, input.uv);
    float4 result = input.color * tex; // fx 0 (default) = plain passthrough
    if (input.fx == 1u) // grayscale / desaturate (custom UI material)
    {
        float l = dot(result.rgb, float3(0.299f, 0.587f, 0.114f));
        result.rgb = float3(l, l, l);
    }
    return result;
}
