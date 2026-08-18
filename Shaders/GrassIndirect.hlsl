// Shaders/GrassIndirect.hlsl - GPU-driven grass, INDIRECT/COMPACTED variant.
//
// The opt-in true GPU-driven path. Unlike Grass.hlsl (which draws every generated blade as
// 6*N vertices and lets the VS collapse culled blades to a degenerate point), the compaction
// pass (GrassGenIndirect.cs) writes ONLY the visible blades into a dense buffer and an
// indirect draw-arg buffer {6, instanceCount, 0, 0}. The draw is vkCmdDrawIndirect /
// ExecuteIndirect of 6 verts x instanceCount instances, so this VS keys each blade off
// SV_InstanceID (== gl_InstanceIndex; firstInstance is always 0 so both backends agree) and
// the corner off SV_VertexID (0..5). Everything downstream (lit + shadowed PS, wind, tint)
// is byte-identical to Grass.hlsl - only the blade-index source changes.
#include "Common.hlsli"

// The COMPACTED blade records buffer (compute writes, VS reads), on the SetVertexShaderBuffer
// seam (D3D12 root param 6 / Vulkan set 2, binding 0). Same 32-byte record as Grass.hlsl.
[[vk::binding(0, 2)]] ByteAddressBuffer gGrassBlades : register(t2, space1);

static const uint kBladeBytes = 32u;

static const float3 kGrassBase = float3(0.11f, 0.24f, 0.07f);
static const float3 kGrassTip  = float3(0.34f, 0.55f, 0.20f);

struct VSOutput
{
    float4 posCS    : SV_Position;
    float3 posWS    : TEXCOORD0;
    float3 normalWS : TEXCOORD1;
    float3 tint     : TEXCOORD2;
    float  ao       : TEXCOORD3;
};

VSOutput VSMain(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    const uint corner = vid;          // 0..5 - the two triangles of the quad
    const uint b = iid * kBladeBytes; // one blade per instance (compacted, dense)

    const float3 posWS = asfloat(gGrassBlades.Load3(b));
    const float  height = asfloat(gGrassBlades.Load(b + 12u));
    const float2 dir = asfloat(gGrassBlades.Load2(b + 16u));
    const float  width = asfloat(gGrassBlades.Load(b + 24u));
    const float  phase = asfloat(gGrassBlades.Load(b + 28u));

    const float3 right = float3(dir.x, 0.0f, dir.y);
    const float3 fwd = float3(-dir.y, 0.0f, dir.x);
    const float3 up = float3(0.0f, 1.0f, 0.0f);

    const float t = gWeather.w;
    const float sway = sin(t * 1.6f + phase) * 0.18f + sin(t * 3.3f + phase * 1.7f) * 0.06f;

    const uint qc = (corner == 0u) ? 0u : (corner == 1u) ? 1u
                  : (corner == 2u) ? 2u : (corner == 3u) ? 0u
                  : (corner == 4u) ? 2u : 3u;
    const bool isTop = (qc == 1u || qc == 2u);
    const bool isRight = (qc == 2u || qc == 3u);

    const float hf = isTop ? 1.0f : 0.0f;
    const float widthFrac = isTop ? 0.14f : 1.0f;
    const float sideSign = isRight ? 1.0f : -1.0f;

    const float bend = sway * height * hf * hf;
    const float3 wp = posWS
                    + right * (sideSign * width * 0.5f * widthFrac)
                    + up * (height * hf)
                    + fwd * bend;

    VSOutput o;
    o.posCS = mul(gViewProj, float4(wp, 1.0f));
    o.posWS = wp;
    o.normalWS = normalize(up * 1.6f + fwd * (bend * 4.0f));
    o.tint = lerp(kGrassBase, kGrassTip, hf);
    o.ao = lerp(0.55f, 1.0f, hf);
    return o;
}

float4 PSMain(VSOutput i) : SV_Target
{
    const float3 N = normalize(i.normalWS);
    const float NdotL = saturate(dot(N, gLightDirWS));
    const float shadow = ShadowFactor(i.posWS, NdotL);

    float3 ambient;
    if (gIrradianceIndex != 0u)
        ambient = SampleBindless(gIrradianceIndex, EquirectUV(N)).rgb;
    else
        ambient = gAmbientIntensity.xxx * 3.0f;

    float3 col = i.tint * (ambient + gLightColor * (gLightIntensity * NdotL * shadow));
    col *= i.ao;

    if (gOutputLinear == 0u)
        col = LinearToSRGB(TonemapACES(col * gExposure));
    return float4(col, 1.0f);
}
