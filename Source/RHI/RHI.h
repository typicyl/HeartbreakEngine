// RHI/RHI.h - Render Hardware Interface.
//
// The RHI is the seam between the renderer and a concrete graphics API. The
// renderer is written *only* against these abstract types; D3D12 and Vulkan
// each provide a private implementation behind RHIFactory.
//
// Foundation scope (implemented today):
//   * Device + adapter selection
//   * Swapchain creation / resize / present
//   * A fenced, multi-buffered frame loop
//   * Clearing the back buffer
//
// Roadmap (the seams below are intentionally shaped to grow into these without
// touching the renderer): GPU buffers & textures, descriptor/bindless tables,
// graphics & compute pipelines, command-context recording, and a render graph.
#pragma once

#include "Core/Types.h"
#include "RHI/SurfaceMaterial.h" // hbe::SurfaceParams (per-material OpenPBR values)

#include <glm/glm.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hbe { struct MeshData; } // Assets/Mesh.h

namespace hbe::rhi {

// Which concrete backend implements the RHI.
enum class GraphicsAPI {
    D3D12,
    Vulkan,
    OpenGL, // legacy/portable target (GL 4.6 core; bindless via ARB_bindless_texture)
};

const char* ToString(GraphicsAPI api);

// Optional source of compiled shader bytecode. When set, the backends fetch
// shader binaries (by bare filename, e.g. "MeshPBR.vs.dxil" / ".spv") through
// it INSTEAD of reading the loose `shaders/` folder - so a shipped build serves
// its shaders straight from the asset packs. Return false to fall back to disk.
// Installed by the engine for packed runtime builds; unset in the editor (disk).
using ShaderProvider = std::function<bool(const std::string& leaf, std::vector<u8>& out)>;
void SetShaderProvider(ShaderProvider provider);
// Backends call this from their shader-file read; true (and fills `out`) when a
// provider is installed and has the named shader.
bool LoadShaderBytecode(const std::string& leaf, std::vector<u8>& out);

// Texture/attachment formats. Kept deliberately small; extend as needed. The
// swapchain defaults to an 8-bit UNORM format; HDR output uses RGBA16F.
enum class Format {
    Unknown,
    R8G8B8A8_UNORM,
    R8G8B8A8_SRGB,
    B8G8R8A8_UNORM,
    B8G8R8A8_SRGB,
    R16G16B16A16_FLOAT,
    R32G32B32A32_FLOAT,
    D32_FLOAT,
    D24_UNORM_S8_UINT,
    // Block-compressed (BC) formats: 4x4-texel blocks. APPENDED so the existing values (0..8)
    // never shift - uaf::Texture stores Format as a u32 on disk, so reordering would silently
    // reinterpret every shipped texture. sRGB is a DISTINCT format (Vulkan _SRGB_BLOCK / D3D12
    // _UNORM_SRGB), exactly like the R8G8B8A8 UNORM/SRGB split. BC1/BC4 = 8 bytes/block; the
    // rest = 16. See IsBlockCompressed / BlockBytes / MipCopyRows below.
    BC1_UNORM,   // RGB(+1-bit A), 4bpp - opt-in low-color only
    BC1_SRGB,
    BC3_UNORM,   // RGBA (DXT5), 8bpp - the color default (good quality, PS3/RSX-native)
    BC3_SRGB,
    BC4_UNORM,   // single channel (R), 4bpp
    BC5_UNORM,   // two channel (RG) - tangent-space normals, near-lossless
    BC6H_UFLOAT, // HDR RGB, 8bpp (no CPU encoder yet; enum + sampling only)
    BC7_UNORM,   // RGBA, 8bpp - highest-quality color (no CPU encoder yet; enum + sampling only)
    BC7_SRGB,
};

// True for the 4x4-block BC formats. A block-compressed texture must use block-based staging
// math (ceil(w/4) x ceil(h/4) blocks) and must NEVER route through a bytes-per-pixel path.
inline bool IsBlockCompressed(Format f) {
    switch (f) {
        case Format::BC1_UNORM: case Format::BC1_SRGB:
        case Format::BC3_UNORM: case Format::BC3_SRGB:
        case Format::BC4_UNORM: case Format::BC5_UNORM:
        case Format::BC6H_UFLOAT:
        case Format::BC7_UNORM: case Format::BC7_SRGB: return true;
        default: return false;
    }
}

// Bytes per 4x4 block for a BC format (0 for non-BC). BC1/BC4 = 8; BC3/BC5/BC6H/BC7 = 16.
inline u32 BlockBytes(Format f) {
    switch (f) {
        case Format::BC1_UNORM: case Format::BC1_SRGB:
        case Format::BC4_UNORM: return 8u;
        case Format::BC3_UNORM: case Format::BC3_SRGB:
        case Format::BC5_UNORM: case Format::BC6H_UFLOAT:
        case Format::BC7_UNORM: case Format::BC7_SRGB: return 16u;
        default: return 0u;
    }
}

// The tight source-walk geometry of ONE mip level of `w x h` in `format`, correct for BOTH
// block-compressed and uncompressed layouts: `rowBytes` bytes per row, `rowCount` rows, so the
// mip is `rowBytes * rowCount` tightly-packed bytes. Written ONCE here (not duplicated per
// backend) so D3D12's manual source walk and Vulkan's staging/offset math cannot diverge on the
// sub-4x4 tail mips. `bppUncompressed` is the backend's bytes-per-pixel for non-BC formats.
inline void MipCopyRows(Format format, u32 w, u32 h, u32 bppUncompressed,
                        u64& rowBytes, u32& rowCount) {
    if (IsBlockCompressed(format)) {
        rowBytes = static_cast<u64>((w + 3u) / 4u) * BlockBytes(format); // one block-row
        rowCount = (h + 3u) / 4u;                                        // block rows
    } else {
        rowBytes = static_cast<u64>(w) * bppUncompressed;
        rowCount = h;
    }
}

// Tight byte size of one `w x h` mip of `format` (block-aware). `bppUncompressed` is the
// backend's bytes-per-pixel for the non-BC case (ignored for BC).
inline u64 MipByteSize(Format format, u32 w, u32 h, u32 bppUncompressed) {
    u64 rowBytes = 0;
    u32 rowCount = 0;
    MipCopyRows(format, w, h, bppUncompressed, rowBytes, rowCount);
    return rowBytes * rowCount;
}

struct RenderDeviceDesc {
    GraphicsAPI api = GraphicsAPI::D3D12;

    // Native window the swapchain will present to (Win32 HWND + HINSTANCE).
    void* windowHandle = nullptr;
    void* windowInstance = nullptr;

    u32 width  = 1280;
    u32 height = 720;

    // Number of swapchain images / frames in flight.
    u32 backBufferCount = 3;

    // Swapchain color format. SRGB variants apply gamma on write.
    Format backBufferFormat = Format::R8G8B8A8_UNORM;

    // Enable the API's validation/debug layer (slow; debug builds only).
    bool enableValidation = false;

