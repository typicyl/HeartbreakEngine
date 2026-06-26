// Shaders/BRDF.hlsli - physically-based Cook-Torrance BRDF building blocks.
//
// Metallic-roughness workflow, energy-conserving, matching the conventions used
// by glTF 2.0 / Filament / UE4. All inputs are in linear space; lighting is
// computed in linear space and tonemapped at the end of the pixel shader.
#ifndef HBE_BRDF_HLSLI
#define HBE_BRDF_HLSLI

static const float PI = 3.14159265358979323846f;
static const float EPSILON = 1e-5f;

// Fresnel-Schlick approximation: reflectance at the given view angle.
float3 FresnelSchlick(float cosTheta, float3 F0)
{
    float f = pow(saturate(1.0f - cosTheta), 5.0f);
    return F0 + (1.0f - F0) * f;
}

// Fresnel-Schlick with roughness, used for ambient/IBL to avoid bright edges.
float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    float3 Fr = max((1.0f - roughness).xxx, F0);
    return F0 + (Fr - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

// Trowbridge-Reitz (GGX) normal distribution function.
float DistributionGGX(float NdotH, float roughness)
{
    float a  = roughness * roughness;
    float a2 = a * a;
    float d  = (NdotH * NdotH) * (a2 - 1.0f) + 1.0f;
    return a2 / max(PI * d * d, EPSILON);
}

// Smith geometry term using Schlick-GGX for a single direction.
float GeometrySchlickGGX(float NdotX, float roughness)
{
    // Direct-lighting remapping of roughness to k.
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    return NdotX / (NdotX * (1.0f - k) + k);
}

// Smith geometry term: shadowing-masking over both view and light directions.
float GeometrySmith(float NdotV, float NdotL, float roughness)
{
    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

// --- Cloth (fabric sheen) ---------------------------------------------------
// Charlie distribution (Estevez-Kulla 2017): an inverted-Gaussian NDF that gives
// fabric its bright grazing-angle rim (velvet/cotton fuzz).
float D_Charlie(float roughness, float NdotH)
{
    float invR = 1.0f / max(roughness, 1e-3f);
    float sin2h = max(1.0f - NdotH * NdotH, 1e-4f);
    return (2.0f + invR) * pow(sin2h, invR * 0.5f) / (2.0f * PI);
}

// Neubelt visibility term, matched to the Charlie NDF (soft, no hard falloff).
float V_Neubelt(float NdotV, float NdotL)
{
    return saturate(1.0f / (4.0f * (NdotL + NdotV - NdotL * NdotV)));
}

// Evaluates the full Cook-Torrance BRDF for one analytic light.
//   N  - surface normal              L - direction to light
//   V  - direction to viewer         radiance - incoming light radiance
// Returns outgoing radiance (diffuse + specular), not yet multiplied by NdotL
// of the caller; NdotL is applied inside.
float3 CookTorrance(float3 N, float3 V, float3 L, float3 radiance,
                    float3 albedo, float metallic, float roughness)
{
    float3 H = normalize(V + L);
    float NdotV = max(dot(N, V), EPSILON);
    float NdotL = max(dot(N, L), 0.0f);
    float NdotH = max(dot(N, H), 0.0f);
    float VdotH = max(dot(V, H), 0.0f);

    // Base reflectivity: 0.04 for dielectrics, albedo for metals.
    float3 F0 = lerp(0.04f.xxx, albedo, metallic);

    float  D = DistributionGGX(NdotH, roughness);
    float  G = GeometrySmith(NdotV, NdotL, roughness);
    float3 F = FresnelSchlick(VdotH, F0);

    float3 specular = (D * G * F) / max(4.0f * NdotV * NdotL, EPSILON);

    // Energy conservation: metals have no diffuse component.
    float3 kD = (1.0f - F) * (1.0f - metallic);
    float3 diffuse = kD * albedo / PI;

    return (diffuse + specular) * radiance * NdotL;
}

#endif // HBE_BRDF_HLSLI
