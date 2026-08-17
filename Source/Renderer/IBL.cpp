// Renderer/IBL.cpp
//
// CPU precomputation of image-based lighting from a physically-based atmosphere:
//   * sky         - Rayleigh + Mie single-scattering ray-march (HDR equirect)
//   * irradiance  - cosine-weighted hemisphere convolution (diffuse)
//   * prefiltered - GGX importance-sampled specular, roughness per mip
//   * BRDF LUT    - split-sum environment BRDF integration
// The atmosphere is ray-marched once into the sky equirect; the irradiance and
// prefiltered convolutions then sample that baked sky, so both the background
// and the ambient lighting come from the same physically-based model. Everything
// is equirectangular RGBA32F through the bindless path (no cubemaps/RTs).
#include "Renderer/IBL.h"
#include "Renderer/Renderer.h"
#include "Assets/Mesh.h"
#include "Assets/MeshGenerator.h"
#include "Assets/UAF.h"
#include "Assets/VFS.h"
#include "Core/JobSystem.h"
#include "Core/Log.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace hbe {
namespace {

constexpr f32 kPi = glm::pi<f32>();

// Equirectangular UV <-> direction (matches Common.hlsli EquirectUV).
glm::vec3 DirFromUV(f32 u, f32 v) {
    const f32 phi = (u - 0.5f) * 2.0f * kPi;
    const f32 theta = v * kPi;
    const f32 st = std::sin(theta);
    return {st * std::cos(phi), std::cos(theta), st * std::sin(phi)};
}

// --- Physically-based atmosphere (Rayleigh + Mie single scattering) ----------
// Compact ray-march after wwwtyro's "atmosphere" (Bruneton-family single
// scatter), Earth parameters. Marches the view ray through the atmosphere,
// integrating out-scatter toward the sun per step.
glm::vec2 RaySphere(const glm::vec3& r0, const glm::vec3& rd, f32 sr) {
    const f32 a = glm::dot(rd, rd);
    const f32 b = 2.0f * glm::dot(rd, r0);
    const f32 c = glm::dot(r0, r0) - sr * sr;
    f32 d = b * b - 4.0f * a * c;
    if (d < 0.0f) return {1e9f, -1e9f};
    d = std::sqrt(d);
    return {(-b - d) / (2.0f * a), (-b + d) / (2.0f * a)};
}

glm::vec3 Atmosphere(glm::vec3 r, const glm::vec3& r0, glm::vec3 pSun, f32 iSun,
                     f32 rPlanet, f32 rAtmos, const glm::vec3& kRlh, f32 kMie,
                     f32 shRlh, f32 shMie, f32 g) {
    pSun = glm::normalize(pSun);
    r = glm::normalize(r);
    constexpr int iSteps = 16, jSteps = 8;

    glm::vec2 p = RaySphere(r0, r, rAtmos);
    if (p.x > p.y) return glm::vec3(0.0f);
    // Clamp the far end to the planet surface when the ray dips into the ground.
    const glm::vec2 pg = RaySphere(r0, r, rPlanet);
    if (pg.x <= pg.y && pg.y > 0.0f) p.y = glm::min(p.y, glm::max(pg.x, 0.0f));
    p.x = glm::max(p.x, 0.0f);
    const f32 iStepSize = (p.y - p.x) / static_cast<f32>(iSteps);

    f32 iTime = p.x;
    glm::vec3 totalRlh(0.0f), totalMie(0.0f);
    f32 iOdRlh = 0.0f, iOdMie = 0.0f;

    const f32 mu = glm::dot(r, pSun);
    const f32 mumu = mu * mu;
    const f32 gg = g * g;
    const f32 pRlh = 3.0f / (16.0f * kPi) * (1.0f + mumu);
    const f32 pMie = 3.0f / (8.0f * kPi) * ((1.0f - gg) * (mumu + 1.0f)) /
                     (std::pow(1.0f + gg - 2.0f * mu * g, 1.5f) * (2.0f + gg));

    for (int i = 0; i < iSteps; ++i) {
        const glm::vec3 iPos = r0 + r * (iTime + iStepSize * 0.5f);
        const f32 iHeight = glm::length(iPos) - rPlanet;
        const f32 odStepRlh = std::exp(-iHeight / shRlh) * iStepSize;
        const f32 odStepMie = std::exp(-iHeight / shMie) * iStepSize;
        iOdRlh += odStepRlh;
        iOdMie += odStepMie;

        const f32 jStepSize = RaySphere(iPos, pSun, rAtmos).y / static_cast<f32>(jSteps);
        f32 jTime = 0.0f, jOdRlh = 0.0f, jOdMie = 0.0f;
        for (int j = 0; j < jSteps; ++j) {
            const glm::vec3 jPos = iPos + pSun * (jTime + jStepSize * 0.5f);
            const f32 jHeight = glm::length(jPos) - rPlanet;
            jOdRlh += std::exp(-jHeight / shRlh) * jStepSize;
            jOdMie += std::exp(-jHeight / shMie) * jStepSize;
            jTime += jStepSize;
        }
        const glm::vec3 attn = glm::exp(-(kMie * (iOdMie + jOdMie) + kRlh * (iOdRlh + jOdRlh)));
        totalRlh += odStepRlh * attn;
        totalMie += odStepMie * attn;
        iTime += iStepSize;
    }
    return iSun * (pRlh * kRlh * totalRlh + pMie * kMie * totalMie);
}

// HDR sky radiance for a view direction, plus a crisp sun disc and a dim
// ground bounce below the horizon (so the diffuse IBL has a floor term).
glm::vec3 SkyColor(const glm::vec3& d, const ProceduralSkyParams& p) {
    const glm::vec3 dir = glm::normalize(d);
    const glm::vec3 sunDir = glm::normalize(p.sunDir);

    constexpr f32 rPlanet = 6371e3f, rAtmos = 6471e3f;
    const glm::vec3 kRlh(5.8e-6f, 13.5e-6f, 33.1e-6f); // Rayleigh (sky blue)
    constexpr f32 kMie = 21e-6f, shRlh = 8000.0f, shMie = 1200.0f, gMie = 0.758f;
    const glm::vec3 r0(0.0f, rPlanet + 1000.0f, 0.0f); // observer ~1km up
    const f32 iSun = 22.0f * glm::max(p.sunIntensity / 40.0f, 0.0f);
    const f32 skyScale = glm::max(p.skyIntensity, 0.0f);

    // Night floor: the pure single-scatter atmosphere trends to ~0 once the sun drops
    // below the horizon, but a real night sky keeps a deep-blue glow (matches Sky.hlsl
    // NightSky). Add it so the dynamic-IBL re-bake's ambient/reflections aren't black at
    // night while the visible sky is a lit starfield. Zero during the day, so the boot
    // bake (static daytime sun) is unaffected.
    const f32 night = glm::clamp(-sunDir.y * 6.0f + 0.10f, 0.0f, 1.0f);
    const glm::vec3 nightGlow = glm::vec3(0.028f, 0.05f, 0.10f) *
                                (0.55f + 0.45f * glm::clamp(dir.y, 0.0f, 1.0f)) * night * skyScale;

    if (dir.y < 0.0f) {
        // Below the horizon: dim ground bounce of the horizon sky, tinted by the
        // ground albedo so the lower IBL hemisphere isn't pitch black.
        const glm::vec3 horizon =
            Atmosphere(glm::normalize(glm::vec3(dir.x, 0.03f, dir.z)), r0, sunDir, iSun,
                       rPlanet, rAtmos, kRlh, kMie, shRlh, shMie, gMie);
        return horizon * p.ground * skyScale + nightGlow;
    }

    glm::vec3 col = Atmosphere(dir, r0, sunDir, iSun, rPlanet, rAtmos, kRlh, kMie, shRlh,
                               shMie, gMie);

    // Crisp sun disc (~0.5 deg) for sharp specular highlights, modulated by the
    // atmospheric attenuation already encoded in the scattered colour's brightness.
    const f32 cosSun = glm::dot(dir, sunDir);
    const f32 disc = glm::smoothstep(0.99975f, 0.99995f, cosSun);
    col += p.sunTint * (disc * iSun * 6.0f);
    return col * skyScale + nightGlow;
}

// --- Low-discrepancy sampling ---------------------------------------------
f32 RadicalInverseVdC(u32 bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<f32>(bits) * 2.3283064365386963e-10f;
}
glm::vec2 Hammersley(u32 i, u32 n) {
    return {static_cast<f32>(i) / static_cast<f32>(n), RadicalInverseVdC(i)};
}

glm::vec3 ImportanceSampleGGX(const glm::vec2& Xi, const glm::vec3& N, f32 roughness) {
    const f32 a = roughness * roughness;
    const f32 phi = 2.0f * kPi * Xi.x;
    const f32 cosTheta = std::sqrt((1.0f - Xi.y) / (1.0f + (a * a - 1.0f) * Xi.y));
    const f32 sinTheta = std::sqrt(1.0f - cosTheta * cosTheta);

    const glm::vec3 h{sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta};
    const glm::vec3 up = std::abs(N.z) < 0.999f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0);
    const glm::vec3 tangent = glm::normalize(glm::cross(up, N));
    const glm::vec3 bitangent = glm::cross(N, tangent);
    return glm::normalize(tangent * h.x + bitangent * h.y + N * h.z);
}

