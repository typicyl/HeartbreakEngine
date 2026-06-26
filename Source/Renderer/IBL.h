// Renderer/IBL.h - precomputed image-based lighting maps (bindless).
#pragma once

#include "Core/Types.h"
#include "RHI/RHI.h"

#include <glm/glm.hpp>

#include <filesystem>
#include <vector>

namespace hbe {

class Renderer;
class Scene;

// Bindless handles for a precomputed IBL environment.
struct IBLMaps {
    rhi::TextureHandle irradiance;   // diffuse irradiance (equirect)
    rhi::TextureHandle prefiltered;  // GGX-prefiltered specular (equirect, mipped)
    rhi::TextureHandle brdfLUT;      // split-sum BRDF integration LUT
    rhi::TextureHandle skinLUT;      // pre-integrated subsurface-scattering LUT (skin)
    rhi::TextureHandle sky;          // display-resolution environment (background)
    f32  prefilteredMaxLod = 0.0f;
    bool valid = false;
};

// Tunable parameters of the physically-based atmosphere (Rayleigh + Mie single
// scattering) + sun disc. The whole IBL set is derived from the ray-marched sky,
// so editing these and rebuilding gives a matching skybox and ambient lighting.
// `sunDir`/`sunIntensity`/`skyIntensity`/`sunTint`/`ground` drive the model;
// `horizon`/`zenith` are retained for API/serialization compatibility (the
// gradient is now produced by the atmosphere, so they are no longer sampled).
struct ProceduralSkyParams {
    glm::vec3 horizon{0.75f, 0.80f, 0.90f};
    glm::vec3 zenith{0.18f, 0.36f, 0.72f};
    glm::vec3 ground{0.22f, 0.20f, 0.18f};  // lower-hemisphere bounce albedo
    glm::vec3 sunDir{0.5f, 0.8f, 0.35f};    // points toward the sun
    glm::vec3 sunTint{1.0f, 0.92f, 0.78f};  // sun-disc colour
    f32 sunIntensity = 40.0f;
    f32 skyIntensity = 1.0f;
};

// Builds a procedural-sky IBL set on the CPU and uploads it via `renderer`.
// Equirectangular maps match Shaders/Common.hlsli's EquirectUV mapping.
IBLMaps GenerateProceduralIBL(Renderer& renderer, const ProceduralSkyParams& params = {});

// Bakes a LOCAL environment probe at `position`: CPU ray-casts the scene's mesh
// geometry, lights the hit surfaces with the scene's point/spot lights (so a
// sealed interior goes dark, lit only by its own lamps), and falls back to the
// procedural sky where a ray escapes - then convolves that local environment into
// irradiance + prefiltered maps (the global BRDF LUT is reused). `skyMix` blends
// the open sky back in (0 = fully enclosed). Runs on the CPU job system; call on
// demand (a bake button), never per frame. Reuses GenerateProceduralIBL's
// convolution so probe lighting matches the sky path.
IBLMaps BakeLocalProbe(Renderer& renderer, const Scene& scene,
                       const std::filesystem::path& assetsDir, const glm::vec3& position,
                       f32 range, f32 skyMix, const ProceduralSkyParams& sky = {},
                       const std::filesystem::path& savePath = {});

// Loads a probe's cached irradiance + prefiltered maps from a .hbprobe written by
// BakeLocalProbe (uploads them; no re-bake). Invalid maps if the file is missing.
IBLMaps LoadProbeMaps(Renderer& renderer, const std::filesystem::path& path);

// A suggested probe location.
struct ProbePlacement {
    glm::vec3 position{0.0f};
    glm::vec3 halfExtents{1.0f};
};

// Auto-suggests probe positions: grids the scene's AABB at `spacing` and keeps
// cells that sit above a floor and are enclosed by geometry (interior rooms),
// skipping open exterior + solid/embedded cells. Capped at rhi::kMaxProbes. The
// editor turns each into a ReflectionProbe and bakes it.
std::vector<ProbePlacement> AutoPlaceProbes(const Scene& scene,
                                            const std::filesystem::path& assetsDir, f32 spacing);

// A baked irradiance volume: a regular 3D grid of SH-L1 probes covering the whole
// level, stored as a 2D RGBA32F atlas (width = 4 SH coefficients, height = cell
// count; row index = x + y*dimX + z*dimX*dimY). Sampled trilinearly and evaluated
// by the surface normal for smooth, directional diffuse GI - the upgrade over the
// box-probe grid (no seams, leak-weighted).
struct GiVolume {
    glm::vec3  origin{0.0f};   // world centre of cell (0,0,0)
    glm::vec3  spacing{1.0f};  // cell size (world units)
    glm::ivec3 dims{0};        // grid resolution
    rhi::TextureHandle sh;     // bindless SH atlas (4 coeffs x cells)
    rhi::TextureHandle depth;  // bindless octahedral depth atlas (DDGI visibility)
    bool valid = false;
};

// Bakes the level GI volume: grids the scene AABB, ray-casts a low-res environment
// per cell (geometry + lights + emissive, same model as the probe bake), projects
// each to SH-L1, and packs the grid into one atlas texture. CPU job system; on
// demand (a bake button), never per frame.
GiVolume BakeGIVolume(Renderer& renderer, const Scene& scene,
                      const std::filesystem::path& assetsDir, const ProceduralSkyParams& sky = {},
                      const std::filesystem::path& savePath = {});

// Loads a cached GI volume (.hbgi written by BakeGIVolume) and uploads its SH +
// depth atlases - no re-bake. Invalid if the file is missing/corrupt.
GiVolume LoadGIVolume(Renderer& renderer, const std::filesystem::path& path);

} // namespace hbe