    // Present with vsync (default). false = uncapped: D3D12 Present(0) with
    // tearing when supported; Vulkan MAILBOX (else IMMEDIATE, else FIFO).
    bool vsync = true;
};

// ---------------------------------------------------------------------------
// Scene rendering (additive capability; backends opt in)
// ---------------------------------------------------------------------------

// Opaque handle to a GPU-resident mesh created via IRenderDevice::CreateMesh.
struct MeshHandle {
    u32 id = 0;
    bool IsValid() const { return id != 0; }
};

// Index into the bindless texture array. 0 is a default 1x1 white texture, so a
// zero-initialized material samples white (i.e. uses its color factor as-is).
struct TextureHandle {
    u32 index = 0;
    bool IsValid() const { return index != 0; }
};

// CPU description of a 2D texture to upload. `pixels` is tightly packed and must
// match `format` (e.g. 4 bytes/texel for the R8G8B8A8 formats). SRGB formats are
// interpreted as sRGB-encoded source (decoded to linear on sample).
struct TextureDesc {
    u32 width = 1;
    u32 height = 1;
    // depth > 1 makes this a 3D volume texture (TEXTURE3D / VK_IMAGE_TYPE_3D),
    // used by the volumetric-VFX system. depth == 1 is a normal 2D texture and
    // every existing call site is unaffected.
    u32 depth = 1;
    Format format = Format::R8G8B8A8_UNORM;
    // Pixel data. When mipCount > 1, all mip levels are tightly packed back to
    // back (mip 0 first); mip i has size max(1,width>>i) x max(1,height>>i).
    const void* pixels = nullptr;
    u32 mipCount = 1;
    // When true, the texture also gets an unordered-access view so a compute
    // pass can write it (e.g. the volumetric density-splat writing a 3D volume).
    bool storage = false;
    const char* debugName = nullptr;
};

// A single analytic directional light.
struct DirectionalLight {
    glm::vec3 direction{-0.4f, -1.0f, -0.6f}; // world-space, points from light
    f32       intensity = 3.0f;
    glm::vec3 color{1.0f, 1.0f, 1.0f};
};

// One punctual (point or spot) light. The layout is GPU-ready: it is memcpy'd
// straight into the frame constant buffer, so every field group packs to a
// float4 (matches PunctualLight in Shaders/Common.hlsli).
struct PunctualLight {
    glm::vec3 position{0.0f};
    f32       range = 10.0f;     // light contributes within this radius
    glm::vec3 color{1.0f};
    f32       intensity = 10.0f; // radiant intensity at 1m (inverse-square)
    glm::vec3 direction{0.0f, -1.0f, 0.0f}; // spot axis (points away from light)
    u32       isSpot = 0;
    f32       innerCos = 0.9063f; // cos(25 deg): full intensity inside
    f32       outerCos = 0.8192f; // cos(35 deg): zero beyond
    f32       _pad0 = 0.0f;
    f32       _pad1 = 0.0f;
};

// Upper bound on punctual lights per frame (sized into the constant buffer).
inline constexpr u32 kMaxPunctualLights = 16;

// One baked local light/reflection probe. GPU-ready: memcpy'd into the frame
// constants, so each field group packs to a float4 (matches Probe in Common.hlsli).
struct ProbeData {
    glm::vec3 center{0.0f};
    f32       _p0 = 0.0f;
    glm::vec3 halfExtents{1.0f};
    f32       blend = 1.0f; // edge-softening band in world units (smooth transitions)
    u32       irradianceIndex = 0;
    u32       prefilteredIndex = 0;
    f32       prefilteredMaxLod = 0.0f;
    f32       _p1 = 0.0f;
};

// Upper bound on local probes per frame (sized into the constant buffer). Sized
// for a coarse full-level irradiance grid (auto-placed), not just a few rooms.
inline constexpr u32 kMaxProbes = 64;

// One forward projected decal. `invWorld` maps a world position into the decal's unit
// cube ([-0.5,0.5]); inside it, the box projects albedo/normal/MR onto the surface,
// blended into the material in MeshPBR BEFORE lighting (so decals are correctly lit and
// conform to any geometry - no G-buffer albedo needed in this forward renderer).
// forwardWS = projection axis (box local +Z); tangentWS = box local +X (decal normal-map
// U axis). Mirrors PunctualLight/ProbeData as a fixed constant-buffer array.
struct DecalData {
    glm::mat4 invWorld{1.0f};
    glm::vec3 forwardWS{0.0f, 0.0f, 1.0f}; f32 opacity = 1.0f;
    glm::vec3 tangentWS{1.0f, 0.0f, 0.0f}; f32 angleFade = 2.0f;
    u32 albedoIndex = 0, normalIndex = 0, mrIndex = 0, flags = 0;
    glm::vec4 params{1.0f, 0.8f, 0.0f, 0.0f}; // x=normalStrength, y=roughness, z=metallic, w=coneCos
    // APPEND-ONLY (keeps the 128->144B struct 16-byte aligned; the HLSL Decal mirror in Common.hlsli
    // grows identically). xyz = emissive colour, w = intensity; the shader adds it to the surface
    // emission (shaped by the decal coverage) when the affect-emissive flag bit is set.
    glm::vec4 emissive{0.0f, 0.0f, 0.0f, 0.0f};
};
inline constexpr u32 kMaxDecals = 16;

// Interactive water ripple ring sources (object splashes + rain impacts) uploaded/frame.
inline constexpr u32 kMaxRipples = 16;

// Number of cascaded-shadow-map slices (rendered into a 2x2 atlas).
inline constexpr u32 kMaxShadowCascades = 4;

// One world-anchored painterly "censor": a soft sphere that re-applies the
// painted (brush-stroke) look over whatever dynamic object falls inside it - a
// moving person, etc. CPU-side only: the backend projects center+radius to
// screen each frame and feeds the painterly composite pass (never uploaded raw).
struct CensorData {
    glm::vec3 center{0.0f};    // world-space anchor (entity world pos + offset)
    f32       radius = 2.0f;   // world-space radius of the censor sphere
    f32       feather = 0.5f;  // soft-edge fraction of the radius (0 = hard cut)
    f32       strength = 1.0f; // 0 = no effect, 1 = fully painted inside
};

// Upper bound on simultaneous censors (sized into the composite constant buffer).
inline constexpr u32 kMaxCensors = 4;

// Post-process stack configuration (HDR pipeline). All effects run inside the
// backend after the scene pass; a backend without post support ignores this.
struct PostSettings {
    u32 bloomEnabled = 1;
    f32 bloomIntensity = 0.06f;  // mix factor of the blur pyramid into the HDR
    f32 bloomThreshold = 1.0f;   // radiance above this blooms (soft knee)
    u32 ssaoEnabled = 1;
    f32 ssaoRadius = 0.6f;       // world-space hemisphere radius
    f32 ssaoIntensity = 1.0f;
    u32 fxaaEnabled = 1;
    u32 taaEnabled = 1;          // temporal AA (jittered + reprojected history)
    f32 vignette = 0.18f;        // corner darkening (0 = off)
    f32 saturation = 1.05f;
    f32 contrast = 1.02f;
    // Tone-mapping operator (the modular output stage). 0 = ACES (default, filmic),
    // 1 = AgX (neutral, desaturating), 2 = Tony McMapface (neutral, hue-preserving).
    // All are SDR display transforms; HDR-monitor output is a separate future phase.
    u32 tonemapOperator = 0;

    // Cinematic COLOR GRADE (applied inside the tonemap pass). White balance is a
    // von-Kries scale in LINEAR before the tone curve; lift/gamma/gain (the ASC-CDL
    // shadows/mids/highlights wheels) + optional film grain + chromatic aberration are
    // applied after. Neutral defaults = identity, so this is invisible until dialed in.
    // This is the stage that gives the moody warm-fire / cool-night cinematic look.
    u32 gradeEnabled = 1;
    f32 gradeTemperature = 0.0f;   // white balance: -1 cool (blue) .. +1 warm (amber)
    f32 gradeTint = 0.0f;          // white balance: -1 green .. +1 magenta
    glm::vec3 gradeLift{0.0f};     // SHADOWS colour offset (added, per channel)
    glm::vec3 gradeGamma{1.0f};    // MIDTONES colour power (1 = neutral)
    glm::vec3 gradeGain{1.0f};     // HIGHLIGHTS colour multiply (1 = neutral)
    f32 filmGrain = 0.0f;          // animated luminance grain (0 = off; ~0.03 filmic)
    f32 chromaticAberration = 0.0f;// RGB split toward the frame edges (0 = off)

    // Depth of field. Distances are world-space; everything within focusRange of
    // focusDistance is sharp, ramping to full blur (maxBlur source texels) by 2x
    // focusRange away. On by default with a gentle, wide-focus look.
    u32 dofEnabled = 1;
    f32 dofFocusDistance = 12.0f;
    f32 dofFocusRange = 10.0f;
    f32 dofMaxBlur = 12.0f;

    // Camera motion blur (per-pixel velocity reprojected from depth). Blurs for
    // camera movement; object-motion blur needs a per-object velocity buffer.
    u32 motionBlurEnabled = 1;
    f32 motionBlurIntensity = 1.0f;
    f32 motionBlurMaxRadius = 24.0f; // max blur length, source texels

    // Screen-space reflections (Fresnel-weighted; normals reconstructed from
    // depth - uniform reflectivity without a G-buffer's per-pixel roughness).
    u32 ssrEnabled = 1;
    f32 ssrIntensity = 0.5f;
    f32 ssrMaxDistance = 30.0f; // world-space ray length

    // Auto-exposure / eye adaptation. When on, average scene luminance drives
    // exposure toward `autoExposureKey`; `exposure` stays a manual multiplier
    // applied on top.
    u32 autoExposureEnabled = 1;
    f32 autoExposureKey = 0.18f;  // target middle-grey
    f32 autoExposureSpeed = 2.0f; // adaptation rate per second
    f32 autoExposureMin = 0.05f;  // clamp on the derived exposure
    f32 autoExposureMax = 8.0f;

    // Volumetric fog + light scattering. A ray-march from the camera to the
    // scene depth accumulates sun in-scatter (through the CSM, so windows and
    // gaps throw god-rays) plus punctual-light in-scatter over an exponential
    // height fog, composited into the HDR colour before bloom.
    u32 fogEnabled = 1;
    f32 fogDensity = 0.012f;       // base extinction (per metre) at fogHeight
    f32 fogHeightFalloff = 0.15f;  // density falloff with world height
    f32 fogHeight = 0.0f;          // world Y where the fog is densest
    f32 fogAnisotropy = 0.76f;     // Henyey-Greenstein g (forward scattering)
    f32 fogSunIntensity = 1.0f;    // multiplier on the sun's in-scatter
    f32 fogMaxDistance = 200.0f;   // ray-march far clamp (world units)
    f32 fogAmbient = 0.02f;        // isotropic sky in-scatter
    u32 fogStepCount = 32;         // ray-march samples (fewer = along-ray banding)
    // Tint applied to ALL fog in-scatter (the fog's own colour; 1,1,1 = neutral).
    glm::vec3 fogColor{1.0f, 1.0f, 1.0f};
    // God-ray boost: extra multiplier on the SHADOWED sun in-scatter only, so
    // light shafts through gaps pop without brightening the whole fog (1 = off).
    f32 fogGodRays = 1.0f;

    // Screen-space global illumination: one diffuse bounce gathered from
    // on-screen lit surfaces (cosine-weighted rays over the G-buffer normal,
    // ray-marched against depth), composited additively into the HDR -> colour
    // bleeding from nearby geometry. Misses off-screen light (screen-space).
    u32 ssgiEnabled = 1;
    f32 ssgiIntensity = 0.7f;  // strength of the indirect bounce
    f32 ssgiRadius = 4.0f;     // world-space gather radius
    u32 ssgiSamples = 8;       // hemisphere rays per pixel (TAA denoises)