f32 GeometrySchlickGGX(f32 NdotV, f32 k) { return NdotV / (NdotV * (1.0f - k) + k); }
f32 GeometrySmithIBL(f32 NdotV, f32 NdotL, f32 roughness) {
    const f32 k = (roughness * roughness) * 0.5f; // IBL k
    return GeometrySchlickGGX(NdotV, k) * GeometrySchlickGGX(NdotL, k);
}

// Bilinear sample of the baked sky equirect (wrap in u, clamp in v).
glm::vec3 SampleSkyEquirect(const std::vector<glm::vec4>& sky, u32 w, u32 h,
                            const glm::vec3& dir) {
    const glm::vec3 d = glm::normalize(dir);
    const f32 u = std::atan2(d.z, d.x) * (0.5f / kPi) + 0.5f;
    const f32 v = std::acos(glm::clamp(d.y, -1.0f, 1.0f)) / kPi;
    const f32 fx = u * w - 0.5f, fy = v * h - 0.5f;
    const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
    const f32 tx = fx - x0, ty = fy - y0;
    const auto at = [&](int x, int y) -> glm::vec3 {
        x = ((x % static_cast<int>(w)) + static_cast<int>(w)) % static_cast<int>(w);
        y = glm::clamp(y, 0, static_cast<int>(h) - 1);
        return glm::vec3(sky[static_cast<usize>(y) * w + x]);
    };
    return glm::mix(glm::mix(at(x0, y0), at(x0 + 1, y0), tx),
                    glm::mix(at(x0, y0 + 1), at(x0 + 1, y0 + 1), tx), ty);
}

// --- Map generators (return tightly packed RGBA32F) ------------------------
// All sample the baked sky equirect so the convolutions stay cheap (a texture
// fetch, not an atmosphere ray-march). Rows run across the job system.
std::vector<glm::vec4> GenerateIrradiance(u32 w, u32 h, const std::vector<glm::vec4>& sky,
                                          u32 sw, u32 sh) {
    std::vector<glm::vec4> out(static_cast<usize>(w) * h);
    const f32 delta = 0.045f;
    jobs::ParallelFor(h, 4, [&](u32 yBegin, u32 yEnd) {
        for (u32 y = yBegin; y < yEnd; ++y) {
            for (u32 x = 0; x < w; ++x) {
                const glm::vec3 N = DirFromUV((x + 0.5f) / w, (y + 0.5f) / h);
                const glm::vec3 up = std::abs(N.y) < 0.999f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
                const glm::vec3 right = glm::normalize(glm::cross(up, N));
                const glm::vec3 trueUp = glm::cross(N, right);

                glm::vec3 irradiance(0.0f);
                u32 samples = 0;
                for (f32 phi = 0.0f; phi < 2.0f * kPi; phi += delta) {
                    for (f32 theta = 0.0f; theta < 0.5f * kPi; theta += delta) {
                        const f32 st = std::sin(theta), ct = std::cos(theta);
                        const glm::vec3 tan{st * std::cos(phi), st * std::sin(phi), ct};
                        const glm::vec3 dir = right * tan.x + trueUp * tan.y + N * tan.z;
                        irradiance += SampleSkyEquirect(sky, sw, sh, dir) * ct * st;
                        ++samples;
                    }
                }
                irradiance = kPi * irradiance / static_cast<f32>(glm::max(samples, 1u));
                out[y * w + x] = glm::vec4(irradiance, 1.0f);
            }
        }
    });
    return out;
}

std::vector<glm::vec4> GeneratePrefilteredMip(u32 w, u32 h, f32 roughness, u32 samples,
                                              const std::vector<glm::vec4>& sky, u32 sw, u32 sh) {
    std::vector<glm::vec4> out(static_cast<usize>(w) * h);
    jobs::ParallelFor(h, 4, [&](u32 yBegin, u32 yEnd) {
        for (u32 y = yBegin; y < yEnd; ++y) {
            for (u32 x = 0; x < w; ++x) {
                const glm::vec3 N = DirFromUV((x + 0.5f) / w, (y + 0.5f) / h);
                const glm::vec3 V = N;
                glm::vec3 color(0.0f);
                f32 weight = 0.0f;
                for (u32 i = 0; i < samples; ++i) {
                    const glm::vec3 H = ImportanceSampleGGX(Hammersley(i, samples), N, roughness);
                    const glm::vec3 L = glm::normalize(2.0f * glm::dot(V, H) * H - V);
                    const f32 NdotL = glm::dot(N, L);
                    if (NdotL > 0.0f) {
                        color += SampleSkyEquirect(sky, sw, sh, L) * NdotL;
                        weight += NdotL;
                    }
                }
                color /= glm::max(weight, 1e-3f);
                out[y * w + x] = glm::vec4(color, 1.0f);
            }
        }
    });
    return out;
}

std::vector<glm::vec4> GenerateBrdfLUT(u32 size, u32 samples) {
    std::vector<glm::vec4> out(static_cast<usize>(size) * size);
    for (u32 y = 0; y < size; ++y) {
        const f32 roughness = (y + 0.5f) / size;
        for (u32 x = 0; x < size; ++x) {
            const f32 NdotV = glm::max((x + 0.5f) / size, 1e-3f);
            const glm::vec3 V{std::sqrt(1.0f - NdotV * NdotV), 0.0f, NdotV};
            const glm::vec3 N{0, 0, 1};
            f32 A = 0.0f, B = 0.0f;
            for (u32 i = 0; i < samples; ++i) {
                const glm::vec3 H = ImportanceSampleGGX(Hammersley(i, samples), N, roughness);
                const glm::vec3 L = glm::normalize(2.0f * glm::dot(V, H) * H - V);
                const f32 NdotL = glm::max(L.z, 0.0f);
                const f32 NdotH = glm::max(H.z, 0.0f);
                const f32 VdotH = glm::max(glm::dot(V, H), 0.0f);
                if (NdotL > 0.0f) {
                    const f32 G = GeometrySmithIBL(NdotV, NdotL, roughness);
                    const f32 G_Vis = (G * VdotH) / (NdotH * NdotV);
                    const f32 Fc = std::pow(1.0f - VdotH, 5.0f);
                    A += (1.0f - Fc) * G_Vis;
                    B += Fc * G_Vis;
                }
            }
            out[y * size + x] = glm::vec4(A / samples, B / samples, 0.0f, 1.0f);
        }
    }
    return out;
}

