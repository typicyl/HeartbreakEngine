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
// WHAT HAPPENED to a scene's `giSource`. This exists because the failure was
// SILENT: LoadGIVolume had five bare `return vol;` paths with no logging, and its
// caller tested `if (vol.valid)` with no `else` - so a missing or corrupt `.hbgi`
// left the PREVIOUS scene's volume bound (or none at all) while `giSource` was
// still assigned, and the next save re-wrote a path to a file that never loaded.
// A sealed interior then falls through to MeshPBR.hlsl's sky-irradiance branch,
// which lights it as though the roof were not there - "unbaked" and "broken" look
// identical on screen. One field, so the log, the editor banner and the parity
// test all read the same answer.
//
// `Loaded` means the FILE parsed. On a device-less (headless) renderer the atlases
// are not uploaded, so the handles stay invalid while the status is still Loaded -
// that is deliberate: it is the only way a headless self-test can tell a good
// `.hbgi` from a missing one.
enum class GiStatus : u32 {
    None = 0,  // giSource is empty - no baked volume was ever asked for
    Loaded,    // the .hbgi parsed (handles valid iff the renderer has a device)
    Missing,   // giSource names a file the VFS could not read
    Corrupt,   // read, but bad magic / truncated / nonsense dimensions
    // Parsed fine, but the GPU refused the atlas (descriptor heap exhausted, a
    // device-lost recovery). Distinct from Corrupt because the FILE is good and
    // re-baking would not help. It exists because `status` used to be set to Loaded
    // BEFORE the uploads and never downgraded, so a failed upload rendered exactly
    // like an unbaked level - the sealed room leaks sky light - while the status,
    // the banner and the log all said "loaded".
    UploadFailed,
};

const char* ToString(GiStatus s);

struct GiVolume {
    glm::vec3  origin{0.0f};   // world centre of cell (0,0,0)
    glm::vec3  spacing{1.0f};  // cell size (world units)
    glm::ivec3 dims{0};        // grid resolution
    rhi::TextureHandle sh;     // bindless SH atlas (4 coeffs x cells)
    rhi::TextureHandle depth;  // bindless octahedral depth atlas (DDGI visibility)
    bool valid = false;        // uploaded and bindable (implies status == Loaded)
    GiStatus status = GiStatus::None; // why `valid` is what it is (see above)
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