    // Painterly (oil-on-canvas) finish: a DEDICATED scene-driven pass (Painterly.hlsl,
    // run before tonemap) that repaints the lit HDR as edge-aware brush strokes using
    // the G-buffer (depth/normal) so strokes follow forms and stop at silhouettes,
    // tints them with the light's colour, and lays them on a canvas weave. A "look"
    // field set (volume-overridable). Forces the photographic vignette off.
    u32 painterlyEnabled = 0;
    f32 painterlyRadius = 4.0f;      // stroke size (1..7); scaled to ~px*3 in the shader
    f32 painterlyWarmCool = 0.4f;    // warm-light / cool-shadow grade strength (0 = neutral)
    f32 painterlyStrokeFlow = 0.85f; // how much strokes elongate along forms (0..1)
    f32 painterlyStrength = 0.85f;   // blend the painted result over the original (0..1)
    f32 painterlyEdge = 0.7f;        // keep strokes off silhouettes (depth/normal edges; 0..1)
    f32 painterlyLightTint = 0.4f;   // bleed the light's colour into the strokes (0..1)
    f32 painterlyStrokeDetail = 0.15f; // bristle streaks along strokes (0..1)
    f32 painterlyCanvasScale = 6.0f;   // canvas tooth cell size (pixels; min 2)
    f32 painterlyCanvasStrength = 0.05f; // canvas tooth visibility (0..1)
    f32 painterlyPosterize = 0.0f;     // quantize value into N painted steps (0/<2 = off)
    // Stroke splatting: actual brush-stroke marks splatted over the Kuwahara base
    // (the "real" painterly look). Drawn as instanced oriented quads, both backends.
    u32 painterlyStrokes = 1;          // splat real brush strokes on top (0 = just the filter)
    f32 painterlyStrokeLength = 1.0f;  // stroke length multiplier
    f32 painterlyStrokeDensity = 1.0f; // how densely strokes are packed (higher = more)
    f32 painterlyStrokeSharp = 0.65f;  // stroke definition: 0 = soft/blended, 1 = crisp/opaque
    // Stop-motion "boil": the strokes repaint in discrete steps at this rate (frames/
    // sec) instead of recomputing every rendered frame - the hand-painted/stop-motion
    // look. Lower = slower/choppier; 0 = smooth/continuous (off). Drives a quantized
    // time step folded into the per-stroke seed (CPU-side).
    // Default lowered 8 -> 3: at 8 the pattern re-rolled 8x a second, which reads as
    // a fast shimmer rather than hand-painted stop-motion. Real stop-motion animation
    // is shot on twos or threes (12 or 8 fps), and a PAINTED look wants slower still -
    // each re-roll should register as a new brush pass, not a flicker. 2-4 is the
    // usable range; 0 = smooth/continuous.
    f32 painterlyStrokeBoil = 3.0f;
    // Optional area mask: confine the real brush strokes to a screen-space rect
    // (a "censor box"). Outside the rect only the Kuwahara base shows through.
    u32 painterlyStrokeMask = 0;       // 0 = strokes everywhere, 1 = only inside the rect
    f32 painterlyStrokeMaskMinX = 0.30f; // rect in normalized screen UV (0..1)
    f32 painterlyStrokeMaskMinY = 0.30f;
    f32 painterlyStrokeMaskMaxX = 0.70f;
    f32 painterlyStrokeMaskMaxY = 0.70f;
    // Removed: the automatic 3D surface-stroke painterly renderer (left as an
    // always-off field so old scene files with this key still load). Hand painting
    // (Art Editor) is the painterly path now.
    u32 painterly3D = 0;

    // Shadow quality: number of cascaded-shadow-map slices actually rendered (and
    // sampled). The atlas is always allocated at its full 4x4k / 2x2 layout, but only
    // the first `shadowCascades` tiles are drawn each frame - so this directly scales
    // the shadow RENDER cost (each cascade re-rasterizes every caster: cost is
    // cascades x casters, and on Vulkan each is a per-draw descriptor bind). The split
    // scheme is recomputed over the ACTIVE count in Scene::MakeView, so fewer cascades
    // still cover the full shadowDistance - just coarser per slice. Preset-driven
    // (High = 4 = the authored look; Medium/Low reduce it). Clamped to [1, kMaxShadowCascades].
    u32 shadowCascades = 4;

    // Memberwise, defaulted ON PURPOSE: --test-lightingparity compares the whole
    // post stack rather than a hand-picked list of fields, so a member added below
    // is covered by the parity test the day it is added instead of silently
    // escaping it. Exact float equality is what is wanted - both sides come from
    // the same parse of the same bytes, so any difference is a real one.
    bool operator==(const PostSettings&) const = default;
};

// Per-frame view + lighting environment.
struct SceneView {
    glm::mat4 viewProj{1.0f};
    // Separate view + projection (viewProj == proj * view). Needed by backends that drive an
    // external renderer wanting the camera and projection apart (Effekseer VFX). Populated by the
    // Renderer alongside viewProj; safe CPU-side additions (SceneView is not a GPU cbuffer).
    glm::mat4 view{1.0f};
    glm::mat4 proj{1.0f};
    glm::vec3 cameraPos{0.0f};
    f32       exposure = 1.0f;
    DirectionalLight light;
    f32       ambientIntensity = 0.03f;

    // Image-based lighting: bindless indices for the precomputed maps. When all
    // are 0 the shader falls back to a flat ambient term.
    u32 irradianceIndex = 0;   // diffuse irradiance (equirect)
    u32 prefilteredIndex = 0;  // GGX-prefiltered specular (equirect, mipped)
    u32 brdfLUTIndex = 0;      // split-sum BRDF integration LUT
    f32 prefilteredMaxLod = 0; // mipCount-1 of the prefiltered map
    u32 skinLUTIndex = 0;      // pre-integrated subsurface-scattering LUT (skin)

    // Sky background pass: a bindless equirect environment drawn behind the
    // scene when non-zero. invViewProj unprojects NDC to world for the ray.
    u32 skyIndex = 0;
    glm::mat4 invViewProj{1.0f};

    // Cascaded directional shadows: when shadowsEnabled, the backend renders a
    // depth-only pass per cascade into a 2x2 atlas (DrawShadowPass) and the
    // PBR pass picks the finest cascade containing each pixel. Cascade frusta
    // are camera-fit near -> far; cascadeSplits holds each slice's far plane
    // distance (view space) for diagnostics/selection.
    u32 shadowsEnabled = 0;
    u32 cascadeCount = 0;
    glm::mat4 cascadeViewProj[kMaxShadowCascades]{};
    glm::vec4 cascadeSplits{0.0f};

    // Punctual (point/spot) lights, copied verbatim into the frame constants.
    u32 punctualCount = 0;
    PunctualLight punctualLights[kMaxPunctualLights];

    // Local light/reflection probes: per-pixel box-blended in the shader; outside
    // every probe the global sky IBL (above) applies. Copied into frame constants.
    u32 probeCount = 0;
    ProbeData probes[kMaxProbes];

    // Forward projected decals: each box blends its albedo/normal/MR onto the surfaces
    // it overlaps in MeshPBR (before lighting). Copied into frame constants.
    u32 decalCount = 0;
    DecalData decals[kMaxDecals];

    // Water surface (Gerstner) - GLOBAL scene ocean params + interactive ripple ring
    // sources; fed to Water.hlsl via the gWave*/gWater*/gRipples frame constants.
    glm::vec4 waterWaveA[4]{}; // (dirX, dirZ, amplitude, wavelength)
    glm::vec4 waterWaveB[4]{}; // (speed, steepness, 0, 0)
    glm::vec4 waterShallow{0.10f, 0.30f, 0.42f, 5.0f}; // (rgb grazing tint, fresnel power)
    glm::vec4 waterDeep{0.02f, 0.08f, 0.13f, 0.10f};   // (rgb body, reflection roughness)
    glm::vec4 waterParams{0.5f, 1.0f, 1.0f, 0.0f};     // (foam, rippleStrength, rippleScale, fftOn)
    u32 rippleCount = 0;
    glm::vec4 ripples[kMaxRipples]{}; // (centerX, centerZ, impactTime, strength)

    // FFT ocean (Tessendorf): when fftOcean != 0 the water VS displaces the grid from the GPU
    // FFT displacement buffer (bound via SetVertexShaderBuffer) instead of the Gerstner sum.
    // waterParams.w carries the on/off flag to the shader; these size the world<->tile mapping.
    u32 fftOcean = 0;      // 0 = Gerstner, 1 = FFT displacement buffer
    f32 fftPatch = 128.0f; // world metres the FFT tile spans (world XZ -> tile index)
    f32 fftHeight = 1.0f;  // vertical displacement scale

    // Depth-based water: how the water PS grades absorption/foam/soft edges by the water-column
    // depth it reads from the scene depth buffer (bindless slot filled by the backend).
    f32 waterAbsorptionDepth = 6.0f; // metres to full deep-colour absorption
    f32 waterShorelineWidth = 1.5f;  // metres of the shoreline foam band
    f32 waterEdgeFade = 0.5f;        // metres of soft depth-fade where water meets geometry

    // World-anchored painterly censors (CensorComponent): each re-paints a
    // dynamic object inside a soft sphere. CPU-side; the backend projects these
    // tests them in 3D and feeds the painterly passes (CollectCensors below).
    u32 censorCount = 0;
    CensorData censors[kMaxCensors];

    // Baked SH-L1 irradiance volume (the diffuse-GI upgrade). giShIndex 0 = none.
    glm::vec3  giOrigin{0.0f};
    glm::vec3  giInvSpacing{0.0f};
    glm::ivec3 giDims{0};
    u32        giShIndex = 0;
    u32        giDepthIndex = 0; // octahedral depth atlas (DDGI visibility)

    // Seconds since the previous frame; drives temporal post effects (e.g.
    // auto-exposure adaptation). CPU-side only - set by the renderer.
    f32 deltaTime = 0.0f;
    // Accumulated seconds since start; animates the sky (cloud drift, twinkle).
    f32 timeSeconds = 0.0f;