// --- Pre-integrated subsurface scattering (Penner 2011) --------------------
// A 2D LUT: x = N.L (0=back-lit .. 1=front-lit), y = curvature (0=flat ..
// 1=tight). Each texel integrates a skin diffusion profile over a ring of
// incident directions, giving the soft, reddened wrap of light around the
// terminator that defines skin. Sampled per-pixel in MeshPBR for the subsurface
// material; curvature is reconstructed from screen-space derivatives.
glm::vec3 SkinDiffusionProfile(f32 r) {
    // d'Eon/Luebke 6-Gaussian skin profile (GPU Gems 3), per-channel weights.
    const auto G = [](f32 v, f32 x) { return std::exp(-(x * x) / (2.0f * v)) / (2.0f * kPi * v); };
    return G(0.0064f, r) * glm::vec3(0.233f, 0.455f, 0.649f) +
           G(0.0484f, r) * glm::vec3(0.100f, 0.336f, 0.344f) +
           G(0.1870f, r) * glm::vec3(0.118f, 0.198f, 0.000f) +
           G(0.5670f, r) * glm::vec3(0.113f, 0.007f, 0.007f) +
           G(1.9900f, r) * glm::vec3(0.358f, 0.004f, 0.000f) +
           G(7.4100f, r) * glm::vec3(0.078f, 0.000f, 0.000f);
}

std::vector<glm::vec4> GenerateSkinLUT(u32 size) {
    std::vector<glm::vec4> out(static_cast<usize>(size) * size);
    jobs::ParallelFor(size, 4, [&](u32 yBegin, u32 yEnd) {
        for (u32 y = yBegin; y < yEnd; ++y) {
            // Curvature -> ring radius. Small radius (high curvature) scatters most.
            const f32 curvature = (y + 0.5f) / size;
            const f32 radius = 1.0f / glm::max(curvature * 8.0f, 1e-3f);
            for (u32 x = 0; x < size; ++x) {
                const f32 NdotL = (x + 0.5f) / size * 2.0f - 1.0f; // -1..1
                const f32 theta = std::acos(glm::clamp(NdotL, -1.0f, 1.0f));
                glm::vec3 light(0.0f), weights(0.0f);
                const f32 inc = 0.02f;
                for (f32 a = -kPi * 0.5f; a <= kPi * 0.5f; a += inc) {
                    const f32 diffuse = glm::clamp(std::cos(theta + a), 0.0f, 1.0f);
                    const f32 sampleDist = std::abs(2.0f * radius * std::sin(a * 0.5f));
                    const glm::vec3 w = SkinDiffusionProfile(sampleDist);
                    light += diffuse * w;
                    weights += w;
                }
                const glm::vec3 d = light / glm::max(weights, glm::vec3(1e-4f));
                out[y * size + x] = glm::vec4(d, 1.0f);
            }
        }
    });
    return out;
}

} // namespace

IBLMaps GenerateProceduralIBL(Renderer& renderer, const ProceduralSkyParams& params) {
    IBLMaps maps;
    if (!renderer.SupportsScene()) return maps;

    const auto t0 = std::chrono::high_resolution_clock::now();

    // --- Sky: ray-march the atmosphere once into the equirect (parallel rows).
    // Irradiance + prefiltered then sample this baked sky. ---
    constexpr u32 kSkyW = 1024, kSkyH = 512;
    std::vector<glm::vec4> skyPx(static_cast<usize>(kSkyW) * kSkyH);
    jobs::ParallelFor(kSkyH, 8, [&](u32 yBegin, u32 yEnd) {
        for (u32 y = yBegin; y < yEnd; ++y) {
            for (u32 x = 0; x < kSkyW; ++x) {
                const glm::vec3 dir = DirFromUV((x + 0.5f) / kSkyW, (y + 0.5f) / kSkyH);
                skyPx[y * kSkyW + x] = glm::vec4(SkyColor(dir, params), 1.0f);
            }
        }
    });
    {
        rhi::TextureDesc d;
        d.width = kSkyW; d.height = kSkyH;
        d.format = rhi::Format::R32G32B32A32_FLOAT;
        d.pixels = skyPx.data();
        d.debugName = "ibl_sky";
        maps.sky = renderer.UploadTexture(d);
    }

    // --- Irradiance ---
    constexpr u32 kIrrW = 64, kIrrH = 32;
    {
        std::vector<glm::vec4> px = GenerateIrradiance(kIrrW, kIrrH, skyPx, kSkyW, kSkyH);
        rhi::TextureDesc d;
        d.width = kIrrW; d.height = kIrrH;
        d.format = rhi::Format::R32G32B32A32_FLOAT;
        d.pixels = px.data();
        d.debugName = "ibl_irradiance";
        maps.irradiance = renderer.UploadTexture(d);
    }

    // --- Prefiltered specular (roughness per mip) ---
    constexpr u32 kPreW = 128, kPreH = 64, kMips = 5;
    {
        std::vector<glm::vec4> packed;
        for (u32 mip = 0; mip < kMips; ++mip) {
            const u32 mw = glm::max(1u, kPreW >> mip);
            const u32 mh = glm::max(1u, kPreH >> mip);
            const f32 roughness = static_cast<f32>(mip) / (kMips - 1);
            const u32 samples = mip == 0 ? 1u : 96u;
            std::vector<glm::vec4> level =
                GeneratePrefilteredMip(mw, mh, roughness, samples, skyPx, kSkyW, kSkyH);
            packed.insert(packed.end(), level.begin(), level.end());
        }
        rhi::TextureDesc d;
        d.width = kPreW; d.height = kPreH;
        d.format = rhi::Format::R32G32B32A32_FLOAT;
        d.pixels = packed.data();
        d.mipCount = kMips;
        d.debugName = "ibl_prefiltered";
        maps.prefiltered = renderer.UploadTexture(d);
        maps.prefilteredMaxLod = static_cast<f32>(kMips - 1);
    }

    // --- BRDF LUT ---
    constexpr u32 kLut = 128;
    {
        std::vector<glm::vec4> px = GenerateBrdfLUT(kLut, 256);
        rhi::TextureDesc d;
        d.width = kLut; d.height = kLut;
        d.format = rhi::Format::R32G32B32A32_FLOAT;
        d.pixels = px.data();
        d.debugName = "ibl_brdf_lut";
        maps.brdfLUT = renderer.UploadTexture(d);
    }

    // --- Pre-integrated skin SSS LUT ---
    {
        constexpr u32 kSkin = 128;
        std::vector<glm::vec4> px = GenerateSkinLUT(kSkin);
        rhi::TextureDesc d;
        d.width = kSkin; d.height = kSkin;
        d.format = rhi::Format::R32G32B32A32_FLOAT;
        d.pixels = px.data();
        d.debugName = "ibl_skin_lut";
        maps.skinLUT = renderer.UploadTexture(d);
    }

    maps.valid = maps.irradiance.IsValid() && maps.prefiltered.IsValid() && maps.brdfLUT.IsValid();

    const auto t1 = std::chrono::high_resolution_clock::now();
    const f64 ms = std::chrono::duration<f64, std::milli>(t1 - t0).count();
    HBE_INFO("IBL: generated atmosphere environment in {:.1f} ms (irr={}, pre={}, lut={})",
             ms, maps.irradiance.index, maps.prefiltered.index, maps.brdfLUT.index);
    return maps;
}

