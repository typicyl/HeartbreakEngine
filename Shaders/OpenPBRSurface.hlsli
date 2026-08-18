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

// Anisotropic GGX (Trowbridge-Reitz) NDF. at/ab are the tangent/bitangent alpha (roughness^2-scale)
// values; TdotH/BdotH are the half-vector projected onto the tangent frame. Reduces to isotropic GGX
// when at == ab. Used for brushed-metal-style specular (OpenPBR specular_roughness_anisotropy).
float DistributionGGXAniso(float NdotH, float TdotH, float BdotH, float at, float ab)
{
    float d = (TdotH * TdotH) / max(at * at, 1e-8f) +
              (BdotH * BdotH) / max(ab * ab, 1e-8f) + NdotH * NdotH;
    return 1.0f / max(PI * at * ab * d * d, EPSILON);
}

// Maps a (perceptual) roughness r in [0,1] and anisotropy a in [0,1] to tangent/bitangent alpha,
// per the OpenPBR mapping: at = r^2 * sqrt(2/(1+(1-a)^2)), ab = (1-a)*at. a = 0 -> at == ab == r^2.
void AnisoAlpha(float roughness, float anisotropy, out float at, out float ab)
{
    float r2 = roughness * roughness;
    float k = 1.0f - saturate(anisotropy);
    at = max(r2 * sqrt(2.0f / (1.0f + k * k)), 1e-4f);
    ab = max(k * at, 1e-4f);
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

// ------------------------------------------------------------------------------------------------
// Thin-film iridescence (OpenPBR thin_film_*) - Belcour & Barla 2017, "A Practical Extension to
// Microfacet Theory for the Modeling of Varying Iridescence". This is the standard practical port
// (as used by glTF KHR_materials_iridescence): the airy reflectance of a single thin film over the
// base, integrated against CIE colour-matching sensitivities so the interference reads as the
// familiar soap-bubble / oil-slick / beetle-shell rainbow. It REPLACES the specular Fresnel term
// (it already contains the angular Fresnel falloff), so callers blend it in by thin_film_weight and
// at weight 0 the effect is entirely absent (the block is #if-gated in MeshPBR anyway).
// ------------------------------------------------------------------------------------------------
float  IriIorToF0(float t, float i) { float r = (t - i) / (t + i); return r * r; }
float3 IriIorToF0(float3 t, float i) { float3 r = (t - i) / (t + i); return r * r; }
float3 IriF0ToIor(float3 f0) { float3 s = sqrt(f0); return (float3(1, 1, 1) + s) / max(float3(1, 1, 1) - s, 1e-4f); }
float  IriFSchlick(float f0, float c) { return f0 + (1.0f - f0) * pow(saturate(1.0f - c), 5.0f); }
float3 IriFSchlick(float3 f0, float c) { return f0 + (float3(1, 1, 1) - f0) * pow(saturate(1.0f - c), 5.0f); }

// XYZ spectral integral of the phase term against CIE sensitivities (Belcour eq. via a Gaussian
// fit), converted to linear Rec.709. `opd` = optical path difference (nm); `shift` = per-channel
// phase shift (radians).
float3 EvalSensitivity(float opd, float3 shift)
{
    float phase = 2.0f * PI * opd * 1.0e-9f;
    float3 val = float3(5.4856e-13f, 4.4201e-13f, 5.2481e-13f);
    float3 pos = float3(1.6810e+06f, 1.7953e+06f, 2.2084e+06f);
    float3 var = float3(4.3278e+09f, 9.3046e+09f, 6.6121e+09f);
    float3 xyz = val * sqrt(2.0f * PI * var) * cos(pos * phase + shift) * exp(-var * phase * phase);
    xyz.x += 9.7470e-14f * sqrt(2.0f * PI * 4.5282e+09f) *
             cos(2.2399e+06f * phase + shift.x) * exp(-4.5282e+09f * phase * phase);
    xyz /= 1.0685e-7f;
    // Linear-sRGB (Rec.709) from CIE XYZ, written as explicit dot products to avoid matrix-order
    // ambiguity between HLSL/GLSL conventions.
    float3 rgb;
    rgb.x = dot(float3(3.2404542f, -1.5371385f, -0.4985314f), xyz);
    rgb.y = dot(float3(-0.9692660f, 1.8760108f, 0.0415560f), xyz);
    rgb.z = dot(float3(0.0556434f, -0.2040259f, 1.0572252f), xyz);
    return rgb;
}

// Iridescent Fresnel reflectance of a thin film of `filmIor` and `thicknessNm` (nanometres) over a
// base of reflectance `baseF0`, viewed at `cosTheta1` in a medium of `outsideIor` (1.0 = air).
// Returns the full angular reflectance (drop-in for the specular Fresnel). thicknessNm -> 0 (or the
// film IOR == outside IOR) reduces to the plain base Fresnel, so it is a smooth no-op at the edges.
float3 EvalIridescence(float outsideIor, float filmIor, float cosTheta1, float thicknessNm, float3 baseF0)
{
    // Force the film IOR back to the surrounding medium as thickness -> 0 (no film -> plain Fresnel).
    float iridIor = lerp(outsideIor, filmIor, smoothstep(0.0f, 0.03f, thicknessNm));
    float ratio = outsideIor / iridIor;
    float sinTheta2Sq = ratio * ratio * (1.0f - cosTheta1 * cosTheta1);
    float cosTheta2Sq = 1.0f - sinTheta2Sq;
    if (cosTheta2Sq < 0.0f) return float3(1, 1, 1); // total internal reflection in the film
    float cosTheta2 = sqrt(cosTheta2Sq);

    // First interface (outside/film).
    float R12 = IriFSchlick(IriIorToF0(iridIor, outsideIor), cosTheta1);
    float T121 = 1.0f - R12;
    float phi12 = (iridIor < outsideIor) ? PI : 0.0f;
    float phi21 = PI - phi12;

    // Second interface (film/base).
    float3 baseIor = IriF0ToIor(clamp(baseF0, 0.0f, 0.9999f));
    float3 R23 = IriFSchlick(IriIorToF0(baseIor, iridIor), cosTheta2);
    float3 phi23 = float3((baseIor.x < iridIor) ? PI : 0.0f,
                          (baseIor.y < iridIor) ? PI : 0.0f,
                          (baseIor.z < iridIor) ? PI : 0.0f);

    // Optical path difference through the film and the total phase per channel.
    float opd = 2.0f * iridIor * thicknessNm * cosTheta2;
    float3 phi = float3(phi21, phi21, phi21) + phi23;

    // Airy summation: DC term (m=0) + two interference orders (m=1,2).
    float3 R123 = clamp(R12 * R23, 1e-5f, 0.9999f);
    float3 r123 = sqrt(R123);
    float3 Rs = (T121 * T121) * R23 / (float3(1, 1, 1) - R123);
    float3 I = float3(R12, R12, R12) + Rs;
    float3 Cm = Rs - float3(T121, T121, T121);
    [unroll] for (int m = 1; m <= 2; ++m)
    {
        Cm *= r123;
        float3 Sm = 2.0f * EvalSensitivity(float(m) * opd, float(m) * phi);
        I += Cm * Sm;
    }
    return max(I, float3(0, 0, 0));
}

#endif // HBE_OPENPBR_HLSLI