    // Weather for the analytic sky: cloud cover/density (0 = clear sky) and an
    // overcast gray-out (0..1). Fed to Sky.hlsl via gWeather.
    f32 cloudCoverage = 0.0f;
    f32 cloudDensity = 0.6f;
    f32 overcast = 0.0f;
    // Wind velocity in cloud-UV units/sec (clouds drift by this * timeSeconds).
    f32 windVelX = 0.0f;
    f32 windVelZ = 0.0f;

    // Weather surface response (fed to gWeather2/gWeather3; drives the wet/puddle/
    // snow block in MeshPBR). 0 across the board = clear/dry, and the shader early-outs.
    f32 wetness = 0.0f;         // 0 dry .. 1 soaked (darkens albedo, boosts gloss)
    f32 puddles = 0.0f;         // 0 none .. 1 standing water in flat/low areas
    f32 snowAmount = 0.0f;      // 0 none .. 1 snow on up-facing surfaces
    f32 precipIntensity = 0.0f; // RAIN intensity for puddle ripples (0 for snow/none)
    f32 puddleScale = 6.0f;     // puddle noise world tiling (m)
    f32 snowScale = 4.0f;       // snow break-up noise world tiling (m)
    f32 cloudVolumetric = 0.0f; // 1 = raymarched volumetric clouds (else the 2D layer)
    f32 cloudQuality = 0.4f;    // volumetric cloud step-count scale (0..1 -> 28..96 steps)

    // HDR post-process stack settings.
    PostSettings post;
};

// Collect the frame's active world-anchored censors for the painterly passes. The
// shader does a TRUE 3D sphere test (reconstructs each pixel's world position from
// depth and measures distance to the censor center), so there is NO screen
// projection here - that makes the censor immune to camera angle (the old
// project-to-screen-circle approach degenerated at some orientations) and lets it
// affect ANY geometry inside the sphere, static or dynamic. Fills
// outCensors[i] = (worldCenter.xyz, worldRadius) and packs per-censor strength /
// feather into the matching component of outStrength / outFeather (.x..w).
inline u32 CollectCensors(const SceneView& view, glm::vec4 outCensors[kMaxCensors],
                          glm::vec4& outStrength, glm::vec4& outFeather) {
    outStrength = glm::vec4(0.0f);
    outFeather = glm::vec4(0.0f);
    u32 n = 0;
    for (u32 i = 0; i < view.censorCount && n < kMaxCensors; ++i) {
        const CensorData& c = view.censors[i];
        if (c.strength <= 0.0f || c.radius <= 0.0f) continue;
        outCensors[n] = glm::vec4(c.center, c.radius);
        outStrength[static_cast<int>(n)] = glm::clamp(c.strength, 0.0f, 1.0f);
        // Clamp feather away from 0 so inner radius stays < outer (no smoothstep
        // divide-by-zero in the shader); 0.01 reads as an effectively hard edge.
        outFeather[static_cast<int>(n)] = glm::clamp(c.feather, 0.01f, 1.0f);
        ++n;
    }
    return n;
}

// Material feature flags (packed into DrawItem::materialFlags).
enum MaterialFlags : u32 {
    MaterialFlag_None       = 0,
    MaterialFlag_Subsurface = 1u << 0, // skin / SSS shading (pre-integrated)
    MaterialFlag_Cloth      = 1u << 1, // fabric sheen (Charlie lobe)
    MaterialFlag_Eye        = 1u << 2, // parallax-iris eye shading
    MaterialFlag_Hair       = 1u << 3, // anisotropic Kajiya-Kay hair
    MaterialFlag_Transparent= 1u << 4, // alpha-blended transparency pass
    MaterialFlag_NoShadow   = 1u << 5, // excluded from the shadow-casting pass
    // A "solid" transparent (e.g. a paint stroke): alpha-blended like Transparent,
    // but WRITES depth + velocity so depth-based post (depth of field, TAA) treats it
    // as a real in-focus surface instead of inheriting the far-background depth and
    // blurring where it floats off a surface. Use together with Transparent.
    MaterialFlag_DepthWrite = 1u << 6,
    // Dynamic-layer object (player / NPC / interactable): exempt from the painterly
    // finish so it reads crisp against the painted static world. The forward pass
    // writes this into HDR alpha (the painterly mask); a composite restores the
    // object's lit colour over the painterly result. Lighting/shadows/GI/fog stay on.
    MaterialFlag_PainterlyExempt = 1u << 7,
    // Terrain hole mask: the thickness-texture slot (unused by terrain) carries a
    // single-channel hole mask; the forward pass clips (discards) pixels where it is
    // set, cutting visual holes in the terrain so cliff/cave models show through.
    MaterialFlag_TerrainHole = 1u << 8,
    // Terrain splat: blend up to 4 tiling material textures by a painted weight mask.
    // The 4 layer albedos ride in the (terrain-unused) albedo/normal/mr/ao slots and
    // the weight mask in the emissive slot; subsurfaceRadius carries the tile scale.
    MaterialFlag_TerrainSplat = 1u << 9,
    // Painterly censor target: the entity carries a CensorComponent, so the
    // painterly passes paint brush strokes onto ITS surface (and keep the painted
    // look on it) - confining the censor to the object, not the volume around it.
    MaterialFlag_Censored = 1u << 10,
    // The entity is a water surface: drawn in a dedicated water pass (Gerstner-wave VS +
    // reflective PS), not the normal opaque/transparent mesh path.
    MaterialFlag_Water = 1u << 11,
    // Vegetation: the MeshPBR VS sways this draw's vertices in the wind (stiffer at the
    // base, leaf flutter at the tips). VS-only; no shader-variant / PSO change.
    MaterialFlag_Wind = 1u << 12,
    // Alpha-cutout foliage: the PS discards texels whose albedo alpha is below the cutoff, so a
    // leaf-cluster texture on a quad renders as leaf shapes (not a solid card). PS-only discard in
    // the opaque pass; pairs with the two-sided foliage PSO where available.
    MaterialFlag_AlphaTest = 1u << 13,
    // Unified material LAYERS (Source/Material): blend up to 4 material layers by per-layer masks
    // with arbitrary projection (UV/World/Triplanar), height blending, and RNM normal blending -
    // the generalisation of TerrainSplat. Reuses the same overloaded splat texture slots; the GPU
    // resolver lives in Shaders/MaterialLayered.hlsli. Consumption in MeshPBR + the scene component
    // wiring is the LIVE-mode integration (docs/Design-MaterialAuthoring.md, Part D); BAKED mode
    // needs no flag (it composites offline into the normal single-material path). Default off ->
    // byte-identical to today for every existing draw.
    MaterialFlag_Layered = 1u << 14,
};

// Curated OpenPBR shader-specialization variants (P3). The opaque forward pass binds one
// pixel-shader variant per draw so a material only pays for the lobes it uses. This is a small
// hand-picked set keyed by the material's shading model, NOT a 2^N feature matrix; anything not
// covered routes to Full (the correctness fallback). ORDER + NAMES must stay in lockstep with the
// MeshPBR_<VAR>.ps registration in cmake/ShaderCompile.cmake, the backend PS-load tables, and
// material::ComputeShaderVariant (Source/RHI/MaterialCompiler.h).
enum class ShaderVariant : u32 {
    Std = 0,   // base OpenPBR dielectric+metal (+ world features); the ~90% case
    Coat,      // + coat lobe
    Sss,       // + subsurface
    Fuzz,      // + fuzz / cloth sheen
    Hair,      // Kajiya-Kay hair
    Eye,       // parallax-iris eye
    Full,      // every lobe compiled in (fallback; also transparent/preview pass)
    Count,
};

// One mesh instance to draw with an OpenPBR Surface material.
struct DrawItem {
    MeshHandle mesh;
    glm::mat4  transform{1.0f};
    // Physically-based material VALUES (OpenPBR Surface parameter set). Textures below stay
    // as bindless handles - SurfaceParams owns values only (see RHI/SurfaceMaterial.h). The
    // GPU packer reads the legacy-equivalent fields (base_color/geometry_opacity/base_metalness/
    // specular_roughness/subsurface_*/coat_*/emission_*) and emits the same ObjectConstants bytes.
    SurfaceParams surface;
    // Bindless texture indices (0 = use the constant factor / no map).
    TextureHandle albedoTexture;
    TextureHandle normalTexture;
    TextureHandle mrTexture;   // glTF packing: B = metallic, G = roughness
    TextureHandle aoTexture;
    TextureHandle emissiveTexture;
    TextureHandle thicknessTexture;        // back-light transmission thickness (0 = none)
    u32        materialFlags = MaterialFlag_None;
    // OpenPBR shader specialization (P3): which curated MeshPBR pixel-shader variant the opaque
    // pass binds for this draw. Computed by material::ComputeShaderVariant from surface + flags.
    // Defaults to Full so an un-set DrawItem always renders correctly (just unspecialized).
    ShaderVariant shaderVariant = ShaderVariant::Full;

