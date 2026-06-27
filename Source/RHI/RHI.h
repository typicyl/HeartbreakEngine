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
};

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
    Format format = Format::R8G8B8A8_UNORM;
    // Pixel data. When mipCount > 1, all mip levels are tightly packed back to
    // back (mip 0 first); mip i has size max(1,width>>i) x max(1,height>>i).
    const void* pixels = nullptr;
    u32 mipCount = 1;
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

// Number of cascaded-shadow-map slices (rendered into a 2x2 atlas).
inline constexpr u32 kMaxShadowCascades = 4;

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
    u32 fogStepCount = 24;         // ray-march samples
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
    // Removed: the automatic 3D surface-stroke painterly renderer (left as an
    // always-off field so old scene files with this key still load). Hand painting
    // (Art Editor) is the painterly path now.
    u32 painterly3D = 0;
};

// Per-frame view + lighting environment.
struct SceneView {
    glm::mat4 viewProj{1.0f};
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

    // HDR post-process stack settings.
    PostSettings post;
};

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
};

// One mesh instance to draw with a metallic-roughness material.
struct DrawItem {
    MeshHandle mesh;
    glm::mat4  transform{1.0f};
    glm::vec4  baseColor{1.0f};
    f32        metallic  = 0.0f;
    f32        roughness = 0.5f;
    // Bindless texture indices (0 = use the constant factor / no map).
    TextureHandle albedoTexture;
    TextureHandle normalTexture;
    TextureHandle mrTexture;   // glTF packing: B = metallic, G = roughness
    TextureHandle aoTexture;
    TextureHandle emissiveTexture;
    glm::vec3  emissiveColor{0.0f};   // linear radiance added after lighting
    f32        emissiveIntensity = 1.0f;
    glm::vec3  subsurfaceColor{1.0f, 0.3f, 0.2f};
    f32        subsurfaceRadius = 1.0f;    // scatter scale (curvature multiplier)
    TextureHandle thicknessTexture;        // back-light transmission thickness (0 = none)
    u32        materialFlags = MaterialFlag_None;

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
};

// One vertex of the in-game UI overlay. Positions are in NDC (the UI system
// applies canvas scaling on the CPU); `texIndex` selects a bindless texture
// (0 = the 1x1 white default, i.e. a solid quad). Straight-alpha blending,
// triangle list.
struct UIVertex {
    f32 x = 0, y = 0;          // NDC
    f32 u = 0, v = 0;          // texture coordinates
    f32 r = 1, g = 1, b = 1, a = 1;
    u32 texIndex = 0;          // bindless texture (0 = white)
};

// One camera-facing particle billboard vertex, in WORLD space (the VS transforms
// by the frame's viewProj). texIndex 0 = a procedural soft round dot.
struct ParticleVertex {
    f32 x = 0, y = 0, z = 0;   // world position
    f32 u = 0, v = 0;          // sprite UV
    f32 r = 1, g = 1, b = 1, a = 1;
    u32 texIndex = 0;          // bindless sprite (0 = soft dot)
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

    // Uploads a CPU mesh to the GPU. Returns an invalid handle if unsupported.
    virtual MeshHandle CreateMesh(const hbe::MeshData&) { return {}; }

    // Re-uploads vertex/index data into an EXISTING mesh's GPU buffers (in place,
    // no reallocation when the data still fits - true for editing where the
    // topology is fixed, e.g. terrain sculpting). Synchronous, like CreateMesh.
    virtual void UpdateMesh(MeshHandle, const hbe::MeshData&) {}

    // Uploads a 2D texture into the bindless array; returns its index handle.
    virtual TextureHandle CreateTexture(const TextureDesc&) { return {}; }

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

    // Records draws for `count` items using the analytic PBR pipeline. Must be
    // called between BeginFrame (after the clear) and EndFrame.
    virtual void DrawScene(const SceneView&, const DrawItem*, u32 /*count*/) {}

    // Draws the in-game UI overlay (alpha-blended 2D triangles in reference-
    // canvas space, no depth) over the scene. Call after DrawScene, before
    // RenderUI/EndFrame. No-op when unsupported.
    virtual void DrawUIOverlay(const UIVertex*, u32 /*count*/) {}

    // -- Editor viewport: render the scene into an offscreen target that the
    //    UI displays in a panel (Unity-style). When the backend reports a
    //    non-zero texture id, the scene renders offscreen and the swapchain
    //    shows only the UI; otherwise the scene renders straight to the screen.
    // Requests the offscreen target be (re)sized; applied at the next BeginFrame.
    virtual void ResizeViewport(u32 /*width*/, u32 /*height*/) {}
    // Backend-specific ImGui texture id for the offscreen color (0 = none).
    virtual u64 GetViewportTextureId() { return 0; }

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