void RebakeProceduralIBLInto(Renderer& renderer, const ProceduralSkyParams& params, IBLMaps& maps) {
    // LEAK-FREE dynamic re-bake: only the sun-dependent AMBIENT maps (irradiance +
    // prefiltered specular) are regenerated and written IN PLACE via UpdateTexture (which
    // reuses the same GPU resource + bindless slot - the RHI has no DestroyTexture, so a
    // fresh UploadTexture would leak a slot every call). The uploaded maps keep their BOOT
    // resolution (so UpdateTexture's size/mip match check passes); only the internal sky
    // SOURCE is marched at a lower res, which is plenty for these low-res convolutions and
    // makes the dynamic bake ~10x cheaper than the full boot bake. The sky background is
    // analytic (already tracks the sun) and the BRDF/skin LUTs are sun-independent, so
    // neither is touched here.
    if (!renderer.SupportsScene() || !maps.irradiance.IsValid() || !maps.prefiltered.IsValid())
        return;

    // Low-res atmosphere source for the convolutions (NOT uploaded).
    constexpr u32 kSkyW = 256, kSkyH = 128;
    std::vector<glm::vec4> skyPx(static_cast<usize>(kSkyW) * kSkyH);
    jobs::ParallelFor(kSkyH, 8, [&](u32 yBegin, u32 yEnd) {
        for (u32 y = yBegin; y < yEnd; ++y)
            for (u32 x = 0; x < kSkyW; ++x)
                skyPx[y * kSkyW + x] =
                    glm::vec4(SkyColor(DirFromUV((x + 0.5f) / kSkyW, (y + 0.5f) / kSkyH), params),
                              1.0f);
    });

    // Irradiance - SAME resolution as the boot bake so UpdateTexture reuses the handle.
    constexpr u32 kIrrW = 64, kIrrH = 32;
    {
        std::vector<glm::vec4> px = GenerateIrradiance(kIrrW, kIrrH, skyPx, kSkyW, kSkyH);
        rhi::TextureDesc d;
        d.width = kIrrW; d.height = kIrrH;
        d.format = rhi::Format::R32G32B32A32_FLOAT;
        d.pixels = px.data();
        d.debugName = "ibl_irradiance";
        renderer.UpdateTexture(maps.irradiance, d);
    }

    // Prefiltered specular - SAME resolution + mip count as the boot bake.
    constexpr u32 kPreW = 128, kPreH = 64, kMips = 5;
    {
        std::vector<glm::vec4> packed;
        for (u32 mip = 0; mip < kMips; ++mip) {
            const u32 mw = glm::max(1u, kPreW >> mip);
            const u32 mh = glm::max(1u, kPreH >> mip);
            const f32 roughness = static_cast<f32>(mip) / (kMips - 1);
            const u32 samples = mip == 0 ? 1u : 96u;
            std::vector<glm::vec4> level =
                GeneratePrefilteredMip(mw, mh, roughness, samples, skyPx, kSkyW, kSkyH);
            packed.insert(packed.end(), level.begin(), level.end());
        }
        rhi::TextureDesc d;
        d.width = kPreW; d.height = kPreH;
        d.format = rhi::Format::R32G32B32A32_FLOAT;
        d.pixels = packed.data();
        d.mipCount = kMips;
        d.debugName = "ibl_prefiltered";
        renderer.UpdateTexture(maps.prefiltered, d);
    }
}

// --- Local environment probe bake ------------------------------------------
namespace {

struct Tri3 {
    glm::vec3 a, b, c;
    glm::vec3 albedo{0.5f}; // surface base colour (so bounce light is tinted)
};
struct BakeLight {
    glm::vec3 pos, color, axis; // axis = spot direction (points away from light)
    f32 intensity, range, innerCos, outerCos;
    bool spot;
};

// Möller-Trumbore; t > eps on hit (one-sided test disabled - rooms are closed).
bool RayTri(const glm::vec3& o, const glm::vec3& d, const Tri3& t, f32& outT) {
    const glm::vec3 e1 = t.b - t.a, e2 = t.c - t.a;
    const glm::vec3 p = glm::cross(d, e2);
    const f32 det = glm::dot(e1, p);
    if (std::abs(det) < 1e-8f) return false;
    const f32 inv = 1.0f / det;
    const glm::vec3 tv = o - t.a;
    const f32 u = glm::dot(tv, p) * inv;
    if (u < -1e-5f || u > 1.0f + 1e-5f) return false;
    const glm::vec3 q = glm::cross(tv, e1);
    const f32 v = glm::dot(d, q) * inv;
    if (v < -1e-5f || u + v > 1.0f + 1e-5f) return false;
    const f32 tt = glm::dot(e2, q) * inv;
    if (tt <= 1e-3f) return false;
    outT = tt;
    return true;
}

// Windowed inverse-square falloff (matches the punctual-light feel closely).
f32 DistAtten(f32 dist, f32 range) {
    const f32 d2 = dist * dist;
    const f32 num = glm::clamp(1.0f - (d2 * d2) / (range * range * range * range), 0.0f, 1.0f);
    return (num * num) / (d2 + 1.0f);
}

// World-space triangle soup of the scene's meshes (rebuilt from each entity's
// MeshRef provenance). Shared by the probe bake + auto-placement.
std::vector<Tri3> GatherSceneTris(const Scene& scene, const std::filesystem::path& assetsDir) {
    std::vector<Tri3> tris;
    const entt::registry& reg = scene.Registry();
    glm::vec3 curAlbedo(0.5f);
    const auto addMesh = [&](const MeshData& md, const glm::mat4& world) {
        for (size_t i = 0; i + 2 < md.indices.size(); i += 3) {
            Tri3 t;
            t.a = glm::vec3(world * glm::vec4(md.vertices[md.indices[i]].position, 1.0f));
            t.b = glm::vec3(world * glm::vec4(md.vertices[md.indices[i + 1]].position, 1.0f));
            t.c = glm::vec3(world * glm::vec4(md.vertices[md.indices[i + 2]].position, 1.0f));
            t.albedo = curAlbedo;
            tris.push_back(t);
        }
    };
    for (const entt::entity e : reg.view<const Transform, const MeshRef>()) {
        const std::string& src = reg.get<const MeshRef>(e).source;
        const glm::mat4 world = scene.WorldMatrix(e);
        // Tint the bounce by the surface's base colour (texture-average ignored).
        curAlbedo = glm::vec3(0.5f);
        if (const MeshInstance* mi = reg.try_get<const MeshInstance>(e))
            curAlbedo = glm::clamp(glm::vec3(mi->surface.base_color), glm::vec3(0.0f), glm::vec3(1.0f));
        if (src.rfind("prim:", 0) == 0) {
            MeshData md = mesh::GeneratePrimitive(src.substr(5));
            if (!md.vertices.empty()) addMesh(md, world);
        } else if (src.rfind("uaf:", 0) == 0) {
            std::string rel = src.substr(4);
            int submesh = 0;
            if (const auto h = rel.find_last_of('#'); h != std::string::npos) {
                submesh = std::atoi(rel.c_str() + h + 1);
                rel = rel.substr(0, h);
            }
            if (std::optional<Model> model = uaf::ReadMesh(assetsDir / rel);
                model && submesh >= 0 && submesh < static_cast<int>(model->size())) {
                addMesh((*model)[static_cast<size_t>(submesh)], world);
            }
        }
    }
    return tris;
}

// Nearest hit distance along a ray (maxDist if none); also counts a hit.
bool RayNearest(const glm::vec3& o, const glm::vec3& d, const std::vector<Tri3>& tris, f32 maxDist,
                f32& outDist) {
    f32 best = maxDist;
    bool any = false;
    for (const Tri3& t : tris) {
        f32 tt;
        if (RayTri(o, d, t, tt) && tt < best) {
            best = tt;
            any = true;
        }
    }
    outDist = best;
    return any;
}

// --- BVH for fast ray-casts (median split; built once per bake) ------------
// Without this, each ray brute-forces every triangle (a 30k-83k-tri level made a
// volume bake take minutes); with it, a ray is ~O(log N).
struct Bvh {
    struct Node {
        glm::vec3 mn, mx;
        int left, count;
    }; // count>0: leaf (left = tri-range start); count==0: internal (children left, left+1)
    std::vector<Node> nodes;
    std::vector<int> idx; // reordered triangle indices
};

void BvhBuild(Bvh& b, const std::vector<Tri3>& tris, int start, int end,
              const std::vector<glm::vec3>& cent) {
    Bvh::Node node;
    node.mn = glm::vec3(1e9f);
    node.mx = glm::vec3(-1e9f);
    for (int i = start; i < end; ++i) {
        const Tri3& t = tris[b.idx[i]];
        node.mn = glm::min(node.mn, glm::min(t.a, glm::min(t.b, t.c)));
        node.mx = glm::max(node.mx, glm::max(t.a, glm::max(t.b, t.c)));
    }
    const int self = static_cast<int>(b.nodes.size());
    b.nodes.push_back(node);
    if (end - start <= 4) {
        b.nodes[self].left = start;
        b.nodes[self].count = end - start;
        return;
    }
    const glm::vec3 ext = node.mx - node.mn;
    const int axis = ext.x > ext.y ? (ext.x > ext.z ? 0 : 2) : (ext.y > ext.z ? 1 : 2);
    const int mid = (start + end) / 2;
    std::nth_element(b.idx.begin() + start, b.idx.begin() + mid, b.idx.begin() + end,
                     [&](int a, int c) { return cent[a][axis] < cent[c][axis]; });
    b.nodes[self].count = 0;
    b.nodes[self].left = static_cast<int>(b.nodes.size());
    BvhBuild(b, tris, start, mid, cent);
    BvhBuild(b, tris, mid, end, cent);
}

Bvh BuildBvh(const std::vector<Tri3>& tris) {
    Bvh b;
    if (tris.empty()) return b;
    b.idx.resize(tris.size());
    std::vector<glm::vec3> cent(tris.size());
    for (int i = 0; i < static_cast<int>(tris.size()); ++i) {
        b.idx[i] = i;
        cent[i] = (tris[i].a + tris[i].b + tris[i].c) * (1.0f / 3.0f);
    }
    b.nodes.reserve(tris.size() * 2);
    BvhBuild(b, tris, 0, static_cast<int>(tris.size()), cent);
    return b;
}

bool RayAabb(const glm::vec3& o, const glm::vec3& invD, const glm::vec3& mn, const glm::vec3& mx,
             f32 tMax) {
    const glm::vec3 t0 = (mn - o) * invD, t1 = (mx - o) * invD;
    const glm::vec3 tmin = glm::min(t0, t1), tmax = glm::max(t0, t1);
    const f32 lo = glm::max(glm::max(tmin.x, tmin.y), tmin.z);
    const f32 hi = glm::min(glm::min(tmax.x, tmax.y), tmax.z);
    return hi >= glm::max(lo, 0.0f) && lo <= tMax;
}

bool RayNearestBvh(const Bvh& b, const std::vector<Tri3>& tris, const glm::vec3& o,
                   const glm::vec3& d, f32 maxDist, f32& outDist, int& outTri) {
    f32 best = maxDist;
    int hit = -1;
    if (!b.nodes.empty()) {
        const glm::vec3 sd(std::abs(d.x) < 1e-8f ? 1e-8f : d.x, std::abs(d.y) < 1e-8f ? 1e-8f : d.y,
                           std::abs(d.z) < 1e-8f ? 1e-8f : d.z);
        const glm::vec3 invD = 1.0f / sd;
        int stack[64];
        int sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const Bvh::Node& n = b.nodes[stack[--sp]];
            if (!RayAabb(o, invD, n.mn, n.mx, best)) continue;
            if (n.count > 0) {
                for (int i = n.left; i < n.left + n.count; ++i) {
                    f32 t;
                    const int ti = b.idx[i];
                    if (RayTri(o, d, tris[ti], t) && t < best) {
                        best = t;
                        hit = ti;
                    }
                }
            } else if (sp + 2 <= 64) {
                stack[sp++] = n.left;
                stack[sp++] = n.left + 1;
            }
        }
    }
    outDist = best;
    outTri = hit;
    return hit >= 0;
}