    // Art Editor surface paint: a per-object paint canvas composited over the
    // material in the forward pass. `paintColorTexture` is RGB pigment + A
    // coverage; `paintHeightTexture` R = relief height (0.5 neutral) that
    // perturbs the shading normal. Both carry mip chains and are sampled with a
    // distance-derived LOD bias so far strokes average into broader washes.
    TextureHandle paintColorTexture;
    TextureHandle paintHeightTexture;
    f32        paintOpacity = 1.0f;     // global blend of the paint over the base
    f32        paintHeightScale = 0.0f; // relief strength (0 = flat, color only)
    f32        paintLodBias = 1.0f;     // distance->mip averaging strength
    f32        paintTexel = 0.0f;       // 1 / paint resolution (height differencing)
    // Paint coordinate projection: 0 = mesh UV, 1 = box (world-scaled, no stretch).
    u32        paintProjMode = 0;
    glm::vec3  paintBoxCenter{0.0f};    // local AABB center
    glm::vec3  paintBoxScale{1.0f};     // object world scale
    f32        paintBoxInvM = 1.0f;     // 1 / max(extent*scale) (uniform density)

    // Terrain splat: 4 layers' material textures (albedo / normal / metal-rough),
    // blended by the weight mask (emissiveTexture) at the terrain-wide UV and tiled
    // by world XZ / subsurfaceRadius. Only used when MaterialFlag_TerrainSplat is set.
    TextureHandle splatAlbedo[4];
    TextureHandle splatNormal[4];
    TextureHandle splatMR[4];
    f32           splatRough[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // per-layer roughness factor

    // 3D painterly: when true, the surface-stroke renderer paints this item (set
    // for STATIC, non-skinned world geometry; characters/dynamics shade normally).
    bool surfaceStrokes = false;

    // Skeletal skinning: when boneCount > 0, `bones` points at the joint
    // palette (global * inverseBind per joint) for this draw. The pointer must
    // stay valid through the DrawShadowPass/DrawScene calls of the frame; the
    // backend copies it into a per-frame GPU arena.
    const glm::mat4* bones = nullptr;
    u32 boneCount = 0;

    // Per-object motion vectors (G-buffer velocity, for TAA + motion blur).
    // `prevTransform` is this entity's world matrix from the PREVIOUS frame
    // (equal to `transform` for static/first-seen entities, i.e. zero motion).
    // `prevBones` is the previous frame's joint palette for skinned motion;
    // when null the skinned velocity falls back to the rigid `prevTransform`.
    // Both must stay valid through DrawScene; the backend copies them.
    glm::mat4 prevTransform{1.0f};
    const glm::mat4* prevBones = nullptr;

    // GPU instancing (CPU-side, filled by the Renderer's run-builder over the
    // SORTED item array): 1 = a normal single draw; N > 1 = the HEAD of an
    // N-instance run of identical-material items (the next N-1 items are its
    // followers); 0 = consumed by a preceding run head (both the shadow and
    // scene passes must skip it). Same markers drive BOTH passes, so the
    // per-index shadow<->scene coupling becomes per-run and stays consistent.
    u32 instanceRun = 1;

    // Per-cascade shadow visibility: bit c set = this item can affect cascade c.
    // The shadow pass deliberately receives the FULL (unculled) item list, because
    // an off-screen object still casts into the view - but that meant every caster
    // was re-rasterized into EVERY cascade, which measured at 58% of all GPU time
    // (5.6 ms of 9.7 ms at 2000 casters). A near cascade covers a tiny world
    // volume, so most casters cannot possibly affect it.
    // The Renderer fills this by testing each item's world AABB against each
    // cascade's light frustum; that frustum's near plane is already pulled back to
    // the scene bounds (Scene::MakeView), so the test is conservative - anything
    // that could cast into the slice is inside it.
    // 0xFF (all bits) = "affects every cascade", so an item the renderer never
    // touches behaves exactly as before.
    u8 cascadeMask = 0xFF;

    // LOD cross-fade screen-door factor (main pass only; the shadow pass is VS-only so it never
    // dithers). 1.0 = fully opaque (every non-fading draw). A transitioning mesh emits TWO draws:
    // the finer LOD with lodDither = f in [0,1) (keeps the stippled pixels where noise < f, fading
    // OUT as f->0) and the coarser LOD with lodDither = -f (keeps noise >= f, fading IN). Together
    // they cover every pixel with no overlap, so the LOD swap dissolves instead of popping.
    f32 lodDither = 1.0f;

    // Facial blendshapes: `morphTexture` is a bindless RGBA16F delta atlas
    // (width = vertex count, one ROW per morph target = xyz position delta). The VS
    // accumulates `morphCount` active targets - their atlas rows in `morphTargets`,
    // weights in `morphWeights` - into the vertex BEFORE skinning. The defaults
    // (invalid texture / count 0) leave every non-morph draw byte-identical.
    TextureHandle morphTexture;
    u32 morphVertexCount = 0;
    u32 morphCount = 0;
    u32 morphTargets[8] = {};
    f32 morphWeights[8] = {};
};

// One vertex of the in-game UI overlay. Positions are in NDC (the UI system
// applies canvas scaling on the CPU); `texIndex` selects a bindless texture
// (0 = the 1x1 white default, i.e. a solid quad). Straight-alpha blending,
// triangle list. The clip rect (also NDC, min..max) discards fragments
// outside it in the pixel shader - ScrollViews clip their content with it.
// The default sentinel (-2..2) covers all of NDC = no clipping, so plain
// aggregate-initialized vertices behave exactly as before.
struct UIVertex {
    f32 x = 0, y = 0;          // NDC
    f32 u = 0, v = 0;          // texture coordinates
    f32 r = 1, g = 1, b = 1, a = 1;
    u32 texIndex = 0;          // bindless texture (0 = white)
    f32 clipX0 = -2.0f, clipY0 = -2.0f; // NDC clip rect min (sentinel = open)
    f32 clipX1 = 2.0f, clipY1 = 2.0f;   // NDC clip rect max
    // Per-element EFFECT id (P7 custom UI materials): 0 = normal (the shader is a plain
    // color*tex passthrough, so default-init vertices are pixel-identical to before);
    // 1 = grayscale/desaturate. The whole design stays a single bindless draw - the
    // effect is a shader branch, not a blend-state or pipeline change. Extensible.
    u32 fx = 0;
};
static_assert(sizeof(UIVertex) == 56,
              "UIVertex layout is mirrored in the DX12/Vulkan input layouts "
              "(D3D12 uiLayout / Vulkan uiAttrs) and UI.hlsl - keep in sync. (The GL "
              "backend binds a subset and auto-tracks the stride; fx there = 0.)");

// One camera-facing particle billboard vertex, in WORLD space (the VS transforms
// by the frame's viewProj). texIndex 0 = a procedural soft round dot.
struct ParticleVertex {
    f32 x = 0, y = 0, z = 0;   // world position
    f32 u = 0, v = 0;          // sprite UV
    f32 r = 1, g = 1, b = 1, a = 1;
    u32 texIndex = 0;          // bindless sprite (0 = soft dot)
};

// The CPU particle path packs all alpha + additive billboards into ONE shared per-frame
// vertex buffer; both backends clamp the COMBINED vertex count to this (6 MiB / stride).
// Systems that APPEND to the shared list (weather precipitation) cap themselves against
// it so they yield to authored VFX instead of silently overflowing the tail. Keep in
// sync with the backends' kParticleVertexBufferSize (6 MiB).
constexpr u32 kMaxCpuParticleVertices =
    static_cast<u32>((6u * 1024u * 1024u) / sizeof(ParticleVertex));

// One GPU-EXPANDED particle batch: one emitter's worth of billboards, drawn
// straight out of a record buffer with no CPU vertex expansion at all.
//
// The buffer this indexes into has ONE 64-byte stride and is laid out per batch as
//   [emitter record: 2 elements = vfx::GpuEmitter][particle 0][particle 1]...
// so `recordFirst` selects the whole batch: the backend points the buffer's base at
// that element (D3D12 root-SRV GPU virtual address, Vulkan dynamic storage-buffer
// offset) and the VS reads the emitter at byte 0 and particle i at 128 + i*64.
// The base is NEVER a firstInstance - D3D12's SV_InstanceID excludes
// StartInstanceLocation while Vulkan's gl_InstanceIndex includes firstInstance, so
// expressing it that way would make the two backends silently disagree. The draw is
// DrawInstanced(6*count,1,0,0) / vkCmdDraw(cmd, 6*count, 1, 0, 0), firstInstance 0.
//
// VULKAN ALIGNMENT: recordFirst * 64 must be a multiple of
// minStorageBufferOffsetAlignment, which the spec permits to be as large as 256.
// Every producer therefore 256-byte-aligns (4 elements) every emitter block; the
// backend still logs and skips a batch that would violate it rather than diverging.
struct GpuParticleBatch {
    u32 recordFirst = 0; // element index of this emitter's 2-element record block
    u32 count = 0;       // live particles following it
    u32 additive = 0;    // 0 = alpha blend, 1 = additive
};

// Elements the per-emitter record occupies, in units of the buffer's 64-byte
// stride. C++ twin: sizeof(vfx::GpuEmitter) == 128. HLSL twin: kVfxEmitterBytes.
inline constexpr u32 kGpuParticleEmitterElements = 2;

// Element alignment of an emitter block inside a record buffer. 4 elements = 256 B,
// the largest minStorageBufferOffsetAlignment the Vulkan spec permits, so a layout
// built to this is legal on every desktop part rather than on the one it was tested
// on. Producers align to it; both backends draw the identical result.
inline constexpr u32 kGpuParticleBlockAlign = 4;

// Elements ONE emitter batch may occupy, record block included. This is the bind
// window a record buffer declares via GpuBufferDesc::maxBindElements, so it is also
// the hard per-emitter particle ceiling on BOTH backends - the batch is clamped to
// it at the draw so D3D12 and Vulkan can never disagree about the tail.
inline constexpr u32 kMaxGpuParticleBatchElements = 65536;

// Distinct record BUFFERS whose batches can draw in one frame. Two are in use: the
// per-frame CpuWrite ring the CPU-simulated gpuExpand emitters upload into, and the
// persistent device-local buffer the GPU simulation writes with compute. A batch
// carries only an element offset, so the buffer identity has to come from the group.
inline constexpr u32 kMaxGpuParticleGroups = 4;

// Render-side params for a BAKED NanoVDB volume - the source-agnostic RUNTIME representation.
// The world AABB clips the raymarch (empty-space skip); the grid's own affine map handles
// world->index sampling.
// densityScale is applied ONCE, in the shader (the baked grid stores RAW density) - never also
// bake a scale into the grid. Carries only look knobs + bounds, nothing about how it was made.
struct VolumeRenderParams {
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{1.0f};
    f32 densityScale = 1.0f;
    f32 emission = 2.5f;   // temperature/blackbody glow (inert until a temperature grid, P4+)
    f32 extinction = 1.5f; // absorption (denser/darker smoke)
    i32 stepCount = 96;    // raymarch steps
    i32 shadowSteps = 4;   // self-shadow march steps (0 = none)
    // Translation placing the baked grid in the world (the entity's world position). boundsMin/Max
    // stay the grid's LOCAL AABB; the raymarch offsets the RayBox by this and un-offsets before
    // sampling. Translation only - rotation/scale of the volume are not supported.
    glm::vec3 worldOffset{0.0f};
    // Look color. Default albedo (1,1,1) renders EXISTING grids byte-identically to today's grey smoke
    // (the shader multiplies the SCATTERED light by albedo; 1 is a no-op). emissionColor tints the glow
    // (fire=warm, magic=blue) and is INERT until a temperature grid is fed (emission term is 0 while
    // temp==0). emissionMode: 0=blackbody(temp), 1=tint only, 2=tint x blackbody.
    glm::vec3 albedo{1.0f};                       // single-scatter tint (dust=brown, magic/clouds tints)
    glm::vec3 emissionColor{1.0f, 0.45f, 0.15f};  // glow tint
    i32       emissionMode = 0;
};

// ---------------------------------------------------------------------------
// GPU compute + GPU-writable structured buffers
// ---------------------------------------------------------------------------
//
// These types are the general compute seam - a structured buffer a compute shader
// WRITES and a vertex shader READS, plus a way to run a kernel over it - which is
// what GPU particle simulation + GPU vertex expansion (and the volume solver) need.
//
// Deliberately NOT here: indirect draw. Every draw the particle system issues has
// a CPU-known vertex count (the spawn scheduler stays on the CPU - it owns an f64
// accumulator and the bit-exactness contract that --test-vfxcompat pins), so
// DrawInstanced(6*N,1,0,0) / vkCmdDraw(cmd,6*N,1,0,0) is sufficient and sidesteps
// the D3D12 CommandSignature machinery and the multiDrawIndirect /
// drawIndirectFirstInstance feature gates entirely.

// Opaque handle to a GPU buffer created via CreateGpuBuffer. Slot-recycling: a
// destroyed handle's id may be reissued, so never hold one past DestroyGpuBuffer.
struct GpuBufferHandle {
    u32 id = 0;
    bool IsValid() const { return id != 0; }
};

// Bit flags for GpuBufferDesc::usage. ShaderWrite and CpuWrite are mutually
// exclusive (a GPU-written buffer must be device-local; D3D12 forbids
// ALLOW_UNORDERED_ACCESS on the UPLOAD heap) - CreateGpuBuffer rejects both.
namespace GpuBufferUsage {
enum : u32 {
    // Readable by a shader as a StructuredBuffer (D3D12 SRV / Vulkan read-only
    // storage buffer). Required for SetVertexShaderBuffer and for compute SRVs.
    ShaderRead = 1u << 0,
    // Writable by a compute dispatch as an RWStructuredBuffer (D3D12 UAV /
    // Vulkan storage buffer). Implies device-local storage.
    ShaderWrite = 1u << 1,
    // Bindable as a vertex stream (per-vertex or per-instance attributes).
    VertexBuffer = 1u << 2,
    // Host-visible and persistently mapped; MapGpuBuffer returns a pointer.
    // Allocated as a per-frame-in-flight RING so the CPU can refill the current
    // slot without racing the GPU - MapGpuBuffer hands back the CURRENT frame's
    // slot, and every bind uses that same slot.
    CpuWrite = 1u << 3,
    // Usable as the source of an indirect draw/dispatch (D3D12 ExecuteIndirect
    // argument buffer / Vulkan VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT). Implies
    // device-local + ShaderWrite (a compute kernel fills the arg words), and the
    // backend transitions it to the indirect-argument state for the draw. Used by
    // the GPU-compacted grass path; see SetGrassIndirect.
    IndirectArgs = 1u << 4,
};
}

struct GpuBufferDesc {
    u32 elementCount = 0;
    u32 elementStride = 0; // bytes per element (e.g. sizeof(vfx::GpuParticle) == 64)
    u32 usage = GpuBufferUsage::ShaderRead;
    // Elements a SINGLE bind may address starting at its bind offset. 0 means the
    // buffer is only ever bound at offset 0, so the whole thing is visible.
    //
    // This exists for Vulkan, and it is not optional there: a dynamic storage-buffer
    // descriptor written with VK_WHOLE_SIZE must be bound with a dynamic offset of 0
    // (VUID-vkCmdBindDescriptorSets-pDescriptorSets-06715), and the bound range must
    // still fit the allocation (VUID-...-01979). So a buffer that IS bound at a
    // non-zero offset gets a BOUNDED range of maxBindElements, and the backend pads
    // the allocation by that same window so every legal offset stays in bounds.
    // D3D12 root SRVs take any offset and ignore this field - which is exactly why
    // it has to be declared here rather than discovered per backend.
    u32 maxBindElements = 0;
    const char* debugName = nullptr;
};

// Opaque handle to a compute pipeline created via CreateComputePipeline.
struct ComputePipelineHandle {
    u32 id = 0;
    bool IsValid() const { return id != 0; }
};

// Per-dispatch resource caps. Both backends build a root signature / descriptor
// set layout sized exactly from ComputePipelineDesc, so these are ceilings, not
// costs.
inline constexpr u32 kMaxComputeUavs = 4;
inline constexpr u32 kMaxComputeSrvs = 4;
// Root/uniform constant block per dispatch. Copied at QueueCompute time, so the
// caller's pointer does not have to outlive the call.
inline constexpr u32 kMaxComputeConstantBytes = 256;
// Dispatches that may be queued in one frame. Raised from 16 to 128 for the GPU
// Eulerian fluid solver, whose one-substep dispatch chain is 10 + pressureIterations
// (~49 at the default 40 iters). Both backends size only fixed C-arrays / a per-pipeline
// descriptor-pool + CB ring by this (a few hundred KB total across all compute pipelines);
// nothing else assumes <= 16. A substep must fit in ONE drain so the inter-dispatch barriers
// serialize its whole read-after-write chain.
inline constexpr u32 kMaxQueuedComputeDispatches = 128;

// A compute pipeline built from a precompiled kernel. `shaderName` names the
// product cmake/ShaderCompile.cmake emits: "Foo" loads shaders/Foo.cs.dxil on
// D3D12 and shaders/Foo.cs.spv on Vulkan. A kernel that is not registered in
// ShaderCompile.cmake produces no file and CreateComputePipeline fails loudly
// (the fog/ssgi silent-dormancy precedent).
//
// BINDING CONVENTION - the shader MUST declare its resources exactly like this,
// with an explicit [[vk::binding]] on every one (the -fvk-*-shift remapping in
// ShaderCompile.cmake would otherwise land Vulkan's bindings somewhere else):
//   constants  cbuffer            : register(b0)      [[vk::binding(0, 0)]]
//   uav i      RWStructuredBuffer : register(u<i>)    [[vk::binding(1 + i, 0)]]
//   srv i      StructuredBuffer   : register(t<i>)    [[vk::binding(1 + uavCount + i, 0)]]
struct ComputePipelineDesc {
    const char* shaderName = nullptr;  // "VfxSim" -> VfxSim.cs.dxil / VfxSim.cs.spv
    const char* entryPoint = "CSMain"; // Vulkan needs it by name; DXIL ignores it
    u32 constantBytes = 0;             // <= kMaxComputeConstantBytes
    u32 uavCount = 0;                  // <= kMaxComputeUavs
    u32 srvCount = 0;                  // <= kMaxComputeSrvs
    const char* debugName = nullptr;
};

// One queued dispatch. Buffers are bound at the CURRENT frame's ring slot.
struct ComputeDispatch {
    ComputePipelineHandle pipeline;
    const void* constants = nullptr; // copied immediately by QueueCompute
    u32 constantBytes = 0;
    GpuBufferHandle uavs[kMaxComputeUavs]{};
    u32 uavCount = 0;
    GpuBufferHandle srvs[kMaxComputeSrvs]{};
    u32 srvCount = 0;
    u32 groupsX = 1, groupsY = 1, groupsZ = 1;
};

// Default reference canvas the UI is authored against (configurable per
// project; see ui::CanvasConfig).
inline constexpr f32 kUICanvasWidth = 1920.0f;
inline constexpr f32 kUICanvasHeight = 1080.0f;

// Abstract GPU device + presentation. One instance owns the swapchain and the
// per-frame synchronization needed to drive a clear-to-present loop.
class IRenderDevice : public NonCopyable {
public:
    virtual ~IRenderDevice() = default;

