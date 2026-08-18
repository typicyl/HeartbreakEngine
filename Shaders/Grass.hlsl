// Shaders/Grass.hlsl - GPU-driven grass blades (VS builds each blade from SV_VertexID).
//
// The GPU twin of the vegetation grass path. A compute pass (GrassGen.cs) writes one
// 32-byte blade record per blade into a device-local buffer; this VS reads that buffer
// and expands each blade into a bent, tapered 6-vertex quad on the CPU-known count
// (DrawInstanced(6*N,1,0,0), never firstInstance - the SV_InstanceID vs gl_InstanceIndex
// divergence the particle path documents). It reuses the MAIN graphics root signature
// (FrameConstants b0 + the bindless table + the record SRV at t2/space1), so the PS is
// FULLY LIT + SHADOWED exactly like the mesh path - no bespoke lighting bindings.
#include "Common.hlsli"

// The blade records buffer (compute writes, VS reads), on the SetVertexShaderBuffer seam
// (D3D12 root param 6 / Vulkan set 2, binding 0). ByteAddressBuffer: hand-packed 32 B.
[[vk::binding(0, 2)]] ByteAddressBuffer gGrassBlades : register(t2, space1);

// One blade record - MUST match veg::GpuBlade (C++) and GrassGen.cs:
//   [0]  float3 posWS   (blade base, world space, terrain height baked in)
//   [12] float  height
//   [16] float2 dir     (blade facing: cos/sin of yaw)
//   [24] float  width
//   [28] float  phase   (wind phase; height==0 marks a culled/degenerate blade)
static const uint kBladeBytes = 32u;
static const uint kVertsPerBlade = 6u;

// Stylised grass colour (base darker, tip lighter). Kept in the shader (not per-blade) so
// the record stays 32 B; species tinting can move into the record later.
static const float3 kGrassBase = float3(0.11f, 0.24f, 0.07f);
static const float3 kGrassTip  = float3(0.34f, 0.55f, 0.20f);

struct VSOutput
{
    float4 posCS    : SV_Position;
    float3 posWS    : TEXCOORD0;
    float3 normalWS : TEXCOORD1;
    float3 tint     : TEXCOORD2;
    float  ao       : TEXCOORD3; // base-of-blade ambient occlusion
};

VSOutput VSMain(uint vid : SV_VertexID)
{
    const uint bld = vid / kVertsPerBlade;
    const uint corner = vid - bld * kVertsPerBlade;
    const uint b = bld * kBladeBytes;

    const float3 posWS = asfloat(gGrassBlades.Load3(b));
    const float  height = asfloat(gGrassBlades.Load(b + 12u));
    const float2 dir = asfloat(gGrassBlades.Load2(b + 16u));
    const float  width = asfloat(gGrassBlades.Load(b + 24u));
    const float  phase = asfloat(gGrassBlades.Load(b + 28u));

    const float3 right = float3(dir.x, 0.0f, dir.y);
    const float3 fwd = float3(-dir.y, 0.0f, dir.x);
    const float3 up = float3(0.0f, 1.0f, 0.0f);

    // Wind: bend the blade forward, more toward the tip (quadratic), swaying over time.
    const float t = gWeather.w;
    const float sway = sin(t * 1.6f + phase) * 0.18f + sin(t * 3.3f + phase * 1.7f) * 0.06f;

    // 2 triangles of a tapered quad. Quad corners: 0=baseL 1=tipL 2=tipR 3=baseR;
    // index order [0,1,2, 0,2,3]. Computed with selects (no dynamically-indexed array).
    const uint qc = (corner == 0u) ? 0u : (corner == 1u) ? 1u
                  : (corner == 2u) ? 2u : (corner == 3u) ? 0u
                  : (corner == 4u) ? 2u : 3u;
    const bool isTop = (qc == 1u || qc == 2u);
    const bool isRight = (qc == 2u || qc == 3u);

    const float hf = isTop ? 1.0f : 0.0f;
    const float widthFrac = isTop ? 0.14f : 1.0f; // taper toward the tip
    const float sideSign = isRight ? 1.0f : -1.0f;

    // A blade whose compute pass culled it has height 0 -> this collapses to a point.
    const float bend = sway * height * hf * hf;
    const float3 wp = posWS
                    + right * (sideSign * width * 0.5f * widthFrac)
                    + up * (height * hf)
                    + fwd * bend;

    VSOutput o;
    o.posCS = mul(gViewProj, float4(wp, 1.0f));
    o.posWS = wp;
    // Sky-biased normal (grass reads bright, lit from above) tilted by the bend.
    o.normalWS = normalize(up * 1.6f + fwd * (bend * 4.0f));
    o.tint = lerp(kGrassBase, kGrassTip, hf);
    o.ao = lerp(0.55f, 1.0f, hf); // darker near the ground
    return o;
}

float4 PSMain(VSOutput i) : SV_Target
{
    const float3 N = normalize(i.normalWS);
    const float NdotL = saturate(dot(N, gLightDirWS));
    const float shadow = ShadowFactor(i.posWS, NdotL);

    // Ambient from the scene IBL irradiance (matches the mesh path's sky ambient); flat
    // ambient fallback when no IBL is bound.
    float3 ambient;
    if (gIrradianceIndex != 0u)
        ambient = SampleBindless(gIrradianceIndex, EquirectUV(N)).rgb;
    else
        ambient = gAmbientIntensity.xxx * 3.0f;

    float3 col = i.tint * (ambient + gLightColor * (gLightIntensity * NdotL * shadow));
    col *= i.ao;

    // Emit linear radiance for the HDR post stack; tonemap inline only if it is off.
    if (gOutputLinear == 0u)
        col = LinearToSRGB(TonemapACES(col * gExposure));
    return float4(col, 1.0f);
}