// Scene point/spot lights + emissive surfaces as bake emitters.
std::vector<BakeLight> GatherBakeLights(const Scene& scene) {
    std::vector<BakeLight> lights;
    const entt::registry& reg = scene.Registry();
    for (const entt::entity e : reg.view<const Transform, const PointLightComponent>()) {
        const auto& pl = reg.get<const PointLightComponent>(e);
        lights.push_back({glm::vec3(scene.WorldMatrix(e)[3]), pl.color, glm::vec3(0, -1, 0),
                          pl.intensity, glm::max(pl.range, 0.01f), 1.0f, -1.0f, false});
    }
    for (const entt::entity e : reg.view<const Transform, const SpotLightComponent>()) {
        const auto& sl = reg.get<const SpotLightComponent>(e);
        const glm::mat4 w = scene.WorldMatrix(e);
        const glm::vec3 axis = glm::normalize(glm::vec3(w * glm::vec4(0, -1, 0, 0)));
        lights.push_back({glm::vec3(w[3]), sl.color, axis, sl.intensity,
                          glm::max(sl.range, 0.01f), std::cos(glm::radians(sl.innerAngle)),
                          std::cos(glm::radians(sl.outerAngle)), true});
    }
    for (const entt::entity e : reg.view<const Transform, const MeshInstance>()) {
        const auto& mi = reg.get<const MeshInstance>(e);
        const glm::vec3 em = mi.surface.emission_color * mi.surface.emission_luminance;
        const f32 lum = glm::dot(em, glm::vec3(0.2126f, 0.7152f, 0.0722f));
        if (lum < 0.02f) continue;
        lights.push_back({glm::vec3(scene.WorldMatrix(e)[3]), em, glm::vec3(0, -1, 0), 2.5f,
                          glm::clamp(lum * 6.0f, 4.0f, 40.0f), 1.0f, -1.0f, false});
    }
    return lights;
}

// Radiance seen from `position` along `dir`: a lit hit surface, or the sky.
glm::vec3 ShadeEnvDir(const Bvh& bvh, const std::vector<Tri3>& tris,
                      const std::vector<BakeLight>& lights, const glm::vec3& position,
                      const glm::vec3& dir, f32 range, f32 skyMix, const ProceduralSkyParams& sky) {
    f32 best;
    int hit;
    if (!RayNearestBvh(bvh, tris, position, dir, range, best, hit)) return SkyColor(dir, sky);
    const glm::vec3 Q = position + dir * best;
    glm::vec3 Ng = glm::normalize(glm::cross(tris[hit].b - tris[hit].a, tris[hit].c - tris[hit].a));
    if (glm::dot(Ng, dir) > 0.0f) Ng = -Ng;
    const glm::vec3 albedo = tris[hit].albedo;
    glm::vec3 rad = albedo * 0.02f;
    for (const BakeLight& L : lights) {
        const glm::vec3 toL = L.pos - Q;
        const f32 dist = glm::length(toL);
        if (dist >= L.range || dist < 1e-3f) continue;
        const glm::vec3 Lp = toL / dist;
        const f32 ndl = glm::max(glm::dot(Ng, Lp), 0.0f);
        if (ndl <= 0.0f) continue;
        f32 atten = DistAtten(dist, L.range);
        if (L.spot) {
            const f32 cd = glm::dot(-Lp, L.axis);
            atten *= glm::clamp((cd - L.outerCos) / glm::max(L.innerCos - L.outerCos, 1e-4f), 0.0f,
                                1.0f);
        }
        rad += albedo * L.color * (L.intensity * atten * ndl);
    }
    return glm::mix(rad, SkyColor(dir, sky), skyMix);
}

// Equirect environment by ray-casting from `position` (job-parallel over rows).
std::vector<glm::vec4> BuildCellEnv(const Bvh& bvh, const std::vector<Tri3>& tris,
                                    const std::vector<BakeLight>& lights, const glm::vec3& position,
                                    f32 range, f32 skyMix, const ProceduralSkyParams& sky, u32 w,
                                    u32 h) {
    std::vector<glm::vec4> env(static_cast<usize>(w) * h);
    jobs::ParallelFor(h, 2, [&](u32 yBegin, u32 yEnd) {
        for (u32 y = yBegin; y < yEnd; ++y)
            for (u32 x = 0; x < w; ++x)
                env[y * w + x] = glm::vec4(
                    ShadeEnvDir(bvh, tris, lights, position,
                                DirFromUV((x + 0.5f) / w, (y + 0.5f) / h), range, skyMix, sky),
                    1.0f);
    });
    return env;
}