    // Acquire/prepare the next back buffer and begin recording this frame.
    virtual void BeginFrame() = 0;

    // Clear the current back buffer to a linear RGBA color.
    virtual void ClearBackBuffer(f32 r, f32 g, f32 b, f32 a) = 0;

    // Finish recording, submit, present, and advance frame synchronization.
    virtual void EndFrame() = 0;

    // Recreate the swapchain for a new client size. Safe to call with 0 (no-op).
    virtual void Resize(u32 width, u32 height) = 0;

    // Block until all submitted GPU work has completed (e.g. before shutdown).
    virtual void WaitForGpuIdle() = 0;

    // -- Scene rendering (optional capability) ------------------------------
    // Backends that implement geometry rendering override these. The default
    // implementations make a backend behave as a clear-only device, so a
    // partially-implemented backend still builds and presents.

    // True if this backend can upload meshes and record DrawScene.
    virtual bool SupportsSceneRendering() const { return false; }

    // True if this backend can record DrawUIOverlay, INDEPENDENTLY of scene
    // rendering. A backend whose UI shaders loaded but whose mesh shader did
    // not can still present the menu / safe-mode modal (this stays true while
    // SupportsSceneRendering() is false).
    virtual bool SupportsUIOverlay() const { return false; }

    // Uploads a CPU mesh to the GPU. Returns an invalid handle if unsupported.
    virtual MeshHandle CreateMesh(const hbe::MeshData&) { return {}; }

