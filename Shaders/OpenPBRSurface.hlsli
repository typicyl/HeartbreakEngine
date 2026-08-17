// Shaders/OpenPBRSurface.hlsli - OpenPBR Surface v1.1.1 building blocks.
//
// P2 (OpenPBR shading). These helpers replace the metallic-roughness core of MeshPBR's ShadeDirect
// with the OpenPBR base response while STAYING BACKWARD-COMPATIBLE: at the OpenPBR default parameters
// (specular_ior = 1.5, specular_weight = 1, specular_color = white, base_diffuse_roughness = 0) each
// helper reduces exactly to the previous look -
//   * IorToF0(1.5)            = 0.04            (== the old fixed dielectric F0)
//   * FresnelF82Tint(white)   = Schlick         (== the old metal Fresnel)
//   * OrenNayarDiffuse(sig=0) = NdotL           (== the old Lambert diffuse)
// so only materials that actually set the new OpenPBR parameters change appearance. The extended
// parameters are read from the appended cbuffer block gMatExt (Common.hlsli / rhi::GpuSurfaceMaterialExt).
//
// Scope note: P2 implements the OpenPBR BASE (dielectric specular from IOR, F82-tint metal,
// Oren-Nayar diffuse, energy-coupled) plus the coat IOR/tint. Fuzz, coat darkening, anisotropy,
// subsurface diffusion, transmission and thin-film are refined in later phases (P4-P7); their params
// already exist in gMatExt but are not fully evaluated here.
#ifndef HBE_OPENPBR_HLSLI
#define HBE_OPENPBR_HLSLI

#include "BRDF.hlsli" // PI, GGX/Smith, Charlie/Neubelt (shared microfacet terms)

// Normal-incidence reflectance of a dielectric interface from its IOR (relative to air).
// ior 1.5 -> 0.04. Used for both the base dielectric specular and the coat.
float IorToF0(float ior)
{
    float r = (ior - 1.0f) / (ior + 1.0f);
    return r * r;
}

// Schlick-Fresnel with an explicit grazing value F90 (roughness-faded by the caller). Reduces to
// standard Schlick when F90 = 1.
float3 FresnelSchlickF90(float cosTheta, float3 F0, float3 F90)
{
    return F0 + (F90 - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

// F82-tint conductor Fresnel (Kutz 2021; OpenPBR metal). F0 = base_color (normal-incidence colour);
// `edgeTint` (specular_color) tints the response near the F82 peak (~82 deg). When edgeTint = white
// this is IDENTICAL to Schlick, so default metals are unchanged. mu = VdotH.
float3 FresnelF82Tint(float mu, float3 F0, float3 edgeTint)
{
    const float muBar = 1.0f / 7.0f; // cos of ~81.7 deg, where the tint is applied
    float3 fSchlickMu = F0 + (1.0f - F0) * pow(saturate(1.0f - mu), 5.0f);
    float  sBar = pow(saturate(1.0f - muBar), 5.0f);
    float3 fSchlickBar = F0 + (1.0f - F0) * sBar;
    // Target reflectance at muBar = edgeTint * Schlick(muBar); when edgeTint = 1 the correction is 0.
    float3 target = edgeTint * fSchlickBar;
    float  denom = muBar * pow(1.0f - muBar, 6.0f);
    float  num = mu * pow(saturate(1.0f - mu), 6.0f);
    return fSchlickMu - (num / denom) * (fSchlickBar - target);
}

// Energy-preserving Oren-Nayar diffuse (Fujii's "improved" qualitative model). Returns the diffuse
// response (to be multiplied by albedo/PI and radiance). sigma = base_diffuse_roughness in [0,1];
// sigma = 0 gives exactly Lambert (NdotL), so default materials are unchanged.
float OrenNayarDiffuse(float NdotL, float NdotV, float LdotV, float sigma)
{
    float s = LdotV - NdotL * NdotV;
    float t = (s > 0.0f) ? s / max(max(NdotL, NdotV), 1e-4f) : s;
    float sig2 = sigma * sigma;
    float A = 1.0f - 0.5f * sig2 / (sig2 + 0.33f);
    float B = 0.45f * sig2 / (sig2 + 0.09f);
    return saturate(NdotL) * (A + B * max(0.0f, t));
}

#endif // HBE_OPENPBR_HLSLI