// Project an env equirect to SH-L1 irradiance: 4 RGB coefficients with the
// dω integration + cosine-convolution (A0=pi, A1=2pi/3) folded in, so the shader
// evaluates E(N) = c0*0.282095 + 0.488603*(c1*N.y + c2*N.z + c3*N.x).
void ProjectEnvToSH(const std::vector<glm::vec4>& env, u32 w, u32 h, glm::vec3 shOut[4]) {
    glm::vec3 c0(0.0f), c1(0.0f), c2(0.0f), c3(0.0f);
    for (u32 y = 0; y < h; ++y) {
        const f32 sw = std::sin(((y + 0.5f) / h) * kPi); // dθ weight
        for (u32 x = 0; x < w; ++x) {
            const glm::vec3 dir = DirFromUV((x + 0.5f) / w, (y + 0.5f) / h);
            const glm::vec3 L = glm::vec3(env[y * w + x]) * sw;
            c0 += L * 0.282095f;
            c1 += L * (0.488603f * dir.y);
            c2 += L * (0.488603f * dir.z);
            c3 += L * (0.488603f * dir.x);
        }
    }
    const f32 IF = (2.0f * kPi * kPi) / static_cast<f32>(w * h); // dω = sinθ·(π/h)·(2π/w)
    shOut[0] = c0 * (IF * kPi);
    shOut[1] = c1 * (IF * (2.0f * kPi / 3.0f));
    shOut[2] = c2 * (IF * (2.0f * kPi / 3.0f));
    shOut[3] = c3 * (IF * (2.0f * kPi / 3.0f));
}

// Octahedral UV [0,1]^2 -> unit direction (matches GiOctEncode in Common.hlsli).
glm::vec3 OctDecodeDir(f32 u, f32 v) {
    const glm::vec2 f = glm::vec2(u, v) * 2.0f - 1.0f;
    glm::vec3 n(f.x, f.y, 1.0f - std::abs(f.x) - std::abs(f.y));
    const f32 t = glm::max(-n.z, 0.0f);
    n.x += n.x >= 0.0f ? -t : t;
    n.y += n.y >= 0.0f ? -t : t;
    return glm::normalize(n);
}

} // namespace

IBLMaps BakeLocalProbe(Renderer& renderer, const Scene& scene,
                       const std::filesystem::path& assetsDir, const glm::vec3& position,
                       f32 range, f32 skyMix, const ProceduralSkyParams& sky,
                       const std::filesystem::path& savePath) {
    IBLMaps maps;
    if (!renderer.SupportsScene()) return maps;
    const auto t0 = std::chrono::high_resolution_clock::now();
    range = glm::max(range, 1.0f);
    skyMix = glm::clamp(skyMix, 0.0f, 1.0f);

    // --- Gather world-space triangles + the scene's point/spot lights --------
    std::vector<Tri3> tris = GatherSceneTris(scene, assetsDir);
    std::vector<BakeLight> lights = GatherBakeLights(scene);
    const Bvh bvh = BuildBvh(tris);

    // --- Build the local environment equirect by ray-casting -----------------
    constexpr u32 kEnvW = 128, kEnvH = 64;
    std::vector<glm::vec4> env =
        BuildCellEnv(bvh, tris, lights, position, range, skyMix, sky, kEnvW, kEnvH);

    // --- Convolve the local environment (shared with the sky path) -----------
    constexpr u32 kIrrW = 64, kIrrH = 32, kPreW = 128, kPreH = 64, kMips = 5;
    std::vector<glm::vec4> irrPx = GenerateIrradiance(kIrrW, kIrrH, env, kEnvW, kEnvH);
    {
        rhi::TextureDesc d;
        d.width = kIrrW; d.height = kIrrH;
        d.format = rhi::Format::R32G32B32A32_FLOAT;
        d.pixels = irrPx.data();
        d.debugName = "probe_irradiance";
        maps.irradiance = renderer.UploadTexture(d);
    }
    std::vector<glm::vec4> prePacked;
    for (u32 mip = 0; mip < kMips; ++mip) {
        const u32 mw = glm::max(1u, kPreW >> mip);
        const u32 mh = glm::max(1u, kPreH >> mip);
        const f32 roughness = static_cast<f32>(mip) / (kMips - 1);
        const u32 samples = mip == 0 ? 1u : 64u;
        std::vector<glm::vec4> level =
            GeneratePrefilteredMip(mw, mh, roughness, samples, env, kEnvW, kEnvH);
        prePacked.insert(prePacked.end(), level.begin(), level.end());
    }
    {
        rhi::TextureDesc d;
        d.width = kPreW; d.height = kPreH;
        d.format = rhi::Format::R32G32B32A32_FLOAT;
        d.pixels = prePacked.data();
        d.mipCount = kMips;
        d.debugName = "probe_prefiltered";
        maps.prefiltered = renderer.UploadTexture(d);
        maps.prefilteredMaxLod = static_cast<f32>(kMips - 1);
    }
    maps.valid = maps.irradiance.IsValid() && maps.prefiltered.IsValid();

    // Cache to disk (.hbprobe) so the probe reloads without re-baking.
    if (maps.valid && !savePath.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(savePath.parent_path(), ec);
        if (std::ofstream f{savePath, std::ios::binary}) {
            const u32 magic = 0x42525048u, ver = 1, iw = kIrrW, ih = kIrrH, pw = kPreW, ph = kPreH,
                      pm = kMips;
            const f32 maxLod = maps.prefilteredMaxLod;
            const auto wr = [&](const void* p, usize n) {
                f.write(reinterpret_cast<const char*>(p), static_cast<std::streamsize>(n));
            };
            wr(&magic, 4); wr(&ver, 4); wr(&iw, 4); wr(&ih, 4);
            wr(irrPx.data(), irrPx.size() * sizeof(glm::vec4));
            wr(&pw, 4); wr(&ph, 4); wr(&pm, 4); wr(&maxLod, 4);
            wr(prePacked.data(), prePacked.size() * sizeof(glm::vec4));
        }
    }

    const auto t1 = std::chrono::high_resolution_clock::now();
    const f64 ms = std::chrono::duration<f64, std::milli>(t1 - t0).count();
    HBE_INFO("Probe: baked local env in {:.1f} ms ({} tris, {} lights, irr={}, pre={})", ms,
             tris.size(), lights.size(), maps.irradiance.index, maps.prefiltered.index);
    return maps;
}

