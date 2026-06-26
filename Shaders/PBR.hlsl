// Shaders/PBR.hlsl - forward physically-based shading pass.
//
// Metallic-roughness material with a single directional light plus a constant
// ambient term (placeholder for image-based lighting). Compile with DXC:
//
//   DXIL (D3D12):
//     dxc -T vs_6_6 -E VSMain PBR.hlsl -Fo PBR.vs.dxil
//     dxc -T ps_6_6 -E PSMain PBR.hlsl -Fo PBR.ps.dxil
//
//   SPIR-V (Vulkan):
//     dxc -spirv -T vs_6_6 -E VSMain PBR.hlsl -Fo PBR.vs.spv
//     dxc -spirv -T ps_6_6 -E PSMain PBR.hlsl -Fo PBR.ps.spv
//
#include "Common.hlsli"
#include "BRDF.hlsli"

// Material textures (sRGB base color; linear for the rest).
Texture2D    gBaseColorMap        : register(t0);
Texture2D    gNormalMap           : register(t1);
Texture2D    gMetallicRoughnessMap: register(t2);
Texture2D    gOcclusionMap        : register(t3);
SamplerState gLinearSampler       : register(s0);

struct VSInput
{
    float3 positionOS : POSITION;
    float3 normalOS   : NORMAL;
    float4 tangentOS  : TANGENT;   // .w = handedness (+/-1)
    float2 uv         : TEXCOORD0;
};

struct VSOutput
{
    float4 positionCS : SV_Position;
    float3 positionWS : TEXCOORD0;
    float3 normalWS   : TEXCOORD1;
    float4 tangentWS  : TEXCOORD2;
    float2 uv         : TEXCOORD3;
};

VSOutput VSMain(VSInput input)
{
    VSOutput o;
    float4 posWS = mul(gModel, float4(input.positionOS, 1.0f));
    o.positionWS = posWS.xyz;
    o.positionCS = mul(gViewProj, posWS);
    o.normalWS   = normalize(mul((float3x3)gNormalMatrix, input.normalOS));
    o.tangentWS  = float4(normalize(mul((float3x3)gModel, input.tangentOS.xyz)), input.tangentOS.w);
    o.uv         = input.uv;
    return o;
}

// Reconstructs a world-space normal from the tangent-space normal map.
float3 ApplyNormalMap(float3 N, float4 T, float2 uv)
{
    float3 tangentNormal = gNormalMap.Sample(gLinearSampler, uv).xyz * 2.0f - 1.0f;
    float3 n = normalize(N);
    float3 t = normalize(T.xyz - n * dot(n, T.xyz)); // Gram-Schmidt
    float3 b = cross(n, t) * T.w;
    float3x3 TBN = float3x3(t, b, n);
    return normalize(mul(tangentNormal, TBN));
}

float4 PSMain(VSOutput input) : SV_Target
{
    // --- Sample material ---------------------------------------------------
    float4 baseColor = gBaseColorMap.Sample(gLinearSampler, input.uv) * gBaseColorFactor;
    float3 albedo    = baseColor.rgb;

    float2 mr        = gMetallicRoughnessMap.Sample(gLinearSampler, input.uv).bg; // glTF: B=metal, G=rough
    float  metallic  = mr.x * gMetallicFactor;
    float  roughness = clamp(mr.y * gRoughnessFactor, 0.04f, 1.0f);
    float  occlusion = gOcclusionMap.Sample(gLinearSampler, input.uv).r;

    float3 N = ApplyNormalMap(input.normalWS, input.tangentWS, input.uv);
    float3 V = normalize(gCameraPosWS - input.positionWS);

    // --- Direct lighting (one directional light) ---------------------------
    float3 L         = normalize(gLightDirWS);
    float3 radiance  = gLightColor * gLightIntensity;
    float3 Lo        = CookTorrance(N, V, L, radiance, albedo, metallic, roughness);

    // --- Ambient (flat IBL placeholder) ------------------------------------
    float3 F0 = lerp(0.04f.xxx, albedo, metallic);
    float3 kS = FresnelSchlickRoughness(max(dot(N, V), 0.0f), F0, roughness);
    float3 kD = (1.0f - kS) * (1.0f - metallic);
    float3 ambient = kD * albedo * gAmbientIntensity * occlusion;

    // --- Composite + tonemap ----------------------------------------------
    float3 color = ambient + Lo;
    color *= gExposure;
    color  = TonemapACES(color);
    color  = LinearToSRGB(color);

    return float4(color, baseColor.a);
}