    // Re-uploads vertex/index data into an EXISTING mesh's GPU buffers, IN PLACE.
    // NEVER GROWS: an upload larger than the original allocation is REFUSED, on
    // every backend, and reported by returning false. Reserve the headroom up
    // front with CreateMeshReserved if the geometry is going to change size.
    //
    // The return value is not advisory. A refused upload leaves the GPU holding
    // the PREVIOUS geometry, so a caller that has already updated its own CPU
    // copy - bounds, colliders, anything derived - is now describing something
    // that is not on the screen. Check it.
    virtual bool UpdateMesh(MeshHandle, const hbe::MeshData&) { return false; }

    // Like CreateMesh, but ALLOCATES for the stated capacity while uploading only
    // `initial`. This is the front door for geometry that is generated rather than
    // loaded: a procedural mesh whose vertex count moves with its parameters needs
    // room to grow, and UpdateMesh deliberately will not resize for it.
    // A capacity below the initial contents is raised to fit rather than refused.
    virtual MeshHandle CreateMeshReserved(const hbe::MeshData& initial, u32 /*vertexCapacity*/,
                                          u32 /*indexCapacity*/) {
        return CreateMesh(initial);
    }

    // Uploads a 2D texture into the bindless array; returns its index handle.
    virtual TextureHandle CreateTexture(const TextureDesc&) { return {}; }

    // Creates an uninitialised texture writable by a compute pass. With depth>1
    // it is a 3D volume (TEXTURE3D / VK_IMAGE_TYPE_3D). It always gets a sampled
    // SRV (the returned handle) and, when desc.storage, a UAV the backend records
    // internally for a compute pass to bind. Returns an invalid handle when
    // unsupported. (General 3D-texture allocator; the volume solver's Phase-3
    // texture-field path and any future compute-written volume use it.)
    virtual TextureHandle CreateVolumeTexture(const TextureDesc&) { return {}; }

    // Feeds a BAKED NanoVDB volume for this frame - the RUNTIME volume path. `density` is a raw,
    // byte-exact NanoVDB grid blob (from the baker / streamed from a .hbvol by VolumeCache); the
    // PNanoVDB raymarch (VolumeRaymarch.hlsl) samples it via a StructuredBuffer<uint>. `temperature`
    // is an OPTIONAL second grid (same frame/bounds) that drives blackbody/tint emission (fire glow);
    // temperature==nullptr / tempSize==0 keeps the glow inert (grey smoke). densitySize==0 /
    // density==nullptr disables the volume this frame. Pointers must stay valid through the frame
    // (like SetParticles). The volume sim is offline-only: it bakes to .hbvol and this feeds the
    // cached grids. No-op on backends without the volume raymarch.
    virtual void SetVolumeGrid(const void* /*density*/, usize /*densitySize*/,
                               const void* /*temperature*/, usize /*tempSize*/,
                               const VolumeRenderParams& /*params*/) {}

    // -- GPU compute + GPU-writable structured buffers (optional capability) --
    // True when CreateGpuBuffer / CreateComputePipeline / QueueCompute do real
    // work. False on clear-only backends (GL), which inherit the no-ops below.
    virtual bool SupportsGpuCompute() const { return false; }

    // Allocates a structured buffer of elementCount * elementStride bytes. With
    // GpuBufferUsage::CpuWrite it is a per-frame-in-flight RING (one allocation
    // per slot); otherwise it is a single device-local allocation. Returns an
    // invalid handle on failure (bad desc, out of memory, unsupported backend).
    virtual GpuBufferHandle CreateGpuBuffer(const GpuBufferDesc&) { return {}; }

    // CPU pointer to the CURRENT frame's slot of a CpuWrite buffer (nullptr for
    // any other buffer). Valid until the next BeginFrame; write, do not read
    // (it is write-combined upload memory).
    virtual void* MapGpuBuffer(GpuBufferHandle) { return nullptr; }

    // Blocking read-back of the current slot's first `bytes` bytes into `dst`.
    // Flushes the GPU, so this is a debug/validation tool - never a per-frame
    // call. Returns false when unsupported or the range is out of bounds.
    virtual bool ReadGpuBuffer(GpuBufferHandle, void* /*dst*/, u32 /*bytes*/) { return false; }

    // Releases a buffer. Waits for GPU idle first (the buffer may still be
    // referenced by in-flight command lists), so treat it as a load-time call.
    virtual void DestroyGpuBuffer(GpuBufferHandle) {}

    // Release a mesh / texture created by CreateMesh / CreateTexture, reclaiming its
    // VRAM. NON-BLOCKING: the resource is deferred a few frames (until no in-flight frame
    // can reference it) and its handle id / bindless slot is recycled for a later Create.
    // The CALLER guarantees no live draw references the handle (streaming despawn proves
    // this with a mark-sweep against every resident entity). Default no-op: a backend that
    // has not implemented reclaim simply leaks, exactly as before.
    virtual void DestroyMesh(MeshHandle) {}
    virtual void DestroyTexture(TextureHandle) {}

    // True when DestroyMesh/DestroyTexture actually free VRAM (and recycle the handle).
    // The streaming reclaim sweep only runs when this is true: on a backend that still
    // no-ops the destroys, dropping resources from the shared cache would force a re-upload
    // on the next respawn AND leak the old resource, so such a backend keeps the old
    // never-reclaim behaviour untouched until it implements real reclaim.
    virtual bool SupportsResourceReclaim() const { return false; }

    // True when the backend can create/sample BC (block-compressed) textures. Core on D3D12;
    // gated on the textureCompressionBC feature on Vulkan. When false, load-time variant
    // resolution must fall back to the uncompressed .uaf (never hand a BC desc to CreateTexture).
    virtual bool SupportsBlockCompression() const { return false; }

    // Builds a compute pipeline from a precompiled kernel. Invalid handle when
    // the shader is missing or the backend has no compute.
    virtual ComputePipelineHandle CreateComputePipeline(const ComputePipelineDesc&) { return {}; }

    // Destroys a compute pipeline and recycles its slot (waits for the GPU to idle first, since it
    // may still be referenced by in-flight command lists). Used by tools that RE-compile a kernel at
    // runtime (the editor Material-graph GPU preview) so re-creation does not accumulate pipelines.
    virtual void DestroyComputePipeline(ComputePipelineHandle) {}

    // Queues a dispatch to run at the START of the next frame, BEFORE any render
    // pass opens. Call it before Renderer::RenderScene - Vulkan cannot record compute
    // inside an active render pass, and ClearBackBuffer opens one that stays open
    // through DrawScene. Both backends execute the queue at the same point (their
    // BeginFrame), so the two frame timelines stay comparable. The queue is
    // consumed and cleared every frame; queueing more than
    // kMaxQueuedComputeDispatches drops the excess with a warning.
    virtual void QueueCompute(const ComputeDispatch&) {}

    // Binds a ShaderRead buffer as the vertex shader's structured buffer
    // (`StructuredBuffer<T> gVfxRecords : register(t2, space1)` /
    // `[[vk::binding(0, 2)]]`) for this frame's DrawScene, starting at element
    // `firstElement`. That element offset is the per-batch base - it is applied
    // as a BUFFER OFFSET (D3D12 root-SRV GPU virtual address / Vulkan dynamic
    // storage-buffer offset), never as a firstInstance, so the D3D12
    // SV_InstanceID vs Vulkan gl_InstanceIndex divergence cannot arise.
    // An invalid handle unbinds. One frame only, like SetParticles.
    virtual void SetVertexShaderBuffer(GpuBufferHandle, u32 /*firstElement*/) {}

    // Re-uploads pixel data (all mips) into an EXISTING bindless texture in place
    // (no new slot/resource), the texture-equivalent of UpdateMesh. The desc must
    // match the texture's created width/height/mipCount/format. Used for live
    // surface painting (re-uploading a paint canvas as it is edited). Synchronous.
    virtual void UpdateTexture(TextureHandle, const TextureDesc&) {}

    // Renders the directional-light shadow map for this frame (depth-only pass
    // over `items` from SceneView::lightViewProj). Must be called after
    // BeginFrame and BEFORE ClearBackBuffer/DrawScene; no-op when unsupported
    // or SceneView::shadowsEnabled is 0.
    virtual void DrawShadowPass(const SceneView&, const DrawItem*, u32 /*count*/) {}