IBLMaps LoadProbeMaps(Renderer& renderer, const std::filesystem::path& path) {
    IBLMaps maps;
    if (!renderer.SupportsScene()) return maps;
    // Pack-aware: a shipped build serves .hbprobe from the mounted .uap packs.
    // This was a raw ifstream, which - together with .hbprobe being absent from
    // the asset registry entirely - meant a baked probe could NEVER ship: the
    // cooker skipped the extension, and even a packed one would not have been
    // read. The caller (SceneSerializer) tests `maps.valid` with no else branch,
    // so the whole failure was one silent fallback to sky-leaked ambient.
    const std::optional<std::vector<u8>> bytes = vfs::ReadFile(path);
    if (!bytes) {
        HBE_WARN("Probe: '{}' could not be read - this probe contributes NO local "
                 "reflection or bounce (the area falls back to the sky). Re-bake it, "
                 "or clear the probe's source.",
                 path.string());
        return maps;
    }
    usize cursor = 0;
    bool truncated = false;
    const auto rd = [&](void* p, usize n) {
        if (cursor + n > bytes->size()) {
            truncated = true;
            return;
        }
        std::memcpy(p, bytes->data() + cursor, n);
        cursor += n;
    };
    u32 magic = 0, ver = 0, iw = 0, ih = 0, pw = 0, ph = 0, pm = 0;
    f32 maxLod = 0.0f;
    rd(&magic, 4); rd(&ver, 4);
    if (truncated || magic != 0x42525048u) {
        HBE_WARN("Probe: '{}' is not a usable .hbprobe - re-bake it.", path.string());
        return maps;
    }
    rd(&iw, 4); rd(&ih, 4);
    // Size the allocations from the file's own dimensions only AFTER bounding them
    // against the bytes actually present, so a corrupt header cannot ask for a
    // multi-gigabyte vector.
    if (truncated || static_cast<u64>(iw) * ih * sizeof(glm::vec4) > bytes->size()) {
        HBE_WARN("Probe: '{}' has a corrupt irradiance header - re-bake it.", path.string());
        return maps;
    }
    std::vector<glm::vec4> irr(static_cast<usize>(iw) * ih);
    rd(irr.data(), irr.size() * sizeof(glm::vec4));
    rd(&pw, 4); rd(&ph, 4); rd(&pm, 4); rd(&maxLod, 4);
    usize preTotal = 0;
    for (u32 m = 0; m < pm && !truncated; ++m)
        preTotal += static_cast<usize>(glm::max(1u, pw >> m)) * glm::max(1u, ph >> m);
    if (truncated || preTotal * sizeof(glm::vec4) > bytes->size()) {
        HBE_WARN("Probe: '{}' has a corrupt prefiltered header - re-bake it.", path.string());
        return maps;
    }
    std::vector<glm::vec4> pre(preTotal);
    rd(pre.data(), pre.size() * sizeof(glm::vec4));
    if (truncated || iw == 0 || pw == 0) {
        HBE_WARN("Probe: '{}' is truncated - re-bake it.", path.string());
        return maps; // truncated / bad
    }
    rhi::TextureDesc di;
    di.width = iw; di.height = ih; di.format = rhi::Format::R32G32B32A32_FLOAT;
    di.pixels = irr.data(); di.debugName = "probe_irradiance";
    maps.irradiance = renderer.UploadTexture(di);
    rhi::TextureDesc dp;
    dp.width = pw; dp.height = ph; dp.format = rhi::Format::R32G32B32A32_FLOAT;
    dp.pixels = pre.data(); dp.mipCount = pm; dp.debugName = "probe_prefiltered";
    maps.prefiltered = renderer.UploadTexture(dp);
    maps.prefilteredMaxLod = maxLod;
    maps.valid = maps.irradiance.IsValid() && maps.prefiltered.IsValid();
    return maps;
}

std::vector<ProbePlacement> AutoPlaceProbes(const Scene& scene,
                                            const std::filesystem::path& assetsDir, f32 spacing) {
    std::vector<ProbePlacement> out;
    const std::vector<Tri3> tris = GatherSceneTris(scene, assetsDir);
    if (tris.empty()) return out;
    const Bvh bvh = BuildBvh(tris);

    glm::vec3 mn(1e9f), mx(-1e9f);
    for (const Tri3& t : tris) {
        mn = glm::min(mn, glm::min(t.a, glm::min(t.b, t.c)));
        mx = glm::max(mx, glm::max(t.a, glm::max(t.b, t.c)));
    }
    const glm::vec3 size = glm::max(mx - mn, glm::vec3(0.1f));
    // Adaptive spacing so a 3D grid over the WHOLE level (indoor + outdoor) fits
    // the probe budget. The grid IS the baked-lighting volume - every cell in the
    // level gets a probe (skipping only cells buried in solid geometry).
    const f32 vol = size.x * size.y * size.z;
    const f32 fit = std::cbrt(vol / static_cast<f32>(glm::max(rhi::kMaxProbes, 1u)));
    spacing = glm::max(glm::max(spacing, fit), 1.0f);

    const glm::vec3 dirs[6] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                               {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
    const f32 he = spacing * 0.62f; // overlap neighbours for a smooth volume blend
    for (f32 z = mn.z + spacing * 0.5f; z <= mx.z + 1e-3f; z += spacing) {
        for (f32 y = mn.y + spacing * 0.5f; y <= mx.y + 1e-3f; y += spacing) {
            for (f32 x = mn.x + spacing * 0.5f; x <= mx.x + 1e-3f; x += spacing) {
                if (out.size() >= rhi::kMaxProbes) {
                    HBE_INFO("Probe: auto-placed {} (budget cap; raise kMaxProbes for denser GI).",
                             out.size());
                    return out;
                }
                const glm::vec3 c(x, y, z);
                // Skip cells buried inside solid geometry (don't waste the budget).
                int nearHits = 0;
                for (const glm::vec3& d : dirs) {
                    f32 dd;
                    int dt;
                    if (RayNearestBvh(bvh, tris, c, d, 0.35f, dd, dt)) ++nearHits;
                }
                if (nearHits >= 3) continue;
                out.push_back({c, glm::vec3(he)});
            }
        }
    }
    HBE_INFO("Probe: auto-placed {} probes over the level (spacing {:.1f}, {} tris).", out.size(),
             spacing, tris.size());
    return out;
}

GiVolume BakeGIVolume(Renderer& renderer, const Scene& scene,
                      const std::filesystem::path& assetsDir, const ProceduralSkyParams& sky,
                      const std::filesystem::path& savePath) {
    GiVolume vol;
    if (!renderer.SupportsScene()) return vol;
    const auto t0 = std::chrono::high_resolution_clock::now();

    const std::vector<Tri3> tris = GatherSceneTris(scene, assetsDir);
    if (tris.empty()) return vol;
    const std::vector<BakeLight> lights = GatherBakeLights(scene);
    const Bvh bvh = BuildBvh(tris);

    glm::vec3 mn(1e9f), mx(-1e9f);
    for (const Tri3& t : tris) {
        mn = glm::min(mn, glm::min(t.a, glm::min(t.b, t.c)));
        mx = glm::max(mx, glm::max(t.a, glm::max(t.b, t.c)));
    }
    const glm::vec3 size = glm::max(mx - mn, glm::vec3(0.5f));
    // Adaptive spacing so the grid fits a cell budget (each cell ray-casts a small
    // env, so this is the bake-time/quality knob).
    constexpr u32 kMaxCells = 2048;
    const f32 spacing = glm::max(std::cbrt((size.x * size.y * size.z) / kMaxCells), 1.0f);
    glm::ivec3 dims = glm::clamp(glm::ivec3(glm::ceil(size / spacing)) + glm::ivec3(1),
                                 glm::ivec3(2), glm::ivec3(40));
    const glm::vec3 origin = mn; // cell (0,0,0) centre
    const u32 cells = static_cast<u32>(dims.x) * dims.y * dims.z;
    const f32 range = glm::length(size) + 5.0f;

    // SH atlas: 4 coeffs (width) x cells (height); row = x + y*dimX + z*dimX*dimY.
    // Depth atlas (DDGI visibility): an 8x8 octahedral distance map per cell,
    // flattened to width 64 (R = distance to geometry, G = distance^2).
    std::vector<glm::vec4> atlas(static_cast<usize>(4) * cells);
    constexpr u32 kEW = 32, kEH = 16, kOcta = 8;
    std::vector<glm::vec4> depthAtlas(static_cast<usize>(kOcta * kOcta) * cells);
    jobs::ParallelFor(cells, 8, [&](u32 cBegin, u32 cEnd) {
        for (u32 ci = cBegin; ci < cEnd; ++ci) {
            const int x = static_cast<int>(ci) % dims.x;
            const int y = (static_cast<int>(ci) / dims.x) % dims.y;
            const int z = static_cast<int>(ci) / (dims.x * dims.y);
            const glm::vec3 pos = origin + glm::vec3(x, y, z) * spacing;
            std::vector<glm::vec4> env(static_cast<usize>(kEW) * kEH); // single-thread (cell-parallel)
            for (u32 yy = 0; yy < kEH; ++yy)
                for (u32 xx = 0; xx < kEW; ++xx)
                    env[yy * kEW + xx] = glm::vec4(
                        ShadeEnvDir(bvh, tris, lights, pos,
                                    DirFromUV((xx + 0.5f) / kEW, (yy + 0.5f) / kEH), range, 0.0f,
                                    sky),
                        1.0f);
            glm::vec3 sh[4];
            ProjectEnvToSH(env, kEW, kEH, sh);
            for (int k = 0; k < 4; ++k)
                atlas[static_cast<usize>(ci) * 4 + k] = glm::vec4(sh[k], 1.0f);
            // Octahedral depth: distance to the nearest geometry in each direction.
            for (u32 oy = 0; oy < kOcta; ++oy)
                for (u32 ox = 0; ox < kOcta; ++ox) {
                    const glm::vec3 od = OctDecodeDir((ox + 0.5f) / kOcta, (oy + 0.5f) / kOcta);
                    f32 dd;
                    int dt;
                    const f32 dist = RayNearestBvh(bvh, tris, pos, od, range, dd, dt) ? dd : range;
                    depthAtlas[static_cast<usize>(ci) * (kOcta * kOcta) + oy * kOcta + ox] =
                        glm::vec4(dist, dist * dist, 0.0f, 0.0f);
                }
        }
    });

    rhi::TextureDesc d;
    d.width = 4;
    d.height = cells;
    d.format = rhi::Format::R32G32B32A32_FLOAT;
    d.pixels = atlas.data();
    d.debugName = "gi_volume_sh";
    vol.sh = renderer.UploadTexture(d);
    {
        rhi::TextureDesc dd;
        dd.width = kOcta * kOcta;
        dd.height = cells;
        dd.format = rhi::Format::R32G32B32A32_FLOAT;
        dd.pixels = depthAtlas.data();
        dd.debugName = "gi_volume_depth";
        vol.depth = renderer.UploadTexture(dd);
    }
    vol.origin = origin;
    vol.spacing = glm::vec3(spacing);
    vol.dims = dims;
    vol.valid = vol.sh.IsValid();
    vol.status = vol.valid ? GiStatus::Loaded : GiStatus::None;

    // Cache to disk (.hbgi) so the volume reloads without re-baking.
    if (vol.valid && !savePath.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(savePath.parent_path(), ec);
        if (std::ofstream f{savePath, std::ios::binary}) {
            const u32 magic = 0x56494748u, ver = 1; // 'HGIV'
            const auto wr = [&](const void* p, usize n) {
                f.write(reinterpret_cast<const char*>(p), static_cast<std::streamsize>(n));
            };
            wr(&magic, 4); wr(&ver, 4);
            wr(&dims.x, 4); wr(&dims.y, 4); wr(&dims.z, 4);
            wr(&origin.x, 12); wr(&spacing, 4);
            wr(atlas.data(), atlas.size() * sizeof(glm::vec4));
            wr(depthAtlas.data(), depthAtlas.size() * sizeof(glm::vec4));
        }
    }

    const auto t1 = std::chrono::high_resolution_clock::now();
    HBE_INFO("GI: baked irradiance volume {}x{}x{} ({} cells) in {:.1f} ms ({} tris).", dims.x,
             dims.y, dims.z, cells, std::chrono::duration<f64, std::milli>(t1 - t0).count(),
             tris.size());
    return vol;
}

const char* ToString(GiStatus s) {
    switch (s) {
        case GiStatus::None:    return "none";
        case GiStatus::Loaded:  return "loaded";
        case GiStatus::Missing: return "missing";
        case GiStatus::Corrupt: return "corrupt";
        case GiStatus::UploadFailed: return "upload-failed";
    }
    return "?";
}

GiVolume LoadGIVolume(Renderer& renderer, const std::filesystem::path& path) {
    GiVolume vol;
    // NOTE: the file is PARSED even without a device. It used to early-out on
    // !SupportsScene, which made "no GPU" indistinguishable from "no file" - so a
    // headless test could never prove that a missing `.hbgi` is reported rather
    // than silently inherited. Only the UPLOAD is skipped below.
    //
    // Pack-aware: a shipped build serves .hbgi from the mounted packs, not disk.
    // This was a raw ifstream, so baked GI silently vanished in shipped builds
    // (the scene fell back to sky-only ambient with no warning).
    const std::optional<std::vector<u8>> bytes = vfs::ReadFile(path);
    if (!bytes) {
        vol.status = GiStatus::Missing;
        HBE_WARN("GI: '{}' could not be read - the scene will render with NO baked "
                 "volume (a sealed interior will leak sky light). Re-bake it, or "
                 "clear the scene's giSource.",
                 path.string());
        return vol;
    }
    usize cursor = 0;
    bool truncated = false;
    const auto rd = [&](void* p, usize n) {
        if (cursor + n > bytes->size()) { truncated = true; return; }
        std::memcpy(p, bytes->data() + cursor, n);
        cursor += n;
    };
    const auto corrupt = [&](const char* why) {
        vol = GiVolume{};
        vol.status = GiStatus::Corrupt;
        HBE_WARN("GI: '{}' is not a usable .hbgi ({}) - rendering with NO baked "
                 "volume. Re-bake it.", path.string(), why);
        return vol;
    };
    u32 magic = 0, ver = 0;
    rd(&magic, 4); rd(&ver, 4);
    if (truncated || magic != 0x56494748u) return corrupt("bad magic / truncated header");
    glm::ivec3 dims(0);
    glm::vec3 origin(0.0f);
    f32 spacing = 1.0f;
    rd(&dims.x, 4); rd(&dims.y, 4); rd(&dims.z, 4);
    rd(&origin.x, 12); rd(&spacing, 4);
    if (truncated) return corrupt("truncated grid header");
    if (dims.x <= 0 || dims.y <= 0 || dims.z <= 0) return corrupt("non-positive dimensions");
    const usize cells = static_cast<usize>(dims.x) * dims.y * dims.z;
    if (cells == 0 || cells > 1'000'000) return corrupt("implausible cell count"); // sanity
    std::vector<glm::vec4> atlas(4 * cells), depthAtlas(64 * cells);
    rd(atlas.data(), atlas.size() * sizeof(glm::vec4));
    rd(depthAtlas.data(), depthAtlas.size() * sizeof(glm::vec4));
    if (truncated) return corrupt("truncated atlas payload");
    // The file is good. Everything below this line is GPU-only.
    vol.status = GiStatus::Loaded;
    vol.origin = origin;
    vol.spacing = glm::vec3(spacing);
    vol.dims = dims;
    if (!renderer.SupportsScene()) return vol; // headless: parsed, not uploaded
    {
        rhi::TextureDesc d;
        d.width = 4; d.height = static_cast<u32>(cells);
        d.format = rhi::Format::R32G32B32A32_FLOAT;
        d.pixels = atlas.data(); d.debugName = "gi_volume_sh";
        vol.sh = renderer.UploadTexture(d);
    }
    {
        rhi::TextureDesc d;
        d.width = 64; d.height = static_cast<u32>(cells);
        d.format = rhi::Format::R32G32B32A32_FLOAT;
        d.pixels = depthAtlas.data(); d.debugName = "gi_volume_depth";
        vol.depth = renderer.UploadTexture(d);
    }
    vol.valid = vol.sh.IsValid() && vol.depth.IsValid();
    // The file was good and the GPU was not. Say so, instead of reporting Loaded
    // with handles nothing can sample: the shader gates on the SH index, so an
    // invalid handle falls silently back to sky irradiance and a sealed interior
    // lights as though it had no roof - indistinguishable from never having baked.
    if (!vol.valid) {
        vol.status = GiStatus::UploadFailed;
        HBE_WARN("GI: '{}' parsed but could not be uploaded (no descriptor/texture) - "
                 "the scene will render with NO baked volume (a sealed interior will "
                 "leak sky light). This is a GPU-resource failure, not a bad file.",
                 path.string());
    }
    return vol;
}

} // namespace hbe