    // Sets this frame's particle billboards (world-space). They're drawn inside
    // DrawScene's HDR pass (depth-tested against the scene, no depth write) right
    // before post, so bloom catches additive sparks. `alpha` verts blend over the
    // scene; `additive` verts add. Pointers must stay valid through DrawScene.
    virtual void SetParticles(const ParticleVertex* /*alpha*/, u32 /*alphaCount*/,
                              const ParticleVertex* /*additive*/, u32 /*addCount*/) {}

    // Sets this frame's GPU-EXPANDED particle batches. `records` is a ShaderRead
    // buffer of 64-byte elements holding [emitter record][particles] blocks (see
    // GpuParticleBatch); each batch becomes one draw with no vertex buffer and no
    // input layout - the VS builds the quad from SV_VertexID (Shaders/ParticleGpu.hlsl).
    //
    // Runs INSIDE the same block as SetParticles, in the same HDR pass, so both
    // paths blend against each other correctly: CPU alpha, GPU alpha, CPU additive,
    // GPU additive. Pointers must stay valid through DrawScene; one frame only.
    // An invalid handle or count 0 disables the path this frame at zero cost.
    //
    // CALLS ACCUMULATE, up to kMaxGpuParticleGroups. A GpuParticleBatch carries an
    // element offset and nothing else, so batches living in two different record
    // buffers - the CpuWrite upload ring and the compute-written simulation buffer -
    // cannot share one call. Each call adds one (buffer, batches) group; the whole
    // set is consumed and cleared by DrawScene.
    virtual void SetGpuParticles(GpuBufferHandle, const GpuParticleBatch*, u32 /*count*/) {}

    // --- Effekseer VFX (backend-owned; DX12 only today, no-ops elsewhere) ---------------------
    // The backend owns the Effekseer runtime (it needs the native device). These forward the
    // Heartbreak-facing VFX seam (Vfx/EffekseerBackend) to it. `VfxAvailable` reports whether the
    // backend actually has Effekseer; the rest are inert when it does not, so gameplay can always
    // call them and fall back to the native particle system.
    virtual bool VfxAvailable() const { return false; }
    virtual u32  VfxLoadEffect(const char* /*absPath*/) { return 0; }
    virtual int  VfxPlay(u32 /*effectId*/, const glm::vec3& /*worldPos*/) { return -1; }
    virtual void VfxStop(int /*handle*/) {}
    virtual void VfxStopAll() {}
    virtual void VfxSetLocation(int /*handle*/, const glm::vec3& /*worldPos*/) {}
    virtual bool VfxExists(int /*handle*/) const { return false; }
    // Advance the VFX simulation once per frame (before DrawScene). The backend draws the effects
    // itself inside DrawScene's HDR transparent slot, using SceneView.view / SceneView.proj.
    virtual void VfxUpdate(f32 /*dt*/) {}
    virtual int  VfxLiveInstanceCount() const { return 0; }

    // GPU-DRIVEN GRASS: a compute-written blade buffer (`blades`, a ByteAddressBuffer of
    // 32-byte records) drawn as `bladeCount` blades of 6 vertices each. Bound on the same
    // VS structured-buffer seam as SetVertexShaderBuffer (t2/space1) and drawn inside
    // DrawScene's opaque HDR pass with a dedicated lit PSO, so blades are shadowed exactly
    // like meshes. One frame only; an invalid handle / count 0 disables it at zero cost.
    // No-op on clear-only backends (grass simply does not appear there).
    virtual void SetGrass(GpuBufferHandle /*blades*/, u32 /*bladeCount*/) {}

    // GPU-DRIVEN GRASS, INDIRECT variant: the compaction path. `blades` is a COMPACTED
    // buffer the generate compute filled with only the visible blades, and `args` is a
    // 16-byte draw-argument buffer {vtxPerInstance=6, instanceCount, 0, 0} the SAME compute
    // wrote (instanceCount = how many blades survived the cull). The draw is issued via
    // ExecuteIndirect / vkCmdDrawIndirect as 6 verts x instanceCount instances (the VS keys
    // each blade off SV_InstanceID / gl_InstanceIndex; firstInstance is always 0 so the two
    // backends agree). `maxBlades` bounds the compacted region for the SRV view. This is the
    // opt-in true GPU-driven path; SetGrass (fixed count + degenerate-cull) stays the default.
    // Calling this instead of SetGrass for a frame selects the indirect draw. Both no-op on
    // devices without SupportsIndirectDraw().
    virtual void SetGrassIndirect(GpuBufferHandle /*blades*/, GpuBufferHandle /*args*/,
                                  u32 /*maxBlades*/) {}
    // True when the backend implements the indirect grass path (D3D12 + Vulkan). Clear-only
    // backends return false and the caller falls back to SetGrass.
    virtual bool SupportsIndirectDraw() const { return false; }

    // Records draws for `count` items using the analytic PBR pipeline. Must be
    // called between BeginFrame (after the clear) and EndFrame.
    virtual void DrawScene(const SceneView&, const DrawItem*, u32 /*count*/) {}

    // Draws the in-game UI overlay (alpha-blended 2D triangles in reference-
    // canvas space, no depth) over the scene. Call after DrawScene, before
    // RenderUI/EndFrame. No-op when unsupported.
    virtual void DrawUIOverlay(const UIVertex*, u32 /*count*/) {}

    // -- World-space ("physical") UI: a canvas rendered into a texture that a
    //    lit mesh in the scene displays (e.g. a settings page on a notebook).
    // Creates a render-target-capable bindless texture: written raw (UNORM) by
    // DrawUIToTexture, sampled with sRGB decode like any albedo map. Fixed size
    // for its lifetime. Invalid handle when unsupported (GL) or out of slots.
    virtual TextureHandle CreateUITarget(u32 /*width*/, u32 /*height*/) { return {}; }
    // Renders UI triangles (same NDC convention as DrawUIOverlay) into `target`
    // (a CreateUITarget texture), clearing it to transparent black first. Must be
    // called after BeginFrame and BEFORE DrawShadowPass/ClearBackBuffer - the
    // scene pass samples the texture the same frame (the shadow-map write-then-
    // sample precedent). Multiple calls per frame (one per canvas) are allowed.
    // No-op when unsupported.
    virtual void DrawUIToTexture(TextureHandle /*target*/, const UIVertex*, u32 /*count*/) {}

    // -- Editor viewport: render the scene into an offscreen target that the
    //    UI displays in a panel (Unity-style). When the backend reports a
    //    non-zero texture id, the scene renders offscreen and the swapchain
    //    shows only the UI; otherwise the scene renders straight to the screen.
    // Requests the offscreen target be (re)sized; applied at the next BeginFrame.
    virtual void ResizeViewport(u32 /*width*/, u32 /*height*/) {}
    // Backend-specific ImGui texture id for the offscreen color (0 = none).
    virtual u64 GetViewportTextureId() { return 0; }
    // Reads the offscreen viewport color (the final tonemapped+FXAA frame) back to
    // CPU as tightly-packed canonical RGBA8 (top row first). Blocking; editor-only
    // (needs the offscreen target). Returns false when unsupported / not ready.
    // Used by the offline movie render to capture frames.
    virtual bool ReadbackViewportColor(std::vector<u8>& /*outRGBA*/, u32& /*w*/, u32& /*h*/) {
        return false;
    }

    // -- Per-pass GPU profiler (timestamp queries). Off by default; enable at runtime
    //    (--gpuprofile or the dev menu) to log a per-pass GPU-time breakdown every ~2s -
    //    the tool for finding which pass dominates a frame WITHOUT an external capture.
    //    Costs ~1-3 ms/frame while active (the timestamp marks serialise the pipeline),
    //    so it stays opt-in. No-op on backends without timestamp support.
    virtual void SetGpuProfileEnabled(bool /*enable*/) {}
    virtual bool GpuProfileActive() const { return false; }

    // -- Editor asset preview: a SECOND independent offscreen scene (the
    //    orbiting mesh preview of the Asset Viewer, a la Unreal's static-mesh
    //    editor). Rendered as its own HDR mini-pass + tonemap, before the main
    //    scene. All no-ops when unsupported.
    // Requests the preview target be (re)sized; applied at the next BeginFrame.
    // 0x0 keeps the current target.
    virtual void ResizePreview(u32 /*width*/, u32 /*height*/) {}
    // ImGui texture id of the tonemapped preview (0 = none/unsupported).
    virtual u64 GetPreviewTextureId() { return 0; }
    // Renders `items` into the preview target with its own camera/lighting.
    // Must be called after BeginFrame and BEFORE DrawShadowPass/ClearBackBuffer.
    virtual void DrawPreviewScene(const SceneView&, const DrawItem*, u32 /*count*/) {}
    // ImGui texture id for a texture created via CreateTexture, so editor
    // panels can display it (thumbnails). Valid for the device's lifetime;
    // returns 0 when unsupported or before InitUI.
    virtual u64 GetTextureUIHandle(TextureHandle) { return 0; }

    // -- Editor UI (Dear ImGui) overlay, optional capability ----------------
    virtual bool SupportsUI() const { return false; }
    // One-time ImGui platform+renderer init for `nativeWindowHandle` (HWND).
    virtual bool InitUI(void* /*nativeWindowHandle*/) { return false; }
    // Starts an ImGui frame (call before building widgets, before BeginFrame).
    virtual void BeginUIFrame() {}
    // Records the built ImGui draw data into the current frame's commands.
    // Call after DrawScene and before EndFrame.
    virtual void RenderUI() {}
    virtual void ShutdownUI() {}

    // Diagnostics.
    virtual GraphicsAPI GetAPI() const = 0;
    virtual const char* GetAdapterName() const = 0;
};

} // namespace hbe::rhi
