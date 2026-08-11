// RHI/Vulkan/VulkanDevice.cpp - Vulkan implementation of IRenderDevice.
//
// Full parity with the D3D12 backend: a render-pass-based frame loop with a
// depth buffer and an analytic Cook-Torrance PBR mesh pass. Per-frame constants
// use a uniform buffer; per-draw constants use a dynamic uniform buffer arena
// addressed with dynamic offsets.
#include "RHI/Vulkan/VulkanDevice.h"
#include "RHI/Vulkan/VulkanSurface.h" // OS-abstracted VkSurfaceKHR creation - the ONE Win32 seam
#include "Core/Platform.h"
#include "Assets/Mesh.h"
#include "Assets/StrokeGen.h"
#include "Core/Log.h"

#include <vulkan/vulkan.h> // core only: the platform macro lives in VulkanSurface_Win32.cpp now

#if HBE_EDITOR
// The editor's ImGui PLATFORM backend is still Win32 (ImGui_ImplWin32_*) - a SEPARATE port
// blocker from the swapchain surface, which is now abstracted behind VulkanSurface.h. That
// backend is the ONLY reason this file still pulls in <windows.h>, and it does so ONLY in an
// editor build: the shipped RUNTIME compiles this Vulkan device with no Win32 dependency at all.
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <imgui.h>
#  include <imgui_impl_win32.h>
#  include <imgui_impl_vulkan.h>
#endif

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace hbe::rhi {
namespace {

#define VK_CHECK(expr, what)                                                    \
    do {                                                                        \
        const VkResult _r = (expr);                                            \
        if (_r != VK_SUCCESS) {                                                 \
            HBE_ERROR("[Vulkan] {} failed (VkResult={})", what,                \
                      static_cast<i32>(_r));                                    \
            return false;                                                       \
        }                                                                       \
    } while (0)

VkFormat ToVkFormat(Format f) {
    switch (f) {
        case Format::R8G8B8A8_UNORM:     return VK_FORMAT_R8G8B8A8_UNORM;
        case Format::R8G8B8A8_SRGB:      return VK_FORMAT_R8G8B8A8_SRGB;
        case Format::B8G8R8A8_UNORM:     return VK_FORMAT_B8G8R8A8_UNORM;
        case Format::B8G8R8A8_SRGB:      return VK_FORMAT_B8G8R8A8_SRGB;
        case Format::R16G16B16A16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case Format::R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case Format::D32_FLOAT:          return VK_FORMAT_D32_SFLOAT;
        case Format::D24_UNORM_S8_UINT:  return VK_FORMAT_D24_UNORM_S8_UINT;
        default:                         return VK_FORMAT_UNDEFINED;
    }
}

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        HBE_ERROR("[Vulkan] {}", data->pMessage);
    } else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        HBE_WARN("[Vulkan] {}", data->pMessage);
    }
    return VK_FALSE;
}

VkImageMemoryBarrier ImageBarrier(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                                  VkAccessFlags srcAccess, VkAccessFlags dstAccess) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return b;
}

std::vector<u8> ReadBinaryFile(const std::wstring& path) {
    // A shipped build serves shaders from its packs via the RHI shader provider;
    // fall back to the loose shaders/ folder (editor / unpacked dev runs).
    const auto slash = path.find_last_of(L"\\/");
    const std::wstring leafW = slash == std::wstring::npos ? path : path.substr(slash + 1);
    std::string leaf;
    leaf.reserve(leafW.size());
    for (wchar_t c : leafW) leaf.push_back(static_cast<char>(c)); // shader names are ASCII
    std::vector<u8> provided;
    if (rhi::LoadShaderBytecode(leaf, provided)) return provided;

    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const std::streamsize size = f.tellg();
    f.seekg(0);
    std::vector<u8> data(static_cast<usize>(size));
    f.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

std::wstring ExecutableDir() {
    const std::filesystem::path dir = hbe::platform::ExecutableDir();
    // The trailing separator is part of this function's contract - callers concatenate a
    // shader filename straight onto it.
    return dir.empty() ? std::wstring() : (dir.wstring() + L"\\");
}

constexpr u64 AlignUp(u64 value, u64 alignment) {
    return alignment == 0 ? value : (value + alignment - 1) & ~(alignment - 1);
}

// Uniform buffer layouts (must match Shaders/Common.hlsli, same packing as the
// D3D12 backend's FrameCB/ObjectCB).
struct FrameUBO {
    glm::mat4 viewProj;
    glm::vec3 cameraPos; f32 exposure;
    glm::vec3 lightDir;  f32 lightIntensity;
    glm::vec3 lightColor; f32 ambient;
    u32 irradianceIndex; u32 prefilteredIndex; u32 brdfLUTIndex; f32 prefilteredMaxLod;
    glm::mat4 invViewProj;
    u32 skyIndex; u32 outputLinear; f32 screenTexel[2]; // (1/w, 1/h) for SV_Position->UV
    glm::mat4 cascadeViewProj[kMaxShadowCascades];
    glm::vec4 cascadeSplits;
    u32 shadowMapIndex; u32 cascadeCount; u32 _padF2[2];
    u32 punctualCount; u32 skinLUTIndex; u32 taaActive; u32 _padF3;
    PunctualLight punctualLights[kMaxPunctualLights]; // rhi layout is GPU-ready
    glm::mat4 prevViewProj; // previous frame's view-proj (TAA; D3D12 path uses it)
    glm::vec4 stroke0{0.0f}; // 3D painterly: (sizeWorld, widthFrac, sharpness, flow)
    glm::vec4 stroke1{0.0f}; // (bristle, sizeJitter, angleJitter, distanceFade)
    u32 probeCount = 0; u32 _padPr[3] = {};
    ProbeData probes[kMaxProbes]; // rhi layout is GPU-ready
    glm::vec3 giOrigin{0.0f}; u32 giShIndex = 0;
    glm::vec3 giInvSpacing{0.0f}; u32 giDepthIndex = 0;
    glm::ivec3 giDims{0}; u32 _padGi1 = 0;
    glm::vec4 weather{0.0f};  // x=coverage, y=density, z=overcast, w=time
    glm::vec4 weather1{0.0f}; // xy = wind velocity (cloud-UV/sec)
    glm::vec4 weather2{0.0f}; // x=wetness, y=puddles, z=snow, w=precipIntensity
    glm::vec4 weather3{6.0f, 4.0f, 0.0f, 0.0f}; // x=puddleScale(m), y=snowScale(m)
    u32 decalCount = 0; u32 _padDecal[3] = {};
    DecalData decals[kMaxDecals];
    glm::vec4 waveA[4]{};
    glm::vec4 waveB[4]{};
    glm::vec4 waterShallow{0.10f, 0.30f, 0.42f, 5.0f};
    glm::vec4 waterDeep{0.02f, 0.08f, 0.13f, 0.10f};
    glm::vec4 waterParams{0.5f, 1.0f, 1.0f, 0.0f}; // .w = FFT-on flag
    u32 rippleCount = 0; f32 fftParams[3] = {128.0f, 1.0f, 0.0f}; // (tilePatch m, heightScale, _)
    glm::vec4 ripples[kMaxRipples]{};
    // Depth-based water (see Common.hlsli tail). sceneDepthIndex 0 = skip depth grading.
    u32 sceneDepthIndex = 0; f32 absorptionDepth = 6.0f; f32 shorelineWidth = 1.5f; f32 edgeFade = 0.5f;
};

// Per-pass constants of the post stack (Shaders/PostCommon.hlsli).
struct PostUBO {
    u32 input0 = 0; u32 input1 = 0; u32 input2 = 0; u32 input3 = 0;
    glm::vec2 outTexel{0.0f}; glm::vec2 inTexel{0.0f};
    glm::vec4 params0{0.0f};
    glm::vec4 params1{0.0f};
    glm::vec4 params2{0.0f};
    glm::vec4 params3{0.0f}; // extra pass params (brush-stroke area mask: minX,minY,maxX,maxY)
    // World-anchored painterly censors (3D sphere test in the shader). Each:
    // censors[i] = (worldCenter.xyz, worldRadius); strength/feather packed per
    // component. Filled for the stroke pass + composite; 0 count elsewhere.
    glm::vec4 censors[kMaxCensors]{};
    glm::vec4 censorStrength{0.0f}; // per-censor strength (.x=censor0 ... .w=censor3)
    glm::vec4 censorFeather{0.0f};  // per-censor feather fraction (.x..w)
    glm::uvec4 censorCount{0u};     // .x = active censor count
    // Cinematic color grade (Tonemap pass; appended, must match PostCommon.hlsli).
    glm::vec4 grade0{0.0f};             // (temperature, tint, filmGrain, chromAberration)
    glm::vec4 grade1{0.0f, 0.0f, 0.0f, 0.0f}; // (lift.rgb, gradeEnabled)
    glm::vec4 grade2{1.0f, 1.0f, 1.0f, 0.0f}; // (gamma.rgb, timeSeconds)
    glm::vec4 grade3{1.0f, 1.0f, 1.0f, 0.0f}; // (gain.rgb, -)
};

// One offscreen color target of the post chain (image + view + framebuffer).
struct PostTargetVk {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    u32 width = 0, height = 0;
};

struct ObjectUBO {
    glm::mat4 model;
    glm::mat4 normalMatrix;
    glm::vec4 baseColor;
    f32 metallic; f32 roughness; u32 albedoIndex; u32 normalIndex;
    u32 mrIndex; u32 aoIndex; u32 flags; f32 _pad0;
    glm::vec3 subsurfaceColor; f32 _pad1;
    glm::vec3 emissiveColor; f32 emissiveIntensity;
    u32 emissiveIndex; u32 skinned; u32 boneOffset; u32 boneCount;
    glm::mat4 prevModel;       // previous-frame world matrix (motion vectors)
    u32 prevBoneOffset; u32 thicknessIndex; f32 subsurfaceRadius; f32 _padObj;
    u32 paintColorIndex; u32 paintHeightIndex; f32 paintOpacity; f32 paintHeightScale;
    f32 paintLodBias; f32 paintTexel; u32 paintProjMode; f32 paintBoxInvM;
    glm::vec3 paintBoxCenter; f32 _padBoxC;
    glm::vec3 paintBoxScale; f32 _padBoxS;
    // Terrain splat: 4 layers' albedo/normal/MR bindless indices (must match the
    // gSplat* uint4 block in Common.hlsli). 0 for non-terrain draws.
    glm::uvec4 splatAlbedo{0}; glm::uvec4 splatNormal{0}; glm::uvec4 splatMR{0};
    glm::vec4 splatRough{1.0f}; // 4 layers' roughness factor
    // GPU instancing (matches gInstanced/gInstanceBase in Common.hlsli). Zero-init
    // keeps every single draw byte-compatible with the pre-instancing ABI.
    u32 instanced = 0; u32 instanceBase = 0; u32 _padInst0 = 0; u32 _padInst1 = 0;
    // Facial blendshapes (must match the gMorph* block in Common.hlsli). Zero-init
    // keeps every non-morph draw byte-compatible.
    u32 morphTexIndex = 0; u32 morphCount = 0; u32 _padMorph0 = 0; u32 _padMorph1 = 0;
    glm::uvec4 morphTargets[2] = {glm::uvec4(0), glm::uvec4(0)};
    glm::vec4  morphWeights[2] = {glm::vec4(0.0f), glm::vec4(0.0f)};
    // Clearcoat lobe (wet skin / sweat / wet eyes / varnish). Must match ObjectConstants
    // in Common.hlsli. Zero-init = off.
    f32 clearcoat = 0.0f;
    f32 clearcoatRoughness = 0.08f;
    f32 _padCc0 = 0.0f;
    f32 _padCc1 = 0.0f;
};

// Copies a DrawItem's blendshape fields into the object UBO (bindless atlas index +
// up to 8 active target rows/weights). morphTexIndex 0 = no morphs.
inline void FillMorphUBO(ObjectUBO& ocb, const DrawItem& it) {
    if (it.morphTexture.index == 0u) return; // no morphs (the common case) -> zero-init defaults
    ocb.morphTexIndex = it.morphTexture.index;
    ocb.morphCount = it.morphCount < 8u ? it.morphCount : 8u;
    for (u32 m = 0; m < 8u; ++m) {
        ocb.morphTargets[m >> 2][m & 3] = it.morphTargets[m];
        ocb.morphWeights[m >> 2][m & 3] = it.morphWeights[m];
    }
}

struct GpuTextureVk {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

u32 BytesPerPixel(Format f) {
    switch (f) {
        case Format::R8G8B8A8_UNORM:
        case Format::R8G8B8A8_SRGB:
        case Format::B8G8R8A8_UNORM:
        case Format::B8G8R8A8_SRGB:      return 4;
        case Format::R16G16B16A16_FLOAT: return 8;
        case Format::R32G32B32A32_FLOAT: return 16;
        default:                         return 4;
    }
}

struct GpuMeshVk {
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory indexMemory = VK_NULL_HANDLE;
    u32 indexCount = 0;
    VkDeviceSize vbSize = 0; // allocated sizes (for in-place UpdateMesh)
    VkDeviceSize ibSize = 0;
    // 3D painterly: per-instance brush-stroke seeds over this mesh's surface.
    VkBuffer strokeBuffer = VK_NULL_HANDLE;
    VkDeviceMemory strokeMemory = VK_NULL_HANDLE;
    u32 strokeCount = 0;
};

class VulkanDevice final : public IRenderDevice {
public:
    bool Initialize(const RenderDeviceDesc& desc);

    void BeginFrame() override;
    void ClearBackBuffer(f32 r, f32 g, f32 b, f32 a) override;
    void EndFrame() override;
    void Resize(u32 width, u32 height) override;
    void WaitForGpuIdle() override;

    bool SupportsSceneRendering() const override { return meshPipelineReady_; }
    MeshHandle CreateMesh(const hbe::MeshData& mesh) override;
    MeshHandle CreateMeshReserved(const hbe::MeshData& initial, u32 vertexCapacity,
                                  u32 indexCapacity) override;
    VkDeviceSize reserveVertices_ = 0; // set only during CreateMeshReserved; 0 = exact
    VkDeviceSize reserveIndices_ = 0;
    bool UpdateMesh(MeshHandle handle, const hbe::MeshData& mesh) override;
    TextureHandle CreateTexture(const TextureDesc& desc) override;
    TextureHandle CreateVolumeTexture(const TextureDesc& desc) override;
    void SetVolumeParticles(const VolumeBlob* blobs, u32 count, const VolumeParams& params) override;
    void SetVolumeGrid(const void* bytes, usize byteSize, const VolumeRenderParams& params) override;
    bool SupportsGpuCompute() const override { return true; }
    GpuBufferHandle CreateGpuBuffer(const GpuBufferDesc& desc) override;
    void* MapGpuBuffer(GpuBufferHandle handle) override;
    bool ReadGpuBuffer(GpuBufferHandle handle, void* dst, u32 bytes) override;
    void DestroyGpuBuffer(GpuBufferHandle handle) override;
    ComputePipelineHandle CreateComputePipeline(const ComputePipelineDesc& desc) override;
    void QueueCompute(const ComputeDispatch& d) override;
    void SetVertexShaderBuffer(GpuBufferHandle handle, u32 firstElement) override;
    void UpdateTexture(TextureHandle handle, const TextureDesc& desc) override;
    void DrawShadowPass(const SceneView& view, const DrawItem* items, u32 count) override;
    void SetParticles(const ParticleVertex* alpha, u32 alphaCount,
                      const ParticleVertex* additive, u32 addCount) override;
    void SetGpuParticles(GpuBufferHandle records, const GpuParticleBatch* batches,
                         u32 count) override;
    void DrawScene(const SceneView& view, const DrawItem* items, u32 count) override;
    void DrawUIOverlay(const UIVertex* vertices, u32 count) override;
    TextureHandle CreateUITarget(u32 width, u32 height) override;
    void DrawUIToTexture(TextureHandle target, const UIVertex* vertices, u32 count) override;

    // GPU profiler toggle: MUST live OUTSIDE #if HBE_EDITOR - the per-pass breakdown is a
    // RUNTIME diagnostic, so the override has to exist in the runtime build or the call
    // dispatches to the no-op base default (D3D12 twin has the same note).
    void SetGpuProfileEnabled(bool enable) override {
        gpuProfileRequested_ = enable;
        gpuProfile_ = enable && gpuProfileAvail_;
    }
    bool GpuProfileActive() const override { return gpuProfile_; }

#if HBE_EDITOR
    bool SupportsUI() const override { return true; }
    bool InitUI(void* nativeWindowHandle) override;
    void BeginUIFrame() override;
    void RenderUI() override;
    void ShutdownUI() override;

    void ResizeViewport(u32 width, u32 height) override { pendingVpW_ = width; pendingVpH_ = height; }
    u64 GetViewportTextureId() override {
        return viewportReady_ ? reinterpret_cast<u64>(vpImguiTex_) : 0;
    }
    bool ReadbackViewportColor(std::vector<u8>& outRGBA, u32& w, u32& h) override;
    u64 GetTextureUIHandle(TextureHandle handle) override;

    void ResizePreview(u32 width, u32 height) override {
        if (width > 0 && height > 0) { pendingPrevW_ = width; pendingPrevH_ = height; }
    }
    u64 GetPreviewTextureId() override {
        return previewReady_ ? reinterpret_cast<u64>(prevImguiTex_) : 0;
    }
    void DrawPreviewScene(const SceneView& view, const DrawItem* items, u32 count) override;
#endif

    GraphicsAPI GetAPI() const override { return GraphicsAPI::Vulkan; }
    const char* GetAdapterName() const override { return adapterName_.c_str(); }

    ~VulkanDevice() override;

private:
    bool CreateInstance(bool validation);
    void SetupDebugMessenger();
    bool PickPhysicalDevice();
    bool CreateLogicalDevice();
    bool CreateSwapchain();
    bool CreateSwapchainImageViews();
    bool CreateDepthResources();
    bool CreateRenderPass();
    bool CreateFramebuffers();
    bool CreateSyncAndCommands();
    bool CreateDescriptorResources();
    bool CreateBindlessResources();
    bool CreateMeshPipeline();
    bool CreateShadowResources();
#if HBE_EDITOR
    bool CreateViewportTarget(u32 width, u32 height);
    void DestroyViewportTarget();
#endif
    void DestroySwapchainDependents();
    bool RecreateSwapchain();

    u32 FindMemoryType(u32 typeBits, VkMemoryPropertyFlags props) const;
    bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags props, VkBuffer& buffer, VkDeviceMemory& memory);
    VkShaderModule LoadShaderModule(const std::wstring& path);

    static constexpr u32 kMaxFramesInFlight = 3;
    static constexpr VkDeviceSize kObjectArenaSize = 1u << 22; // 4 MB / frame (~16k draws)

    VkInstance       instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR     surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice         device_ = VK_NULL_HANDLE;
    VkQueue          queue_ = VK_NULL_HANDLE;
    u32              queueFamily_ = 0;
    VkPhysicalDeviceProperties deviceProps_{};

    VkSwapchainKHR   swapchain_ = VK_NULL_HANDLE;
    VkFormat         swapFormat_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkColorSpaceKHR  swapColorSpace_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D       extent_{};
    std::vector<VkImage>       images_;
    std::vector<VkImageView>   imageViews_;
    std::vector<VkFramebuffer> framebuffers_;

    VkImage        depthImage_ = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory_ = VK_NULL_HANDLE;
    VkImageView    depthView_ = VK_NULL_HANDLE;
    VkFormat       depthFormat_ = VK_FORMAT_D32_SFLOAT;

    VkRenderPass   renderPass_ = VK_NULL_HANDLE;

    VkCommandPool    commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer  commandBuffers_[kMaxFramesInFlight]{};
    VkSemaphore      imageAvailable_[kMaxFramesInFlight]{};
    VkFence          inFlight_[kMaxFramesInFlight]{};
    std::vector<VkSemaphore> renderFinished_;

    // Mesh pipeline + descriptors.
    VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool      descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet       descriptorSets_[kMaxFramesInFlight]{};
    VkPipelineLayout      pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline            meshPipeline_ = VK_NULL_HANDLE;       // main MRT pass
    VkPipeline            meshPipelineTransparent_ = VK_NULL_HANDLE; // alpha-blended pass
    VkPipeline            waterPipeline_ = VK_NULL_HANDLE; // Gerstner water surface
    VkPipeline            meshPipelineTransparentDepth_ = VK_NULL_HANDLE; // solid transparent (depth+vel)
    VkPipeline            skyPipeline_ = VK_NULL_HANDLE;        // background pass
    VkPipeline            meshPipelineSingle_ = VK_NULL_HANDLE; // editor preview (single color)
    VkPipeline            skyPipelineSingle_ = VK_NULL_HANDLE;  // editor preview sky
    VkPipeline            strokeSurfacePipe_ = VK_NULL_HANDLE;  // 3D painterly surface strokes
    bool                  strokeSurfaceReady_ = false;

    // -- In-game UI overlay (alpha-blended textured 2D triangles) -------------
    VkPipeline uiPipeline_ = VK_NULL_HANDLE; // uses pipelineLayout_ (bindless set 1)
    static constexpr u64 kUIVertexBufferSize = 2u << 20; // 2 MB/frame (~40k verts @ 52 B)
    VkBuffer       uiVertexBuffers_[kMaxFramesInFlight]{};
    VkDeviceMemory uiVertexMemory_[kMaxFramesInFlight]{};
    u8*            uiVertexCpu_[kMaxFramesInFlight] = {};
    bool                  meshPipelineReady_ = false;

    // -- World-space UI (canvas -> texture -> lit quad in the scene) ----------
    // Same UI pipeline against a small R8G8B8A8_UNORM offscreen pass whose
    // finalLayout hands the image to the fragment shader (the render pass does
    // ALL layout transitions - the vpRenderPass_ precedent). Images are MUTABLE:
    // UNORM attachment view (raw UI output) + SRGB sampled view (albedo decode).
    VkRenderPass worldUIRenderPass_ = VK_NULL_HANDLE;
    VkPipeline uiWorldPipeline_ = VK_NULL_HANDLE;
    static constexpr u32 kMaxUITargets = 8;
    struct UITargetVk {
        VkImage img = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkImageView attachView = VK_NULL_HANDLE; // UNORM (framebuffer)
        VkImageView sampleView = VK_NULL_HANDLE; // SRGB (bindless)
        VkFramebuffer fb = VK_NULL_HANDLE;
        u32 w = 0, h = 0;
    };
    std::unordered_map<u32, UITargetVk> uiTargets_; // key = bindless slot
    // Own per-frame vertex buffers with a bump head (the overlay's are memcpy'd
    // at offset 0 per call and would alias; several canvases draw per frame).
    VkBuffer       uiWorldVertexBuffers_[kMaxFramesInFlight]{};
    VkDeviceMemory uiWorldVertexMemory_[kMaxFramesInFlight]{};
    u8*            uiWorldVertexCpu_[kMaxFramesInFlight] = {};
    u64            uiWorldVertexHead_ = 0; // reset in BeginFrame

    // -- Particle billboards (world-space, drawn in the HDR scene pass) -------
    VkPipeline particlePipeline_ = VK_NULL_HANDLE;    // alpha blend
    VkPipeline particlePipelineAdd_ = VK_NULL_HANDLE; // additive blend
    static constexpr u64 kParticleVertexBufferSize = 6u << 20; // 6 MB/frame
    VkBuffer       particleVertexBuffers_[kMaxFramesInFlight]{};
    VkDeviceMemory particleVertexMemory_[kMaxFramesInFlight]{};
    u8*            particleVertexCpu_[kMaxFramesInFlight] = {};
    const ParticleVertex* particleAlpha_ = nullptr;
    const ParticleVertex* particleAdd_ = nullptr;
    u32 particleAlphaCount_ = 0;
    u32 particleAddCount_ = 0;
    // GPU vertex expansion: no vertex input state at all - the VS builds the quad
    // from gl_VertexIndex out of the record buffer bound on set 2
    // (Shaders/ParticleGpu.hlsl). D3D12 twin: particleGpuPSO_ / particleGpuPSOAdd_.
    VkPipeline particleGpuPipeline_ = VK_NULL_HANDLE;    // alpha blend
    VkPipeline particleGpuPipelineAdd_ = VK_NULL_HANDLE; // additive blend
    // One group per record BUFFER (SetGpuParticles accumulates): the CpuWrite ring
    // the CPU-simulated emitters upload into, and the device-local buffer the GPU
    // simulation's compute pass writes. A batch carries only an element offset, so
    // the buffer identity has to live here. D3D12 twin: the same array.
    struct GpuParticleGroup {
        GpuBufferHandle buffer;
        const GpuParticleBatch* batches = nullptr;
        u32 count = 0;
    };
    GpuParticleGroup particleGpuGroups_[kMaxGpuParticleGroups]{};
    u32 particleGpuGroupCount_ = 0;
    bool particleGpuAlignWarned_ = false;
    void DrawGpuParticleBatches(VkCommandBuffer cmd, bool additive);
    // The groups have a ONE-FRAME lifetime: `batches` points into a vector the engine
    // rebuilds every frame, so a group that survives a frame is a dangling pointer
    // with a stale count. DrawScene has early returns, so the clear cannot live only
    // at its end. D3D12's twin is the same method at the same call sites.
    void ClearGpuParticleGroups() {
        for (u32 g = 0; g < particleGpuGroupCount_; ++g) particleGpuGroups_[g] = {};
        particleGpuGroupCount_ = 0;
    }

    VkBuffer       frameUBO_[kMaxFramesInFlight]{};
    VkDeviceMemory frameUBOMem_[kMaxFramesInFlight]{};
    void*          frameUBOMapped_[kMaxFramesInFlight]{};
    VkBuffer       objectArena_[kMaxFramesInFlight]{};
    VkDeviceMemory objectArenaMem_[kMaxFramesInFlight]{};
    u8*            objectArenaMapped_[kMaxFramesInFlight]{};
    VkDeviceSize   objectStride_ = 0;

    // -- Skinning: per-frame joint-palette arena (set 0 binding 3, storage) --
    static constexpr VkDeviceSize kBoneArenaSize = 1u << 22; // 4 MB
    VkBuffer       boneArena_[kMaxFramesInFlight]{};
    VkDeviceMemory boneArenaMem_[kMaxFramesInFlight]{};
    u8*            boneArenaMapped_[kMaxFramesInFlight]{};
    u64            boneHead_ = 0; // reset each frame
    // Copies a palette into this frame's arena; returns the element offset
    // (in matrices) for ObjectUBO::boneOffset, or UINT32_MAX when full.
    u32 AllocBones(const glm::mat4* mats, u32 count) {
        const u64 bytes = static_cast<u64>(count) * sizeof(glm::mat4);
        if (boneHead_ + bytes > kBoneArenaSize) return UINT32_MAX;
        std::memcpy(boneArenaMapped_[frameIndex_] + boneHead_, mats, bytes);
        const u32 offset = static_cast<u32>(boneHead_ / sizeof(glm::mat4));
        boneHead_ += bytes;
        return offset;
    }

    // -- GPU instancing: per-frame instance-transform arena (binding 4) -------
    // 3 matrices per instance (model, normalMatrix, prevModel); each pass appends
    // its runs independently (shadow first, then scene - no cross-pass coupling).
    static constexpr VkDeviceSize kInstanceArenaSize = 1u << 22; // 4 MB (~21k instances)
    VkBuffer       instanceArena_[kMaxFramesInFlight]{};
    VkDeviceMemory instanceArenaMem_[kMaxFramesInFlight]{};
    u8*            instanceArenaMapped_[kMaxFramesInFlight]{};
    u64            instanceHead_ = 0; // reset each frame
    std::vector<u32> shadowInstanceCounts_; // scratch: instances per run head
    // Reserves `count` instances; returns a write pointer + the base INSTANCE
    // index for ObjectUBO::instanceBase, or null when the arena is full.
    glm::mat4* AllocInstances(u32 count, u32& outBase) {
        const u64 bytes = static_cast<u64>(count) * 3u * sizeof(glm::mat4);
        if (instanceHead_ + bytes > kInstanceArenaSize) {
            outBase = 0;
            return nullptr;
        }
        outBase = static_cast<u32>(instanceHead_ / (3u * sizeof(glm::mat4)));
        glm::mat4* dst =
            reinterpret_cast<glm::mat4*>(instanceArenaMapped_[frameIndex_] + instanceHead_);
        instanceHead_ += bytes;
        return dst;
    }

    // -- Cascaded shadow maps (depth-only pass into a 2x2 atlas) -------------
    static constexpr u32 kShadowDim = 4096;     // atlas; one cascade per 2048 tile
    static constexpr u32 kShadowTileDim = kShadowDim / 2;
    VkImage        shadowImage_ = VK_NULL_HANDLE;
    VkDeviceMemory shadowMemory_ = VK_NULL_HANDLE;
    VkImageView    shadowView_ = VK_NULL_HANDLE;
    VkRenderPass   shadowRenderPass_ = VK_NULL_HANDLE;
    VkFramebuffer  shadowFramebuffer_ = VK_NULL_HANDLE;
    VkPipeline     shadowPipeline_ = VK_NULL_HANDLE;
    u32            shadowSrvSlot_ = 0; // bindless slot the PBR pass samples
    // The shadow pass needs per-cascade frame UBOs (light matrices): one
    // buffer per frame holding kMaxShadowCascades aligned FrameUBO regions,
    // with one set-0 clone per (frame, cascade) pointing at its region.
    VkBuffer       shadowFrameUBO_[kMaxFramesInFlight]{};
    VkDeviceMemory shadowFrameUBOMem_[kMaxFramesInFlight]{};
    u8*            shadowFrameUBOMapped_[kMaxFramesInFlight]{};
    VkDeviceSize   shadowFrameStride_ = 0;
    VkDescriptorSet shadowDescriptorSets_[kMaxFramesInFlight][kMaxShadowCascades]{};
    bool           shadowReady_ = false;
    bool           shadowPassRun_ = false; // shadow map rendered this frame

    // -- HDR pipeline + post-process stack ------------------------------------
    // Scene renders into an RGBA16F target with a sampleable depth, then:
    // SSAO (+blur) -> bloom down/up pyramid -> tonemap -> FXAA -> final target
    // (editor viewport texture or the swapchain). Post passes are fullscreen
    // triangles reading inputs through the bindless table.
    static constexpr u32 kBloomMaxMips = 6;
    bool CreatePostRenderPasses();
    bool CreatePostPipelines();
    bool CreatePostTargets(u32 width, u32 height);
    void DestroyPostTargets();
    bool CreatePostTarget(u32 w, u32 h, VkFormat fmt, VkRenderPass pass, u32 srvSlot,
                          PostTargetVk& out);
    void RunPostStack(const SceneView& view);
    // One fullscreen pass into `target` (render pass chosen by the caller).
    void DrawPostPass(VkPipeline pipe, VkRenderPass pass, const PostTargetVk& target,
                      const PostUBO& cb);
    u32  AllocPostConstants(const PostUBO& cb); // returns dynamic offset

    // --- GPU pass profiler (timestamp queries) ------------------------------
    // Writes a GPU timestamp at the current point in the command buffer and
    // labels it; consecutive marks' delta = that pass's GPU time. Results are
    // read back a few frames later (when the slot's fence is signalled) and
    // logged every ~2s so the per-pass cost is visible without an external tool.
    void GpuMark(const char* name);
    static constexpr u32 kMaxGpuMarks = 40;
    VkQueryPool gpuPool_[kMaxFramesInFlight]{};
    const char* gpuNames_[kMaxFramesInFlight][kMaxGpuMarks]{};
    u32  gpuCount_ = 0;                       // marks written into the current frame
    u32  gpuCountSlot_[kMaxFramesInFlight]{}; // marks pending in each slot
    bool gpuValid_[kMaxFramesInFlight]{};
    f64  gpuPeriodNs_ = 0.0;                  // ns per timestamp tick (0 = unsupported)
    u32  gpuFrameCounter_ = 0;
    bool gpuProfile_ = false;          // ACTIVE this run (marks written + read back)
    bool gpuProfileAvail_ = false;     // pool created (GPU supports timestamps)
    bool gpuProfileRequested_ = false; // runtime toggle target (SetGpuProfileEnabled)

    bool postPipelinesReady_ = false; // render passes + pipelines + shaders
    bool postReady_ = false;          // targets sized and usable this frame
    u32  sceneW_ = 0, sceneH_ = 0;    // size the post targets were built for

    VkRenderPass hdrRenderPass_ = VK_NULL_HANDLE;     // color+gbuffer+velocity+D32, sampled after
    // Depth-graded water reopens the HDR targets with the depth attachment READ-ONLY, so the
    // water PS can SAMPLE scene depth (via slotDepthRO_) while still depth-TESTING against it -
    // the Vulkan twin of D3D12's read-only DSV. LOAD variant; leaves everything SHADER_READ_ONLY.
    VkRenderPass hdrWaterRenderPass_ = VK_NULL_HANDLE;
    VkRenderPass previewRenderPass_ = VK_NULL_HANDLE; // single color + D32 (editor preview)
    VkRenderPass postPass16_ = VK_NULL_HANDLE;        // RGBA16F, discard -> sampled
    VkRenderPass postPass16Load_ = VK_NULL_HANDLE;    // RGBA16F, load (additive) -> sampled
    VkRenderPass postPass8_ = VK_NULL_HANDLE;         // RGBA8, discard -> sampled

    PostTargetVk hdr_;          // + own depth below
    PostTargetVk gbuffer_;      // RGBA16F: octN.rg, rough.b, metal.a (no framebuffer)
    PostTargetVk velocity_;     // RG16F: screen motion vectors (no framebuffer)
    VkImage hdrDepth_ = VK_NULL_HANDLE;
    VkDeviceMemory hdrDepthMem_ = VK_NULL_HANDLE;
    VkImageView hdrDepthView_ = VK_NULL_HANDLE;
    PostTargetVk ssaoRaw_, ssaoBlur_; // half-res RGBA8
    PostTargetVk bloom_[kBloomMaxMips]; // framebuffers work with both 16F passes
    PostTargetVk ldr_;          // tonemapped RGBA8, pre-FXAA
    PostTargetVk taaHistory_[2]; // TAA accumulation (ping-pong)
    PostTargetVk dof_;           // depth-of-field result (LDR)
    PostTargetVk motionBlur_;    // motion blur result (LDR)
    PostTargetVk ssr_;           // screen-space reflections (HDR)
    PostTargetVk ssgi_;          // screen-space GI composite (HDR, full-res)
    PostTargetVk ssgiHalf_;      // SSGI GI-only term at reduced res (upscaled into ssgi_)
    PostTargetVk painterly_;     // painterly oil-on-canvas repaint (HDR, full-res)
    PostTargetVk painterlyHalf_; // Kuwahara underpainting at HALF-res (upscaled to painterly_)
    PostTargetVk painterlyComp_; // painterly + dynamic-layer crisp objects composited back
    PostTargetVk vol_;           // volumetric fog composite (HDR, full-res)
    PostTargetVk volHalf_;       // fog (inscatter+transmittance) at reduced res (upscaled into vol_)
    PostTargetVk volPart_;       // volumetric-particles composite (HDR, full-res)
    PostTargetVk volPartHalf_;   // volumetric-particles raymarch at reduced res
    PostTargetVk adaptedLum_[2]; // auto-exposure adapted luminance (1x1, ping-pong)
    u32 bloomCount_ = 0;

    u32 slotHdr_ = 0, slotDepth_ = 0, slotSsaoRaw_ = 0, slotSsaoBlur_ = 0, slotLdr_ = 0;
    u32 slotDepthRO_ = 0; // 2nd depth SRV declared DEPTH_STENCIL_READ_ONLY_OPTIMAL (water pass)
    u32 slotGbuffer_ = 0, slotVelocity_ = 0;
    u32 slotTaaHistory_[2] = {};
    u32 slotDof_ = 0;
    u32 slotMotionBlur_ = 0;
    u32 slotSsr_ = 0;
    u32 slotSsgi_ = 0;
    u32 slotSsgiHalf_ = 0;
    u32 slotPainterly_ = 0;
    u32 slotPainterlyHalf_ = 0;
    u32 slotPainterlyComp_ = 0;
    u32 slotVol_ = 0;
    u32 slotVolHalf_ = 0;
    u32 slotVolPart_ = 0;
    u32 slotVolPartHalf_ = 0;
    u32 slotAdaptedLum_[2] = {};
    u32 slotBloom_[kBloomMaxMips] = {};

    VkPipeline ssaoPipe_ = VK_NULL_HANDLE, ssaoBlurPipe_ = VK_NULL_HANDLE,
               bloomDownPipe_ = VK_NULL_HANDLE, bloomUpPipe_ = VK_NULL_HANDLE,
               tonemapPipe_ = VK_NULL_HANDLE, fxaaPipe_ = VK_NULL_HANDLE,
               taaPipe_ = VK_NULL_HANDLE, dofPipe_ = VK_NULL_HANDLE,
               motionBlurPipe_ = VK_NULL_HANDLE, ssrPipe_ = VK_NULL_HANDLE,
               exposurePipe_ = VK_NULL_HANDLE, volPipe_ = VK_NULL_HANDLE,
               ssgiPipe_ = VK_NULL_HANDLE, painterlyPipe_ = VK_NULL_HANDLE,
               brushStrokesPipe_ = VK_NULL_HANDLE,
               compositePipe_ = VK_NULL_HANDLE, applyPipe_ = VK_NULL_HANDLE,
               volPartPipe_ = VK_NULL_HANDLE,     // volumetric-particles raymarch (LEGACY splat)
               volRaymarchPipe_ = VK_NULL_HANDLE; // NanoVDB baked-volume raymarch (runtime)

    // Temporal AA: jittered camera + reprojected history accumulation (mirrors D3D12).
    bool taaReady_ = false;
    u32  taaHistoryIndex_ = 0;
    bool taaHistoryValid_ = false;
    u64  taaFrame_ = 0;
    glm::mat4 taaPrevViewProj_{1.0f};
    bool dofReady_ = false; // DoF pipeline built (optional; absent = no DoF)
    bool motionBlurReady_ = false;
    bool ssrReady_ = false;
    bool exposureReady_ = false;
    bool volReady_ = false;  // volumetric fog pipeline built
    bool volPartReady_ = false; // volumetric-particles raymarch pipeline built (LEGACY splat)
    bool volRaymarchReady_ = false; // NanoVDB baked-volume raymarch pipeline built (runtime)
    bool ssgiReady_ = false; // screen-space GI pipeline built
    bool painterlyReady_ = false; // painterly repaint pipeline built
    bool brushStrokesReady_ = false; // brush-stroke splat pipeline built
    u32  adaptIndex_ = 0;     // adapted-luminance target written this frame
    bool adaptValid_ = false; // a prior adapted value exists (reset on resize)

    // Per-pass constants: a small dynamic-offset arena + set-0 clones.
    static constexpr VkDeviceSize kPostArenaSize = 1u << 14; // 16 KB (~64 passes)
    VkBuffer       postArena_[kMaxFramesInFlight]{};
    VkDeviceMemory postArenaMem_[kMaxFramesInFlight]{};
    u8*            postArenaMapped_[kMaxFramesInFlight]{};
    VkDeviceSize   postStride_ = 0;
    u32            postHead_ = 0; // reset each frame
    VkDescriptorSet postSets_[kMaxFramesInFlight]{};

    VkSampler clampSampler_ = VK_NULL_HANDLE; // immutable, set 0 binding 2

    std::vector<GpuMeshVk> meshes_;

    // -- Bindless textures (set 1) ------------------------------------------
    static constexpr u32 kMaxBindlessTextures = 4096;
    VkSampler             bindlessSampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout bindlessLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool      bindlessPool_ = VK_NULL_HANDLE;
    VkDescriptorSet       bindlessSet_ = VK_NULL_HANDLE;
    u32                   bindlessNextSlot_ = 0;
    std::vector<GpuTextureVk> textures_;
    // Per-bindless-slot view so editor thumbnails can re-bind a texture.
    std::unordered_map<u32, VkImageView> slotViews_;
    std::unordered_map<u32, VkImage> slotImages_; // slot -> image (in-place UpdateTexture)
    // What each slot's image was actually CREATED as. UpdateTexture must validate the
    // caller's desc against this: without it, the staging buffer is sized from the DESC and
    // memcpy'd from the caller's pointer, so a mismatched desc both copies out of bounds
    // into the image AND reads out of bounds from caller memory. D3D12 already rejects a
    // mismatch outright (it can ask the resource via GetDesc); Vulkan has no such query, so
    // the dimensions have to be remembered here.
    struct SlotImageInfo {
        u32 width = 0, height = 0, mipCount = 1;
        Format format = Format::R8G8B8A8_UNORM;
    };
    std::unordered_map<u32, SlotImageInfo> slotImageInfo_;
    // Volume textures (CreateVolumeTexture): SRV slot -> STORAGE_IMAGE view the
    // compute splat binds for writing (VV2). Kept in GENERAL layout so the same
    // image serves both the storage write and the sampled raymarch read.
    std::unordered_map<u32, VkImageView> volumeStorageView_;
    std::unordered_map<u32, u64> uiTextureIds_; // bindless slot -> ImGui id

    // -- Volumetric VFX compute (VV2) -------------------------------------------
    // Compute pipeline that splats a 3D density/temperature volume. Lazily built
    // the first enabled frame (off by default -> zero cost on low-end GPUs).
    // Dispatched in BeginFrame (must be OUTSIDE any render pass). Enable is
    // HBE_VOLTEST env scaffolding for now (VV5 wires the real per-emitter enable).
    u32 volDim_ = 96;           // volume voxel dim (from VolumeParams.resolution, first enable)
    bool volInit_ = false, volFailed_ = false;
    TextureHandle volTex_;
    VkImage        volImage_ = VK_NULL_HANDLE;
    VkImageView    volStorageView_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout volSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout      volPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline            volSplatPipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool      volDescPool_ = VK_NULL_HANDLE;
    // Per-frame-in-flight so the CPU can refill them without racing the GPU (the
    // slot's fence is waited on before reuse). The volume image + storage view are
    // shared (one froxel volume).
    VkDescriptorSet volSet_[kMaxFramesInFlight]{};
    VkBuffer        volParamsBuf_[kMaxFramesInFlight]{};
    VkDeviceMemory  volParamsMem_[kMaxFramesInFlight]{};
    void*           volParamsMapped_[kMaxFramesInFlight]{};
    VkBuffer        volBlobBuf_[kMaxFramesInFlight]{};
    VkDeviceMemory  volBlobMem_[kMaxFramesInFlight]{};
    void*           volBlobMapped_[kMaxFramesInFlight]{};
    const VolumeBlob* volBlobs_ = nullptr;
    u32 volBlobCount_ = 0;
    VolumeParams volParams_{};
    bool EnsureVolumeResources();
    void DispatchVolumeSplat();
    // NanoVDB baked-volume raymarch (RUNTIME volume path). Per-frame host-visible SSBO holding the
    // raw NanoVDB blob (grow-on-demand); the PNanoVDB raymarch samples it via post-set binding 6.
    VkBuffer        volGridBuf_[kMaxFramesInFlight]{};
    VkDeviceMemory  volGridMem_[kMaxFramesInFlight]{};
    void*           volGridMapped_[kMaxFramesInFlight]{};
    VkDeviceSize    volGridCapacity_[kMaxFramesInFlight]{};
    const void*     volGridBytes_ = nullptr;
    usize           volGridSize_ = 0;
    VolumeRenderParams volRenderParams_{};
    bool EnsureVolumeGridBuffer(usize size);

    // --- General GPU compute + GPU-writable structured buffers ----------------
    // The generalisation of the volumetric path above. D3D12 twin:
    // GpuBufferD3D12 / ComputePipelineD3D12 / ExecuteQueuedCompute in
    // D3D12Device.cpp - same members, same call points, same frame position.
    struct GpuBufferVk {
        // slots == 1 for a device-local buffer; framesInFlight_ for CpuWrite
        // (per-frame ring, so the CPU can refill without racing the GPU).
        VkBuffer       buf[kMaxFramesInFlight]{};
        VkDeviceMemory mem[kMaxFramesInFlight]{};
        u8*            cpu[kMaxFramesInFlight]{};
        // Set 2 (binding 0, STORAGE_BUFFER_DYNAMIC) so SetVertexShaderBuffer can
        // apply a per-batch element base as a dynamic offset - the exact twin of
        // D3D12 offsetting a root SRV's GPU virtual address.
        VkDescriptorSet vsSet[kMaxFramesInFlight]{};
        u32 slots = 1;
        u32 stride = 0;
        u32 count = 0;
        u32 usage = 0;
        VkDeviceSize bytes = 0;      // logical size (elementCount * stride)
        VkDeviceSize allocBytes = 0; // bytes + the bind window's padding
        u32 maxBindElements = 0;     // 0 = only ever bound at offset 0
        bool alive = false;
    };
    struct ComputePipelineVk {
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout      layout = VK_NULL_HANDLE;
        VkPipeline            pipeline = VK_NULL_HANDLE;
        VkDescriptorPool      pool = VK_NULL_HANDLE;
        // One set per (frame slot, queue slot): a set is written at dispatch
        // record time, so re-using one within a frame would race itself.
        VkDescriptorSet sets[kMaxFramesInFlight][kMaxQueuedComputeDispatches]{};
        VkBuffer        cb[kMaxFramesInFlight]{};
        VkDeviceMemory  cbMem[kMaxFramesInFlight]{};
        u8*             cbMapped[kMaxFramesInFlight]{};
        VkDeviceSize    cbStride = 0;
        u32 constantBytes = 0;
        u32 uavCount = 0;
        u32 srvCount = 0;
        bool alive = false;
    };
    struct QueuedComputeVk {
        ComputeDispatch d;
        u8 constants[kMaxComputeConstantBytes] = {};
    };
    std::vector<GpuBufferVk>       gpuBuffers_;    // handle.id - 1
    std::vector<u32>               gpuBufferFree_; // recycled indices
    std::vector<ComputePipelineVk> computePipes_;  // handle.id - 1
    QueuedComputeVk computeQueue_[kMaxQueuedComputeDispatches];
    u32 computeQueueCount_ = 0;
    // Set 2 of pipelineLayout_ (the general VS-visible structured buffer).
    static constexpr u32 kMaxVsBufferSets = 64;
    VkDescriptorSetLayout vsBufferLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool      vsBufferPool_ = VK_NULL_HANDLE;
    GpuBufferHandle vsBuffer_{};
    u32 vsBufferFirstElement_ = 0;
    // Never-read placeholder bound to set 2 when no real vsBuffer_ is set: the water pipeline
    // statically uses set 2 (Water.hlsl gOceanDisp), so it must be bound for EVERY water draw
    // even on the Gerstner path (VUID-vkCmdDraw-None-08600). Created lazily.
    GpuBufferHandle dummyVsBuffer_{};
    bool vsBufferAlignWarned_ = false;
    GpuBufferVk* ResolveGpuBuffer(GpuBufferHandle h);
    bool CreateVsBufferSetLayout();
    void ExecuteQueuedCompute();

    // -- Editor viewport (offscreen scene target) ---------------------------
    VkImage        vpColor_ = VK_NULL_HANDLE;
    VkDeviceMemory vpColorMem_ = VK_NULL_HANDLE;
    VkImageView    vpColorView_ = VK_NULL_HANDLE;
    VkImage        vpDepth_ = VK_NULL_HANDLE;
    VkDeviceMemory vpDepthMem_ = VK_NULL_HANDLE;
    VkImageView    vpDepthView_ = VK_NULL_HANDLE;
    VkRenderPass   vpRenderPass_ = VK_NULL_HANDLE;
    VkFramebuffer  vpFramebuffer_ = VK_NULL_HANDLE;
    VkDescriptorSet vpImguiTex_ = VK_NULL_HANDLE;
    u32 vpW_ = 0, vpH_ = 0, pendingVpW_ = 0, pendingVpH_ = 0;
    bool viewportReady_ = false;
    // Reused host-visible staging buffer for ReadbackViewportColor (movie render).
    VkBuffer       readbackBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory readbackMem_ = VK_NULL_HANDLE;
    VkDeviceSize   readbackSize_ = 0;

    // -- Editor asset preview (independent mini-scene: HDR pass + tonemap) ---
#if HBE_EDITOR
    bool CreatePreviewTargets(u32 width, u32 height);
    void DestroyPreviewTargets();
#endif
    PostTargetVk prevHdr_;       // framebuffer pairs with prevDepth below
    VkImage prevDepth_ = VK_NULL_HANDLE;
    VkDeviceMemory prevDepthMem_ = VK_NULL_HANDLE;
    VkImageView prevDepthView_ = VK_NULL_HANDLE;
    PostTargetVk prevLdr_;
    VkDescriptorSet prevImguiTex_ = VK_NULL_HANDLE;
    u32 slotPrevHdr_ = 0;
    u32 prevW_ = 0, prevH_ = 0, pendingPrevW_ = 0, pendingPrevH_ = 0;
    bool previewReady_ = false;
    // The preview needs its own frame UBO (its camera differs from the main
    // scene's, and the shared frameUBO_'s final bytes win at execution time).
    VkBuffer       previewFrameUBO_[kMaxFramesInFlight]{};
    VkDeviceMemory previewFrameUBOMem_[kMaxFramesInFlight]{};
    void*          previewFrameUBOMapped_[kMaxFramesInFlight]{};
    VkDescriptorSet previewSets_[kMaxFramesInFlight]{};

    bool uiInitialized_ = false;

    u32  framesInFlight_ = 2;
    u32  frameIndex_ = 0;
    u32  imageIndex_ = 0;
    u32  desiredImageCount_ = 3;
    u32  width_ = 0;
    u32  height_ = 0;
    bool validation_ = false;
    bool vsync_ = true; // FIFO vs MAILBOX/IMMEDIATE (uncapped) present mode
    bool frameActive_ = false;
    bool renderPassActive_ = false;
    VkClearColorValue clearColor_{{0, 0, 0, 1}};
    // The native window handles kept OPAQUE (HWND/HINSTANCE on Windows): stored as void* and
    // handed to vk_surface::CreateWindowSurface, so no Win32 type appears in this device. Only
    // used to create the surface at init.
    void* windowHandle_ = nullptr;
    void* windowInstance_ = nullptr;

    std::string adapterName_ = "Unknown Vulkan Device";

    PFN_vkCreateDebugUtilsMessengerEXT  pfnCreateDebugMessenger_ = nullptr;
    PFN_vkDestroyDebugUtilsMessengerEXT pfnDestroyDebugMessenger_ = nullptr;
};

bool VulkanDevice::Initialize(const RenderDeviceDesc& desc) {
    windowHandle_ = desc.windowHandle;      // opaque; the surface backend casts + resolves the
    windowInstance_ = desc.windowInstance;  // module handle (nullptr -> current module) itself
    width_ = desc.width;
    height_ = desc.height;
    validation_ = desc.enableValidation;
    vsync_ = desc.vsync;
    desiredImageCount_ = std::clamp<u32>(desc.backBufferCount, 2, kMaxFramesInFlight);
    framesInFlight_ = desiredImageCount_;
    // SAY SO WHEN THE REQUEST IS REDUCED. The two backends cap at different numbers
    // (D3D12 at 4, Vulkan at kMaxFramesInFlight), so the same RenderDeviceDesc can produce
    // a different amount of CPU/GPU overlap on each - which shows up as a frame-pacing
    // difference nobody can account for. Clamping is fine; clamping silently is not.
    if (desc.backBufferCount != desiredImageCount_) {
        HBE_WARN("[Vulkan] backBufferCount {} clamped to {} (this backend allows 2..{}).",
                 desc.backBufferCount, desiredImageCount_, kMaxFramesInFlight);
    }
    swapFormat_ = ToVkFormat(desc.backBufferFormat);

    if (!CreateInstance(validation_)) return false;
    if (validation_) SetupDebugMessenger();

    VK_CHECK(vk_surface::CreateWindowSurface(instance_, windowHandle_, windowInstance_, &surface_),
             "vk_surface::CreateWindowSurface");

    if (!PickPhysicalDevice()) return false;
    if (!CreateLogicalDevice()) return false;
    if (!CreateSwapchain()) return false;
    if (!CreateSwapchainImageViews()) return false;
    if (!CreateDepthResources()) return false;
    if (!CreateRenderPass()) return false;
    if (!CreateFramebuffers()) return false;
    if (!CreateSyncAndCommands()) return false;

    // Scene-rendering resources are optional; failure leaves a clear-only device.
    if (CreateDescriptorResources() && CreateBindlessResources() && CreateMeshPipeline()) {
        meshPipelineReady_ = true;
        if (!CreateShadowResources()) {
            HBE_WARN("[Vulkan] Shadow resources unavailable; shadows disabled.");
        }
    } else {
        HBE_WARN("[Vulkan] Mesh pipeline unavailable; scene rendering disabled.");
    }

    HBE_INFO("[Vulkan] Device initialized ({} images, {}x{}, scene={})",
             static_cast<u32>(images_.size()), extent_.width, extent_.height,
             meshPipelineReady_ ? "on" : "off");
    return true;
}

bool VulkanDevice::CreateInstance(bool validation) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Heartbreak Engine";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "Heartbreak Engine";
    app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app.apiVersion = VK_API_VERSION_1_2;

    // The surface extensions are whatever THIS platform's surface needs (VK_KHR_surface + the
    // OS-specific one); the backend owns that list so this file names no Win32 extension.
    std::vector<const char*> extensions = vk_surface::RequiredInstanceExtensions();
    std::vector<const char*> layers;
    if (validation) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        layers.push_back("VK_LAYER_KHRONOS_validation");
    }

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = static_cast<u32>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    ci.enabledLayerCount = static_cast<u32>(layers.size());
    ci.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();

    VkResult r = vkCreateInstance(&ci, nullptr, &instance_);
    if (r == VK_ERROR_LAYER_NOT_PRESENT && validation) {
        HBE_WARN("[Vulkan] Validation layer unavailable; retrying without it.");
        ci.enabledLayerCount = 0;
        ci.ppEnabledLayerNames = nullptr;
        extensions.pop_back();
        ci.enabledExtensionCount = static_cast<u32>(extensions.size());
        ci.ppEnabledExtensionNames = extensions.data();
        validation_ = false;
        r = vkCreateInstance(&ci, nullptr, &instance_);
    }
    VK_CHECK(r, "vkCreateInstance");
    return true;
}

void VulkanDevice::SetupDebugMessenger() {
    pfnCreateDebugMessenger_ = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
    pfnDestroyDebugMessenger_ = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
    if (!pfnCreateDebugMessenger_) return;

    VkDebugUtilsMessengerCreateInfoEXT ci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    ci.pfnUserCallback = DebugCallback;
    pfnCreateDebugMessenger_(instance_, &ci, nullptr, &debugMessenger_);
    HBE_INFO("[Vulkan] Debug messenger enabled");
}

bool VulkanDevice::PickPhysicalDevice() {
    u32 count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) {
        HBE_ERROR("[Vulkan] No Vulkan physical devices found.");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    auto findQueueFamily = [&](VkPhysicalDevice pd, u32& outFamily) -> bool {
        u32 qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> props(qcount);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, props.data());
        for (u32 i = 0; i < qcount; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface_, &present);
            if ((props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                outFamily = i;
                return true;
            }
        }
        return false;
    };

    VkPhysicalDevice fallback = VK_NULL_HANDLE;
    u32 fallbackFamily = 0;
    for (VkPhysicalDevice pd : devices) {
        u32 family = 0;
        if (!findQueueFamily(pd, family)) continue;
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(pd, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            physical_ = pd;
            queueFamily_ = family;
            deviceProps_ = props;
            adapterName_ = props.deviceName;
            break;
        }
        if (!fallback) {
            fallback = pd;
            fallbackFamily = family;
            deviceProps_ = props;
            adapterName_ = props.deviceName;
        }
    }
    if (!physical_) {
        physical_ = fallback;
        queueFamily_ = fallbackFamily;
    }
    if (!physical_) {
        HBE_ERROR("[Vulkan] No device with a graphics+present queue family.");
        return false;
    }
    HBE_INFO("[Vulkan] Adapter: {}", adapterName_);
    return true;
}

bool VulkanDevice::CreateLogicalDevice() {
    const f32 priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceFeatures features{};
    // Anisotropic filtering for the bindless material sampler (when supported).
    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(physical_, &supported);
    features.samplerAnisotropy = supported.samplerAnisotropy;

    // Bindless: descriptor indexing (core in Vulkan 1.2). Enable runtime arrays,
    // non-uniform indexing, partially-bound + update-after-bind, variable count.
    VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    features12.descriptorIndexing = VK_TRUE;
    features12.runtimeDescriptorArray = VK_TRUE;
    features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    features12.descriptorBindingPartiallyBound = VK_TRUE;
    features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    features12.descriptorBindingVariableDescriptorCount = VK_TRUE;

    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.pNext = &features12;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qci;
    ci.enabledExtensionCount = 1;
    ci.ppEnabledExtensionNames = deviceExtensions;
    ci.pEnabledFeatures = &features;

    VK_CHECK(vkCreateDevice(physical_, &ci, nullptr, &device_), "vkCreateDevice");
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
    return true;
}

bool VulkanDevice::CreateSwapchain() {
    VkSurfaceCapabilitiesKHR caps{};
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps),
             "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

    u32 fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &fmtCount, formats.data());

    VkSurfaceFormatKHR chosen = formats.empty()
        ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
        : formats[0];
    for (const auto& f : formats) {
        if (f.format == swapFormat_ && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }
    swapFormat_ = chosen.format;
    swapColorSpace_ = chosen.colorSpace;

    if (caps.currentExtent.width != UINT32_MAX) {
        extent_ = caps.currentExtent;
    } else {
        extent_.width  = std::clamp(width_, caps.minImageExtent.width,  caps.maxImageExtent.width);
        extent_.height = std::clamp(height_, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent_.width == 0 || extent_.height == 0) {
        return false;
    }

    u32 imageCount = std::max(desiredImageCount_, caps.minImageCount);
    if (caps.maxImageCount > 0) imageCount = std::min(imageCount, caps.maxImageCount);

    // Present mode: FIFO = vsync (always available). vsync off prefers MAILBOX
    // (uncapped, no tearing - newest frame wins), else IMMEDIATE (uncapped,
    // tearing), else falls back to FIFO with a warning.
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsync_) {
        u32 pmCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &pmCount, nullptr);
        std::vector<VkPresentModeKHR> modes(pmCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &pmCount,
                                                  modes.data());
        const auto has = [&](VkPresentModeKHR m) {
            return std::find(modes.begin(), modes.end(), m) != modes.end();
        };
        if (has(VK_PRESENT_MODE_MAILBOX_KHR)) {
            presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
            HBE_INFO("[Vulkan] vsync off: MAILBOX present.");
        } else if (has(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
            presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            HBE_INFO("[Vulkan] vsync off: IMMEDIATE present (tearing).");
        } else {
            HBE_WARN("[Vulkan] vsync off requested but only FIFO available.");
        }
    }

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = imageCount;
    ci.imageFormat = swapFormat_;
    ci.imageColorSpace = swapColorSpace_;
    ci.imageExtent = extent_;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = presentMode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = VK_NULL_HANDLE;

    VK_CHECK(vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_), "vkCreateSwapchainKHR");

    u32 actual = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &actual, nullptr);
    images_.resize(actual);
    vkGetSwapchainImagesKHR(device_, swapchain_, &actual, images_.data());

    width_ = extent_.width;
    height_ = extent_.height;
    return true;
}

bool VulkanDevice::CreateSwapchainImageViews() {
    imageViews_.resize(images_.size(), VK_NULL_HANDLE);
    for (usize i = 0; i < images_.size(); ++i) {
        VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ci.image = images_[i];
        ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ci.format = swapFormat_;
        ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device_, &ci, nullptr, &imageViews_[i]), "vkCreateImageView(swap)");
    }
    return true;
}

bool VulkanDevice::CreateDepthResources() {
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = depthFormat_;
    ici.extent = {extent_.width, extent_.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(device_, &ici, nullptr, &depthImage_), "vkCreateImage(depth)");

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, depthImage_, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(device_, &ai, nullptr, &depthMemory_), "vkAllocateMemory(depth)");
    vkBindImageMemory(device_, depthImage_, depthMemory_, 0);

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = depthImage_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = depthFormat_;
    vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &depthView_), "vkCreateImageView(depth)");
    return true;
}

bool VulkanDevice::CreateRenderPass() {
    VkAttachmentDescription color{};
    color.format = swapFormat_;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentDescription depth{};
    depth.format = depthFormat_;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    const VkAttachmentDescription attachments[2] = {color, depth};
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = 2;
    ci.pAttachments = attachments;
    ci.subpassCount = 1;
    ci.pSubpasses = &subpass;
    ci.dependencyCount = 1;
    ci.pDependencies = &dep;
    VK_CHECK(vkCreateRenderPass(device_, &ci, nullptr, &renderPass_), "vkCreateRenderPass");
    return true;
}

bool VulkanDevice::CreateFramebuffers() {
    framebuffers_.resize(imageViews_.size(), VK_NULL_HANDLE);
    for (usize i = 0; i < imageViews_.size(); ++i) {
        const VkImageView attachments[2] = {imageViews_[i], depthView_};
        VkFramebufferCreateInfo ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        ci.renderPass = renderPass_;
        ci.attachmentCount = 2;
        ci.pAttachments = attachments;
        ci.width = extent_.width;
        ci.height = extent_.height;
        ci.layers = 1;
        VK_CHECK(vkCreateFramebuffer(device_, &ci, nullptr, &framebuffers_[i]), "vkCreateFramebuffer");
    }
    return true;
}

bool VulkanDevice::CreateSyncAndCommands() {
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queueFamily_;
    VK_CHECK(vkCreateCommandPool(device_, &pci, nullptr, &commandPool_), "vkCreateCommandPool");

    VkCommandBufferAllocateInfo abi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    abi.commandPool = commandPool_;
    abi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    abi.commandBufferCount = framesInFlight_;
    VK_CHECK(vkAllocateCommandBuffers(device_, &abi, commandBuffers_), "vkAllocateCommandBuffers");

    VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fen{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fen.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (u32 i = 0; i < framesInFlight_; ++i) {
        VK_CHECK(vkCreateSemaphore(device_, &sem, nullptr, &imageAvailable_[i]), "vkCreateSemaphore");
        VK_CHECK(vkCreateFence(device_, &fen, nullptr, &inFlight_[i]), "vkCreateFence");
    }

    // GPU pass profiler: one timestamp query pool per frame in flight. Gated behind
    // the debug/validation flag - its per-pass vkCmdWriteTimestamp markers serialize the
    // pipeline (each waits for prior work to reach BOTTOM_OF_PIPE), which measurably
    // inflates frame time. Leaving it ALWAYS-on cost ~1-3 ms/frame in shipped Vulkan
    // builds (enough to miss a 120 Hz vsync deadline that DX12 - which has no profiler -
    // was hitting). Now it only runs with --validation. Disabled gracefully if the
    // device can't timestamp (period 0).
    // The timestamp POOL is created whenever the GPU can timestamp; ACTIVATION (writing
    // the per-pass marks + reading them back, which serialises the pipeline ~1-3 ms/frame)
    // is a runtime toggle via SetGpuProfileEnabled - so a --gpuprofile run or the dev menu
    // can turn it on for diagnosis WITHOUT paying the cost in a normal shipped frame, and
    // WITHOUT needing --validation (which the KHRONOS layer may not even be present for).
    gpuPeriodNs_ = static_cast<f64>(deviceProps_.limits.timestampPeriod);
    if (gpuPeriodNs_ > 0.0) {
        VkQueryPoolCreateInfo qpci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpci.queryCount = kMaxGpuMarks;
        gpuProfileAvail_ = true;
        for (u32 i = 0; i < framesInFlight_; ++i) {
            if (vkCreateQueryPool(device_, &qpci, nullptr, &gpuPool_[i]) != VK_SUCCESS) {
                gpuProfileAvail_ = false;
                break;
            }
        }
        gpuProfile_ = gpuProfileAvail_ && gpuProfileRequested_; // honor a pre-init request
    }
    renderFinished_.resize(images_.size(), VK_NULL_HANDLE);
    for (auto& s : renderFinished_) {
        VK_CHECK(vkCreateSemaphore(device_, &sem, nullptr, &s), "vkCreateSemaphore");
    }
    return true;
}

bool VulkanDevice::CreateDescriptorResources() {
    // Immutable linear-clamp sampler for the post passes (set 0 binding 2;
    // mirrors the D3D12 static sampler s1).
    {
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = VK_LOD_CLAMP_NONE;
        VK_CHECK(vkCreateSampler(device_, &sci, nullptr, &clampSampler_),
                 "vkCreateSampler(clamp)");
    }

    // Per-frame UBO (binding 0) + per-draw dynamic UBO (binding 1) + the
    // immutable clamp sampler (binding 2) + the joint-palette storage buffer
    // (binding 3, vertex skinning) + the instance-transform storage buffer
    // (binding 4, GPU instancing).
    VkDescriptorSetLayoutBinding bindings[7]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[2].descriptorCount = 1;
    // VERTEX too: the BrushStrokes pass samples the HDR in its vertex shader (stroke
    // colour + Sobel flow orientation); a fragment-only sampler is invalid from the VS.
    bindings[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[2].pImmutableSamplers = &clampSampler_;
    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    // Volumetric raymarch: the 3D density/temperature volume (Texture3D can't live
    // in the Texture2D[] bindless array). Only the volumetric-particles pass writes
    // it into its post set; every other pass leaves it unbound (doesn't sample it).
    bindings[5].binding = 5;
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[5].descriptorCount = 1;
    bindings[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    // NanoVDB baked-volume raymarch: the grid blob as a readonly StructuredBuffer<uint>. Like
    // binding 5, only the NanoVDB volume pass writes it into its post set; every other pass /
    // set leaves it unbound (never sampled). Distinct from binding 5 because a sampled image
    // and a storage buffer cannot share one binding in this shared layout.
    bindings[6].binding = 6;
    bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[6].descriptorCount = 1;
    bindings[6].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 7;
    lci.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(device_, &lci, nullptr, &descriptorLayout_),
             "vkCreateDescriptorSetLayout");

    // Sets per frame: 1 main + kMaxShadowCascades shadow clones + 1 post +
    // 1 editor asset preview.
    const u32 setsPerFrame = 1 + kMaxShadowCascades + 1 + 1;
    VkDescriptorPoolSize sizes[5]{};
    sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[0].descriptorCount = framesInFlight_ * setsPerFrame;
    sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    sizes[1].descriptorCount = framesInFlight_ * setsPerFrame;
    sizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLER;
    sizes[2].descriptorCount = framesInFlight_ * setsPerFrame;
    sizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sizes[3].descriptorCount = framesInFlight_ * setsPerFrame * 3; // bones + instances + volume grid (b6)
    sizes[4].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    sizes[4].descriptorCount = framesInFlight_ * setsPerFrame; // binding 5 volume (post sets)
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = framesInFlight_ * setsPerFrame;
    pci.poolSizeCount = 5;
    pci.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(device_, &pci, nullptr, &descriptorPool_),
             "vkCreateDescriptorPool");

    const VkDeviceSize minAlign = deviceProps_.limits.minUniformBufferOffsetAlignment;
    objectStride_ = AlignUp(sizeof(ObjectUBO), minAlign);
    shadowFrameStride_ = AlignUp(sizeof(FrameUBO), minAlign);
    postStride_ = AlignUp(sizeof(PostUBO), minAlign);

    for (u32 i = 0; i < framesInFlight_; ++i) {
        if (!CreateBuffer(sizeof(FrameUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          frameUBO_[i], frameUBOMem_[i])) {
            return false;
        }
        vkMapMemory(device_, frameUBOMem_[i], 0, sizeof(FrameUBO), 0, &frameUBOMapped_[i]);

        if (!CreateBuffer(kObjectArenaSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          objectArena_[i], objectArenaMem_[i])) {
            return false;
        }
        void* mapped = nullptr;
        vkMapMemory(device_, objectArenaMem_[i], 0, kObjectArenaSize, 0, &mapped);
        objectArenaMapped_[i] = static_cast<u8*>(mapped);

        if (!CreateBuffer(kBoneArenaSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          boneArena_[i], boneArenaMem_[i])) {
            return false;
        }
        void* boneMapped = nullptr;
        vkMapMemory(device_, boneArenaMem_[i], 0, kBoneArenaSize, 0, &boneMapped);
        boneArenaMapped_[i] = static_cast<u8*>(boneMapped);

        if (!CreateBuffer(kInstanceArenaSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          instanceArena_[i], instanceArenaMem_[i])) {
            return false;
        }
        void* instMapped = nullptr;
        vkMapMemory(device_, instanceArenaMem_[i], 0, kInstanceArenaSize, 0, &instMapped);
        instanceArenaMapped_[i] = static_cast<u8*>(instMapped);
    }

    // Writes bindings 0/1/3/4 of one set-0 clone (binding 2 is immutable). The
    // bone + instance arenas ride along with the frame index of the frame UBO.
    const auto writeSet = [&](VkDescriptorSet set, VkBuffer frameBuf, VkDeviceSize frameOffset,
                              VkBuffer dynBuf, VkDeviceSize dynRange, VkBuffer boneBuf,
                              VkBuffer instBuf) {
        VkDescriptorBufferInfo frameInfo{frameBuf, frameOffset, sizeof(FrameUBO)};
        VkDescriptorBufferInfo objInfo{dynBuf, 0, dynRange};
        VkDescriptorBufferInfo boneInfo{boneBuf, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo instInfo{instBuf, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet writes[4]{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[0].dstSet = set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &frameInfo;
        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[1].dstSet = set;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[1].pBufferInfo = &objInfo;
        writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[2].dstSet = set;
        writes[2].dstBinding = 3;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].pBufferInfo = &boneInfo;
        writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[3].dstSet = set;
        writes[3].dstBinding = 4;
        writes[3].descriptorCount = 1;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[3].pBufferInfo = &instInfo;
        vkUpdateDescriptorSets(device_, 4, writes, 0, nullptr);
    };

    const auto allocSets = [&](VkDescriptorSet* out, u32 n) {
        VkDescriptorSetLayout layouts[kMaxFramesInFlight * kMaxShadowCascades];
        for (u32 i = 0; i < n; ++i) layouts[i] = descriptorLayout_;
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = descriptorPool_;
        ai.descriptorSetCount = n;
        ai.pSetLayouts = layouts;
        return vkAllocateDescriptorSets(device_, &ai, out) == VK_SUCCESS;
    };

    if (!allocSets(descriptorSets_, framesInFlight_)) return false;
    for (u32 i = 0; i < framesInFlight_; ++i) {
        writeSet(descriptorSets_[i], frameUBO_[i], 0, objectArena_[i], sizeof(ObjectUBO),
                 boneArena_[i], instanceArena_[i]);
    }

    // Shadow-pass clones of set 0, one per cascade: binding 0 points at that
    // cascade's region of the per-frame shadow UBO (light matrices); binding 1
    // shares the object arena.
    for (u32 i = 0; i < framesInFlight_; ++i) {
        if (!CreateBuffer(shadowFrameStride_ * kMaxShadowCascades,
                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          shadowFrameUBO_[i], shadowFrameUBOMem_[i])) {
            return false;
        }
        void* mapped = nullptr;
        vkMapMemory(device_, shadowFrameUBOMem_[i], 0,
                    shadowFrameStride_ * kMaxShadowCascades, 0, &mapped);
        shadowFrameUBOMapped_[i] = static_cast<u8*>(mapped);

        if (!allocSets(shadowDescriptorSets_[i], kMaxShadowCascades)) return false;
        for (u32 c = 0; c < kMaxShadowCascades; ++c) {
            writeSet(shadowDescriptorSets_[i][c], shadowFrameUBO_[i],
                     shadowFrameStride_ * c, objectArena_[i], sizeof(ObjectUBO),
                     boneArena_[i], instanceArena_[i]);
        }
    }

    // Post-pass clones of set 0: binding 0 shares the main frame UBO (the post
    // shaders read gViewProj/gExposure), binding 1 is the small post arena
    // addressed with dynamic offsets (one PostUBO per pass).
    for (u32 i = 0; i < framesInFlight_; ++i) {
        if (!CreateBuffer(kPostArenaSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          postArena_[i], postArenaMem_[i])) {
            return false;
        }
        void* mapped = nullptr;
        vkMapMemory(device_, postArenaMem_[i], 0, kPostArenaSize, 0, &mapped);
        postArenaMapped_[i] = static_cast<u8*>(mapped);

        if (!allocSets(&postSets_[i], 1)) return false;
        writeSet(postSets_[i], frameUBO_[i], 0, postArena_[i], sizeof(PostUBO),
                 boneArena_[i], instanceArena_[i]);
    }

    // Editor asset-preview clones of set 0: binding 0 = the preview's own
    // frame UBO (its camera), binding 1 shares the object arena (preview
    // object/post constants live at the arena TAIL so the main scene's writes
    // never collide).
    for (u32 i = 0; i < framesInFlight_; ++i) {
        if (!CreateBuffer(sizeof(FrameUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          previewFrameUBO_[i], previewFrameUBOMem_[i])) {
            return false;
        }
        vkMapMemory(device_, previewFrameUBOMem_[i], 0, sizeof(FrameUBO), 0,
                    &previewFrameUBOMapped_[i]);
        if (!allocSets(&previewSets_[i], 1)) return false;
        writeSet(previewSets_[i], previewFrameUBO_[i], 0, objectArena_[i],
                 sizeof(ObjectUBO), boneArena_[i], instanceArena_[i]);
    }
    return true;
}

bool VulkanDevice::CreateBindlessResources() {
    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(physical_, &supported);

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    sci.anisotropyEnable = supported.samplerAnisotropy;
    sci.maxAnisotropy =
        supported.samplerAnisotropy
            ? (std::min)(8.0f, deviceProps_.limits.maxSamplerAnisotropy) : 1.0f;
    VK_CHECK(vkCreateSampler(device_, &sci, nullptr, &bindlessSampler_), "vkCreateSampler(bindless)");

    // Set 1: binding 0 = sampler, binding 1 = variable-count sampled-image array
    // (the array is the highest binding so it can be variable-count).
    VkDescriptorSetLayoutBinding binds[2]{};
    binds[0].binding = 0;
    binds[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    binds[0].descriptorCount = 1;
    binds[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    binds[1].binding = 1;
    binds[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    binds[1].descriptorCount = kMaxBindlessTextures;
    // VERTEX too: the BrushStrokes pass samples the bindless HDR texture in its
    // vertex shader (per-stroke colour + flow); fragment-only would be invalid there.
    binds[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorBindingFlags flags[2] = {
        0,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
        VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT,
    };
    VkDescriptorSetLayoutBindingFlagsCreateInfo bf{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bf.bindingCount = 2;
    bf.pBindingFlags = flags;

    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.pNext = &bf;
    lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    lci.bindingCount = 2;
    lci.pBindings = binds;
    VK_CHECK(vkCreateDescriptorSetLayout(device_, &lci, nullptr, &bindlessLayout_),
             "vkCreateDescriptorSetLayout(bindless)");

    VkDescriptorPoolSize sizes[2]{};
    sizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLER;
    sizes[0].descriptorCount = 1;
    sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    sizes[1].descriptorCount = kMaxBindlessTextures;
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pci.maxSets = 1;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(device_, &pci, nullptr, &bindlessPool_),
             "vkCreateDescriptorPool(bindless)");

    u32 variableCount = kMaxBindlessTextures;
    VkDescriptorSetVariableDescriptorCountAllocateInfo varCount{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
    varCount.descriptorSetCount = 1;
    varCount.pDescriptorCounts = &variableCount;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.pNext = &varCount;
    ai.descriptorPool = bindlessPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &bindlessLayout_;
    VK_CHECK(vkAllocateDescriptorSets(device_, &ai, &bindlessSet_),
             "vkAllocateDescriptorSets(bindless)");

    VkDescriptorImageInfo sampInfo{};
    sampInfo.sampler = bindlessSampler_;
    VkWriteDescriptorSet sw{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    sw.dstSet = bindlessSet_;
    sw.dstBinding = 0;
    sw.descriptorCount = 1;
    sw.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    sw.pImageInfo = &sampInfo;
    vkUpdateDescriptorSets(device_, 1, &sw, 0, nullptr);

    // Default white texture at slot 0.
    const u32 white = 0xFFFFFFFFu;
    TextureDesc desc;
    desc.width = 1;
    desc.height = 1;
    desc.format = Format::R8G8B8A8_UNORM;
    desc.pixels = &white;
    CreateTexture(desc);
    return true;
}

TextureHandle VulkanDevice::CreateTexture(const TextureDesc& desc) {
    if (!desc.pixels || bindlessNextSlot_ >= kMaxBindlessTextures) return {};
    const u32 slot = bindlessNextSlot_++;
    const VkFormat fmt = ToVkFormat(desc.format);
    const u32 bpp = BytesPerPixel(desc.format);
    const u32 mipCount = desc.mipCount < 1 ? 1 : desc.mipCount;

    // Tightly-packed staging size across all mips.
    VkDeviceSize total = 0;
    for (u32 mip = 0; mip < mipCount; ++mip) {
        const u32 mw = std::max(1u, desc.width >> mip);
        const u32 mh = std::max(1u, desc.height >> mip);
        total += static_cast<VkDeviceSize>(mw) * mh * bpp;
    }

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    if (!CreateBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      staging, stagingMem)) {
        --bindlessNextSlot_;
        return {};
    }
    void* p = nullptr;
    vkMapMemory(device_, stagingMem, 0, total, 0, &p);
    std::memcpy(p, desc.pixels, static_cast<usize>(total));
    vkUnmapMemory(device_, stagingMem);

    GpuTextureVk tex;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent = {desc.width, desc.height, 1};
    ici.mipLevels = mipCount;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // CHECK THE RETURN CODES. None of these three calls was checked, so an out-of-memory
    // device handed back a VALID bindless handle pointing at a null image: every draw that
    // sampled it read garbage or tripped validation, and nothing anywhere reported a
    // failure. D3D12 already checks, rolls the bindless slot back and returns an invalid
    // handle - which is the documented failure signal callers test for.
    const auto abandon = [&](const char* what) -> TextureHandle {
        HBE_ERROR("[Vulkan] CreateTexture failed at {} ({}x{} mip{})", what, desc.width,
                  desc.height, mipCount);
        if (tex.image) vkDestroyImage(device_, tex.image, nullptr);
        if (tex.memory) vkFreeMemory(device_, tex.memory, nullptr);
        vkDestroyBuffer(device_, staging, nullptr);
        vkFreeMemory(device_, stagingMem, nullptr);
        --bindlessNextSlot_; // give the slot back, exactly as D3D12 does
        return {};
    };
    if (vkCreateImage(device_, &ici, nullptr, &tex.image) != VK_SUCCESS) {
        tex.image = VK_NULL_HANDLE;
        return abandon("vkCreateImage");
    }

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, tex.image, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device_, &mai, nullptr, &tex.memory) != VK_SUCCESS) {
        tex.memory = VK_NULL_HANDLE;
        return abandon("vkAllocateMemory");
    }
    if (vkBindImageMemory(device_, tex.image, tex.memory, 0) != VK_SUCCESS)
        return abandon("vkBindImageMemory");

    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = commandPool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &cbai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    const VkImageSubresourceRange fullRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, mipCount, 0, 1};
    auto transition = [&](VkImageLayout o, VkImageLayout n, VkAccessFlags sa, VkAccessFlags da,
                          VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = o; b.newLayout = n;
        b.srcAccessMask = sa; b.dstAccessMask = da;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = tex.image;
        b.subresourceRange = fullRange;
        vkCmdPipelineBarrier(cmd, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    transition(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               0, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    std::vector<VkBufferImageCopy> regions(mipCount);
    VkDeviceSize offset = 0;
    for (u32 mip = 0; mip < mipCount; ++mip) {
        const u32 mw = std::max(1u, desc.width >> mip);
        const u32 mh = std::max(1u, desc.height >> mip);
        VkBufferImageCopy& r = regions[mip];
        r = {};
        r.bufferOffset = offset;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
        r.imageExtent = {mw, mh, 1};
        offset += static_cast<VkDeviceSize>(mw) * mh * bpp;
    }
    vkCmdCopyBufferToImage(cmd, staging, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           mipCount, regions.data());

    transition(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_); // simple synchronous upload
    vkFreeCommandBuffers(device_, commandPool_, 1, &cmd);
    vkDestroyBuffer(device_, staging, nullptr);
    vkFreeMemory(device_, stagingMem, nullptr);

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = tex.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = fmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipCount, 0, 1};
    vkCreateImageView(device_, &vci, nullptr, &tex.view);

    VkDescriptorImageInfo ii{};
    ii.imageView = tex.view;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = bindlessSet_;
    w.dstBinding = 1;
    w.dstArrayElement = slot;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);

    slotViews_[slot] = tex.view;
    slotImages_[slot] = tex.image;
    slotImageInfo_[slot] = SlotImageInfo{desc.width, desc.height,
                                         desc.mipCount < 1 ? 1u : desc.mipCount, desc.format};
    textures_.push_back(tex);
    return TextureHandle{slot};
}

TextureHandle VulkanDevice::CreateVolumeTexture(const TextureDesc& desc) {
    // Uninitialised, compute-writable, sampled image. depth>1 => 3D. Kept in
    // GENERAL layout for its whole life so the same image is valid for both the
    // storage-image write (compute splat) and the sampled raymarch read - no
    // per-frame layout transitions, just a memory barrier between the two (VV2).
    if (bindlessNextSlot_ >= kMaxBindlessTextures) return {};
    const VkFormat fmt = ToVkFormat(desc.format);
    const u32 depth = std::max(1u, desc.depth);
    const bool is3D = depth > 1;

    GpuTextureVk tex;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = is3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent = {desc.width, desc.height, depth};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | (desc.storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ici, nullptr, &tex.image) != VK_SUCCESS) return {};

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, tex.image, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(device_, &mai, nullptr, &tex.memory);
    vkBindImageMemory(device_, tex.image, tex.memory, 0);

    // Transition UNDEFINED -> GENERAL once (its permanent layout).
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = commandPool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &cbai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = tex.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);
    vkFreeCommandBuffers(device_, commandPool_, 1, &cmd);

    const VkImageViewType viewType = is3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    // Sampled view -> bindless (binding 1), imageLayout GENERAL for this slot.
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = tex.image;
    vci.viewType = viewType;
    vci.format = fmt;
    vci.subresourceRange = range;
    vkCreateImageView(device_, &vci, nullptr, &tex.view);

    const u32 slot = bindlessNextSlot_++;
    VkDescriptorImageInfo ii{};
    ii.imageView = tex.view;
    ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = bindlessSet_;
    w.dstBinding = 1;
    w.dstArrayElement = slot;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);

    // Storage view for the compute splat (VV2 binds it as a STORAGE_IMAGE).
    if (desc.storage) {
        VkImageView storageView = VK_NULL_HANDLE;
        VkImageViewCreateInfo sci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        sci.image = tex.image;
        sci.viewType = viewType;
        sci.format = fmt;
        sci.subresourceRange = range;
        vkCreateImageView(device_, &sci, nullptr, &storageView);
        volumeStorageView_[slot] = storageView;
    }

    slotViews_[slot] = tex.view;
    slotImages_[slot] = tex.image;
    textures_.push_back(tex);
    return TextureHandle{slot};
}

void VulkanDevice::UpdateTexture(TextureHandle handle, const TextureDesc& desc) {
    if (!handle.IsValid() || !desc.pixels) return;
    const auto imgIt = slotImages_.find(handle.index);
    if (imgIt == slotImages_.end() || imgIt->second == VK_NULL_HANDLE) return;
    VkImage image = imgIt->second;
    const u32 bpp = BytesPerPixel(desc.format);
    const u32 mipCount = desc.mipCount < 1 ? 1 : desc.mipCount;

    // VALIDATE AGAINST WHAT THE IMAGE ACTUALLY IS - this is the D3D12 behaviour, and its
    // absence here was not merely a missing check. Everything below sizes the staging
    // buffer from the DESC and then memcpy's that many bytes out of desc.pixels, so a
    // caller passing a bigger desc than the image reads PAST THE END of its own buffer and
    // then copies past the end of the image. D3D12 returns early on exactly this mismatch;
    // matching it turns a memory-corrupting call into a no-op on both backends.
    const auto infoIt = slotImageInfo_.find(handle.index);
    if (infoIt == slotImageInfo_.end()) return; // not an UpdateTexture-capable slot
    const SlotImageInfo& info = infoIt->second;
    if (info.width != desc.width || info.height != desc.height ||
        info.mipCount != mipCount || info.format != desc.format) {
        HBE_WARN("[Vulkan] UpdateTexture refused: {}x{} mip{} fmt{} does not match the "
                 "{}x{} mip{} fmt{} this texture was created as.",
                 desc.width, desc.height, mipCount, static_cast<int>(desc.format),
                 info.width, info.height, info.mipCount, static_cast<int>(info.format));
        return;
    }

    VkDeviceSize total = 0;
    for (u32 mip = 0; mip < mipCount; ++mip) {
        const u32 mw = std::max(1u, desc.width >> mip);
        const u32 mh = std::max(1u, desc.height >> mip);
        total += static_cast<VkDeviceSize>(mw) * mh * bpp;
    }

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    if (!CreateBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      staging, stagingMem))
        return;
    void* p = nullptr;
    vkMapMemory(device_, stagingMem, 0, total, 0, &p);
    std::memcpy(p, desc.pixels, static_cast<usize>(total));
    vkUnmapMemory(device_, stagingMem);

    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = commandPool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &cbai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    const VkImageSubresourceRange fullRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, mipCount, 0, 1};
    auto transition = [&](VkImageLayout o, VkImageLayout n, VkAccessFlags sa, VkAccessFlags da,
                          VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = o; b.newLayout = n;
        b.srcAccessMask = sa; b.dstAccessMask = da;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = fullRange;
        vkCmdPipelineBarrier(cmd, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    transition(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    std::vector<VkBufferImageCopy> regions(mipCount);
    VkDeviceSize offset = 0;
    for (u32 mip = 0; mip < mipCount; ++mip) {
        const u32 mw = std::max(1u, desc.width >> mip);
        const u32 mh = std::max(1u, desc.height >> mip);
        VkBufferImageCopy& r = regions[mip];
        r = {};
        r.bufferOffset = offset;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
        r.imageExtent = {mw, mh, 1};
        offset += static_cast<VkDeviceSize>(mw) * mh * bpp;
    }
    vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           mipCount, regions.data());

    transition(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_); // simple synchronous upload (matches CreateTexture)
    vkFreeCommandBuffers(device_, commandPool_, 1, &cmd);
    vkDestroyBuffer(device_, staging, nullptr);
    vkFreeMemory(device_, stagingMem, nullptr);
}

bool VulkanDevice::CreateMeshPipeline() {
    const std::wstring dir = ExecutableDir() + L"shaders\\";
    VkShaderModule vs = LoadShaderModule(dir + L"MeshPBR.vs.spv");
    VkShaderModule ps = LoadShaderModule(dir + L"MeshPBR.ps.spv");
    if (vs == VK_NULL_HANDLE || ps == VK_NULL_HANDLE) {
        HBE_WARN("[Vulkan] MeshPBR SPIR-V not found next to the executable.");
        if (vs) vkDestroyShaderModule(device_, vs, nullptr);
        if (ps) vkDestroyShaderModule(device_, ps, nullptr);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "VSMain";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps;
    stages[1].pName = "PSMain";

    VkVertexInputBindingDescription binding{0, sizeof(hbe::Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[6] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(hbe::Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(hbe::Vertex, normal)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(hbe::Vertex, tangent)},
        {3, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(hbe::Vertex, uv)},
        {4, 0, VK_FORMAT_R16G16B16A16_UINT,   offsetof(hbe::Vertex, joints)},
        {5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(hbe::Vertex, weights)},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 6;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE; // double-sided default (transparent, sky, preview-bg)
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    // Opaque meshes back-face cull. Front faces are COUNTER-clockwise in this
    // setup (CLOCKWISE showed the inside of objects). Matches the D3D12 side.
    VkPipelineRasterizationStateCreateInfo rsCull = rs;
    rsCull.cullMode = VK_CULL_MODE_BACK_BIT;
    rsCull.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;

    // The main HDR pass has 3 colour attachments (colour + G-buffer + velocity);
    // the legacy non-post path and the preview pass have one. Each bound
    // attachment needs a blend state.
    const VkColorComponentFlags rgba = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                       VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendAttachmentState cbaArr[3]{};
    for (auto& a : cbaArr) { a.colorWriteMask = rgba; a.blendEnable = VK_FALSE; }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; // overridden to 3 below once the post stack is known
    cb.pAttachments = cbaArr;

    const VkDynamicState dynamics[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynamics;

    // Set 2 = the general VS-visible structured buffer (SetVertexShaderBuffer).
    // Purely additive: it does not disturb sets 0/1, and a pipeline whose shaders
    // never declare set 2 simply never has it bound. Twin of D3D12 root param 6.
    if (!CreateVsBufferSetLayout()) {
        vkDestroyShaderModule(device_, vs, nullptr);
        vkDestroyShaderModule(device_, ps, nullptr);
        return false;
    }
    const VkDescriptorSetLayout setLayouts[3] = {descriptorLayout_, bindlessLayout_,
                                                 vsBufferLayout_};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 3;
    plci.pSetLayouts = setLayouts;
    if (vkCreatePipelineLayout(device_, &plci, nullptr, &pipelineLayout_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, vs, nullptr);
        vkDestroyShaderModule(device_, ps, nullptr);
        return false;
    }

    // HDR post stack: when the post render passes + pipelines build, the scene
    // renders into an RGBA16F target (tonemapped later); otherwise fall back
    // to the legacy direct-to-swapchain path (shaders tonemap inline).
    postPipelinesReady_ = CreatePostRenderPasses() && CreatePostPipelines();
    if (!postPipelinesReady_) {
        HBE_WARN("[Vulkan] Post-process pipelines unavailable; HDR stack disabled.");
    }
    // The HDR pass writes 3 colour attachments (colour + G-buffer + velocity).
    if (postPipelinesReady_) cb.attachmentCount = 3;

    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rsCull; // opaque mesh: back-face cull
    pci.pMultisampleState = &ms;
    pci.pDepthStencilState = &ds;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &dyn;
    pci.layout = pipelineLayout_;
    pci.renderPass = postPipelinesReady_ ? hdrRenderPass_ : renderPass_;
    pci.subpass = 0;

    const VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pci, nullptr, &meshPipeline_);

    // Transparent variant: straight-alpha blend on the colour attachment, depth
    // test LESS_OR_EQUAL with writes OFF, no writes to the G-buffer/velocity.
    // Drawn back-to-front after opaques. Real alpha blending (no dither / TAA).
    if (r == VK_SUCCESS && postPipelinesReady_) {
        VkPipelineColorBlendAttachmentState tcba[3]{};
        for (auto& a : tcba) { a.colorWriteMask = rgba; a.blendEnable = VK_FALSE; }
        tcba[0].blendEnable = VK_TRUE;
        tcba[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        tcba[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        tcba[0].colorBlendOp = VK_BLEND_OP_ADD;
        tcba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        tcba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        tcba[0].alphaBlendOp = VK_BLEND_OP_ADD;
        tcba[1].colorWriteMask = 0; // keep G-buffer
        tcba[2].colorWriteMask = 0; // keep velocity
        VkPipelineColorBlendStateCreateInfo tcb = cb;
        tcb.attachmentCount = 3;
        tcb.pAttachments = tcba;
        VkPipelineDepthStencilStateCreateInfo tds = ds;
        tds.depthWriteEnable = VK_FALSE;
        tds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkGraphicsPipelineCreateInfo tp = pci;
        tp.pRasterizationState = &rs; // transparent stays double-sided
        tp.pColorBlendState = &tcb;
        tp.pDepthStencilState = &tds;
        if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &tp, nullptr,
                                      &meshPipelineTransparent_) != VK_SUCCESS) {
            HBE_WARN("[Vulkan] Transparent mesh pipeline failed; transparency disabled.");
            meshPipelineTransparent_ = VK_NULL_HANDLE;
        }

        // Gerstner water surface: the mesh vertex input (via tp/vi) + the transparent
        // blend/depth state (tcb/tds are still in their depth-LE-no-write form here) +
        // its own Water VS/PS. Two-sided (rs). Built before tds/tcba are mutated for the
        // depth-writing transparent variant below.
        {
            VkShaderModule wvs = LoadShaderModule(dir + L"Water.vs.spv");
            VkShaderModule wps = LoadShaderModule(dir + L"Water.ps.spv");
            if (wvs != VK_NULL_HANDLE && wps != VK_NULL_HANDLE) {
                VkPipelineShaderStageCreateInfo wst[2] = {stages[0], stages[1]};
                wst[0].module = wvs;
                wst[1].module = wps;
                VkGraphicsPipelineCreateInfo wpci = tp;
                wpci.pStages = wst;
                if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &wpci, nullptr,
                                              &waterPipeline_) != VK_SUCCESS) {
                    HBE_WARN("[Vulkan] Water pipeline failed to create.");
                    waterPipeline_ = VK_NULL_HANDLE;
                }
            }
            if (wvs != VK_NULL_HANDLE) vkDestroyShaderModule(device_, wvs, nullptr);
            if (wps != VK_NULL_HANDLE) vkDestroyShaderModule(device_, wps, nullptr);
        }
        // "Solid transparent" variant (MaterialFlag_DepthWrite, e.g. paint strokes):
        // same alpha blend but WRITES depth + velocity so DoF/TAA treat the stroke as a
        // real in-focus surface (no off-surface blur). G-buffer (tcba[1]) stays masked.
        tcba[2].colorWriteMask = rgba; // velocity
        tds.depthWriteEnable = VK_TRUE;
        if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &tp, nullptr,
                                      &meshPipelineTransparentDepth_) != VK_SUCCESS) {
            meshPipelineTransparentDepth_ = VK_NULL_HANDLE;
        }

        // Particle billboards: own VS/PS + vertex layout, world-space, depth-test
        // (no write), into the HDR colour only. Alpha + additive variants. Built
        // before the UI block which reuses `stages`.
        VkShaderModule pvs = LoadShaderModule(dir + L"Particle.vs.spv");
        VkShaderModule pps = LoadShaderModule(dir + L"Particle.ps.spv");
        if (pvs != VK_NULL_HANDLE && pps != VK_NULL_HANDLE) {
            VkPipelineShaderStageCreateInfo pst[2] = {stages[0], stages[1]};
            pst[0].module = pvs;
            pst[1].module = pps;
            VkVertexInputBindingDescription pbind{0, sizeof(ParticleVertex),
                                                  VK_VERTEX_INPUT_RATE_VERTEX};
            VkVertexInputAttributeDescription pattrs[4] = {
                {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(ParticleVertex, x)},
                {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ParticleVertex, u)},
                {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ParticleVertex, r)},
                {3, 0, VK_FORMAT_R32_UINT, offsetof(ParticleVertex, texIndex)},
            };
            VkPipelineVertexInputStateCreateInfo pvi{
                VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            pvi.vertexBindingDescriptionCount = 1;
            pvi.pVertexBindingDescriptions = &pbind;
            pvi.vertexAttributeDescriptionCount = 4;
            pvi.pVertexAttributeDescriptions = pattrs;
            VkPipelineDepthStencilStateCreateInfo pds = ds;
            pds.depthWriteEnable = VK_FALSE;
            pds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
            const auto makeParticle = [&](bool additive, VkPipeline& outPipe) {
                VkPipelineColorBlendAttachmentState pcba[3]{};
                for (auto& a : pcba) { a.colorWriteMask = rgba; a.blendEnable = VK_FALSE; }
                pcba[0].blendEnable = VK_TRUE;
                pcba[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                pcba[0].dstColorBlendFactor =
                    additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                pcba[0].colorBlendOp = VK_BLEND_OP_ADD;
                pcba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                pcba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                pcba[0].alphaBlendOp = VK_BLEND_OP_ADD;
                pcba[1].colorWriteMask = 0; // keep G-buffer
                pcba[2].colorWriteMask = 0; // keep velocity
                VkPipelineColorBlendStateCreateInfo pcb = cb;
                pcb.attachmentCount = 3;
                pcb.pAttachments = pcba;
                VkGraphicsPipelineCreateInfo pp = pci;
                pp.pStages = pst;
                pp.pVertexInputState = &pvi;
                pp.pRasterizationState = &rs; // double-sided billboards
                pp.pDepthStencilState = &pds;
                pp.pColorBlendState = &pcb;
                return vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pp, nullptr,
                                                 &outPipe) == VK_SUCCESS;
            };
            if (!makeParticle(false, particlePipeline_) ||
                !makeParticle(true, particlePipelineAdd_)) {
                HBE_WARN("[Vulkan] Particle pipeline failed; particles disabled.");
                particlePipeline_ = particlePipelineAdd_ = VK_NULL_HANDLE;
            }

            // GPU-expanded variant: identical blend/depth/attachment state, but an
            // EMPTY vertex input state - the quad comes from gl_VertexIndex and the
            // records come from set 2. Failure here leaves the CPU path untouched
            // (the GPU path is opt-in per emitter), so it is a warning only.
            VkShaderModule gvs = LoadShaderModule(dir + L"ParticleGpu.vs.spv");
            VkShaderModule gps = LoadShaderModule(dir + L"ParticleGpu.ps.spv");
            if (particlePipeline_ != VK_NULL_HANDLE && gvs != VK_NULL_HANDLE &&
                gps != VK_NULL_HANDLE) {
                VkPipelineShaderStageCreateInfo gst[2] = {stages[0], stages[1]};
                gst[0].module = gvs;
                gst[1].module = gps;
                VkPipelineVertexInputStateCreateInfo gvi{
                    VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
                gvi.vertexBindingDescriptionCount = 0;
                gvi.vertexAttributeDescriptionCount = 0;
                const auto makeParticleGpu = [&](bool additive, VkPipeline& outPipe) {
                    VkPipelineColorBlendAttachmentState pcba[3]{};
                    for (auto& a : pcba) { a.colorWriteMask = rgba; a.blendEnable = VK_FALSE; }
                    pcba[0].blendEnable = VK_TRUE;
                    pcba[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                    pcba[0].dstColorBlendFactor =
                        additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                    pcba[0].colorBlendOp = VK_BLEND_OP_ADD;
                    pcba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                    pcba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                    pcba[0].alphaBlendOp = VK_BLEND_OP_ADD;
                    pcba[1].colorWriteMask = 0; // keep G-buffer
                    pcba[2].colorWriteMask = 0; // keep velocity
                    VkPipelineColorBlendStateCreateInfo pcb = cb;
                    pcb.attachmentCount = 3;
                    pcb.pAttachments = pcba;
                    VkGraphicsPipelineCreateInfo pp = pci;
                    pp.pStages = gst;
                    pp.pVertexInputState = &gvi;
                    pp.pRasterizationState = &rs;
                    pp.pDepthStencilState = &pds;
                    pp.pColorBlendState = &pcb;
                    return vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pp, nullptr,
                                                     &outPipe) == VK_SUCCESS;
                };
                if (!makeParticleGpu(false, particleGpuPipeline_) ||
                    !makeParticleGpu(true, particleGpuPipelineAdd_)) {
                    HBE_WARN("[Vulkan] GPU particle expansion pipeline failed; emitters with "
                             "gpuExpand draw nothing (CPU path is unaffected).");
                    particleGpuPipeline_ = particleGpuPipelineAdd_ = VK_NULL_HANDLE;
                }
            } else if (particlePipeline_ != VK_NULL_HANDLE) {
                HBE_WARN("[Vulkan] ParticleGpu shaders missing; GPU particle expansion off.");
            }
            if (gvs != VK_NULL_HANDLE) vkDestroyShaderModule(device_, gvs, nullptr);
            if (gps != VK_NULL_HANDLE) vkDestroyShaderModule(device_, gps, nullptr);

            vkDestroyShaderModule(device_, pvs, nullptr);
            vkDestroyShaderModule(device_, pps, nullptr);
        }

        // 3D painterly surface strokes: own VS/PS, PER-INSTANCE vertex layout
        // (StrokeInstance; the quad's 6 corners come from gl_VertexIndex), PBR-lit,
        // depth-test LE no-write, alpha-over, G-buffer/velocity preserved.
        VkShaderModule svs = LoadShaderModule(dir + L"StrokeSurface.vs.spv");
        VkShaderModule sps = LoadShaderModule(dir + L"StrokeSurface.ps.spv");
        if (svs != VK_NULL_HANDLE && sps != VK_NULL_HANDLE) {
            VkPipelineShaderStageCreateInfo sst[2] = {stages[0], stages[1]};
            sst[0].module = svs;
            sst[1].module = sps;
            VkVertexInputBindingDescription sbind{0, sizeof(hbe::StrokeInstance),
                                                  VK_VERTEX_INPUT_RATE_INSTANCE};
            VkVertexInputAttributeDescription sattrs[5] = {
                {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(hbe::StrokeInstance, posOS)},
                {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(hbe::StrokeInstance, normalOS)},
                {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(hbe::StrokeInstance, tangentOS)},
                {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(hbe::StrokeInstance, uv)},
                {4, 0, VK_FORMAT_R32_SFLOAT, offsetof(hbe::StrokeInstance, seed)},
            };
            VkPipelineVertexInputStateCreateInfo svi{
                VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            svi.vertexBindingDescriptionCount = 1;
            svi.pVertexBindingDescriptions = &sbind;
            svi.vertexAttributeDescriptionCount = 5;
            svi.pVertexAttributeDescriptions = sattrs;
            VkPipelineColorBlendAttachmentState scba[3]{};
            for (auto& a : scba) { a.colorWriteMask = rgba; a.blendEnable = VK_FALSE; }
            scba[0].blendEnable = VK_TRUE;
            scba[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            scba[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            scba[0].colorBlendOp = VK_BLEND_OP_ADD;
            scba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            scba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            scba[0].alphaBlendOp = VK_BLEND_OP_ADD;
            scba[1].colorWriteMask = 0; // keep G-buffer
            scba[2].colorWriteMask = 0; // keep velocity
            VkPipelineColorBlendStateCreateInfo scb = cb;
            scb.attachmentCount = 3;
            scb.pAttachments = scba;
            VkPipelineDepthStencilStateCreateInfo sds = ds;
            sds.depthWriteEnable = VK_FALSE;
            sds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
            VkGraphicsPipelineCreateInfo spp = pci;
            spp.pStages = sst;
            spp.pVertexInputState = &svi;
            spp.pRasterizationState = &rs; // strokes never cull
            spp.pDepthStencilState = &sds;
            spp.pColorBlendState = &scb;
            strokeSurfaceReady_ = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &spp,
                                                            nullptr, &strokeSurfacePipe_) ==
                                  VK_SUCCESS;
            if (!strokeSurfaceReady_)
                HBE_WARN("[Vulkan] Stroke-surface pipeline failed; 3D painterly disabled.");
            vkDestroyShaderModule(device_, svs, nullptr);
            vkDestroyShaderModule(device_, sps, nullptr);
        }
    }
    // Single-colour variant for the editor preview mini-pass (previewRenderPass_,
    // no G-buffer/velocity). The shared PS still writes location 1/2; Vulkan
    // ignores fragment outputs without a matching attachment.
    if (r == VK_SUCCESS && postPipelinesReady_) {
        VkPipelineColorBlendStateCreateInfo cbSingle = cb;
        cbSingle.attachmentCount = 1;
        VkGraphicsPipelineCreateInfo single = pci;
        single.pColorBlendState = &cbSingle;
        single.renderPass = previewRenderPass_;
        if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &single, nullptr,
                                      &meshPipelineSingle_) != VK_SUCCESS) {
            HBE_WARN("[Vulkan] Single-colour mesh pipeline failed; preview disabled.");
            meshPipelineSingle_ = VK_NULL_HANDLE;
        }
    }
    vkDestroyShaderModule(device_, vs, nullptr);
    vkDestroyShaderModule(device_, ps, nullptr);
    if (r != VK_SUCCESS) {
        HBE_ERROR("[Vulkan] vkCreateGraphicsPipelines failed (VkResult={})", static_cast<i32>(r));
        return false;
    }
    HBE_INFO("[Vulkan] Mesh PBR pipeline created.");

    // Sky background pipeline: fullscreen triangle (no vertex input) at the far
    // plane; LESS_EQUAL depth test, no depth writes. Same layout/render pass as
    // the mesh pipeline (also compatible with the offscreen viewport pass).
    VkShaderModule skyVs = LoadShaderModule(dir + L"Sky.vs.spv");
    VkShaderModule skyPs = LoadShaderModule(dir + L"Sky.ps.spv");
    if (skyVs != VK_NULL_HANDLE && skyPs != VK_NULL_HANDLE) {
        stages[0].module = skyVs;
        stages[1].module = skyPs;

        VkPipelineVertexInputStateCreateInfo skyVi{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

        VkPipelineDepthStencilStateCreateInfo skyDs = ds;
        skyDs.depthWriteEnable = VK_FALSE;
        skyDs.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

        // The sky only writes colour; mask off the G-buffer + velocity
        // attachments so sky pixels keep their cleared "no surface" values.
        VkPipelineColorBlendAttachmentState skyCba[3];
        for (u32 i = 0; i < 3; ++i) {
            skyCba[i] = cbaArr[i];
            if (i > 0) skyCba[i].colorWriteMask = 0;
        }
        VkPipelineColorBlendStateCreateInfo skyCb = cb;
        skyCb.pAttachments = skyCba;

        VkGraphicsPipelineCreateInfo skyPci = pci;
        skyPci.pRasterizationState = &rs; // fullscreen sky: never cull
        skyPci.pVertexInputState = &skyVi;
        skyPci.pDepthStencilState = &skyDs;
        skyPci.pColorBlendState = &skyCb;

        if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &skyPci, nullptr,
                                      &skyPipeline_) != VK_SUCCESS) {
            HBE_WARN("[Vulkan] Sky pipeline creation failed; no background pass.");
            skyPipeline_ = VK_NULL_HANDLE;
        }

        // Single-colour sky variant for the preview mini-pass.
        if (postPipelinesReady_ && skyPipeline_ != VK_NULL_HANDLE) {
            VkPipelineColorBlendStateCreateInfo skyCbSingle = cb;
            skyCbSingle.attachmentCount = 1;
            skyCbSingle.pAttachments = cbaArr;
            VkGraphicsPipelineCreateInfo skyPciSingle = skyPci;
            skyPciSingle.pColorBlendState = &skyCbSingle;
            skyPciSingle.renderPass = previewRenderPass_;
            if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &skyPciSingle, nullptr,
                                          &skyPipelineSingle_) != VK_SUCCESS) {
                skyPipelineSingle_ = VK_NULL_HANDLE;
            }
        }
    } else {
        HBE_WARN("[Vulkan] Sky SPIR-V not found; no background pass.");
    }
    if (skyVs) vkDestroyShaderModule(device_, skyVs, nullptr);
    if (skyPs) vkDestroyShaderModule(device_, skyPs, nullptr);

    // In-game UI overlay pipeline: textured 2D triangles, alpha blend, no
    // depth. Reuses the mesh pipeline layout so the shader sees the bindless
    // texture set (set 1); same render pass as the scene.
    {
        VkShaderModule uiVs = LoadShaderModule(dir + L"UI.vs.spv");
        VkShaderModule uiPs = LoadShaderModule(dir + L"UI.ps.spv");
        bool ok = uiVs != VK_NULL_HANDLE && uiPs != VK_NULL_HANDLE;
        if (ok) {
            stages[0].module = uiVs;
            stages[1].module = uiPs;

            VkVertexInputBindingDescription uiBinding{0, sizeof(UIVertex),
                                                      VK_VERTEX_INPUT_RATE_VERTEX};
            VkVertexInputAttributeDescription uiAttrs[5] = {
                {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(UIVertex, x)},
                {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(UIVertex, u)},
                {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(UIVertex, r)},
                {3, 0, VK_FORMAT_R32_UINT, offsetof(UIVertex, texIndex)},
                {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                 offsetof(UIVertex, clipX0)}, // NDC clip rect
            };
            VkPipelineVertexInputStateCreateInfo uiVi{
                VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            uiVi.vertexBindingDescriptionCount = 1;
            uiVi.pVertexBindingDescriptions = &uiBinding;
            uiVi.vertexAttributeDescriptionCount = 5;
            uiVi.pVertexAttributeDescriptions = uiAttrs;

            VkPipelineDepthStencilStateCreateInfo uiDs{
                VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};

            VkPipelineColorBlendAttachmentState uiCba = cbaArr[0];
            uiCba.blendEnable = VK_TRUE;
            uiCba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            uiCba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            uiCba.colorBlendOp = VK_BLEND_OP_ADD;
            uiCba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            uiCba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            uiCba.alphaBlendOp = VK_BLEND_OP_ADD;
            VkPipelineColorBlendStateCreateInfo uiCb = cb;
            uiCb.attachmentCount = 1; // UI target is single-colour (cb may be MRT)
            uiCb.pAttachments = &uiCba;

            VkGraphicsPipelineCreateInfo uiPci = pci;
            uiPci.pVertexInputState = &uiVi;
            uiPci.pDepthStencilState = &uiDs;
            uiPci.pColorBlendState = &uiCb;
            // UI is screen-space 2D: never cull. `pci` still points at rsCull
            // (the opaque mesh's back-face cull), which would drop the
            // oppositely-wound UI quads, so force the double-sided state.
            uiPci.pRasterizationState = &rs;
            uiPci.layout = pipelineLayout_; // mesh layout: set 1 = bindless
            // The UI overlay draws into the post-FXAA final target (swapchain
            // or viewport pass), not the HDR scene pass.
            uiPci.renderPass = renderPass_;

            ok = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &uiPci, nullptr,
                                           &uiPipeline_) == VK_SUCCESS;

            // World-UI variant: the same UI pipeline against a small offscreen
            // pass (canvas texture a lit quad in the scene samples). The pass's
            // finalLayout hands the image to the fragment shader - it does ALL
            // layout transitions (vpRenderPass_ precedent). Failure only disables
            // world-space canvases, never the overlay.
            if (ok && worldUIRenderPass_ == VK_NULL_HANDLE) {
                VkAttachmentDescription color{};
                color.format = VK_FORMAT_R8G8B8A8_UNORM;
                color.samples = VK_SAMPLE_COUNT_1_BIT;
                color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
                VkSubpassDescription subpass{};
                subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
                subpass.colorAttachmentCount = 1;
                subpass.pColorAttachments = &colorRef;

                VkSubpassDependency deps[2]{};
                deps[0].srcSubpass = VK_SUBPASS_EXTERNAL; // prior frame's sampling
                deps[0].dstSubpass = 0;
                deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
                deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
                deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                deps[1].srcSubpass = 0; // this frame's scene pass samples the page
                deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
                deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
                deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

                VkRenderPassCreateInfo rci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
                rci.attachmentCount = 1;
                rci.pAttachments = &color;
                rci.subpassCount = 1;
                rci.pSubpasses = &subpass;
                rci.dependencyCount = 2;
                rci.pDependencies = deps;
                if (vkCreateRenderPass(device_, &rci, nullptr, &worldUIRenderPass_) !=
                    VK_SUCCESS)
                    worldUIRenderPass_ = VK_NULL_HANDLE;
            }
            if (ok && worldUIRenderPass_ != VK_NULL_HANDLE) {
                VkGraphicsPipelineCreateInfo uiwPci = uiPci;
                uiwPci.renderPass = worldUIRenderPass_;
                if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &uiwPci, nullptr,
                                              &uiWorldPipeline_) != VK_SUCCESS) {
                    HBE_WARN("[Vulkan] world-UI pipeline unavailable.");
                    uiWorldPipeline_ = VK_NULL_HANDLE;
                }
            }
        }
        const VkMemoryPropertyFlags hostVisible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (u32 i = 0; ok && i < framesInFlight_; ++i) {
            ok = CreateBuffer(kUIVertexBufferSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              hostVisible, uiVertexBuffers_[i], uiVertexMemory_[i]);
            if (ok) {
                void* mapped = nullptr;
                ok = vkMapMemory(device_, uiVertexMemory_[i], 0, kUIVertexBufferSize, 0,
                                 &mapped) == VK_SUCCESS;
                uiVertexCpu_[i] = static_cast<u8*>(mapped);
            }
        }
        // World-UI vertex buffers: SEPARATE from the overlay's (offset-0 memcpy
        // per call would alias) - bump-allocated across the frame per canvas.
        if (uiWorldPipeline_ != VK_NULL_HANDLE) {
            for (u32 i = 0; i < framesInFlight_; ++i) {
                bool wok = CreateBuffer(kUIVertexBufferSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                        hostVisible, uiWorldVertexBuffers_[i],
                                        uiWorldVertexMemory_[i]);
                if (wok) {
                    void* mapped = nullptr;
                    wok = vkMapMemory(device_, uiWorldVertexMemory_[i], 0, kUIVertexBufferSize,
                                      0, &mapped) == VK_SUCCESS;
                    uiWorldVertexCpu_[i] = static_cast<u8*>(mapped);
                }
                if (!wok) {
                    HBE_WARN("[Vulkan] world-UI vertex buffers unavailable.");
                    vkDestroyPipeline(device_, uiWorldPipeline_, nullptr);
                    uiWorldPipeline_ = VK_NULL_HANDLE;
                    break;
                }
            }
        }
        // Particle vertex buffers (host-visible, mapped); same lifetime as UI.
        if (particlePipeline_ != VK_NULL_HANDLE) {
            for (u32 i = 0; i < framesInFlight_; ++i) {
                if (!CreateBuffer(kParticleVertexBufferSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                  hostVisible, particleVertexBuffers_[i],
                                  particleVertexMemory_[i])) {
                    particlePipeline_ = particlePipelineAdd_ = VK_NULL_HANDLE;
                    break;
                }
                void* mapped = nullptr;
                if (vkMapMemory(device_, particleVertexMemory_[i], 0, kParticleVertexBufferSize,
                                0, &mapped) == VK_SUCCESS) {
                    particleVertexCpu_[i] = static_cast<u8*>(mapped);
                }
            }
        }
        if (!ok) {
            HBE_WARN("[Vulkan] UI overlay pipeline unavailable.");
            if (uiPipeline_) vkDestroyPipeline(device_, uiPipeline_, nullptr);
            uiPipeline_ = VK_NULL_HANDLE;
        }
        if (uiVs) vkDestroyShaderModule(device_, uiVs, nullptr);
        if (uiPs) vkDestroyShaderModule(device_, uiPs, nullptr);
    }
    return true;
}

bool VulkanDevice::CreateShadowResources() {
    // --- Depth image sampled by the PBR pass through the bindless array ----
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_D32_SFLOAT;
    ici.extent = {kShadowDim, kShadowDim, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(device_, &ici, nullptr, &shadowImage_), "vkCreateImage(shadow)");

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, shadowImage_, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(device_, &mai, nullptr, &shadowMemory_), "vkAllocateMemory(shadow)");
    vkBindImageMemory(device_, shadowImage_, shadowMemory_, 0);

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = shadowImage_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_D32_SFLOAT;
    vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &shadowView_), "vkCreateImageView(shadow)");

    // --- Depth-only render pass: clear -> write -> sample -------------------
    VkAttachmentDescription depth{};
    depth.format = VK_FORMAT_D32_SFLOAT;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference depthRef{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 1;
    rpci.pAttachments = &depth;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = 2;
    rpci.pDependencies = deps;
    VK_CHECK(vkCreateRenderPass(device_, &rpci, nullptr, &shadowRenderPass_),
             "vkCreateRenderPass(shadow)");

    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = shadowRenderPass_;
    fci.attachmentCount = 1;
    fci.pAttachments = &shadowView_;
    fci.width = kShadowDim;
    fci.height = kShadowDim;
    fci.layers = 1;
    VK_CHECK(vkCreateFramebuffer(device_, &fci, nullptr, &shadowFramebuffer_),
             "vkCreateFramebuffer(shadow)");

    // --- Depth-only pipeline: MeshPBR vertex shader, no fragment stage ------
    const std::wstring dir = ExecutableDir() + L"shaders\\";
    VkShaderModule vs = LoadShaderModule(dir + L"MeshPBR.vs.spv");
    if (vs == VK_NULL_HANDLE) return false;

    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    stage.module = vs;
    stage.pName = "VSMain";

    VkVertexInputBindingDescription binding{0, sizeof(hbe::Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[6] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(hbe::Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(hbe::Vertex, normal)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(hbe::Vertex, tangent)},
        {3, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(hbe::Vertex, uv)},
        {4, 0, VK_FORMAT_R16G16B16A16_UINT,   offsetof(hbe::Vertex, joints)},
        {5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(hbe::Vertex, weights)},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 6;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    rs.depthBiasEnable = VK_TRUE;
    rs.depthBiasConstantFactor = 1.25f;
    rs.depthBiasSlopeFactor = 2.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 0;

    const VkDynamicState dynamics[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynamics;

    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.stageCount = 1;
    pci.pStages = &stage;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pDepthStencilState = &ds;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &dyn;
    pci.layout = pipelineLayout_;
    pci.renderPass = shadowRenderPass_;
    pci.subpass = 0;

    const VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pci, nullptr,
                                                 &shadowPipeline_);
    vkDestroyShaderModule(device_, vs, nullptr);
    if (r != VK_SUCCESS) {
        HBE_ERROR("[Vulkan] Shadow pipeline creation failed (VkResult={})", static_cast<i32>(r));
        return false;
    }

    // Reserve a bindless slot for the PBR pass to sample.
    if (bindlessNextSlot_ >= kMaxBindlessTextures) return false;
    shadowSrvSlot_ = bindlessNextSlot_++;
    VkDescriptorImageInfo ii{};
    ii.imageView = shadowView_;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = bindlessSet_;
    w.dstBinding = 1;
    w.dstArrayElement = shadowSrvSlot_;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);

    shadowReady_ = true;
    HBE_INFO("[Vulkan] Cascaded shadow atlas ready ({0}x{0}, {1} tiles, bindless slot {2}).",
             kShadowDim, kMaxShadowCascades, shadowSrvSlot_);
    return true;
}

bool VulkanDevice::CreatePostRenderPasses() {
    // Single-color-attachment pass: optionally loads the previous contents
    // (additive bloom upsample), always ends sampleable by the next pass.
    const auto makeColorPass = [&](VkFormat format, bool load, VkRenderPass& out) -> bool {
        VkAttachmentDescription color{};
        color.format = format;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = load ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                   : VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &colorRef;

        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass = 0;
        deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
        deps[1].srcSubpass = 0;
        deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        ci.attachmentCount = 1;
        ci.pAttachments = &color;
        ci.subpassCount = 1;
        ci.pSubpasses = &sub;
        ci.dependencyCount = 2;
        ci.pDependencies = deps;
        VK_CHECK(vkCreateRenderPass(device_, &ci, nullptr, &out), "vkCreateRenderPass(post)");
        return true;
    };

    if (!makeColorPass(VK_FORMAT_R16G16B16A16_SFLOAT, false, postPass16_)) return false;
    if (!makeColorPass(VK_FORMAT_R16G16B16A16_SFLOAT, true, postPass16Load_)) return false;
    if (!makeColorPass(VK_FORMAT_R8G8B8A8_UNORM, false, postPass8_)) return false;

    // Shared subpass dependencies for the scene passes (color + depth, sampled
    // afterwards by the post stack).
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    // Builds a scene render pass with `colorCount` color attachments + depth,
    // all stored and left SHADER_READ_ONLY for the post stack. colorFormats[i]
    // gives each color attachment's format.
    const auto makeScenePass = [&](u32 colorCount, const VkFormat* colorFormats,
                                   VkRenderPass& out) -> bool {
        VkAttachmentDescription atts[4]{};
        VkAttachmentReference colorRefs[3]{};
        for (u32 i = 0; i < colorCount; ++i) {
            atts[i].format = colorFormats[i];
            atts[i].samples = VK_SAMPLE_COUNT_1_BIT;
            atts[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            atts[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            atts[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            atts[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            atts[i].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            atts[i].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            colorRefs[i] = {i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        }
        VkAttachmentDescription& depth = atts[colorCount];
        depth = atts[0];
        depth.format = depthFormat_;
        VkAttachmentReference depthRef{colorCount, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = colorCount;
        sub.pColorAttachments = colorRefs;
        sub.pDepthStencilAttachment = &depthRef;

        VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        ci.attachmentCount = colorCount + 1;
        ci.pAttachments = atts;
        ci.subpassCount = 1;
        ci.pSubpasses = &sub;
        ci.dependencyCount = 2;
        ci.pDependencies = deps;
        VK_CHECK(vkCreateRenderPass(device_, &ci, nullptr, &out), "vkCreateRenderPass(scene)");
        return true;
    };

    // Main HDR scene pass: colour + thin G-buffer + velocity + depth, all
    // sampled by the post stack afterwards.
    const VkFormat hdrFormats[3] = {VK_FORMAT_R16G16B16A16_SFLOAT,   // colour
                                    VK_FORMAT_R16G16B16A16_SFLOAT,   // G-buffer
                                    VK_FORMAT_R16G16_SFLOAT};        // velocity
    if (!makeScenePass(3, hdrFormats, hdrRenderPass_)) return false;
    // Editor preview mini-pass: single colour + depth (no G-buffer/velocity).
    const VkFormat prevFormats[1] = {VK_FORMAT_R16G16B16A16_SFLOAT};
    if (!makeScenePass(1, prevFormats, previewRenderPass_)) return false;

    // Depth-graded water pass: the same colour+G-buffer+velocity+depth attachments as
    // hdrRenderPass_ (so hdr_.framebuffer is compatible), but LOADED (the lit scene is
    // preserved) and with the depth attachment in DEPTH_STENCIL_READ_ONLY_OPTIMAL. That
    // lets the water PS sample the scene depth (slotDepthRO_) while depth-TESTING against
    // it in the same subpass - water + particles write no depth, so read-only is safe.
    // Ends by handing colour + depth back SHADER_READ_ONLY, exactly like hdrRenderPass_,
    // so RunPostStack (which ends this pass) and its slotDepth_ sampling are unchanged.
    {
        VkAttachmentDescription atts[4]{};
        VkAttachmentReference colorRefs[3]{};
        for (u32 i = 0; i < 3; ++i) {
            atts[i].format = hdrFormats[i];
            atts[i].samples = VK_SAMPLE_COUNT_1_BIT;
            atts[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;   // preserve the lit HDR scene
            atts[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            atts[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            atts[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            atts[i].initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; // where hdrRenderPass_ left it
            atts[i].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;   // hand back to post
            colorRefs[i] = {i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        }
        VkAttachmentDescription& depth = atts[3];
        depth = atts[0];
        depth.format = depthFormat_;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;   // preserve the scene depth for testing + sampling
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE; // post stack samples it (slotDepth_)
        // Read-only: usable as a read-only depth attachment AND a sampled image at once.
        VkAttachmentReference depthRef{3, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};

        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 3;
        sub.pColorAttachments = colorRefs;
        sub.pDepthStencilAttachment = &depthRef;

        // External deps: the prior HDR pass left everything SHADER_READ_ONLY; make it available
        // for colour blend (read+write), read-only depth-test, and depth sampling; then hand it
        // back for the post stack's fragment-shader reads.
        VkSubpassDependency wdeps[2]{};
        wdeps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        wdeps[0].dstSubpass = 0;
        wdeps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        wdeps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        wdeps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        wdeps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
        wdeps[1].srcSubpass = 0;
        wdeps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        wdeps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        wdeps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        wdeps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        wdeps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        ci.attachmentCount = 4;
        ci.pAttachments = atts;
        ci.subpassCount = 1;
        ci.pSubpasses = &sub;
        ci.dependencyCount = 2;
        ci.pDependencies = wdeps;
        VK_CHECK(vkCreateRenderPass(device_, &ci, nullptr, &hdrWaterRenderPass_),
                 "vkCreateRenderPass(hdrWater)");
    }
    return true;
}

bool VulkanDevice::CreatePostPipelines() {
    const std::wstring dir = ExecutableDir() + L"shaders\\";

    // All post passes: fullscreen triangle, no vertex input, no depth ops.
    // blend: 0 = opaque, 1 = additive (ONE/ONE), 2 = straight alpha-over.
    // (bool callers implicitly convert: false->0 opaque, true->1 additive.)
    const auto makePipe = [&](const wchar_t* name, VkRenderPass pass, int blend,
                              VkPipeline& out) -> bool {
        VkShaderModule vs = LoadShaderModule(dir + name + std::wstring(L".vs.spv"));
        VkShaderModule ps = LoadShaderModule(dir + name + std::wstring(L".ps.spv"));
        if (vs == VK_NULL_HANDLE || ps == VK_NULL_HANDLE) {
            if (vs) vkDestroyShaderModule(device_, vs, nullptr);
            if (ps) vkDestroyShaderModule(device_, ps, nullptr);
            return false;
        }
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName = "VSMain";
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = ps;
        stages[1].pName = "PSMain";

        VkPipelineVertexInputStateCreateInfo vi{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        if (blend == 1) { // additive
            cba.blendEnable = VK_TRUE;
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.colorBlendOp = VK_BLEND_OP_ADD;
            cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.alphaBlendOp = VK_BLEND_OP_ADD;
        } else if (blend == 2) { // straight alpha over
            cba.blendEnable = VK_TRUE;
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cba.colorBlendOp = VK_BLEND_OP_ADD;
            cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cba.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        VkPipelineColorBlendStateCreateInfo cb{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &cba;
        const VkDynamicState dynamics[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn{
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates = dynamics;

        VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pci.stageCount = 2;
        pci.pStages = stages;
        pci.pVertexInputState = &vi;
        pci.pInputAssemblyState = &ia;
        pci.pViewportState = &vp;
        pci.pRasterizationState = &rs;
        pci.pMultisampleState = &ms;
        pci.pDepthStencilState = &ds;
        pci.pColorBlendState = &cb;
        pci.pDynamicState = &dyn;
        pci.layout = pipelineLayout_;
        pci.renderPass = pass;
        pci.subpass = 0;

        const VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pci, nullptr, &out);
        vkDestroyShaderModule(device_, vs, nullptr);
        vkDestroyShaderModule(device_, ps, nullptr);
        return r == VK_SUCCESS;
    };

    bool ok = true;
    ok = ok && makePipe(L"SSAO", postPass8_, false, ssaoPipe_);
    ok = ok && makePipe(L"SSAOBlur", postPass8_, false, ssaoBlurPipe_);
    ok = ok && makePipe(L"BloomDown", postPass16_, false, bloomDownPipe_);
    ok = ok && makePipe(L"BloomUp", postPass16Load_, true, bloomUpPipe_);
    ok = ok && makePipe(L"Tonemap", postPass8_, false, tonemapPipe_);
    // FXAA renders into the final pass (swapchain render pass; also compatible
    // with the editor viewport pass, which has identical attachments).
    ok = ok && makePipe(L"FXAA", renderPass_, false, fxaaPipe_);
    // TAA is optional (its absence disables only temporal AA); RGBA8 like LDR.
    taaReady_ = makePipe(L"TAA", postPass8_, false, taaPipe_);
    dofReady_ = makePipe(L"DoF", postPass8_, false, dofPipe_);
    motionBlurReady_ = makePipe(L"MotionBlur", postPass8_, false, motionBlurPipe_);
    ssrReady_ = makePipe(L"SSR", postPass16_, false, ssrPipe_);            // HDR target
    exposureReady_ = makePipe(L"Exposure", postPass16_, false, exposurePipe_); // 1x1 HDR
    volReady_ = makePipe(L"VolumetricFog", postPass16_, false, volPipe_); // HDR composite
    volPartReady_ = makePipe(L"VolumetricParticles", postPass16_, false, volPartPipe_); // raymarch
    // NanoVDB baked-volume raymarch (the runtime volume path): same half-res target/blend as the
    // splat raymarch, but samples a StructuredBuffer<uint> grid via PNanoVDB (post-set binding 6).
    volRaymarchReady_ = makePipe(L"VolumeRaymarch", postPass16_, false, volRaymarchPipe_);
    ssgiReady_ = makePipe(L"SSGI", postPass16_, false, ssgiPipe_);        // HDR composite
    painterlyReady_ = makePipe(L"Painterly", postPass16_, false, painterlyPipe_); // HDR repaint
    // Brush-stroke splat: alpha-over, drawn into the painterly target with LOAD so
    // strokes accumulate over the Kuwahara base (same render pass bloom-up uses).
    brushStrokesReady_ = makePipe(L"BrushStrokes", postPass16Load_, 2, brushStrokesPipe_);
    // NOTE: no "Copy" (passthrough upscale) pipeline. It existed for the half-res
    // Kuwahara underpainting, which was abandoned - the painterly pass runs FULL res now
    // (see the comment at the Kuwahara draw), so nothing ever bound it. It was still
    // gating `ok`, which meant a missing Copy shader silently killed the ENTIRE post
    // stack for a pass that never ran. D3D12 never had one; the backends now agree.
    // Reduced-res effect composite (SSGI GI / fog) upscaled back over the full-res HDR.
    ok = ok && makePipe(L"ApplyHalfRes", postPass16_, false, applyPipe_);
    // Dynamic-layer composite: lerp painterly vs crisp lit colour by the HDR-alpha mask.
    ok = ok && makePipe(L"PainterlyComposite", postPass16_, false, compositePipe_);
    return ok;
}

bool VulkanDevice::CreatePostTarget(u32 w, u32 h, VkFormat fmt, VkRenderPass pass,
                                    u32 srvSlot, PostTargetVk& out) {
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(device_, &ici, nullptr, &out.image), "vkCreateImage(post)");

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, out.image, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(device_, &mai, nullptr, &out.memory), "vkAllocateMemory(post)");
    vkBindImageMemory(device_, out.image, out.memory, 0);

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = out.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = fmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &out.view), "vkCreateImageView(post)");

    if (pass != VK_NULL_HANDLE) {
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = pass;
        fci.attachmentCount = 1;
        fci.pAttachments = &out.view;
        fci.width = w;
        fci.height = h;
        fci.layers = 1;
        VK_CHECK(vkCreateFramebuffer(device_, &fci, nullptr, &out.framebuffer),
                 "vkCreateFramebuffer(post)");
    }
    out.width = w;
    out.height = h;

    // Register (or refresh, after a resize) the bindless slot in place.
    VkDescriptorImageInfo ii{};
    ii.imageView = out.view;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet wds{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wds.dstSet = bindlessSet_;
    wds.dstBinding = 1;
    wds.dstArrayElement = srvSlot;
    wds.descriptorCount = 1;
    wds.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    wds.pImageInfo = &ii;
    vkUpdateDescriptorSets(device_, 1, &wds, 0, nullptr);
    return true;
}

void VulkanDevice::DestroyPostTargets() {
    const auto destroy = [&](PostTargetVk& t) {
        if (t.framebuffer) { vkDestroyFramebuffer(device_, t.framebuffer, nullptr); t.framebuffer = VK_NULL_HANDLE; }
        if (t.view) { vkDestroyImageView(device_, t.view, nullptr); t.view = VK_NULL_HANDLE; }
        if (t.image) { vkDestroyImage(device_, t.image, nullptr); t.image = VK_NULL_HANDLE; }
        if (t.memory) { vkFreeMemory(device_, t.memory, nullptr); t.memory = VK_NULL_HANDLE; }
        t.width = t.height = 0;
    };
    destroy(hdr_);
    destroy(gbuffer_);
    destroy(velocity_);
    destroy(ssaoRaw_);
    destroy(ssaoBlur_);
    destroy(ldr_);
    destroy(taaHistory_[0]);
    destroy(taaHistory_[1]);
    destroy(dof_);
    destroy(motionBlur_);
    destroy(ssr_);
    destroy(ssgi_);
    destroy(ssgiHalf_);
    destroy(painterly_);
    destroy(painterlyHalf_);
    destroy(painterlyComp_);
    destroy(vol_);
    destroy(volHalf_);
    destroy(volPart_);
    destroy(volPartHalf_);
    destroy(adaptedLum_[0]);
    destroy(adaptedLum_[1]);
    for (u32 i = 0; i < kBloomMaxMips; ++i) destroy(bloom_[i]);
    if (hdrDepthView_) { vkDestroyImageView(device_, hdrDepthView_, nullptr); hdrDepthView_ = VK_NULL_HANDLE; }
    if (hdrDepth_) { vkDestroyImage(device_, hdrDepth_, nullptr); hdrDepth_ = VK_NULL_HANDLE; }
    if (hdrDepthMem_) { vkFreeMemory(device_, hdrDepthMem_, nullptr); hdrDepthMem_ = VK_NULL_HANDLE; }
    bloomCount_ = 0;
}

bool VulkanDevice::CreatePostTargets(u32 width, u32 height) {
    if (width == 0 || height == 0) return false;
    vkDeviceWaitIdle(device_);
    DestroyPostTargets();

    // Reserve the bindless slots once; resizes rewrite the same descriptors.
    if (slotHdr_ == 0) {
        if (bindlessNextSlot_ + 24 + kBloomMaxMips > kMaxBindlessTextures) return false;
        slotHdr_ = bindlessNextSlot_++;
        slotDepth_ = bindlessNextSlot_++;
        slotDepthRO_ = bindlessNextSlot_++; // depth sampled during the read-only-depth water pass
        slotGbuffer_ = bindlessNextSlot_++;
        slotVelocity_ = bindlessNextSlot_++;
        slotVol_ = bindlessNextSlot_++;
        slotVolHalf_ = bindlessNextSlot_++;
        slotVolPart_ = bindlessNextSlot_++;
        slotVolPartHalf_ = bindlessNextSlot_++;
        slotSsgi_ = bindlessNextSlot_++;
        slotSsgiHalf_ = bindlessNextSlot_++;
        slotPainterly_ = bindlessNextSlot_++;
        slotPainterlyHalf_ = bindlessNextSlot_++;
        slotPainterlyComp_ = bindlessNextSlot_++;
        slotSsaoRaw_ = bindlessNextSlot_++;
        slotSsaoBlur_ = bindlessNextSlot_++;
        slotLdr_ = bindlessNextSlot_++;
        slotTaaHistory_[0] = bindlessNextSlot_++;
        slotTaaHistory_[1] = bindlessNextSlot_++;
        slotDof_ = bindlessNextSlot_++;
        slotMotionBlur_ = bindlessNextSlot_++;
        slotSsr_ = bindlessNextSlot_++;
        slotAdaptedLum_[0] = bindlessNextSlot_++;
        slotAdaptedLum_[1] = bindlessNextSlot_++;
        for (u32 i = 0; i < kBloomMaxMips; ++i) slotBloom_[i] = bindlessNextSlot_++;
    }

    // HDR color (framebuffer made manually: it pairs with the depth below).
    if (!CreatePostTarget(width, height, VK_FORMAT_R16G16B16A16_SFLOAT, VK_NULL_HANDLE,
                          slotHdr_, hdr_)) {
        return false;
    }

    // Sampleable scene depth.
    {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = depthFormat_;
        ici.extent = {width, height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(device_, &ici, nullptr, &hdrDepth_), "vkCreateImage(hdrDepth)");
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device_, hdrDepth_, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkAllocateMemory(device_, &mai, nullptr, &hdrDepthMem_), "vkAllocateMemory(hdrDepth)");
        vkBindImageMemory(device_, hdrDepth_, hdrDepthMem_, 0);
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = hdrDepth_;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = depthFormat_;
        vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &hdrDepthView_),
                 "vkCreateImageView(hdrDepth)");

        VkDescriptorImageInfo ii{};
        ii.imageView = hdrDepthView_;
        ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet wds{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wds.dstSet = bindlessSet_;
        wds.dstBinding = 1;
        wds.dstArrayElement = slotDepth_;
        wds.descriptorCount = 1;
        wds.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        wds.pImageInfo = &ii;
        vkUpdateDescriptorSets(device_, 1, &wds, 0, nullptr);

        // Second view of the SAME depth image, declared DEPTH_STENCIL_READ_ONLY_OPTIMAL to match
        // the layout the image is in during the read-only-depth water pass (hdrWaterRenderPass_).
        // The post stack keeps sampling slotDepth_ (SHADER_READ_ONLY); only the water PS reads this.
        VkDescriptorImageInfo iiRo = ii;
        iiRo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet wdsRo = wds;
        wdsRo.dstArrayElement = slotDepthRO_;
        wdsRo.pImageInfo = &iiRo;
        vkUpdateDescriptorSets(device_, 1, &wdsRo, 0, nullptr);

        // Thin G-buffer + velocity, written by the forward pass alongside the
        // colour. No own framebuffer; they are attachments 1/2 of the HDR pass.
        if (!CreatePostTarget(width, height, VK_FORMAT_R16G16B16A16_SFLOAT, VK_NULL_HANDLE,
                              slotGbuffer_, gbuffer_))
            return false;
        if (!CreatePostTarget(width, height, VK_FORMAT_R16G16_SFLOAT, VK_NULL_HANDLE,
                              slotVelocity_, velocity_))
            return false;

        const VkImageView attachments[4] = {hdr_.view, gbuffer_.view, velocity_.view,
                                            hdrDepthView_};
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = hdrRenderPass_;
        fci.attachmentCount = 4;
        fci.pAttachments = attachments;
        fci.width = width;
        fci.height = height;
        fci.layers = 1;
        VK_CHECK(vkCreateFramebuffer(device_, &fci, nullptr, &hdr_.framebuffer),
                 "vkCreateFramebuffer(hdr)");
    }

    // Half-resolution SSAO targets.
    const u32 sw = std::max(1u, width / 2);
    const u32 sh = std::max(1u, height / 2);
    if (!CreatePostTarget(sw, sh, VK_FORMAT_R8G8B8A8_UNORM, postPass8_, slotSsaoRaw_, ssaoRaw_))
        return false;
    if (!CreatePostTarget(sw, sh, VK_FORMAT_R8G8B8A8_UNORM, postPass8_, slotSsaoBlur_, ssaoBlur_))
        return false;

    // Bloom pyramid from half resolution down (stop above 8px).
    bloomCount_ = 0;
    for (u32 i = 0; i < kBloomMaxMips; ++i) {
        const u32 w = std::max(1u, width >> (i + 1));
        const u32 h = std::max(1u, height >> (i + 1));
        if (w < 8 || h < 8) break;
        if (!CreatePostTarget(w, h, VK_FORMAT_R16G16B16A16_SFLOAT, postPass16_, slotBloom_[i],
                              bloom_[i])) {
            return false;
        }
        ++bloomCount_;
    }

    // Tonemapped LDR (pre-FXAA).
    if (!CreatePostTarget(width, height, VK_FORMAT_R8G8B8A8_UNORM, postPass8_, slotLdr_, ldr_))
        return false;

    // TAA history (ping-pong). Transition both to SHADER_READ_ONLY up front so the
    // first frame can bind them safely; the TAA render pass (discard) overwrites
    // whichever it targets. They are only sampled once a frame has written one
    // (gated by taaHistoryValid_).
    if (taaReady_) {
        if (!CreatePostTarget(width, height, VK_FORMAT_R8G8B8A8_UNORM, postPass8_,
                              slotTaaHistory_[0], taaHistory_[0]))
            return false;
        if (!CreatePostTarget(width, height, VK_FORMAT_R8G8B8A8_UNORM, postPass8_,
                              slotTaaHistory_[1], taaHistory_[1]))
            return false;
        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool = commandPool_;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(device_, &cbai, &cmd);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        for (const PostTargetVk& h : taaHistory_) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = h.image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                                 nullptr, 1, &b);
        }
        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue_);
        vkFreeCommandBuffers(device_, commandPool_, 1, &cmd);
        taaHistoryValid_ = false; // contents are undefined after a (re)size
    }

    // Depth-of-field result (LDR).
    if (dofReady_) {
        if (!CreatePostTarget(width, height, VK_FORMAT_R8G8B8A8_UNORM, postPass8_, slotDof_, dof_))
            return false;
    }
    if (motionBlurReady_) {
        if (!CreatePostTarget(width, height, VK_FORMAT_R8G8B8A8_UNORM, postPass8_,
                              slotMotionBlur_, motionBlur_))
            return false;
    }
    if (ssrReady_) { // HDR (composites reflections before tonemap)
        if (!CreatePostTarget(width, height, VK_FORMAT_R16G16B16A16_SFLOAT, postPass16_, slotSsr_,
                              ssr_))
            return false;
    }
    if (volReady_) { // HDR (volumetric fog composited before bloom)
        if (!CreatePostTarget(width, height, VK_FORMAT_R16G16B16A16_SFLOAT, postPass16_, slotVol_,
                              vol_))
            return false;
        // Fog (inscatter+transmittance) at HALF res (was quarter): the march's
        // per-step sun-shadow is world-scale blocky at grazing sun angles;
        // quarter-res made the blocks obvious. Half-res + the bilateral upscale
        // reads smooth and the fog march stays a small pass.
        const u32 hw = (width + 1u) / 2u, hh = (height + 1u) / 2u;
        if (!CreatePostTarget(hw, hh, VK_FORMAT_R16G16B16A16_SFLOAT, postPass16_, slotVolHalf_,
                              volHalf_))
            return false;
    }
    if (volPartReady_) { // volumetric particles: full-res composite + half-res raymarch
        if (!CreatePostTarget(width, height, VK_FORMAT_R16G16B16A16_SFLOAT, postPass16_,
                              slotVolPart_, volPart_))
            return false;
        const u32 hw = (width + 1u) / 2u, hh = (height + 1u) / 2u;
        if (!CreatePostTarget(hw, hh, VK_FORMAT_R16G16B16A16_SFLOAT, postPass16_, slotVolPartHalf_,
                              volPartHalf_))
            return false;
    }
    if (ssgiReady_) { // HDR (indirect bounce composited before bloom)
        if (!CreatePostTarget(width, height, VK_FORMAT_R16G16B16A16_SFLOAT, postPass16_, slotSsgi_,
                              ssgi_))
            return false;
        // GI-only term at QUARTER res (GI is very low-frequency + TAA-denoised, so a
        // 1/4-res gather + bilinear upscale is near-lossless and ~16x cheaper).
        const u32 qw = (width + 3u) / 4u, qh = (height + 3u) / 4u;
        if (!CreatePostTarget(qw, qh, VK_FORMAT_R16G16B16A16_SFLOAT, postPass16_, slotSsgiHalf_,
                              ssgiHalf_))
            return false;
    }
    if (painterlyReady_) { // HDR (oil-on-canvas repaint before bloom)
        if (!CreatePostTarget(width, height, VK_FORMAT_R16G16B16A16_SFLOAT, postPass16_,
                              slotPainterly_, painterly_))
            return false;
        // Half-res Kuwahara underpainting (upscaled to painterly_ before the strokes).
        const u32 hw = (width + 1u) / 2u, hh = (height + 1u) / 2u;
        if (!CreatePostTarget(hw, hh, VK_FORMAT_R16G16B16A16_SFLOAT, postPass16_,
                              slotPainterlyHalf_, painterlyHalf_))
            return false;
        // Full-res target for the dynamic-layer composite (painterly + crisp objects).
        if (!CreatePostTarget(width, height, VK_FORMAT_R16G16B16A16_SFLOAT, postPass16_,
                              slotPainterlyComp_, painterlyComp_))
            return false;
    }
    if (exposureReady_) { // 1x1 adapted-luminance ping-pong (auto-exposure)
        if (!CreatePostTarget(1, 1, VK_FORMAT_R16G16B16A16_SFLOAT, postPass16_,
                              slotAdaptedLum_[0], adaptedLum_[0]))
            return false;
        if (!CreatePostTarget(1, 1, VK_FORMAT_R16G16B16A16_SFLOAT, postPass16_,
                              slotAdaptedLum_[1], adaptedLum_[1]))
            return false;
        adaptValid_ = false; // no adapted history after a (re)size
    }

    sceneW_ = width;
    sceneH_ = height;
    HBE_INFO("[Vulkan] HDR post targets ready ({}x{}, {} bloom mips).", width, height,
             bloomCount_);
    return true;
}

void VulkanDevice::GpuMark(const char* name) {
    if (!gpuProfile_ || !frameActive_ || gpuCount_ >= kMaxGpuMarks) return;
    vkCmdWriteTimestamp(commandBuffers_[frameIndex_], VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        gpuPool_[frameIndex_], gpuCount_);
    gpuNames_[frameIndex_][gpuCount_] = name;
    ++gpuCount_;
}

u32 VulkanDevice::AllocPostConstants(const PostUBO& cb) {
    const u32 offset = postHead_;
    if (offset + postStride_ > kPostArenaSize) {
        return 0; // arena exhausted; pass reads stale-but-valid constants
    }
    std::memcpy(postArenaMapped_[frameIndex_] + offset, &cb, sizeof(cb));
    postHead_ += static_cast<u32>(postStride_);
    return offset;
}

void VulkanDevice::DrawPostPass(VkPipeline pipe, VkRenderPass pass, const PostTargetVk& target,
                                const PostUBO& cb) {
    VkCommandBuffer cmd = commandBuffers_[frameIndex_];
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = pass;
    rp.framebuffer = target.framebuffer;
    rp.renderArea.extent = {target.width, target.height};
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    // Post shaders derive UVs from SV_Position, so no Y-flip is needed.
    VkViewport vp{0.0f, 0.0f, static_cast<f32>(target.width), static_cast<f32>(target.height),
                  0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {target.width, target.height}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    const u32 dynOffset = AllocPostConstants(cb);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                            &postSets_[frameIndex_], 1, &dynOffset);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
}

void VulkanDevice::RunPostStack(const SceneView& view) {
    VkCommandBuffer cmd = commandBuffers_[frameIndex_];
    const PostSettings& ps = view.post;
    const glm::vec2 sceneTexel(1.0f / sceneW_, 1.0f / sceneH_);

    // End the HDR scene pass; its final layouts make color + depth sampleable.
    vkCmdEndRenderPass(cmd);
    renderPassActive_ = false;

    // The bindless set stays bound from the scene pass; rebind defensively.
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 1, 1,
                            &bindlessSet_, 0, nullptr);
    // NOTE: GpuMark("particles") deliberately does NOT live here. It is emitted at the
    // end of the particle block in DrawScene, matching D3D12 exactly. The end-of-pass
    // work above (vkCmdEndRenderPass + the defensive rebind) therefore falls into the
    // "ssr" bucket, which is the symmetric choice: D3D12's "ssr" bucket already carries
    // the equivalent work (RunPostStack's four PIXEL_SHADER_RESOURCE barriers land
    // before its own GpuMark("ssr")).

    // --- Screen-space reflections (HDR; composited before bloom/tonemap) ----
    u32 hdrInput = slotHdr_; // what bloom + tonemap read (SSR output when on)
    if (view.post.ssrEnabled && ssrReady_) {
        PostUBO cb;
        cb.input0 = slotHdr_;
        cb.input1 = slotGbuffer_; // per-pixel normal + roughness
        cb.input2 = slotDepth_;
        cb.outTexel = sceneTexel;
        cb.inTexel = sceneTexel;
        cb.params0 = {ps.ssrIntensity, ps.ssrMaxDistance, 1.0f, 0.0f};
        DrawPostPass(ssrPipe_, postPass16_, ssr_, cb);
        hdrInput = slotSsr_;
    }
    GpuMark("ssr");

    // --- Screen-space GI: one indirect bounce, gathered at REDUCED res, then upscaled
    //     + added over the full-res HDR. The SSGI shader outputs the GI term only (a=1),
    //     so ApplyHalfRes does scene + GI. GI is low-frequency + TAA-denoised, so the
    //     reduced-res gather is near-lossless and ~16x cheaper than full-res. ----------
    if (view.post.ssgiEnabled && ssgiReady_) {
        const glm::vec2 giTexel(1.0f / ssgiHalf_.width, 1.0f / ssgiHalf_.height);
        PostUBO cb;
        cb.input0 = hdrInput;   // full-res lit HDR = the ray-march gather source
        cb.input1 = slotGbuffer_;
        cb.input2 = slotDepth_;
        cb.outTexel = giTexel;  // rasterize GI at reduced res
        cb.inTexel = sceneTexel;
        cb.params0 = {ps.ssgiIntensity, ps.ssgiRadius, static_cast<f32>(ps.ssgiSamples), 1.0f};
        DrawPostPass(ssgiPipe_, postPass16_, ssgiHalf_, cb);
        // Upscale + add: scene + GI -> full-res HDR.
        PostUBO ap;
        ap.input0 = hdrInput;
        ap.input1 = slotSsgiHalf_;
        ap.outTexel = sceneTexel;
        ap.inTexel = sceneTexel;
        DrawPostPass(applyPipe_, postPass16_, ssgi_, ap);
        hdrInput = slotSsgi_;
    }
    GpuMark("ssgi");

    // --- Volumetric fog: marched at HALF res (outputs inscatter+transmittance), then
    //     upscaled + applied over the full-res HDR (scene*transmittance + inscatter).
    //     Fog is smooth, so half-res is near-lossless and ~4x cheaper. ----------------
    if (view.post.fogEnabled && volReady_) {
        const glm::vec2 fogTexel(1.0f / volHalf_.width, 1.0f / volHalf_.height);
        PostUBO cb;
        cb.input0 = hdrInput;   // (unused by the fog shader now; bound defensively)
        cb.input2 = slotDepth_;
        cb.outTexel = fogTexel; // march fog at reduced res
        cb.inTexel = sceneTexel;
        cb.params0 = {ps.fogDensity, ps.fogHeightFalloff, ps.fogAnisotropy,
                      static_cast<f32>(ps.fogStepCount)};
        cb.params1 = {ps.fogSunIntensity, ps.fogHeight, ps.fogMaxDistance, ps.fogAmbient};
        cb.params2 = {ps.fogColor.x, ps.fogColor.y, ps.fogColor.z, ps.fogGodRays};
        // Animated dither frame index (TAA integrates it into smooth fog); 0 = static
        // IGN when TAA is off, so the noise never crawls without temporal filtering.
        cb.params3.x = ps.taaEnabled
                           ? glm::mod(glm::floor(view.timeSeconds * 120.0f), 64.0f)
                           : 0.0f;
        DrawPostPass(volPipe_, postPass16_, volHalf_, cb);
        // Upscale + apply: scene * transmittance + inscatter -> full-res HDR. Low-pass
        // the fog buffer here (inTexel = fog's quarter-res texel, params0.x = radius) so
        // the world-scale sun-shadow blocks smooth out instead of reading as cubes.
        PostUBO ap;
        ap.input0 = hdrInput;
        ap.input1 = slotVolHalf_;
        ap.input2 = slotDepth_;     // depth for the bilateral (no silhouette bleed)
        ap.outTexel = sceneTexel;
        ap.inTexel = fogTexel;      // blur offsets in fog (half-res) texels
        ap.params0 = {1.5f, 0.0f, 0.0f, 0.0f}; // fog blur radius (texels); 0 = crisp (SSGI)
        DrawPostPass(applyPipe_, postPass16_, vol_, ap);
        hdrInput = slotVol_;
    }
    GpuMark("fog");

    // --- NanoVDB baked volume: the RUNTIME volume path. Upload the raw NanoVDB blob to this
    //     frame's storage buffer, bind it at post-set binding 6, and PNanoVDB-raymarch it (half
    //     res) + composite. Takes precedence over the legacy splat below; never both draw. ----
    bool nanoVolumeDrawn = false;
    if (volGridSize_ > 0 && volGridBytes_ && volRaymarchReady_ &&
        EnsureVolumeGridBuffer(volGridSize_)) {
        std::memcpy(volGridMapped_[frameIndex_], volGridBytes_, volGridSize_);
        // Bind the grid SSBO into THIS frame's post set at binding 6 (safe: BeginFrame waited on
        // this slot's fence; only the raymarch pass, bound after, reads binding 6).
        VkDescriptorBufferInfo bi{volGridBuf_[frameIndex_], 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet bw{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        bw.dstSet = postSets_[frameIndex_];
        bw.dstBinding = 6;
        bw.descriptorCount = 1;
        bw.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bw.pBufferInfo = &bi;
        vkUpdateDescriptorSets(device_, 1, &bw, 0, nullptr);

        const glm::vec2 vTexel(1.0f / volPartHalf_.width, 1.0f / volPartHalf_.height);
        const VolumeRenderParams& rp = volRenderParams_;
        PostUBO cb;
        cb.input2 = slotDepth_;
        cb.outTexel = vTexel;
        cb.inTexel = sceneTexel;
        cb.params0 = {rp.boundsMin.x, rp.boundsMin.y, rp.boundsMin.z, static_cast<f32>(rp.stepCount)};
        cb.params1 = {rp.boundsMax.x, rp.boundsMax.y, rp.boundsMax.z, rp.densityScale};
        cb.params2 = {rp.emission, static_cast<f32>(rp.shadowSteps), rp.extinction,
                      ps.taaEnabled ? glm::mod(glm::floor(view.timeSeconds * 120.0f), 64.0f) : 0.0f};
        cb.params3 = {rp.worldOffset.x, rp.worldOffset.y, rp.worldOffset.z, 0.0f}; // volume placement
        DrawPostPass(volRaymarchPipe_, postPass16_, volPartHalf_, cb);
        PostUBO ap;
        ap.input0 = hdrInput;
        ap.input1 = slotVolPartHalf_;
        ap.input2 = slotDepth_;
        ap.outTexel = sceneTexel;
        ap.inTexel = vTexel;
        ap.params0 = {1.0f, 0.0f, 0.0f, 0.0f};
        DrawPostPass(applyPipe_, postPass16_, volPart_, ap);
        hdrInput = slotVolPart_;
        nanoVolumeDrawn = true;
    }

    // --- Volumetric particles: raymarch the density/temperature volume (half res),
    //     lit + self-shadowed + blackbody-emissive, composited over the HDR. Mirrors
    //     the D3D12 path; the volume stays in GENERAL (sampled read + storage write),
    //     ordered by the compute->fragment barrier the splat recorded in BeginFrame.
    //     LEGACY splat path - runs only when no NanoVDB grid was drawn above. ---
    if (!nanoVolumeDrawn && volBlobCount_ > 0 && volPartReady_ && volTex_.IsValid() &&
        slotViews_.count(volTex_.index)) {
        // Bind the 3D volume into THIS frame's post set at binding 5. Safe to update:
        // BeginFrame waited on this slot's fence, so the set isn't in flight; only the
        // raymarch pass (bound after this) samples binding 5.
        VkDescriptorImageInfo vi{};
        vi.imageView = slotViews_[volTex_.index];
        vi.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet vw{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        vw.dstSet = postSets_[frameIndex_];
        vw.dstBinding = 5;
        vw.descriptorCount = 1;
        vw.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        vw.pImageInfo = &vi;
        vkUpdateDescriptorSets(device_, 1, &vw, 0, nullptr);

        const glm::vec2 vTexel(1.0f / volPartHalf_.width, 1.0f / volPartHalf_.height);
        PostUBO cb;
        cb.input2 = slotDepth_;
        cb.outTexel = vTexel;
        cb.inTexel = sceneTexel;
        cb.params0 = {volParams_.boundsMin.x, volParams_.boundsMin.y, volParams_.boundsMin.z,
                      static_cast<f32>(volParams_.stepCount)};
        cb.params1 = {volParams_.boundsMax.x, volParams_.boundsMax.y, volParams_.boundsMax.z,
                      1.0f}; // densityMul: 1.0 — the splat already baked densityScale into the volume
        cb.params2 = {volParams_.emission, 4.0f, volParams_.extinction, // emissionMul, shadowSteps, extinction
                      ps.taaEnabled ? glm::mod(glm::floor(view.timeSeconds * 120.0f), 64.0f) : 0.0f};
        cb.params3 = {view.timeSeconds, volParams_.noiseDetail, volParams_.noiseScale, 0.0f}; // time, detail, scale
        DrawPostPass(volPartPipe_, postPass16_, volPartHalf_, cb);
        PostUBO ap;
        ap.input0 = hdrInput;
        ap.input1 = slotVolPartHalf_;
        ap.input2 = slotDepth_;
        ap.outTexel = sceneTexel;
        ap.inTexel = vTexel;
        ap.params0 = {1.0f, 0.0f, 0.0f, 0.0f}; // mild bilateral upscale
        DrawPostPass(applyPipe_, postPass16_, volPart_, ap);
        hdrInput = slotVolPart_;
    }
    GpuMark("volparticles");

    // --- Painterly: repaint the lit HDR as edge-aware brush strokes ----------
    const u32 paintColorSrc = hdrInput; // pre-painterly lit HDR (crisp stroke colour)
    if (ps.painterlyEnabled && !ps.painterly3D && painterlyReady_) {
        PostUBO cb;
        cb.input0 = hdrInput;
        cb.input1 = slotGbuffer_; // octN + roughness/metal (normal for edge stops)
        cb.input2 = slotDepth_;   // depth for silhouette stops
        cb.outTexel = sceneTexel;
        cb.inTexel = sceneTexel;
        cb.params0 = {ps.painterlyRadius, ps.painterlyStrokeFlow, ps.painterlyStrength,
                      ps.painterlyEdge};
        cb.params1 = {ps.painterlyLightTint, ps.painterlyWarmCool, ps.painterlyCanvasScale,
                      ps.painterlyCanvasStrength};
        cb.params2 = {ps.painterlyStrokeDetail, ps.painterlyPosterize, 0.0f, 0.0f};
        // FULL-res Kuwahara: half-res + bilinear upscale blurred the edge-aware regions
        // into mush (the filter's value is crisp region boundaries), which read as ugly
        // blocky/blotchy painting. Painterly is the art style - run it full res; reclaim
        // perf from other passes instead. (cb.outTexel/inTexel stay full-res, set above.)
        DrawPostPass(painterlyPipe_, postPass16_, painterly_, cb);
        hdrInput = slotPainterly_;
        GpuMark("strokefilter");

        // Collect world-anchored censors once (shared by the stroke pass + the
        // composite below). The shader does a 3D sphere test, so no projection here.
        // When "Real brush strokes" is OFF, censors are the ONLY thing that paints
        // strokes - onto any geometry inside the sphere (static or dynamic).
        glm::vec4 censorArr[kMaxCensors];
        glm::vec4 censorStrength;
        glm::vec4 censorFeather;
        const u32 nCensor = CollectCensors(view, censorArr, censorStrength, censorFeather);

        // --- Real brush strokes: splat instanced oriented quads over the base --
        // Runs when global strokes are on OR any censor is active (censor-only mode).
        // Object-anchored + time-quantized "boil" (the strokes stay glued to the object
        // and re-paint in discrete steps - see BrushStrokes.hlsl); rendered every frame.
        if ((ps.painterlyStrokes || nCensor > 0) && brushStrokesReady_) {
            VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            rp.renderPass = postPass16Load_; // LOAD: keep the Kuwahara underpainting
            rp.framebuffer = painterly_.framebuffer;
            rp.renderArea.extent = {painterly_.width, painterly_.height};
            vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
            // Negative-height viewport (VK_KHR_maintenance1): this pass BUILDS its own
            // clip-space quads with the D3D12 Y convention (clip.y = -clip.y in
            // BrushStrokes.hlsl), so flip Y here so the strokes land right-side up.
            VkViewport vp{};
            vp.x = 0.0f;
            vp.y = static_cast<f32>(painterly_.height);
            vp.width = static_cast<f32>(painterly_.width);
            vp.height = -static_cast<f32>(painterly_.height);
            vp.minDepth = 0.0f;
            vp.maxDepth = 1.0f;
            VkRect2D sc{{0, 0}, {painterly_.width, painterly_.height}};
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &sc);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, brushStrokesPipe_);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 1, 1,
                                    &bindlessSet_, 0, nullptr);
            const float density = std::max(ps.painterlyStrokeDensity, 0.1f);
            const float baseLen =
                std::max(ps.painterlyRadius * 6.0f * ps.painterlyStrokeLength, 6.0f);
            // Stop-motion "boil": fold a TIME-QUANTIZED step into the per-stroke seed
            // so the strokes repaint in discrete jumps at painterlyStrokeBoil fps and
            // hold in between (instead of recomputing every frame). Wrapped to a small
            // range so the growing time can't blow up Hash21 (precision -> stripes).
            const float boilPhase =
                (ps.painterlyStrokeBoil > 0.01f)
                    ? std::fmod(std::floor(view.timeSeconds * ps.painterlyStrokeBoil), 256.0f)
                    : 0.0f;
            // Global stroke coverage box: full screen (or the user's censor-box
            // rect) when "Real brush strokes" is on; an EMPTY box when it's off, so
            // strokes paint ONLY inside the censor spheres (gated in the PS below).
            const glm::vec4 strokeRect =
                !ps.painterlyStrokes
                    ? glm::vec4{2.0f, 2.0f, -1.0f, -1.0f} // empty: censor-only
                    : ps.painterlyStrokeMask
                          ? glm::vec4{ps.painterlyStrokeMaskMinX, ps.painterlyStrokeMaskMinY,
                                     ps.painterlyStrokeMaskMaxX, ps.painterlyStrokeMaskMaxY}
                          : glm::vec4{0.0f, 0.0f, 1.0f, 1.0f};
            const auto drawLayer = [&](float lenPx, float widthFrac, float spacingFac, float seed) {
                const float spacing = std::max(3.0f, lenPx * spacingFac / density);
                const u32 cols = std::max(1u, static_cast<u32>(std::ceil(sceneW_ / spacing)));
                const u32 rows = std::max(1u, static_cast<u32>(std::ceil(sceneH_ / spacing)));
                PostUBO sb;
                sb.input0 = paintColorSrc;
                sb.input1 = slotGbuffer_;
                sb.input2 = slotDepth_; // depth: world anchoring + the 3D censor test
                sb.input3 = slotHdr_;   // forward HDR alpha = the per-pixel censored flag
                sb.outTexel = sceneTexel;
                sb.inTexel = sceneTexel;
                sb.params0 = {lenPx, widthFrac, 0.35f, ps.painterlyStrength};
                sb.params1 = {ps.painterlyStrokeSharp, ps.painterlyEdge, 0.30f,
                              std::max(ps.painterlyStrokeDetail * 2.0f, 0.25f)};
                sb.params2 = {static_cast<f32>(cols), static_cast<f32>(rows),
                              ps.painterlyStrokeFlow, seed};
                sb.params3 = strokeRect;
                for (u32 ci = 0; ci < kMaxCensors; ++ci) sb.censors[ci] = censorArr[ci];
                sb.censorStrength = censorStrength;
                sb.censorFeather = censorFeather;
                sb.censorCount = glm::uvec4(nCensor, 0u, 0u, 0u);
                const u32 dynOffset = AllocPostConstants(sb);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                                        &postSets_[frameIndex_], 1, &dynOffset);
                vkCmdDraw(cmd, 6, cols * rows, 0, 0);
            };
            drawLayer(baseLen * 1.8f, 0.55f, 0.62f, 11.0f + boilPhase); // coarse block-in
            drawLayer(baseLen, 0.42f, 0.46f, 37.0f + boilPhase);        // finer detail
            vkCmdEndRenderPass(cmd);
        }
        GpuMark("strokes"); // delta kuwahara->here = the brush-stroke splat (both layers)

        // Dynamic-layer composite: restore crisp lit colour over the painterly where
        // the forward HDR alpha mask = 1 (player / NPCs / interactables), so dynamic
        // objects stand out against the painted static world. paintColorSrc = the
        // pre-painterly lit HDR (integrated, fog/GI on); slotHdr_ = the untouched
        // forward HDR whose alpha carries the per-pixel mask.
        PostUBO comp;
        comp.input0 = slotPainterly_; // painted static world
        comp.input1 = paintColorSrc;  // crisp lit colour for dynamic objects
        comp.input2 = slotHdr_;       // forward HDR: alpha = mask
        comp.input3 = slotDepth_;     // depth for the 3D censor test
        comp.outTexel = sceneTexel;
        comp.inTexel = sceneTexel;
        // World-anchored censors (collected above): feed the feathered sphere so
        // dynamic objects inside a censor keep the painted look (CensorComponent).
        for (u32 ci = 0; ci < kMaxCensors; ++ci) comp.censors[ci] = censorArr[ci];
        comp.censorStrength = censorStrength;
        comp.censorFeather = censorFeather;
        comp.censorCount = glm::uvec4(nCensor, 0u, 0u, 0u);
        DrawPostPass(compositePipe_, postPass16_, painterlyComp_, comp);
        hdrInput = slotPainterlyComp_;
        GpuMark("composite");
    }

    // --- SSAO + blur (half res) --------------------------------------------
    u32 aoSlot = 0; // bindless white: no occlusion
    if (ps.ssaoEnabled) {
        const glm::vec2 ssaoTexel(1.0f / ssaoRaw_.width, 1.0f / ssaoRaw_.height);
        PostUBO cb;
        cb.input0 = slotDepth_;
        cb.input1 = slotGbuffer_; // GTAO uses the shading normal
        cb.outTexel = ssaoTexel;
        cb.inTexel = sceneTexel;
        cb.params0 = {ps.ssaoRadius, ps.ssaoIntensity, 0.0005f, 0.0f};
        DrawPostPass(ssaoPipe_, postPass8_, ssaoRaw_, cb);

        PostUBO blur;
        blur.input0 = slotSsaoRaw_;
        blur.outTexel = ssaoTexel;
        blur.inTexel = ssaoTexel;
        DrawPostPass(ssaoBlurPipe_, postPass8_, ssaoBlur_, blur);
        aoSlot = slotSsaoBlur_;
    }
    GpuMark("ssao");

    // --- Bloom pyramid -------------------------------------------------------
    f32 bloomMix = 0.0f;
    if (ps.bloomEnabled && bloomCount_ > 0) {
        PostUBO cb;
        cb.input0 = hdrInput;
        cb.inTexel = sceneTexel;
        cb.outTexel = {1.0f / bloom_[0].width, 1.0f / bloom_[0].height};
        cb.params0 = {ps.bloomThreshold, ps.bloomThreshold * 0.5f + 1e-4f, 1.0f, 0.0f};
        DrawPostPass(bloomDownPipe_, postPass16_, bloom_[0], cb);
        for (u32 i = 1; i < bloomCount_; ++i) {
            PostUBO down;
            down.input0 = slotBloom_[i - 1];
            down.inTexel = {1.0f / bloom_[i - 1].width, 1.0f / bloom_[i - 1].height};
            down.outTexel = {1.0f / bloom_[i].width, 1.0f / bloom_[i].height};
            DrawPostPass(bloomDownPipe_, postPass16_, bloom_[i], down);
        }
        for (u32 i = bloomCount_ - 1; i-- > 0;) {
            PostUBO up;
            up.input0 = slotBloom_[i + 1];
            up.inTexel = {1.0f / bloom_[i + 1].width, 1.0f / bloom_[i + 1].height};
            up.outTexel = {1.0f / bloom_[i].width, 1.0f / bloom_[i].height};
            up.params0 = {1.0f, 0.0f, 0.0f, 0.0f};
            DrawPostPass(bloomUpPipe_, postPass16Load_, bloom_[i], up);
        }
        bloomMix = ps.bloomIntensity;
    }
    GpuMark("bloom");

    // --- Auto-exposure: average luminance + temporal adaptation (1x1) --------
    u32 lumSlot = 0; // 0 = off; tonemap then uses manual exposure only
    if (view.post.autoExposureEnabled && exposureReady_) {
        const u32 cur = adaptIndex_;
        const u32 prev = cur ^ 1u;
        PostUBO cb;
        cb.input0 = hdrInput;
        cb.input1 = slotAdaptedLum_[prev];
        cb.outTexel = {1.0f, 1.0f};
        cb.inTexel = sceneTexel;
        cb.params0 = {ps.autoExposureSpeed, view.deltaTime, adaptValid_ ? 1.0f : 0.0f, 0.0f};
        DrawPostPass(exposurePipe_, postPass16_, adaptedLum_[cur], cb);
        lumSlot = slotAdaptedLum_[cur];
        adaptIndex_ = prev;
        adaptValid_ = true;
    }
    GpuMark("exposure");

    // --- Tonemap composite -> LDR -------------------------------------------
    {
        PostUBO cb;
        cb.input0 = hdrInput;
        cb.input1 = (bloomMix > 0.0f) ? slotBloom_[0] : 0;
        cb.input2 = aoSlot;
        cb.input3 = lumSlot; // auto-exposure adapted luminance (0 = manual only)
        cb.outTexel = sceneTexel;
        cb.inTexel = sceneTexel;
        // Painterly is handled by its own pass above; here just suppress the
        // photographic vignette so the painted look isn't darkened at the corners.
        const bool paint = ps.painterlyEnabled && painterlyReady_;
        cb.params0 = {bloomMix, 1.0f, paint ? 0.0f : ps.vignette, ps.saturation};
        cb.params1 = {ps.contrast, ps.autoExposureKey, ps.autoExposureMin, ps.autoExposureMax};
        cb.params2 = {0.0f, 0.0f, 0.0f, 0.0f}; // tonemap's built-in smear off
        cb.params3.x = static_cast<f32>(ps.tonemapOperator); // 0=ACES 1=AgX 2=Tony
        cb.grade0 = {ps.gradeTemperature, ps.gradeTint, ps.filmGrain, ps.chromaticAberration};
        cb.grade1 = {ps.gradeLift.x, ps.gradeLift.y, ps.gradeLift.z,
                     static_cast<f32>(ps.gradeEnabled)};
        cb.grade2 = {ps.gradeGamma.x, ps.gradeGamma.y, ps.gradeGamma.z, view.timeSeconds};
        cb.grade3 = {ps.gradeGain.x, ps.gradeGain.y, ps.gradeGain.z, 0.0f};
        DrawPostPass(tonemapPipe_, postPass8_, ldr_, cb);
    }
    GpuMark("tonemap");

    // --- TAA resolve -> history ---------------------------------------------
    // Reproject last frame's accumulation into this frame and blend a small
    // slice of the current frame, turning the per-frame jitter into temporal
    // supersampling. The final pass reads the freshly written history.
    u32 finalInput = slotLdr_;
    if (view.post.taaEnabled && taaReady_) {
        const u32 cur = taaHistoryIndex_;
        const u32 prev = cur ^ 1u;
        PostUBO cb;
        cb.input0 = slotLdr_;               // current tonemapped frame
        cb.input1 = slotTaaHistory_[prev];  // previous accumulation
        cb.input2 = slotDepth_;             // depth (kept for reference)
        cb.input3 = slotVelocity_;          // per-object motion vectors
        cb.outTexel = sceneTexel;
        cb.inTexel = sceneTexel;
        cb.params0 = {0.1f, taaHistoryValid_ ? 1.0f : 0.0f, 0.0f, 0.0f};
        DrawPostPass(taaPipe_, postPass8_, taaHistory_[cur], cb);
        finalInput = slotTaaHistory_[cur];
        taaHistoryIndex_ = prev; // next frame writes the other target
        taaHistoryValid_ = true;
    }
    GpuMark("taa");

    // --- Depth of field -> dof_ ---------------------------------------------
    if (view.post.dofEnabled && dofReady_) {
        PostUBO cb;
        cb.input0 = finalInput; // resolved colour (TAA output or tonemapped LDR)
        cb.input2 = slotDepth_; // reconstruct distance for the circle of confusion
        cb.outTexel = sceneTexel;
        cb.inTexel = sceneTexel;
        cb.params0 = {ps.dofFocusDistance, ps.dofFocusRange, ps.dofMaxBlur, 1.0f};
        DrawPostPass(dofPipe_, postPass8_, dof_, cb);
        finalInput = slotDof_;
    }
    GpuMark("dof");

    // --- Motion blur -> motionBlur_ -----------------------------------------
    if (view.post.motionBlurEnabled && motionBlurReady_) {
        PostUBO cb;
        cb.input0 = finalInput;
        cb.input3 = slotVelocity_; // per-object motion vectors
        cb.outTexel = sceneTexel;
        cb.inTexel = sceneTexel;
        cb.params0 = {ps.motionBlurIntensity, ps.motionBlurMaxRadius, 1.0f, 0.0f};
        DrawPostPass(motionBlurPipe_, postPass8_, motionBlur_, cb);
        finalInput = slotMotionBlur_;
    }
    GpuMark("mblur");

    // --- FXAA -> final target (viewport texture or swapchain) ---------------
    // Begins the final pass and leaves it ACTIVE: the UI overlay (and, in
    // direct-to-swapchain mode, ImGui) record into it before EndFrame/RenderUI
    // close it.
    {
        VkClearValue clears[2]{};
        clears[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        clears[1].depthStencil = {1.0f, 0};
        VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        VkExtent2D ext = extent_;
        rp.renderPass = renderPass_;
        rp.framebuffer = framebuffers_[imageIndex_];
#if HBE_EDITOR
        if (viewportReady_) {
            rp.renderPass = vpRenderPass_;
            rp.framebuffer = vpFramebuffer_;
            ext = {vpW_, vpH_};
        }
#endif
        rp.renderArea.extent = ext;
        rp.clearValueCount = 2;
        rp.pClearValues = clears;
        vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
        renderPassActive_ = true;

        // Negative-height viewport so the UI overlay that follows keeps the
        // same orientation as D3D12; FXAA itself is flip-agnostic.
        VkViewport vp{};
        vp.y = static_cast<f32>(ext.height);
        vp.width = static_cast<f32>(ext.width);
        vp.height = -static_cast<f32>(ext.height);
        vp.maxDepth = 1.0f;
        VkRect2D scissor{{0, 0}, ext};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        PostUBO cb;
        cb.input0 = finalInput; // TAA output when enabled, else the tonemapped LDR
        cb.outTexel = sceneTexel;
        cb.inTexel = sceneTexel;
        cb.params0 = {ps.fxaaEnabled ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fxaaPipe_);
        const u32 dynOffset = AllocPostConstants(cb);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                                &postSets_[frameIndex_], 1, &dynOffset);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    GpuMark("fxaa"); // FXAA into the final target (render pass left open for UI/ImGui)
}

void VulkanDevice::DrawShadowPass(const SceneView& view, const DrawItem* items, u32 count) {
    shadowPassRun_ = false;
    if (!frameActive_ || renderPassActive_ || !shadowReady_ || !view.shadowsEnabled ||
        !items || count == 0) {
        return;
    }
    VkCommandBuffer cmd = commandBuffers_[frameIndex_];

    const u32 cascadeCount = std::min(view.cascadeCount, kMaxShadowCascades);
    for (u32 c = 0; c < cascadeCount; ++c) {
        FrameUBO fcb{};
        fcb.viewProj = view.cascadeViewProj[c];
        std::memcpy(shadowFrameUBOMapped_[frameIndex_] + c * shadowFrameStride_, &fcb,
                    sizeof(fcb));
    }

    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = shadowRenderPass_;
    rp.framebuffer = shadowFramebuffer_;
    rp.renderArea.extent = {kShadowDim, kShadowDim};
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline_);

    // The object arena is shared with DrawScene, which (re)writes the full UBO
    // at the same offsets afterwards; both GPU passes read the final contents.
    // The UBO writes happen once; every cascade reuses the same dynamic offsets.
    const u32 maxDraws = static_cast<u32>(kObjectArenaSize / objectStride_);
    const u32 drawCount = std::min(count, maxDraws);
    shadowInstanceCounts_.clear();
    shadowInstanceCounts_.resize(drawCount, 1);
    for (u32 i = 0; i < drawCount; ++i) {
        const DrawItem& it = items[i];
        if (it.instanceRun == 0) continue; // consumed by a run head (skipped below)
        if (!it.mesh.IsValid() || it.mesh.id > meshes_.size()) continue;
        ObjectUBO ocb{};
        ocb.model = it.transform;
        // Instanced run head: upload the run's transforms once for this pass.
        // Depth-only pass: the normal/prev matrices are never read, so skip the
        // inverse-transpose (write model into all three slots).
        if (it.instanceRun > 1) {
            u32 base = 0;
            if (glm::mat4* inst = AllocInstances(it.instanceRun, base)) {
                for (u32 k = 0; k < it.instanceRun; ++k) {
                    const DrawItem& run = items[i + k];
                    inst[k * 3 + 0] = run.transform;
                    inst[k * 3 + 1] = run.transform;
                    inst[k * 3 + 2] = run.transform;
                }
                ocb.instanced = 1;
                ocb.instanceBase = base;
                shadowInstanceCounts_[i] = it.instanceRun;
            }
        }
        std::memcpy(objectArenaMapped_[frameIndex_] + i * objectStride_, &ocb, sizeof(ocb));
    }

    // One depth-only pass per cascade into its 2x2-atlas tile (negative-height
    // viewport to match the main pass / D3D12 convention).
    for (u32 c = 0; c < cascadeCount; ++c) {
        const f32 tx = static_cast<f32>((c & 1) * kShadowTileDim);
        const f32 ty = static_cast<f32>((c >> 1) * kShadowTileDim);
        VkViewport vp{};
        vp.x = tx;
        vp.y = ty + static_cast<f32>(kShadowTileDim);
        vp.width = static_cast<f32>(kShadowTileDim);
        vp.height = -static_cast<f32>(kShadowTileDim);
        vp.maxDepth = 1.0f;
        VkRect2D scissor{{static_cast<i32>(tx), static_cast<i32>(ty)},
                         {kShadowTileDim, kShadowTileDim}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        const u8 cascadeBit = static_cast<u8>(1u << c);
        u32 lastMesh = 0; // consecutive same-mesh draws skip the IA rebinds
        for (u32 i = 0; i < drawCount; ++i) {
            const DrawItem& it = items[i];
            if (it.instanceRun == 0) continue; // drawn by its run head
            if (!it.mesh.IsValid() || it.mesh.id > meshes_.size()) continue;
            if (it.materialFlags & MaterialFlag_NoShadow) continue; // doesn't cast a shadow
            // Per-cascade culling: this caster cannot affect this cascade's slice.
            // The object UBO above is written once and shared by every cascade, so
            // skipping here removes the draw without disturbing the index coupling.
            if (!(it.cascadeMask & cascadeBit)) continue;
            const GpuMeshVk& gm = meshes_[it.mesh.id - 1];
            const u32 dynOffset = static_cast<u32>(i * objectStride_);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                                    &shadowDescriptorSets_[frameIndex_][c], 1, &dynOffset);
            if (it.mesh.id != lastMesh) {
                const VkDeviceSize voff = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &gm.vertexBuffer, &voff);
                vkCmdBindIndexBuffer(cmd, gm.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
                lastMesh = it.mesh.id;
            }
            vkCmdDrawIndexed(cmd, gm.indexCount, shadowInstanceCounts_[i], 0, 0, 0);
        }
    }

    vkCmdEndRenderPass(cmd);
    shadowPassRun_ = true;
}

u32 VulkanDevice::FindMemoryType(u32 typeBits, VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &mem);
    for (u32 i = 0; i < mem.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    HBE_ERROR("[Vulkan] No suitable memory type found.");
    return 0;
}

bool VulkanDevice::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                VkMemoryPropertyFlags props, VkBuffer& buffer,
                                VkDeviceMemory& memory) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(device_, &bci, nullptr, &buffer), "vkCreateBuffer");

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, props);
    VK_CHECK(vkAllocateMemory(device_, &ai, nullptr, &memory), "vkAllocateMemory(buffer)");
    vkBindBufferMemory(device_, buffer, memory, 0);
    return true;
}

VkShaderModule VulkanDevice::LoadShaderModule(const std::wstring& path) {
    const std::vector<u8> code = ReadBinaryFile(path);
    if (code.empty() || (code.size() % 4) != 0) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const u32*>(code.data());
    VkShaderModule mod = VK_NULL_HANDLE;
    vkCreateShaderModule(device_, &ci, nullptr, &mod);
    return mod;
}

MeshHandle VulkanDevice::CreateMeshReserved(const hbe::MeshData& initial, u32 vertexCapacity,
                                            u32 indexCapacity) {
    // Widen the ALLOCATION only; the upload and gm.indexCount still describe `initial`.
    // gm.vbSize/ibSize mean ALLOCATED, which is exactly what UpdateMesh tests against.
    reserveVertices_ = std::max<VkDeviceSize>(vertexCapacity, initial.vertices.size());
    reserveIndices_ = std::max<VkDeviceSize>(indexCapacity, initial.indices.size());
    const MeshHandle h = CreateMesh(initial);
    reserveVertices_ = reserveIndices_ = 0;
    return h;
}

MeshHandle VulkanDevice::CreateMesh(const hbe::MeshData& mesh) {
    if (mesh.Empty()) return {};

    GpuMeshVk gm;
    const VkDeviceSize vbSize = static_cast<VkDeviceSize>(mesh.vertices.size()) * sizeof(hbe::Vertex);
    const VkDeviceSize ibSize = static_cast<VkDeviceSize>(mesh.indices.size()) * sizeof(u32);
    const VkDeviceSize vbAlloc = std::max(vbSize, reserveVertices_ * sizeof(hbe::Vertex));
    const VkDeviceSize ibAlloc = std::max(ibSize, reserveIndices_ * sizeof(u32));
    const VkMemoryPropertyFlags hostVisible =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    // Device-local (VRAM) vertex/index buffers; data staged through host memory
    // once at load. Keeps per-frame vertex fetch in VRAM instead of over PCIe.
    if (!CreateBuffer(vbAlloc, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, gm.vertexBuffer, gm.vertexMemory) ||
        !CreateBuffer(ibAlloc, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, gm.indexBuffer, gm.indexMemory)) {
        HBE_ERROR("[Vulkan] Failed to allocate buffers for mesh '{}'", mesh.name);
        return {};
    }

    VkBuffer vStage = VK_NULL_HANDLE, iStage = VK_NULL_HANDLE;
    VkDeviceMemory vStageMem = VK_NULL_HANDLE, iStageMem = VK_NULL_HANDLE;
    if (!CreateBuffer(vbSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, hostVisible, vStage, vStageMem) ||
        !CreateBuffer(ibSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, hostVisible, iStage, iStageMem)) {
        return {};
    }
    void* p = nullptr;
    vkMapMemory(device_, vStageMem, 0, vbSize, 0, &p);
    std::memcpy(p, mesh.vertices.data(), vbSize);
    vkUnmapMemory(device_, vStageMem);
    vkMapMemory(device_, iStageMem, 0, ibSize, 0, &p);
    std::memcpy(p, mesh.indices.data(), ibSize);
    vkUnmapMemory(device_, iStageMem);

    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = commandPool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &cbai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    VkBufferCopy vCopy{0, 0, vbSize};
    VkBufferCopy iCopy{0, 0, ibSize};
    vkCmdCopyBuffer(cmd, vStage, gm.vertexBuffer, 1, &vCopy);
    vkCmdCopyBuffer(cmd, iStage, gm.indexBuffer, 1, &iCopy);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);
    vkFreeCommandBuffers(device_, commandPool_, 1, &cmd);
    vkDestroyBuffer(device_, vStage, nullptr);
    vkFreeMemory(device_, vStageMem, nullptr);
    vkDestroyBuffer(device_, iStage, nullptr);
    vkFreeMemory(device_, iStageMem, nullptr);

    gm.indexCount = mesh.IndexCount();
    gm.vbSize = vbAlloc; // ALLOCATED, not uploaded - UpdateMesh tests against this
    gm.ibSize = ibAlloc;

    meshes_.push_back(gm);
    return MeshHandle{static_cast<u32>(meshes_.size())};
}

bool VulkanDevice::UpdateMesh(MeshHandle handle, const hbe::MeshData& mesh) {
    if (!handle.IsValid() || handle.id > meshes_.size() || mesh.Empty()) return false;
    GpuMeshVk& gm = meshes_[handle.id - 1];
    const VkDeviceSize vbSize = static_cast<VkDeviceSize>(mesh.vertices.size()) * sizeof(hbe::Vertex);
    const VkDeviceSize ibSize = static_cast<VkDeviceSize>(mesh.indices.size()) * sizeof(u32);
    if (vbSize > gm.vbSize || ibSize > gm.ibSize) { // must fit; never grows
        HBE_WARN("[Vulkan] UpdateMesh refused: {} vertex + {} index bytes exceed the "
                 "{}/{} reserved for mesh {}. Use CreateMeshReserved.",
                 vbSize, ibSize, gm.vbSize, gm.ibSize, handle.id);
        return false;
    }

    const VkMemoryPropertyFlags hostVisible =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkBuffer vStage = VK_NULL_HANDLE, iStage = VK_NULL_HANDLE;
    VkDeviceMemory vStageMem = VK_NULL_HANDLE, iStageMem = VK_NULL_HANDLE;
    if (!CreateBuffer(vbSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, hostVisible, vStage, vStageMem) ||
        !CreateBuffer(ibSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, hostVisible, iStage, iStageMem)) {
        return false;
    }
    void* p = nullptr;
    vkMapMemory(device_, vStageMem, 0, vbSize, 0, &p);
    std::memcpy(p, mesh.vertices.data(), vbSize);
    vkUnmapMemory(device_, vStageMem);
    vkMapMemory(device_, iStageMem, 0, ibSize, 0, &p);
    std::memcpy(p, mesh.indices.data(), ibSize);
    vkUnmapMemory(device_, iStageMem);

    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = commandPool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &cbai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    VkBufferCopy vCopy{0, 0, vbSize};
    VkBufferCopy iCopy{0, 0, ibSize};
    vkCmdCopyBuffer(cmd, vStage, gm.vertexBuffer, 1, &vCopy);
    vkCmdCopyBuffer(cmd, iStage, gm.indexBuffer, 1, &iCopy);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_); // idle: safe to overwrite buffers prior frames read
    vkFreeCommandBuffers(device_, commandPool_, 1, &cmd);
    vkDestroyBuffer(device_, vStage, nullptr);
    vkFreeMemory(device_, vStageMem, nullptr);
    vkDestroyBuffer(device_, iStage, nullptr);
    vkFreeMemory(device_, iStageMem, nullptr);

    gm.indexCount = mesh.IndexCount();
    return true;
}

void VulkanDevice::BeginFrame() {
    frameActive_ = false;
    renderPassActive_ = false;
    if (swapchain_ == VK_NULL_HANDLE) return;

#if HBE_EDITOR
    // Apply a pending viewport (offscreen target) resize.
    if (viewportReady_ && pendingVpW_ > 0 && pendingVpH_ > 0 &&
        (pendingVpW_ != vpW_ || pendingVpH_ != vpH_)) {
        CreateViewportTarget(pendingVpW_, pendingVpH_);
    }
    // Apply a pending asset-preview resize.
    if (pendingPrevW_ > 0 && pendingPrevH_ > 0 &&
        (pendingPrevW_ != prevW_ || pendingPrevH_ != prevH_)) {
        previewReady_ = CreatePreviewTargets(pendingPrevW_, pendingPrevH_);
    }
#endif

    // (Re)build the HDR/post targets at the scene render size (the editor
    // viewport when active, else the swapchain).
    if (postPipelinesReady_) {
        u32 w = width_, h = height_;
#if HBE_EDITOR
        if (viewportReady_) { w = vpW_; h = vpH_; }
#endif
        if (w > 0 && h > 0 && (w != sceneW_ || h != sceneH_)) {
            postReady_ = CreatePostTargets(w, h);
            if (!postReady_) {
                HBE_ERROR("[Vulkan] Post target creation failed at {}x{}.", w, h);
            }
        }
    }
    postHead_ = 0;
    boneHead_ = 0;
    instanceHead_ = 0;

    vkWaitForFences(device_, 1, &inFlight_[frameIndex_], VK_TRUE, UINT64_MAX);

    // GPU profiler: this slot's fence just signalled, so its timestamps (recorded
    // framesInFlight_ frames ago) are now readable. Log a per-pass breakdown ~every
    // 2s. Read BEFORE the pool is reset below.
    if (gpuProfile_ && gpuValid_[frameIndex_] && gpuCountSlot_[frameIndex_] >= 2) {
        const u32 n = gpuCountSlot_[frameIndex_];
        u64 ticks[kMaxGpuMarks]{};
        if (vkGetQueryPoolResults(device_, gpuPool_[frameIndex_], 0, n, sizeof(ticks), ticks,
                                  sizeof(u64), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            if (++gpuFrameCounter_ >= 180) { // ~2s at 90 FPS
                gpuFrameCounter_ = 0;
                const f64 toMs = gpuPeriodNs_ * 1e-6;
                char buf[640];
                int off = std::snprintf(buf, sizeof(buf), "[Vulkan GPU] total %.2f ms |",
                                        static_cast<f64>(ticks[n - 1] - ticks[0]) * toMs);
                for (u32 i = 1; i < n && off > 0 && off < static_cast<int>(sizeof(buf)) - 24; ++i) {
                    const f64 ms = static_cast<f64>(ticks[i] - ticks[i - 1]) * toMs;
                    off += std::snprintf(buf + off, sizeof(buf) - off, " %s %.2f",
                                         gpuNames_[frameIndex_][i] ? gpuNames_[frameIndex_][i] : "?",
                                         ms);
                }
                HBE_INFO("{}", buf);
            }
        }
    }

    VkResult acq = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                         imageAvailable_[frameIndex_], VK_NULL_HANDLE, &imageIndex_);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
        RecreateSwapchain();
        return;
    }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
        HBE_ERROR("[Vulkan] vkAcquireNextImageKHR failed (VkResult={})", static_cast<i32>(acq));
        return;
    }

    vkResetFences(device_, 1, &inFlight_[frameIndex_]);
    vkResetCommandBuffer(commandBuffers_[frameIndex_], 0);

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffers_[frameIndex_], &bi);
    frameActive_ = true;
    uiWorldVertexHead_ = 0; // world-UI canvases bump-allocate here across the frame

    // GPU profiler: reset this slot's timestamp pool (must be on a recording cmd
    // buffer) and mark the frame start.
    if (gpuProfile_) {
        vkCmdResetQueryPool(commandBuffers_[frameIndex_], gpuPool_[frameIndex_], 0, kMaxGpuMarks);
        gpuCount_ = 0;
        GpuMark("start");
    }

    // Volumetric density splat: compute dispatch, which MUST run outside any
    // render pass -> do it here at frame start (no-op unless enabled).
    DispatchVolumeSplat();

    // Queued general compute (GPU particle sim etc.), same constraint, same
    // point. D3D12Device::BeginFrame calls its twin here too, so both backends
    // run this frame's compute before any render target is bound.
    ExecuteQueuedCompute();
}

void VulkanDevice::ClearBackBuffer(f32 r, f32 g, f32 b, f32 a) {
    if (!frameActive_) return;
    GpuMark("shadow"); // delta start->here = the cascaded shadow pass (runs before clear)
    clearColor_ = {{r, g, b, a}};

    // HDR pass clears 4 attachments (colour + G-buffer + velocity + depth); the
    // legacy/offscreen passes clear 2 (colour + depth).
    VkClearValue clears[4]{};
    clears[0].color = clearColor_;
    u32 clearCount;
    if (postReady_) {
        clears[1].color = {{0.0f, 0.0f, 0.0f, 0.0f}}; // G-buffer (no surface)
        clears[2].color = {{0.0f, 0.0f, 0.0f, 0.0f}}; // velocity
        clears[3].depthStencil = {1.0f, 0};
        clearCount = 4;
    } else {
        clears[1].depthStencil = {1.0f, 0};
        clearCount = 2;
    }

    // Scene target: the HDR pass when the post stack is active, else the
    // offscreen viewport pass when present, else the swapchain.
    const bool offscreen = viewportReady_;
    const VkExtent2D ext = postReady_ ? VkExtent2D{sceneW_, sceneH_}
                         : offscreen  ? VkExtent2D{vpW_, vpH_}
                                      : extent_;

    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = postReady_ ? hdrRenderPass_
                  : offscreen  ? vpRenderPass_
                               : renderPass_;
    rp.framebuffer = postReady_ ? hdr_.framebuffer
                   : offscreen  ? vpFramebuffer_
                                : framebuffers_[imageIndex_];
    rp.renderArea.extent = ext;
    rp.clearValueCount = clearCount;
    rp.pClearValues = clears;

    VkCommandBuffer cmd = commandBuffers_[frameIndex_];
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    renderPassActive_ = true;

    // Negative-height viewport (VK_KHR_maintenance1) flips clip-space Y so Vulkan
    // matches D3D12's top-left origin; both backends share one projection.
    VkViewport vp{};
    vp.x = 0.0f;
    vp.y = static_cast<f32>(ext.height);
    vp.width = static_cast<f32>(ext.width);
    vp.height = -static_cast<f32>(ext.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    VkRect2D scissor{{0, 0}, ext};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

bool VulkanDevice::EnsureVolumeResources() {
    if (volInit_) return !volFailed_;
    volInit_ = true;
    volDim_ = glm::clamp(volParams_.resolution, 32u, 192u); // quality knob (first enable)

    // 3D density/temperature volume (RGBA16F), sampled + storage, GENERAL layout.
    TextureDesc vd{};
    vd.width = volDim_; vd.height = volDim_; vd.depth = volDim_;
    vd.format = Format::R16G16B16A16_FLOAT;
    vd.storage = true;
    vd.debugName = "VolumeDensity";
    volTex_ = CreateVolumeTexture(vd);
    if (!volTex_.IsValid() || !volumeStorageView_.count(volTex_.index)) { volFailed_ = true; return false; }
    volStorageView_ = volumeStorageView_[volTex_.index];
    volImage_ = slotImages_.count(volTex_.index) ? slotImages_[volTex_.index] : VK_NULL_HANDLE;

    // Descriptor set layout: binding 0 = params UBO, binding 1 = storage image,
    // binding 2 = blob SSBO (matches VolumeSplat.cs [[vk::binding(0/1/2, 0)]]).
    VkDescriptorSetLayoutBinding b[3]{};
    b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[2].binding = 2; b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[2].descriptorCount = 1; b[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 3; lci.pBindings = b;
    if (vkCreateDescriptorSetLayout(device_, &lci, nullptr, &volSetLayout_) != VK_SUCCESS) {
        volFailed_ = true; return false;
    }
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1; plci.pSetLayouts = &volSetLayout_;
    if (vkCreatePipelineLayout(device_, &plci, nullptr, &volPipelineLayout_) != VK_SUCCESS) {
        volFailed_ = true; return false;
    }

    const std::wstring dir = ExecutableDir() + L"shaders\\";
    VkShaderModule cs = LoadShaderModule(dir + L"VolumeSplat.cs.spv");
    if (cs == VK_NULL_HANDLE) { HBE_ERROR("[Vulkan] VolumeSplat.cs.spv missing"); volFailed_ = true; return false; }
    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = cs;
    cpci.stage.pName = "CSMain";
    cpci.layout = volPipelineLayout_;
    const VkResult pr = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &cpci, nullptr,
                                                 &volSplatPipeline_);
    vkDestroyShaderModule(device_, cs, nullptr);
    if (pr != VK_SUCCESS) { volFailed_ = true; return false; }

    // Per-frame-in-flight params UBO + blob SSBO (host-visible, mapped).
    const VkDeviceSize blobBytes = static_cast<VkDeviceSize>(kMaxVolumeBlobs) * sizeof(VolumeBlob);
    const VkMemoryPropertyFlags hostVis =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (u32 i = 0; i < kMaxFramesInFlight; ++i) {
        if (!CreateBuffer(256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, hostVis,
                          volParamsBuf_[i], volParamsMem_[i]) ||
            !CreateBuffer(blobBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hostVis,
                          volBlobBuf_[i], volBlobMem_[i])) {
            volFailed_ = true; return false;
        }
        vkMapMemory(device_, volParamsMem_[i], 0, 256, 0, &volParamsMapped_[i]);
        vkMapMemory(device_, volBlobMem_[i], 0, blobBytes, 0, &volBlobMapped_[i]);
    }

    // Descriptor pool + one set per frame-in-flight, each updated once (its own
    // param/blob buffers + the shared storage image).
    VkDescriptorPoolSize ps[3]{};
    ps[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; ps[0].descriptorCount = kMaxFramesInFlight;
    ps[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;  ps[1].descriptorCount = kMaxFramesInFlight;
    ps[2].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; ps[2].descriptorCount = kMaxFramesInFlight;
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = kMaxFramesInFlight; pci.poolSizeCount = 3; pci.pPoolSizes = ps;
    if (vkCreateDescriptorPool(device_, &pci, nullptr, &volDescPool_) != VK_SUCCESS) {
        volFailed_ = true; return false;
    }
    for (u32 i = 0; i < kMaxFramesInFlight; ++i) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = volDescPool_; ai.descriptorSetCount = 1; ai.pSetLayouts = &volSetLayout_;
        if (vkAllocateDescriptorSets(device_, &ai, &volSet_[i]) != VK_SUCCESS) {
            volFailed_ = true; return false;
        }
        VkDescriptorBufferInfo pInfo{volParamsBuf_[i], 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo sInfo{volBlobBuf_[i], 0, VK_WHOLE_SIZE};
        VkDescriptorImageInfo imgInfo{};
        imgInfo.imageView = volStorageView_;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet w[3]{};
        w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = volSet_[i];
        w[0].dstBinding = 0; w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &pInfo;
        w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet = volSet_[i];
        w[1].dstBinding = 1; w[1].descriptorCount = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[1].pImageInfo = &imgInfo;
        w[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[2].dstSet = volSet_[i];
        w[2].dstBinding = 2; w[2].descriptorCount = 1;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[2].pBufferInfo = &sInfo;
        vkUpdateDescriptorSets(device_, 3, w, 0, nullptr);
    }

    HBE_INFO("[Vulkan] Volumetric compute pipeline ready ({}^3 volume).", volDim_);
    return true;
}

void VulkanDevice::SetVolumeParticles(const VolumeBlob* blobs, u32 count,
                                      const VolumeParams& params) {
    volBlobs_ = blobs;
    volBlobCount_ = (blobs && count <= kMaxVolumeBlobs) ? count
                    : (blobs ? kMaxVolumeBlobs : 0u);
    volParams_ = params;
}

void VulkanDevice::SetVolumeGrid(const void* bytes, usize byteSize,
                                 const VolumeRenderParams& params) {
    // Store ptr/size; the upload + descriptor write happen in RunPostStack (the pointer must stay
    // valid through the frame, like SetVolumeParticles).
    volGridBytes_ = (bytes && byteSize > 0) ? bytes : nullptr;
    volGridSize_ = volGridBytes_ ? byteSize : 0u;
    volRenderParams_ = params;
}

// (Re)allocate this frame-in-flight's NanoVDB grid SSBO to hold `size` bytes (host-visible,
// mapped). Grows on demand; safe to free+recreate here because BeginFrame already waited on this
// frame slot's fence, so the previous buffer is no longer in flight.
bool VulkanDevice::EnsureVolumeGridBuffer(usize size) {
    const u32 f = frameIndex_;
    if (volGridBuf_[f] != VK_NULL_HANDLE && volGridCapacity_[f] >= size) return true;
    if (volGridBuf_[f] != VK_NULL_HANDLE) {
        if (volGridMapped_[f]) { vkUnmapMemory(device_, volGridMem_[f]); volGridMapped_[f] = nullptr; }
        vkDestroyBuffer(device_, volGridBuf_[f], nullptr);
        vkFreeMemory(device_, volGridMem_[f], nullptr);
        volGridBuf_[f] = VK_NULL_HANDLE;
        volGridMem_[f] = VK_NULL_HANDLE;
    }
    const VkDeviceSize cap = (static_cast<VkDeviceSize>(size) + 0xFFFFFull) & ~0xFFFFFull; // 1 MiB round
    const VkMemoryPropertyFlags hostVis =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (!CreateBuffer(cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hostVis, volGridBuf_[f], volGridMem_[f]))
        return false;
    vkMapMemory(device_, volGridMem_[f], 0, cap, 0, &volGridMapped_[f]);
    volGridCapacity_[f] = cap;
    return true;
}

void VulkanDevice::DispatchVolumeSplat() {
    if (volBlobCount_ == 0 || volFailed_ || !frameActive_) return; // data-driven
    if (!EnsureVolumeResources()) return;

    // Upload this frame's blobs + params into this slot's buffers.
    std::memcpy(volBlobMapped_[frameIndex_], volBlobs_,
                static_cast<usize>(volBlobCount_) * sizeof(VolumeBlob));
    struct VolCB { // must match VolumeSplat.hlsl's VolumeCB (64 bytes)
        f32 boundsMin[3]; f32 p0;
        f32 boundsMax[3]; f32 p1;
        u32 dim[3];       u32 count;
        f32 densityScale; f32 noiseDetail; f32 noiseScale; f32 p2;
    };
    VolCB cb{};
    cb.boundsMin[0] = volParams_.boundsMin.x; cb.boundsMin[1] = volParams_.boundsMin.y;
    cb.boundsMin[2] = volParams_.boundsMin.z;
    cb.boundsMax[0] = volParams_.boundsMax.x; cb.boundsMax[1] = volParams_.boundsMax.y;
    cb.boundsMax[2] = volParams_.boundsMax.z;
    cb.dim[0] = volDim_; cb.dim[1] = volDim_; cb.dim[2] = volDim_;
    cb.count = volBlobCount_;
    cb.densityScale = volParams_.densityScale;
    cb.noiseDetail = volParams_.noiseDetail;  // splat-side turbulence (de-sphere)
    cb.noiseScale = volParams_.noiseScale;    // stable world scale (no crawl on move)
    std::memcpy(volParamsMapped_[frameIndex_], &cb, sizeof(cb));

    VkCommandBuffer cmd = commandBuffers_[frameIndex_];

    // Cross-frame write-after-read guard: the volume texture is a SINGLE image
    // shared across frames-in-flight (unlike the per-frame blob/param buffers), so
    // this frame's splat WRITE must not begin until the PREVIOUS frame's raymarch
    // READ has finished. A pipeline barrier's first sync scope spans all commands
    // submitted earlier on this (single) queue, so a FRAGMENT->COMPUTE barrier here
    // serializes the prior raymarch read before this dispatch. (D3D12 gets this for
    // free from its UAV<->PIXEL_SHADER_RESOURCE round-trip transition; Vulkan keeps
    // the image permanently GENERAL and so needs it explicitly.) Harmless on frame 0
    // (no prior read in scope).
    {
        VkImageMemoryBarrier war{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        war.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        war.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        war.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        war.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        war.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        war.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        war.image = volImage_;
        war.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &war);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, volSplatPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, volPipelineLayout_, 0, 1,
                            &volSet_[frameIndex_], 0, nullptr);
    const u32 groups = (volDim_ + 3) / 4; // numthreads(4,4,4)
    vkCmdDispatch(cmd, groups, groups, groups);

    // Make the splat writes visible to a later sampled read (VV4 raymarch). The
    // image stays in GENERAL, so this is a pure execution/memory barrier.
    VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    bar.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    bar.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    bar.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image = volImage_;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
}

// ---------------------------------------------------------------------------
// General GPU compute + GPU-writable structured buffers
// ---------------------------------------------------------------------------

VulkanDevice::GpuBufferVk* VulkanDevice::ResolveGpuBuffer(GpuBufferHandle h) {
    if (!h.IsValid() || h.id > gpuBuffers_.size()) return nullptr;
    GpuBufferVk& b = gpuBuffers_[h.id - 1];
    return b.alive ? &b : nullptr;
}

// Set 2, binding 0: STORAGE_BUFFER_DYNAMIC in the VERTEX stage. Dynamic so the
// per-batch element base is a bind-time byte offset (D3D12 does it by offsetting
// the root SRV's GPU virtual address). Its own set index means the dynamic-offset
// count of set 0 - which already has one, the object UBO - is untouched.
bool VulkanDevice::CreateVsBufferSetLayout() {
    if (vsBufferLayout_ != VK_NULL_HANDLE) return true;
    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 1;
    lci.pBindings = &b;
    if (vkCreateDescriptorSetLayout(device_, &lci, nullptr, &vsBufferLayout_) != VK_SUCCESS) {
        HBE_ERROR("[Vulkan] vsBuffer descriptor set layout creation failed.");
        return false;
    }
    VkDescriptorPoolSize ps{};
    ps.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    ps.descriptorCount = kMaxVsBufferSets;
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = kMaxVsBufferSets;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT; // DestroyGpuBuffer frees
    if (vkCreateDescriptorPool(device_, &pci, nullptr, &vsBufferPool_) != VK_SUCCESS) {
        HBE_ERROR("[Vulkan] vsBuffer descriptor pool creation failed.");
        return false;
    }
    return true;
}

GpuBufferHandle VulkanDevice::CreateGpuBuffer(const GpuBufferDesc& desc) {
    if (desc.elementCount == 0 || desc.elementStride == 0) {
        HBE_ERROR("[Vulkan] CreateGpuBuffer: zero elementCount/elementStride.");
        return {};
    }
    if ((desc.usage & GpuBufferUsage::ShaderWrite) && (desc.usage & GpuBufferUsage::CpuWrite)) {
        // Rejected identically on D3D12 (there because the UPLOAD heap cannot
        // carry ALLOW_UNORDERED_ACCESS); kept symmetric so a desc that works on
        // one backend works on both.
        HBE_ERROR("[Vulkan] CreateGpuBuffer: ShaderWrite|CpuWrite is not a legal combination.");
        return {};
    }
    GpuBufferVk b{};
    b.stride = desc.elementStride;
    b.count = desc.elementCount;
    b.usage = desc.usage;
    b.bytes = static_cast<VkDeviceSize>(desc.elementCount) * desc.elementStride;
    // A buffer that is bound at a NON-ZERO dynamic offset needs a bounded descriptor
    // range (VK_WHOLE_SIZE forces the offset to 0 - VUID-...-06715), and that range
    // must still fit the allocation at the largest offset used (VUID-...-01979). So
    // over-allocate by exactly one window: every legal element offset then has a full
    // window behind it. D3D12 needs none of this (root SRVs take any address), which
    // is why maxBindElements is declared in the desc rather than inferred here.
    b.maxBindElements = std::min(desc.maxBindElements, desc.elementCount);
    b.allocBytes =
        b.bytes + static_cast<VkDeviceSize>(b.maxBindElements) * desc.elementStride;
    b.slots = (desc.usage & GpuBufferUsage::CpuWrite) ? framesInFlight_ : 1u;
    b.alive = true;

    VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT; // ReadGpuBuffer
    if (desc.usage & (GpuBufferUsage::ShaderRead | GpuBufferUsage::ShaderWrite))
        usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (desc.usage & GpuBufferUsage::VertexBuffer)
        usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    const VkMemoryPropertyFlags props =
        (desc.usage & GpuBufferUsage::CpuWrite)
            ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
            : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    for (u32 i = 0; i < b.slots; ++i) {
        if (!CreateBuffer(b.allocBytes, usage, props, b.buf[i], b.mem[i])) {
            HBE_ERROR("[Vulkan] CreateGpuBuffer: allocation failed ({} B).",
                      static_cast<u64>(b.allocBytes));
            // No RAII: release the slots that DID succeed before bailing.
            for (u32 j = 0; j < b.slots; ++j) {
                if (b.buf[j]) vkDestroyBuffer(device_, b.buf[j], nullptr);
                if (b.mem[j]) vkFreeMemory(device_, b.mem[j], nullptr);
            }
            return {};
        }
        if (desc.usage & GpuBufferUsage::CpuWrite) {
            void* m = nullptr;
            vkMapMemory(device_, b.mem[i], 0, b.allocBytes, 0, &m);
            b.cpu[i] = static_cast<u8*>(m);
        }
    }

    // One set-2 descriptor per ring slot, written once (the dynamic offset is what
    // changes per bind). The range is BOUNDED whenever the buffer declares a bind
    // window: VK_WHOLE_SIZE would force every dynamic offset to 0
    // (VUID-vkCmdBindDescriptorSets-pDescriptorSets-06715), which is precisely what
    // the per-batch base needs to be non-zero for. The allocation was padded by one
    // window above so offset + range never leaves the buffer (VUID-...-01979).
    if ((desc.usage & GpuBufferUsage::ShaderRead) && CreateVsBufferSetLayout()) {
        for (u32 i = 0; i < b.slots; ++i) {
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = vsBufferPool_;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &vsBufferLayout_;
            if (vkAllocateDescriptorSets(device_, &ai, &b.vsSet[i]) != VK_SUCCESS) {
                HBE_WARN("[Vulkan] CreateGpuBuffer: out of vsBuffer descriptor sets "
                         "(max {}); SetVertexShaderBuffer will skip this buffer.",
                         kMaxVsBufferSets);
                b.vsSet[i] = VK_NULL_HANDLE;
                break;
            }
            const VkDeviceSize range =
                b.maxBindElements
                    ? static_cast<VkDeviceSize>(b.maxBindElements) * b.stride
                    : VK_WHOLE_SIZE;
            VkDescriptorBufferInfo bi{b.buf[i], 0, range};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = b.vsSet[i];
            w.dstBinding = 0;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
            w.pBufferInfo = &bi;
            vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
        }
    }

    u32 index;
    if (!gpuBufferFree_.empty()) {
        index = gpuBufferFree_.back();
        gpuBufferFree_.pop_back();
        gpuBuffers_[index] = b;
    } else {
        index = static_cast<u32>(gpuBuffers_.size());
        gpuBuffers_.push_back(b);
    }
    return GpuBufferHandle{index + 1};
}

void* VulkanDevice::MapGpuBuffer(GpuBufferHandle handle) {
    GpuBufferVk* b = ResolveGpuBuffer(handle);
    if (!b || !(b->usage & GpuBufferUsage::CpuWrite)) return nullptr;
    // Callers map BEFORE RenderScene (the QueueCompute idiom), i.e. before
    // BeginFrame waits on this slot's fence - so wait here, or the CPU could
    // overwrite memory the GPU is still reading from the frame framesInFlight_
    // ago. D3D12 needs no equivalent: MoveToNextFrame already waited on the new
    // slot's fence at the END of the previous EndFrame. The fences are created
    // SIGNALLED, so the first frame does not block.
    vkWaitForFences(device_, 1, &inFlight_[frameIndex_], VK_TRUE, UINT64_MAX);
    return b->cpu[frameIndex_ % b->slots];
}

bool VulkanDevice::ReadGpuBuffer(GpuBufferHandle handle, void* dst, u32 bytes) {
    GpuBufferVk* b = ResolveGpuBuffer(handle);
    if (!b || !dst || bytes == 0 || bytes > b->bytes) return false;
    const u32 slot = frameIndex_ % b->slots;
    if (b->cpu[slot]) { std::memcpy(dst, b->cpu[slot], bytes); return true; }

    // Device-local: flush, copy into a host-visible staging buffer on a one-shot
    // command buffer, wait, map. Debug/validation only (see the RHI comment).
    WaitForGpuIdle();
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    if (!CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      staging, stagingMem)) {
        return false;
    }
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = commandPool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &cbai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    VkBufferMemoryBarrier pre{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    pre.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    pre.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    pre.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre.buffer = b->buf[slot];
    pre.offset = 0;
    pre.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &pre, 0, nullptr);
    VkBufferCopy region{0, 0, bytes};
    vkCmdCopyBuffer(cmd, b->buf[slot], staging, 1, &region);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);
    vkFreeCommandBuffers(device_, commandPool_, 1, &cmd);

    void* mapped = nullptr;
    bool ok = false;
    if (vkMapMemory(device_, stagingMem, 0, bytes, 0, &mapped) == VK_SUCCESS && mapped) {
        std::memcpy(dst, mapped, bytes);
        vkUnmapMemory(device_, stagingMem);
        ok = true;
    }
    vkDestroyBuffer(device_, staging, nullptr);
    vkFreeMemory(device_, stagingMem, nullptr);
    return ok;
}

void VulkanDevice::DestroyGpuBuffer(GpuBufferHandle handle) {
    GpuBufferVk* b = ResolveGpuBuffer(handle);
    if (!b) return;
    WaitForGpuIdle(); // may still be referenced by in-flight command buffers
    if (vsBuffer_.id == handle.id) vsBuffer_ = {};
    for (u32 g = 0; g < particleGpuGroupCount_; ++g) {
        if (particleGpuGroups_[g].buffer.id == handle.id) particleGpuGroups_[g] = {};
    }
    for (u32 i = 0; i < b->slots; ++i) {
        if (b->vsSet[i]) vkFreeDescriptorSets(device_, vsBufferPool_, 1, &b->vsSet[i]);
        b->vsSet[i] = VK_NULL_HANDLE;
        if (b->buf[i]) vkDestroyBuffer(device_, b->buf[i], nullptr);
        if (b->mem[i]) vkFreeMemory(device_, b->mem[i], nullptr); // implicitly unmaps
        b->buf[i] = VK_NULL_HANDLE;
        b->mem[i] = VK_NULL_HANDLE;
        b->cpu[i] = nullptr;
    }
    b->alive = false;
    gpuBufferFree_.push_back(handle.id - 1);
}

ComputePipelineHandle VulkanDevice::CreateComputePipeline(const ComputePipelineDesc& desc) {
    if (!desc.shaderName || desc.uavCount > kMaxComputeUavs || desc.srvCount > kMaxComputeSrvs ||
        desc.constantBytes > kMaxComputeConstantBytes) {
        HBE_ERROR("[Vulkan] CreateComputePipeline: invalid desc.");
        return {};
    }
    ComputePipelineVk p{};
    p.constantBytes = desc.constantBytes;
    p.uavCount = desc.uavCount;
    p.srvCount = desc.srvCount;

    // Layout in the order the binding convention documents: binding 0 = constants
    // UBO, bindings 1..uavCount = the UAVs, then the SRVs. DXC lowers both
    // RWStructuredBuffer and StructuredBuffer to SPIR-V storage buffers, so the
    // descriptor type is the same for both halves; only the HLSL differs.
    VkDescriptorSetLayoutBinding b[1 + kMaxComputeUavs + kMaxComputeSrvs]{};
    u32 n = 0;
    b[n].binding = 0;
    b[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[n].descriptorCount = 1;
    b[n].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    ++n;
    for (u32 i = 0; i < desc.uavCount + desc.srvCount; ++i, ++n) {
        b[n].binding = n;
        b[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[n].descriptorCount = 1;
        b[n].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = n;
    lci.pBindings = b;
    if (vkCreateDescriptorSetLayout(device_, &lci, nullptr, &p.setLayout) != VK_SUCCESS) return {};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &p.setLayout;
    if (vkCreatePipelineLayout(device_, &plci, nullptr, &p.layout) != VK_SUCCESS) {
        vkDestroyDescriptorSetLayout(device_, p.setLayout, nullptr);
        return {};
    }

    const std::string name(desc.shaderName);
    const std::wstring wname(name.begin(), name.end());
    const std::wstring dir = ExecutableDir() + L"shaders\\";
    VkShaderModule cs = LoadShaderModule(dir + wname + L".cs.spv");
    if (cs == VK_NULL_HANDLE) {
        // The fog/ssgi precedent: a kernel missing from cmake/ShaderCompile.cmake
        // produces no file at all. Fail loudly rather than silently dormant.
        HBE_ERROR("[Vulkan] {}.cs.spv missing - is it registered in cmake/ShaderCompile.cmake?",
                  name);
        vkDestroyPipelineLayout(device_, p.layout, nullptr);
        vkDestroyDescriptorSetLayout(device_, p.setLayout, nullptr);
        return {};
    }
    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = cs;
    cpci.stage.pName = desc.entryPoint ? desc.entryPoint : "CSMain";
    cpci.layout = p.layout;
    const VkResult pr =
        vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &cpci, nullptr, &p.pipeline);
    vkDestroyShaderModule(device_, cs, nullptr);
    if (pr != VK_SUCCESS) {
        HBE_ERROR("[Vulkan] vkCreateComputePipelines failed for {}.", name);
        vkDestroyPipelineLayout(device_, p.layout, nullptr);
        vkDestroyDescriptorSetLayout(device_, p.setLayout, nullptr);
        return {};
    }

    // No RAII: any bail-out from here on must release everything built above.
    const auto abandon = [&]() -> ComputePipelineHandle {
        for (u32 i = 0; i < framesInFlight_; ++i) {
            if (p.cb[i]) vkDestroyBuffer(device_, p.cb[i], nullptr);
            if (p.cbMem[i]) vkFreeMemory(device_, p.cbMem[i], nullptr);
        }
        if (p.pool) vkDestroyDescriptorPool(device_, p.pool, nullptr);
        if (p.pipeline) vkDestroyPipeline(device_, p.pipeline, nullptr);
        if (p.layout) vkDestroyPipelineLayout(device_, p.layout, nullptr);
        if (p.setLayout) vkDestroyDescriptorSetLayout(device_, p.setLayout, nullptr);
        return {};
    };

    // Per-frame constants ring: one aligned block per queue slot, so a frame's
    // dispatches never overwrite each other's constants.
    const VkDeviceSize minAlign = deviceProps_.limits.minUniformBufferOffsetAlignment;
    p.cbStride = AlignUp(desc.constantBytes ? desc.constantBytes : 4u, minAlign);
    const VkMemoryPropertyFlags hostVis =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkDeviceSize cbBytes = p.cbStride * kMaxQueuedComputeDispatches;
    for (u32 i = 0; i < framesInFlight_; ++i) {
        if (!CreateBuffer(cbBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, hostVis, p.cb[i],
                          p.cbMem[i])) {
            return abandon();
        }
        void* m = nullptr;
        vkMapMemory(device_, p.cbMem[i], 0, cbBytes, 0, &m);
        p.cbMapped[i] = static_cast<u8*>(m);
    }

    // One descriptor set per (frame slot, queue slot).
    const u32 setCount = framesInFlight_ * kMaxQueuedComputeDispatches;
    VkDescriptorPoolSize ps[2]{};
    u32 psN = 0;
    ps[psN].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    ps[psN].descriptorCount = setCount;
    ++psN;
    if (desc.uavCount + desc.srvCount) {
        ps[psN].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        ps[psN].descriptorCount = setCount * (desc.uavCount + desc.srvCount);
        ++psN;
    }
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = setCount;
    pci.poolSizeCount = psN;
    pci.pPoolSizes = ps;
    if (vkCreateDescriptorPool(device_, &pci, nullptr, &p.pool) != VK_SUCCESS) return abandon();
    for (u32 f = 0; f < framesInFlight_; ++f) {
        VkDescriptorSetLayout layouts[kMaxQueuedComputeDispatches];
        for (u32 i = 0; i < kMaxQueuedComputeDispatches; ++i) layouts[i] = p.setLayout;
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = p.pool;
        ai.descriptorSetCount = kMaxQueuedComputeDispatches;
        ai.pSetLayouts = layouts;
        if (vkAllocateDescriptorSets(device_, &ai, p.sets[f]) != VK_SUCCESS) return abandon();
    }

    p.alive = true;
    computePipes_.push_back(p);
    HBE_INFO("[Vulkan] Compute pipeline '{}' ready ({} UAV, {} SRV, {} B constants).", name,
             desc.uavCount, desc.srvCount, desc.constantBytes);
    return ComputePipelineHandle{static_cast<u32>(computePipes_.size())};
}

void VulkanDevice::QueueCompute(const ComputeDispatch& d) {
    if (!d.pipeline.IsValid() || d.pipeline.id > computePipes_.size()) return;
    if (computeQueueCount_ >= kMaxQueuedComputeDispatches) {
        HBE_WARN("[Vulkan] QueueCompute: more than {} dispatches this frame; dropping.",
                 kMaxQueuedComputeDispatches);
        return;
    }
    QueuedComputeVk& q = computeQueue_[computeQueueCount_++];
    q.d = d;
    q.d.constantBytes = std::min(d.constantBytes, kMaxComputeConstantBytes);
    if (d.constants && q.d.constantBytes) std::memcpy(q.constants, d.constants, q.d.constantBytes);
    q.d.constants = nullptr; // the copy above is what the dispatch reads
}

// Runs every dispatch queued since the last frame. Called from BeginFrame, which
// on Vulkan is the only place compute CAN go (ClearBackBuffer opens a render pass
// that stays open through DrawScene). D3D12's BeginFrame calls its twin at the
// same point so the two frame timelines line up.
void VulkanDevice::ExecuteQueuedCompute() {
    if (computeQueueCount_ == 0 || !frameActive_) { computeQueueCount_ = 0; return; }
    VkCommandBuffer cmd = commandBuffers_[frameIndex_];

    // Cross-frame guard, the buffer twin of the volume splat's image barrier: a device-local
    // buffer is ONE allocation shared by every frame in flight, so this frame's compute must not
    // begin before the previous frame's accesses to it finished. A barrier's first scope spans
    // everything already submitted on this (single) queue.
    //
    // The source scope covers BOTH the previous frame's vertex READ (compute->vertex->compute
    // round-trips: ocean/particle displacement fed to a draw) AND the previous frame's compute
    // WRITE. The latter is REQUIRED by the GPU fluid solver, whose state persists across frames as
    // a compute WRITE (frame f) -> compute READ/WRITE (frame f+1): without COMPUTE_SHADER in the
    // source scope, frame f+1's solver dispatches could observe frame f's writes only partially
    // (nondeterministic corruption). D3D12's twin is the UNCONDITIONAL trailing per-dispatch UAV
    // barrier after every dispatch in ExecuteQueuedCompute - it fires even after the frame's LAST
    // dispatch, so it orders the previous frame's final write against this frame's read. (Note
    // TransitionGpuBuffer no-ops for these buffers: they stay in UNORDERED_ACCESS frame to frame.)
    {
        VkMemoryBarrier war{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        war.srcAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
                            VK_ACCESS_SHADER_WRITE_BIT;
        war.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                                 VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &war, 0, nullptr, 0,
                             nullptr);
    }

    for (u32 qi = 0; qi < computeQueueCount_; ++qi) {
        const QueuedComputeVk& q = computeQueue_[qi];
        ComputePipelineVk& p = computePipes_[q.d.pipeline.id - 1];
        if (!p.alive) continue;

        const u32 uavN = std::min(q.d.uavCount, p.uavCount);
        const u32 srvN = std::min(q.d.srvCount, p.srvCount);
        GpuBufferVk* uav[kMaxComputeUavs] = {};
        GpuBufferVk* srv[kMaxComputeSrvs] = {};
        bool ok = true;
        for (u32 i = 0; i < uavN && ok; ++i) {
            uav[i] = ResolveGpuBuffer(q.d.uavs[i]);
            ok = uav[i] != nullptr;
        }
        for (u32 i = 0; i < srvN && ok; ++i) {
            srv[i] = ResolveGpuBuffer(q.d.srvs[i]);
            ok = srv[i] != nullptr;
        }
        if (!ok) continue;

        // Constants into this (frame, queue) slot, then write the set. The set is
        // private to this slot, so writing it here cannot race a pending submit.
        const VkDeviceSize cbOff = p.cbStride * qi;
        if (q.d.constantBytes) {
            // The slot is strided by the PIPELINE's declared block; the copy is sized
            // by the DISPATCH's. QueueCompute only clamps the latter against
            // kMaxComputeConstantBytes, so a dispatch that passes more than its
            // pipeline declared would write into the next queue slot's block.
            std::memcpy(p.cbMapped[frameIndex_] + cbOff, q.constants,
                        std::min(q.d.constantBytes, p.constantBytes));
        }
        VkDescriptorSet set = p.sets[frameIndex_][qi];
        VkDescriptorBufferInfo infos[1 + kMaxComputeUavs + kMaxComputeSrvs]{};
        VkWriteDescriptorSet writes[1 + kMaxComputeUavs + kMaxComputeSrvs]{};
        u32 w = 0;
        infos[w] = {p.cb[frameIndex_], cbOff, p.cbStride};
        writes[w] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[w].dstSet = set;
        writes[w].dstBinding = 0;
        writes[w].descriptorCount = 1;
        writes[w].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[w].pBufferInfo = &infos[w];
        ++w;
        for (u32 i = 0; i < uavN; ++i, ++w) {
            const u32 s = frameIndex_ % uav[i]->slots;
            infos[w] = {uav[i]->buf[s], 0, VK_WHOLE_SIZE};
            writes[w] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[w].dstSet = set;
            writes[w].dstBinding = 1 + i;
            writes[w].descriptorCount = 1;
            writes[w].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[w].pBufferInfo = &infos[w];
        }
        for (u32 i = 0; i < srvN; ++i, ++w) {
            const u32 s = frameIndex_ % srv[i]->slots;
            infos[w] = {srv[i]->buf[s], 0, VK_WHOLE_SIZE};
            writes[w] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[w].dstSet = set;
            // The layout reserves p.uavCount UAV bindings; skip any the caller
            // left unbound so the SRV bindings stay at their declared indices.
            writes[w].dstBinding = 1 + p.uavCount + i;
            writes[w].descriptorCount = 1;
            writes[w].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[w].pBufferInfo = &infos[w];
        }
        vkUpdateDescriptorSets(device_, w, writes, 0, nullptr);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1, &set, 0,
                                nullptr);
        vkCmdDispatch(cmd, std::max(1u, q.d.groupsX), std::max(1u, q.d.groupsY),
                      std::max(1u, q.d.groupsZ));

        // Compute -> compute, so a following dispatch observes these writes
        // (D3D12's twin is the per-UAV D3D12_RESOURCE_BARRIER_TYPE_UAV).
        if (qi + 1 < computeQueueCount_) {
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0,
                                 nullptr);
        }
    }

    // Compute writes -> this frame's vertex reads (attribute fetch and/or the
    // set-2 structured-buffer read). D3D12 expresses this as a resource-state
    // transition inside TransitionGpuBuffer.
    VkMemoryBarrier post{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    post.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    post.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                         0, 1, &post, 0, nullptr, 0, nullptr);

    computeQueueCount_ = 0; // one frame only, like SetParticles
}

void VulkanDevice::SetVertexShaderBuffer(GpuBufferHandle handle, u32 firstElement) {
    vsBuffer_ = handle;
    vsBufferFirstElement_ = firstElement;
}

void VulkanDevice::DrawScene(const SceneView& view, const DrawItem* items, u32 count) {
    // Every return from here drops this frame's GPU-particle groups. They point into
    // an engine-owned vector that is rebuilt each frame, so carrying one over would
    // dangle - and the group array would fill up and warn-and-drop permanently.
    if (!meshPipelineReady_ || !renderPassActive_) {
        ClearGpuParticleGroups();
        return;
    }
    const bool drawSky = (view.skyIndex != 0) && skyPipeline_ != VK_NULL_HANDLE;
    // Without the post stack there is nothing to resolve, so an empty scene
    // can skip the pass entirely (legacy behavior).
    if ((count == 0 || !items) && !drawSky && !postReady_) {
        ClearGpuParticleGroups();
        return;
    }

    VkCommandBuffer cmd = commandBuffers_[frameIndex_];
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshPipeline_);

    // Bind the bindless texture set (set 1) once for the whole pass.
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 1, 1,
                            &bindlessSet_, 0, nullptr);

    // General VS-visible structured buffer (set 2). The per-batch base rides in
    // the DYNAMIC OFFSET - never in a firstInstance - which is the exact twin of
    // D3D12 adding it to root param 6's GPU virtual address.
    VkDescriptorSet vsBindSet = VK_NULL_HANDLE;
    u32 vsBindDyn = 0;
    if (GpuBufferVk* vb = ResolveGpuBuffer(vsBuffer_)) {
        const u32 s = frameIndex_ % vb->slots;
        const VkDeviceSize off = static_cast<VkDeviceSize>(vsBufferFirstElement_) * vb->stride;
        const VkDeviceSize align = deviceProps_.limits.minStorageBufferOffsetAlignment;
        // A buffer that declared no bind window carries a VK_WHOLE_SIZE descriptor,
        // whose dynamic offset must be 0 (VUID-...-06715). Binding it anywhere else
        // is the caller's error, not something to paper over silently.
        if (off != 0 && vb->maxBindElements == 0) {
            if (!vsBufferAlignWarned_) {
                vsBufferAlignWarned_ = true;
                HBE_ERROR("[Vulkan] SetVertexShaderBuffer: firstElement != 0 on a buffer "
                          "created without GpuBufferDesc::maxBindElements; bind skipped.");
            }
        } else if (vb->vsSet[s] != VK_NULL_HANDLE && off < vb->bytes &&
                   (align == 0 || (off % align) == 0)) {
            vsBindSet = vb->vsSet[s];
            vsBindDyn = static_cast<u32>(off);
        } else if (vb->vsSet[s] != VK_NULL_HANDLE && off < vb->bytes && !vsBufferAlignWarned_) {
            // Deterministic + loud rather than a silent per-backend divergence:
            // D3D12 root SRVs take any byte offset, Vulkan dynamic storage-buffer
            // offsets must be a multiple of minStorageBufferOffsetAlignment.
            vsBufferAlignWarned_ = true;
            HBE_ERROR("[Vulkan] SetVertexShaderBuffer: firstElement*stride ({}) is not a "
                      "multiple of minStorageBufferOffsetAlignment ({}); bind skipped.",
                      static_cast<u64>(off), static_cast<u64>(align));
        }
    }
    // Set 2 MUST be bound for every water draw (the water pipeline statically uses it); when no
    // real vsBuffer_ was set (or its bind was skipped), fall back to a persistent never-read
    // dummy so the Gerstner path does not draw with set 2 unbound (VUID-vkCmdDraw-None-08600).
    if (vsBindSet == VK_NULL_HANDLE) {
        if (!dummyVsBuffer_.IsValid()) {
            GpuBufferDesc dd{};
            dd.elementCount = 1;
            dd.elementStride = 16;
            dd.usage = GpuBufferUsage::ShaderRead;
            dd.debugName = "VsBufferDummy";
            dummyVsBuffer_ = CreateGpuBuffer(dd);
        }
        if (GpuBufferVk* dvb = ResolveGpuBuffer(dummyVsBuffer_)) {
            const u32 ds = frameIndex_ % dvb->slots;
            if (dvb->vsSet[ds] != VK_NULL_HANDLE) vsBindSet = dvb->vsSet[ds];
        }
    }
    if (vsBindSet != VK_NULL_HANDLE)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 2, 1,
                                &vsBindSet, 1, &vsBindDyn);
    vsBuffer_ = {}; // one frame only, like SetParticles

    // Temporal AA: jitter the camera sub-pixel each frame (matches D3D12); the
    // TAA resolve reprojects + accumulates. Kept in the backend (renderer is
    // unaware); gInvViewProj stays consistent with the jittered depth.
    const bool taaOn = postReady_ && taaReady_ && view.post.taaEnabled != 0;
    glm::mat4 curVP = view.viewProj;
    glm::mat4 curInvVP = view.invViewProj;
    const glm::mat4 prevVP = taaPrevViewProj_;
    if (taaOn && sceneW_ > 0 && sceneH_ > 0) {
        const auto halton = [](u32 i, u32 base) {
            f32 f = 1.0f, r = 0.0f;
            while (i) { f /= base; r += f * (i % base); i /= base; }
            return r;
        };
        const u32 s = static_cast<u32>(taaFrame_ % 8) + 1; // 8-sample Halton(2,3)
        const f32 jx = (halton(s, 2) - 0.5f) * 2.0f / static_cast<f32>(sceneW_);
        const f32 jy = (halton(s, 3) - 0.5f) * 2.0f / static_cast<f32>(sceneH_);
        glm::mat4 jitter(1.0f);
        jitter[3][0] = jx;
        jitter[3][1] = jy;
        curVP = jitter * view.viewProj;
        curInvVP = glm::inverse(curVP);
        taaPrevViewProj_ = curVP; // becomes next frame's reprojection basis (prevVP)
        ++taaFrame_;
    }

    // Depth-graded water samples the scene depth during the water draw, which needs a
    // read-only-depth render pass (below). Detect water up front so the frame UBO can point
    // gSceneDepthIndex at the read-only depth SRV; only on the HDR path (matches D3D12's
    // roDsvValid_ gate). The preview path leaves gSceneDepthIndex 0, exactly like D3D12.
    bool waterDepthGrade = false;
    if (postReady_ && waterPipeline_ != VK_NULL_HANDLE && hdrWaterRenderPass_ != VK_NULL_HANDLE &&
        slotDepthRO_ != 0 && items) {
        const u32 md = static_cast<u32>(kObjectArenaSize / objectStride_);
        const u32 dc = std::min(count, md);
        for (u32 i = 0; i < dc; ++i)
            if (items[i].materialFlags & MaterialFlag_Water) { waterDepthGrade = true; break; }
    }

    // Per-frame constants (the post passes read this UBO too).
    FrameUBO fcb;
    fcb.viewProj = curVP;
    fcb.cameraPos = view.cameraPos;
    fcb.exposure = view.exposure;
    fcb.lightDir = glm::normalize(-view.light.direction);
    fcb.lightIntensity = view.light.intensity;
    fcb.lightColor = view.light.color;
    fcb.ambient = view.ambientIntensity;
    fcb.irradianceIndex = view.irradianceIndex;
    fcb.prefilteredIndex = view.prefilteredIndex;
    fcb.brdfLUTIndex = view.brdfLUTIndex;
    fcb.prefilteredMaxLod = view.prefilteredMaxLod;
    fcb.skinLUTIndex = view.skinLUTIndex;
    fcb.taaActive = taaOn ? 1u : 0u; // gates the temporal shadow-dither in ShadowFactor
    fcb.invViewProj = curInvVP;
    fcb.prevViewProj = taaOn ? prevVP : curVP;
    fcb.skyIndex = view.skyIndex;
    fcb.outputLinear = postReady_ ? 1u : 0u;
    for (u32 c = 0; c < kMaxShadowCascades; ++c) {
        fcb.cascadeViewProj[c] = view.cascadeViewProj[c];
    }
    fcb.cascadeSplits = view.cascadeSplits;
    fcb.shadowMapIndex = shadowPassRun_ ? shadowSrvSlot_ : 0;
    fcb.cascadeCount = std::min(view.cascadeCount, kMaxShadowCascades);
    fcb.punctualCount = std::min(view.punctualCount, kMaxPunctualLights);
    std::memcpy(fcb.punctualLights, view.punctualLights, sizeof(fcb.punctualLights));
    fcb.probeCount = std::min(view.probeCount, kMaxProbes);
    std::memcpy(fcb.probes, view.probes, sizeof(fcb.probes));
    fcb.decalCount = std::min(view.decalCount, kMaxDecals);
    std::memcpy(fcb.decals, view.decals, sizeof(fcb.decals));
    std::memcpy(fcb.waveA, view.waterWaveA, sizeof(fcb.waveA));
    std::memcpy(fcb.waveB, view.waterWaveB, sizeof(fcb.waveB));
    fcb.waterShallow = view.waterShallow;
    fcb.waterDeep = view.waterDeep;
    fcb.waterParams = view.waterParams;
    fcb.fftParams[0] = view.fftPatch;
    fcb.fftParams[1] = view.fftHeight;
    // gScreenTexel = 1 / RENDER size (sceneW_/sceneH_), NOT the swapchain (width_): the
    // editor 3D viewport renders smaller than the window, and Water.hlsl samples scene depth
    // at SV_Position * gScreenTexel, so a swapchain-scaled UV mis-taps depth. (W2 depth water)
    fcb.screenTexel[0] = sceneW_ ? 1.0f / static_cast<f32>(sceneW_) : 0.0f;
    fcb.screenTexel[1] = sceneH_ ? 1.0f / static_cast<f32>(sceneH_) : 0.0f;
    fcb.absorptionDepth = view.waterAbsorptionDepth;
    fcb.shorelineWidth = view.waterShorelineWidth;
    fcb.edgeFade = view.waterEdgeFade;
    fcb.sceneDepthIndex = waterDepthGrade ? slotDepthRO_ : 0u; // read-only depth SRV; 0 = skip grading
    fcb.rippleCount = std::min(view.rippleCount, kMaxRipples);
    std::memcpy(fcb.ripples, view.ripples, sizeof(fcb.ripples));
    fcb.giOrigin = view.giOrigin;
    fcb.giInvSpacing = view.giInvSpacing;
    fcb.giDims = view.giDims;
    fcb.weather = {view.cloudCoverage, view.cloudDensity, view.overcast, view.timeSeconds};
    fcb.weather1 = {view.windVelX, view.windVelZ, 0.0f, 0.0f};
    fcb.weather2 = {view.wetness, view.puddles, view.snowAmount, view.precipIntensity};
    fcb.weather3 = {view.puddleScale, view.snowScale, view.cloudVolumetric, view.cloudQuality};
    fcb.giShIndex = view.giShIndex;
    fcb.giDepthIndex = view.giDepthIndex;
    {
        const PostSettings& p = view.post;
        const f32 sizeW = 0.12f * std::max(p.painterlyStrokeLength, 0.05f);
        fcb.stroke0 = {sizeW, 0.40f, p.painterlyStrokeSharp, p.painterlyStrokeFlow};
        fcb.stroke1 = {std::max(p.painterlyStrokeDetail * 2.0f, 0.25f), 0.35f, 0.30f, 1.0f};
    }
    std::memcpy(frameUBOMapped_[frameIndex_], &fcb, sizeof(fcb));

    const u32 maxDraws = static_cast<u32>(kObjectArenaSize / objectStride_);
    const u32 drawCount = std::min(count, maxDraws);

    u32 lastSceneMesh = 0; // consecutive same-mesh draws skip the IA rebinds
    const auto drawItem = [&](u32 i) {
        const DrawItem& it = items[i];
        if (!it.mesh.IsValid() || it.mesh.id > meshes_.size()) return;
        const GpuMeshVk& gm = meshes_[it.mesh.id - 1];

        // Zero-init: the skinning fields are only written for skinned items;
        // uninitialized garbage made non-skinned meshes skin against a wild
        // bone offset -> GPU hang.
        ObjectUBO ocb{};
        ocb.model = it.transform;
        ocb.normalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(it.transform))));
        ocb.baseColor = it.baseColor;
        ocb.metallic = it.metallic;
        ocb.roughness = it.roughness;
        ocb.albedoIndex = it.albedoTexture.index;
        ocb.normalIndex = it.normalTexture.index;
        FillMorphUBO(ocb, it); // facial blendshapes (bindless delta atlas)
        ocb.mrIndex = it.mrTexture.index;
        ocb.aoIndex = it.aoTexture.index;
        ocb.flags = it.materialFlags;
        ocb.subsurfaceColor = it.subsurfaceColor;
        ocb.subsurfaceRadius = it.subsurfaceRadius;
        ocb.thicknessIndex = it.thicknessTexture.index;
        ocb.clearcoat = it.clearcoat;
        ocb.clearcoatRoughness = it.clearcoatRoughness;
        ocb.emissiveColor = it.emissiveColor;
        ocb.emissiveIntensity = it.emissiveIntensity;
        ocb.emissiveIndex = it.emissiveTexture.index;
        ocb.paintColorIndex = it.paintColorTexture.index;
        ocb.paintHeightIndex = it.paintHeightTexture.index;
        ocb.paintOpacity = it.paintOpacity;
        ocb.paintHeightScale = it.paintHeightScale;
        ocb.paintLodBias = it.paintLodBias;
        ocb.paintTexel = it.paintTexel;
        ocb.paintProjMode = it.paintProjMode;
        ocb.paintBoxInvM = it.paintBoxInvM;
        ocb.paintBoxCenter = it.paintBoxCenter;
        ocb.paintBoxScale = it.paintBoxScale;
        ocb.splatAlbedo = {it.splatAlbedo[0].index, it.splatAlbedo[1].index,
                           it.splatAlbedo[2].index, it.splatAlbedo[3].index};
        ocb.splatNormal = {it.splatNormal[0].index, it.splatNormal[1].index,
                           it.splatNormal[2].index, it.splatNormal[3].index};
        ocb.splatMR     = {it.splatMR[0].index, it.splatMR[1].index,
                           it.splatMR[2].index, it.splatMR[3].index};
        ocb.splatRough  = {it.splatRough[0], it.splatRough[1],
                           it.splatRough[2], it.splatRough[3]};
        if (it.bones && it.boneCount > 0) {
            // The shadow pass shares this object UBO (same dynamic offset and
            // double-write trick), so the palette uploaded here serves both.
            const u32 offset = AllocBones(it.bones, it.boneCount);
            if (offset != UINT32_MAX) {
                ocb.skinned = 1;
                ocb.boneOffset = offset;
                ocb.boneCount = it.boneCount;
            }
        }

        // Motion vectors: previous-frame world matrix + previous joint palette
        // (falls back to the current pose when no history -> zero pose motion).
        ocb.prevModel = it.prevTransform;
        ocb.prevBoneOffset = ocb.boneOffset;
        if (ocb.skinned && it.prevBones) {
            const u32 prevOff = AllocBones(it.prevBones, it.boneCount);
            if (prevOff != UINT32_MAX) ocb.prevBoneOffset = prevOff;
        }

        // Instanced run head: upload the whole run's transforms and draw the
        // group in ONE call (followers were skipped by the loop below).
        u32 instances = 1;
        if (it.instanceRun > 1) {
            u32 base = 0;
            if (glm::mat4* inst = AllocInstances(it.instanceRun, base)) {
                for (u32 k = 0; k < it.instanceRun; ++k) {
                    const DrawItem& run = items[i + k];
                    inst[k * 3 + 0] = run.transform;
                    inst[k * 3 + 1] =
                        glm::mat4(glm::transpose(glm::inverse(glm::mat3(run.transform))));
                    inst[k * 3 + 2] = run.prevTransform;
                }
                ocb.instanced = 1;
                ocb.instanceBase = base;
                instances = it.instanceRun;
            }
        }

        const u32 dynOffset = static_cast<u32>(i * objectStride_);
        std::memcpy(objectArenaMapped_[frameIndex_] + dynOffset, &ocb, sizeof(ocb));

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                                &descriptorSets_[frameIndex_], 1, &dynOffset);

        if (it.mesh.id != lastSceneMesh) {
            const VkDeviceSize voff = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &gm.vertexBuffer, &voff);
            vkCmdBindIndexBuffer(cmd, gm.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            lastSceneMesh = it.mesh.id;
        }
        vkCmdDrawIndexed(cmd, gm.indexCount, instances, 0, 0, 0);
    };

    // Opaque pass (transparent items deferred to the blended pass below; items
    // consumed by an instanced run head are skipped - the head draws them).
    for (u32 i = 0; i < drawCount; ++i)
        if (items[i].instanceRun != 0 &&
            !(items[i].materialFlags & MaterialFlag_Transparent))
            drawItem(i);

    // Sky background: after opaques so the depth test rejects covered pixels.
    if (drawSky) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyPipeline_);
        // Set 0 carries a dynamic offset; bind explicitly (covers count == 0).
        const u32 zeroOffset = 0;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                                &descriptorSets_[frameIndex_], 1, &zeroOffset);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }

    // Transparent pass: alpha-blended, back-to-front, over opaques + sky.
    if (meshPipelineTransparent_ != VK_NULL_HANDLE) {
        std::vector<u32> tlist;
        for (u32 i = 0; i < drawCount; ++i)
            if ((items[i].materialFlags & MaterialFlag_Transparent) &&
                !(items[i].materialFlags & MaterialFlag_Water))
                tlist.push_back(i);
        if (!tlist.empty()) {
            std::sort(tlist.begin(), tlist.end(), [&](u32 a, u32 b) {
                const f32 da = glm::distance(glm::vec3(items[a].transform[3]), view.cameraPos);
                const f32 db = glm::distance(glm::vec3(items[b].transform[3]), view.cameraPos);
                return da > db; // farthest first
            });
            VkPipeline cur = meshPipelineTransparent_;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cur);
            for (u32 idx : tlist) {
                // Solid transparents (paint strokes) use the depth-writing variant so
                // DoF/TAA keep them sharp where they float off a surface.
                const VkPipeline want =
                    (meshPipelineTransparentDepth_ != VK_NULL_HANDLE &&
                     (items[idx].materialFlags & MaterialFlag_DepthWrite))
                        ? meshPipelineTransparentDepth_
                        : meshPipelineTransparent_;
                if (want != cur) {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, want);
                    cur = want;
                }
                drawItem(idx);
            }
        }
    }

    // Depth-graded water: end the main HDR pass (its finalLayouts leave colour + depth
    // SHADER_READ_ONLY), then reopen the SAME targets LOADED with the depth attachment
    // READ-ONLY so the water PS can sample scene depth (slotDepthRO_) while depth-testing.
    // Water + particles write no depth, so read-only is safe. renderPassActive_ stays true;
    // RunPostStack's vkCmdEndRenderPass ends THIS pass (its finalLayouts match hdrRenderPass_).
    if (waterDepthGrade) {
        vkCmdEndRenderPass(cmd);
        VkRenderPassBeginInfo wrp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        wrp.renderPass = hdrWaterRenderPass_;
        wrp.framebuffer = hdr_.framebuffer; // compatible: same 4 attachments
        wrp.renderArea.extent = VkExtent2D{sceneW_, sceneH_};
        wrp.clearValueCount = 0; // LOAD - nothing cleared
        vkCmdBeginRenderPass(cmd, &wrp, VK_SUBPASS_CONTENTS_INLINE);
        // Descriptor/pipeline/dynamic-state bindings persist across a render-pass boundary
        // (same pipelineLayout_), but rebind set 1 + reset the negative-height viewport/scissor
        // defensively - mirrors RunPostStack's post-pass rebind. Set 2 (water VS buffer) and the
        // per-draw set 0 are re-applied by the draws themselves.
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 1, 1,
                                &bindlessSet_, 0, nullptr);
        VkViewport wvp{0.0f, static_cast<f32>(sceneH_), static_cast<f32>(sceneW_),
                       -static_cast<f32>(sceneH_), 0.0f, 1.0f};
        VkRect2D wsc{{0, 0}, {sceneW_, sceneH_}};
        vkCmdSetViewport(cmd, 0, 1, &wvp);
        vkCmdSetScissor(cmd, 0, 1, &wsc);
    }

    // Water surfaces: Gerstner-wave reflective water (own pipeline), after the transparent
    // meshes. Frame CB carries the wave/ripple/colour params; drawItem supplies each
    // plane's transform via the shared object UBO path.
    if (waterPipeline_ != VK_NULL_HANDLE) {
        bool waterBound = false;
        for (u32 i = 0; i < drawCount; ++i) {
            if (!(items[i].materialFlags & MaterialFlag_Water)) continue;
            if (!waterBound) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, waterPipeline_);
                waterBound = true;
            }
            drawItem(i);
        }
    }

    GpuMark("scene"); // delta shadow->here = the HDR forward pass (geo+sky+transparent)

    // Particle billboards: depth-tested into the HDR colour (no write), alpha then
    // additive. Set 0 (frame UBO, offset 0) + set 1 (bindless) like the sky pass.
    if (particlePipeline_ != VK_NULL_HANDLE &&
        (particleAlphaCount_ + particleAddCount_) > 0) {
        const u32 maxV = static_cast<u32>(kParticleVertexBufferSize / sizeof(ParticleVertex));
        if ((particleAlphaCount_ + particleAddCount_) > maxV) {
            static bool s_warnedParticleOverflow = false;
            if (!s_warnedParticleOverflow) {
                s_warnedParticleOverflow = true;
                HBE_WARN("Particles: CPU vertex budget exceeded ({} > {} verts); dropping the "
                         "tail. Lower emitter/precip counts or opt into GPU expansion.",
                         particleAlphaCount_ + particleAddCount_, maxV);
            }
        }
        const u32 aN = std::min(particleAlphaCount_, maxV);
        const u32 addN = std::min(particleAddCount_, maxV - aN);
        u8* dst = particleVertexCpu_[frameIndex_];
        if (aN && particleAlpha_) std::memcpy(dst, particleAlpha_, aN * sizeof(ParticleVertex));
        if (addN && particleAdd_)
            std::memcpy(dst + aN * sizeof(ParticleVertex), particleAdd_,
                        addN * sizeof(ParticleVertex));
        const u32 zeroOffset = 0;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                                &descriptorSets_[frameIndex_], 1, &zeroOffset);
        const VkDeviceSize voff = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &particleVertexBuffers_[frameIndex_], &voff);
        if (aN) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, particlePipeline_);
            vkCmdDraw(cmd, aN, 1, 0, 0);
        }
        // GPU-expanded ALPHA batches ride between the two CPU draws so the overall
        // order stays "all alpha, then all additive" - identical to D3D12.
        DrawGpuParticleBatches(cmd, false);
        if (addN) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, particlePipelineAdd_);
            vkCmdDraw(cmd, addN, 1, aN, 0);
        }
        DrawGpuParticleBatches(cmd, true);
    } else if (particleGpuPipeline_ != VK_NULL_HANDLE && particleGpuGroupCount_ > 0) {
        // No CPU billboards this frame, but there are GPU-expanded ones. Set 0 has
        // to be bound here because the block above (which normally does it) is
        // skipped; set 1 is already bound for the whole pass.
        const u32 zeroOffset = 0;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                                &descriptorSets_[frameIndex_], 1, &zeroOffset);
        DrawGpuParticleBatches(cmd, false);
        DrawGpuParticleBatches(cmd, true);
    }
    // One frame only, like SetParticles.
    ClearGpuParticleGroups();

    // GPU profiler: delta scene->here = the particle billboard draws ONLY (alpha +
    // additive), the same window D3D12 measures. It is written INSIDE the still-open
    // HDR render pass, which vkCmdWriteTimestamp permits (GpuMark("scene") above does
    // the same), and it is UNCONDITIONAL - putting it behind `postReady_` would drop
    // the label whenever the post targets are missing and silently fold particle cost
    // into the "ui" bucket, which D3D12 never does.
    GpuMark("particles");

    // HDR resolve: SSAO -> bloom -> tonemap -> FXAA into the final target.
    if (postReady_) {
        RunPostStack(view);
    }
}

void VulkanDevice::SetParticles(const ParticleVertex* alpha, u32 alphaCount,
                                const ParticleVertex* additive, u32 addCount) {
    particleAlpha_ = alpha;
    particleAlphaCount_ = alphaCount;
    particleAdd_ = additive;
    particleAddCount_ = addCount;
}

// APPENDS a group rather than replacing one - see the contract in RHI.h. D3D12's
// twin is character-for-character the same body.
void VulkanDevice::SetGpuParticles(GpuBufferHandle records, const GpuParticleBatch* batches,
                                   u32 count) {
    if (!records.IsValid() || !batches || count == 0) return;
    if (particleGpuGroupCount_ >= kMaxGpuParticleGroups) {
        HBE_WARN("[Vulkan] SetGpuParticles: more than {} record buffers this frame; dropping.",
                 kMaxGpuParticleGroups);
        return;
    }
    GpuParticleGroup& g = particleGpuGroups_[particleGpuGroupCount_++];
    g.buffer = records;
    g.batches = batches;
    g.count = count;
}

// One draw per emitter. The per-batch base rides in set 2's DYNAMIC OFFSET - never
// in a firstInstance, which is 0 here as it is on D3D12 (see the SV_InstanceID /
// gl_InstanceIndex note in RHI.h). D3D12's twin adds the identical byte offset to
// root param 6's GPU virtual address.
void VulkanDevice::DrawGpuParticleBatches(VkCommandBuffer cmd, bool additive) {
    if (particleGpuPipeline_ == VK_NULL_HANDLE || particleGpuGroupCount_ == 0) return;
    const VkDeviceSize align = deviceProps_.limits.minStorageBufferOffsetAlignment;

    bool pipeSet = false;
    for (u32 g = 0; g < particleGpuGroupCount_; ++g) {
        const GpuParticleGroup& grp = particleGpuGroups_[g];
        if (!grp.batches || grp.count == 0) continue;
        GpuBufferVk* rb = ResolveGpuBuffer(grp.buffer);
        if (!rb || rb->stride == 0) continue;
        const u32 slot = frameIndex_ % rb->slots;
        if (rb->vsSet[slot] == VK_NULL_HANDLE) continue;

        // Clamp identically to D3D12: the batch may not exceed the descriptor's bind
        // window. Doing it at the seam (not only at the producers) is what guarantees
        // the two backends draw the same particles even if a producer ever slips.
        const u32 maxCount = rb->maxBindElements > kGpuParticleEmitterElements
                                 ? rb->maxBindElements - kGpuParticleEmitterElements
                                 : 0u;
        for (u32 i = 0; i < grp.count; ++i) {
            const GpuParticleBatch& b = grp.batches[i];
            if (b.count == 0 || (b.additive != 0) != additive) continue;
            const u32 count = maxCount ? std::min(b.count, maxCount) : b.count;
            const VkDeviceSize off = static_cast<VkDeviceSize>(b.recordFirst) * rb->stride;
            const VkDeviceSize need =
                static_cast<VkDeviceSize>(kGpuParticleEmitterElements + count) * rb->stride;
            if (off + need > rb->bytes) continue;
            if (align != 0 && (off % align) != 0) {
                // Loud and deterministic rather than a silent per-backend divergence:
                // D3D12 root SRVs take any offset, Vulkan dynamic storage-buffer
                // offsets do not. Every producer 256-byte-aligns its emitter blocks
                // (rhi::kGpuParticleBlockAlign), which covers the largest limit the
                // spec permits, so reaching this branch means a producer regressed.
                if (!particleGpuAlignWarned_) {
                    particleGpuAlignWarned_ = true;
                    HBE_ERROR("[Vulkan] GPU particle batch offset {} is not a multiple of "
                              "minStorageBufferOffsetAlignment ({}); batch skipped.",
                              static_cast<u64>(off), static_cast<u64>(align));
                }
                continue;
            }
            if (!pipeSet) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  additive ? particleGpuPipelineAdd_ : particleGpuPipeline_);
                pipeSet = true;
            }
            const u32 dyn = static_cast<u32>(off);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 2,
                                    1, &rb->vsSet[slot], 1, &dyn);
            vkCmdDraw(cmd, count * 6u, 1, 0, 0);
        }
    }
}

void VulkanDevice::DrawUIOverlay(const UIVertex* vertices, u32 count) {
    if (uiPipeline_ == VK_NULL_HANDLE || !renderPassActive_ || !vertices || count == 0) {
        return;
    }
    const u32 maxVerts = static_cast<u32>(kUIVertexBufferSize / sizeof(UIVertex));
    count = std::min(count, maxVerts - maxVerts % 3);
    std::memcpy(uiVertexCpu_[frameIndex_], vertices, count * sizeof(UIVertex));

    VkCommandBuffer cmd = commandBuffers_[frameIndex_];
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, uiPipeline_);
    // The shader reads only set 1 (the bindless textures).
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 1, 1,
                            &bindlessSet_, 0, nullptr);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &uiVertexBuffers_[frameIndex_], &offset);
    vkCmdDraw(cmd, count, 1, 0, 0);
}

TextureHandle VulkanDevice::CreateUITarget(u32 width, u32 height) {
    if (uiWorldPipeline_ == VK_NULL_HANDLE || width == 0 || height == 0 ||
        bindlessNextSlot_ >= kMaxBindlessTextures)
        return {};
    if (uiTargets_.size() >= kMaxUITargets) {
        HBE_WARN("[Vulkan] world-UI target limit ({}) reached.", kMaxUITargets);
        return {};
    }

    // MUTABLE image: UNORM attachment view (the UI shader's raw display-space
    // output) + SRGB sampled view (hardware decode when the mesh pass samples the
    // page, exactly like an albedo PNG).
    UITargetVk t;
    t.w = width;
    t.h = height;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ici, nullptr, &t.img) != VK_SUCCESS) return {};
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, t.img, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device_, &mai, nullptr, &t.mem) != VK_SUCCESS) {
        vkDestroyImage(device_, t.img, nullptr);
        return {};
    }
    vkBindImageMemory(device_, t.img, t.mem, 0);

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = t.img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    bool ok = vkCreateImageView(device_, &vci, nullptr, &t.attachView) == VK_SUCCESS;
    vci.format = VK_FORMAT_R8G8B8A8_SRGB;
    ok = ok && vkCreateImageView(device_, &vci, nullptr, &t.sampleView) == VK_SUCCESS;

    if (ok) {
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = worldUIRenderPass_;
        fci.attachmentCount = 1;
        fci.pAttachments = &t.attachView;
        fci.width = width;
        fci.height = height;
        fci.layers = 1;
        ok = vkCreateFramebuffer(device_, &fci, nullptr, &t.fb) == VK_SUCCESS;
    }
    if (!ok) {
        if (t.fb) vkDestroyFramebuffer(device_, t.fb, nullptr);
        if (t.sampleView) vkDestroyImageView(device_, t.sampleView, nullptr);
        if (t.attachView) vkDestroyImageView(device_, t.attachView, nullptr);
        vkDestroyImage(device_, t.img, nullptr);
        vkFreeMemory(device_, t.mem, nullptr);
        return {};
    }

    // One-time transition UNDEFINED -> SHADER_READ_ONLY so sampling the page
    // BEFORE its first DrawUIToTexture (hidden canvas) is legal.
    {
        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool = commandPool_;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(device_, &cbai, &cmd);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = t.img;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                             1, &b);
        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue_);
        vkFreeCommandBuffers(device_, commandPool_, 1, &cmd);
    }

    // Bindless slot samples the SRGB view.
    const u32 slot = bindlessNextSlot_++;
    VkDescriptorImageInfo ii{};
    ii.imageView = t.sampleView;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = bindlessSet_;
    w.dstBinding = 1;
    w.dstArrayElement = slot;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);

    slotViews_[slot] = t.sampleView;
    slotImages_[slot] = t.img;
    uiTargets_[slot] = t;
    HBE_INFO("[Vulkan] world-UI target {}x{} (slot {}).", width, height, slot);
    return TextureHandle{slot};
}

void VulkanDevice::DrawUIToTexture(TextureHandle target, const UIVertex* vertices, u32 count) {
    // Same contract as DrawShadowPass: after BeginFrame, BEFORE the main render
    // pass begins (the scene pass samples the page later this frame). count == 0
    // still runs the render pass for its CLEAR - a fresh/hidden page must read
    // transparent, never uninitialized garbage.
    if (uiWorldPipeline_ == VK_NULL_HANDLE || !frameActive_ || renderPassActive_)
        return;
    const auto it = uiTargets_.find(target.index);
    if (it == uiTargets_.end()) return;
    const UITargetVk& t = it->second;

    if (!vertices) count = 0;
    const u64 remaining = kUIVertexBufferSize - uiWorldVertexHead_;
    const u32 maxVerts = static_cast<u32>(remaining / sizeof(UIVertex));
    count = std::min(count, maxVerts - maxVerts % 3);
    if (count > 0) {
        std::memcpy(uiWorldVertexCpu_[frameIndex_] + uiWorldVertexHead_, vertices,
                    count * sizeof(UIVertex));
    }

    VkCommandBuffer cmd = commandBuffers_[frameIndex_];
    VkClearValue clear{};
    clear.color = {{0.0f, 0.0f, 0.0f, 0.0f}}; // transparent page background
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = worldUIRenderPass_;
    rp.framebuffer = t.fb;
    rp.renderArea = {{0, 0}, {t.w, t.h}};
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    // Negative-height viewport for orientation parity with D3D12 (the shared
    // CPU-side NDC convention the overlay uses).
    VkViewport vp{};
    vp.x = 0.0f;
    vp.y = static_cast<f32>(t.h);
    vp.width = static_cast<f32>(t.w);
    vp.height = -static_cast<f32>(t.h);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    VkRect2D scissor{{0, 0}, {t.w, t.h}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    if (count > 0) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, uiWorldPipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 1, 1,
                                &bindlessSet_, 0, nullptr);
        const VkDeviceSize offset = uiWorldVertexHead_;
        vkCmdBindVertexBuffers(cmd, 0, 1, &uiWorldVertexBuffers_[frameIndex_], &offset);
        vkCmdDraw(cmd, count, 1, 0, 0);
        uiWorldVertexHead_ += static_cast<u64>(count) * sizeof(UIVertex);
    }
    vkCmdEndRenderPass(cmd); // finalLayout hands the image to the fragment shader
}

void VulkanDevice::EndFrame() {
    if (!frameActive_) return;
    VkCommandBuffer cmd = commandBuffers_[frameIndex_];

    if (renderPassActive_) {
        vkCmdEndRenderPass(cmd);
        renderPassActive_ = false;
    }
    // GPU profiler: final mark (UI overlay + ImGui, drawn since FXAA) and hand this
    // slot's timestamps off to be read back when its fence next signals.
    if (gpuProfile_) {
        GpuMark("ui");
        gpuCountSlot_[frameIndex_] = gpuCount_;
        gpuValid_[frameIndex_] = true;
    }
    vkEndCommandBuffer(cmd);

    VkSemaphore waitSem = imageAvailable_[frameIndex_];
    VkSemaphore signalSem = renderFinished_[imageIndex_];
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &waitSem;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &signalSem;
    vkQueueSubmit(queue_, 1, &si, inFlight_[frameIndex_]);

    // Multi-viewport: render + present the ImGui panels dragged out into their own
    // OS windows (each with its own swapchain/surface). Draw data was finalized by
    // ImGui::Render() in RenderUI; this handles the secondary viewports only.
    // (ImGui is editor-only, so this whole block is compiled out of the runtime.)
#if HBE_EDITOR
    if (uiInitialized_ && (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)) {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
#endif

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &signalSem;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &imageIndex_;
    VkResult pr = vkQueuePresentKHR(queue_, &pi);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
        RecreateSwapchain();
    } else if (pr != VK_SUCCESS) {
        HBE_ERROR("[Vulkan] vkQueuePresentKHR failed (VkResult={})", static_cast<i32>(pr));
    }

    frameIndex_ = (frameIndex_ + 1) % framesInFlight_;
}

void VulkanDevice::WaitForGpuIdle() {
    if (device_) vkDeviceWaitIdle(device_);
}

void VulkanDevice::DestroySwapchainDependents() {
    for (VkFramebuffer fb : framebuffers_) {
        if (fb) vkDestroyFramebuffer(device_, fb, nullptr);
    }
    framebuffers_.clear();

    if (depthView_) { vkDestroyImageView(device_, depthView_, nullptr); depthView_ = VK_NULL_HANDLE; }
    if (depthImage_) { vkDestroyImage(device_, depthImage_, nullptr); depthImage_ = VK_NULL_HANDLE; }
    if (depthMemory_) { vkFreeMemory(device_, depthMemory_, nullptr); depthMemory_ = VK_NULL_HANDLE; }

    for (VkImageView v : imageViews_) {
        if (v) vkDestroyImageView(device_, v, nullptr);
    }
    imageViews_.clear();

    for (VkSemaphore s : renderFinished_) {
        if (s) vkDestroySemaphore(device_, s, nullptr);
    }
    renderFinished_.clear();

    if (swapchain_) { vkDestroySwapchainKHR(device_, swapchain_, nullptr); swapchain_ = VK_NULL_HANDLE; }
    images_.clear();
}

bool VulkanDevice::RecreateSwapchain() {
    vkDeviceWaitIdle(device_);
    DestroySwapchainDependents();

    if (!CreateSwapchain()) return false;
    if (!CreateSwapchainImageViews()) return false;
    if (!CreateDepthResources()) return false;
    if (!CreateFramebuffers()) return false;

    VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    renderFinished_.resize(images_.size(), VK_NULL_HANDLE);
    for (auto& s : renderFinished_) {
        if (vkCreateSemaphore(device_, &sem, nullptr, &s) != VK_SUCCESS) return false;
    }
    frameIndex_ = 0;
    HBE_INFO("[Vulkan] Swapchain recreated ({}x{})", extent_.width, extent_.height);
    return true;
}

void VulkanDevice::Resize(u32 width, u32 height) {
    if (width == 0 || height == 0) return;
    // A RESIZE TO THE SAME SIZE HAS NOTHING TO RECREATE. This used to rebuild the whole
    // swapchain unconditionally, and RecreateSwapchain begins with a full device idle - so
    // any caller that pumps Resize on every WM_SIZE, or on a window move, stalled the GPU
    // once per event. D3D12 already returns early on an unchanged size; this is the same
    // cheap guard, and it makes the two backends cost the same for the same call.
    if (width == width_ && height == height_) return;
    width_ = width;
    height_ = height;
    RecreateSwapchain();
}

#if HBE_EDITOR
u64 VulkanDevice::GetTextureUIHandle(TextureHandle handle) {
    if (!uiInitialized_ || handle.index == 0) return 0;
    if (auto it = uiTextureIds_.find(handle.index); it != uiTextureIds_.end()) {
        return it->second;
    }
    // A UI TARGET is handed to ImGui through its UNORM attachView, not the SRGB
    // sampleView that slotViews_ holds: the target carries the UI shader's raw
    // display-space output, and the swapchain the editor presents to is non-sRGB,
    // so an sRGB read would decode once with no re-encode and the `.hbui` authoring
    // canvas would render measurably darker than the same document in the Game tab.
    // The BINDLESS slot keeps the SRGB view - the lit world page IS an albedo map.
    VkImageView srcView = VK_NULL_HANDLE;
    if (const auto uit = uiTargets_.find(handle.index); uit != uiTargets_.end()) {
        srcView = uit->second.attachView;
    } else {
        const auto view = slotViews_.find(handle.index);
        if (view == slotViews_.end()) return 0;
        srcView = view->second;
    }
    if (srcView == VK_NULL_HANDLE) return 0;
    VkDescriptorSet ds = ImGui_ImplVulkan_AddTexture(
        bindlessSampler_, srcView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const u64 id = reinterpret_cast<u64>(ds);
    uiTextureIds_[handle.index] = id;
    return id;
}

bool VulkanDevice::InitUI(void* nativeWindowHandle) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // Multi-viewport: panels detach into their own OS windows (multi-monitor
    // docking). Set BEFORE the backends' Init so the viewport interfaces install.
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    if (!ImGui_ImplWin32_Init(nativeWindowHandle)) {
        HBE_ERROR("[Vulkan] ImGui_ImplWin32_Init failed");
        return false;
    }

    // Multi-viewport: the Win32 platform backend creates the secondary OS windows,
    // but the Vulkan renderer needs a VkSurfaceKHR for each. Provide the Win32
    // surface factory BEFORE ImGui_ImplVulkan_Init (it asserts the handler exists
    // when it installs the viewport interface). The instance already has
    // VK_KHR_win32_surface (the main window uses it).
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        ImGui::GetPlatformIO().Platform_CreateVkSurface =
            [](ImGuiViewport* vp, ImU64 vkInst, const void* alloc, ImU64* outSurface) -> int {
            // vp->PlatformHandleRaw is the secondary window's HWND (an opaque void* here). No
            // HINSTANCE from ImGui -> pass nullptr and let the backend resolve the module.
            // Forward ImGui's allocator so this surface's create/destroy stay symmetric.
            VkSurfaceKHR surf = VK_NULL_HANDLE;
            const VkResult r = vk_surface::CreateWindowSurface(
                reinterpret_cast<VkInstance>(vkInst), vp->PlatformHandleRaw, nullptr, &surf,
                static_cast<const VkAllocationCallbacks*>(alloc));
            *outSurface = reinterpret_cast<ImU64>(surf);
            return static_cast<int>(r);
        };
    }

    ImGui_ImplVulkan_InitInfo init{};
    init.ApiVersion = VK_API_VERSION_1_2;
    init.Instance = instance_;
    init.PhysicalDevice = physical_;
    init.Device = device_;
    init.QueueFamily = queueFamily_;
    init.Queue = queue_;
    init.DescriptorPoolSize = 256; // font + viewport + asset thumbnails
    init.MinImageCount = framesInFlight_;
    init.ImageCount = static_cast<u32>(images_.size());
    init.PipelineInfoMain.RenderPass = renderPass_;
    init.PipelineInfoMain.Subpass = 0;
    init.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    if (!ImGui_ImplVulkan_Init(&init)) {
        HBE_ERROR("[Vulkan] ImGui_ImplVulkan_Init failed");
        return false;
    }
    uiInitialized_ = true;

    if (!CreateViewportTarget(extent_.width, extent_.height)) {
        HBE_WARN("[Vulkan] Viewport target unavailable; scene renders to the window.");
    }

    HBE_INFO("[Vulkan] ImGui initialized.");
    return true;
}

void VulkanDevice::BeginUIFrame() {
    if (!uiInitialized_) return;
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void VulkanDevice::RenderUI() {
    if (!frameActive_) return;
    VkCommandBuffer cmd = commandBuffers_[frameIndex_];

    if (viewportReady_) {
        // End the offscreen scene pass, then run the swapchain UI pass.
        if (renderPassActive_) { vkCmdEndRenderPass(cmd); renderPassActive_ = false; }

        VkClearValue clears[2]{};
        clears[0].color = {{0.05f, 0.05f, 0.06f, 1.0f}};
        clears[1].depthStencil = {1.0f, 0};
        VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rp.renderPass = renderPass_;
        rp.framebuffer = framebuffers_[imageIndex_];
        rp.renderArea.extent = extent_;
        rp.clearValueCount = 2;
        rp.pClearValues = clears;
        vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
        renderPassActive_ = true;

        if (uiInitialized_) {
            ImGui::Render();
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
        }
        vkCmdEndRenderPass(cmd);
        renderPassActive_ = false;
        return;
    }

    if (!uiInitialized_ || !renderPassActive_) return;
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
}

void VulkanDevice::ShutdownUI() {
    if (!uiInitialized_) return;
    if (device_) vkDeviceWaitIdle(device_);
    DestroyViewportTarget();
    DestroyPreviewTargets();
    if (prevImguiTex_) { ImGui_ImplVulkan_RemoveTexture(prevImguiTex_); prevImguiTex_ = VK_NULL_HANDLE; }
    if (vpImguiTex_) { ImGui_ImplVulkan_RemoveTexture(vpImguiTex_); vpImguiTex_ = VK_NULL_HANDLE; }
    if (vpRenderPass_) { vkDestroyRenderPass(device_, vpRenderPass_, nullptr); vpRenderPass_ = VK_NULL_HANDLE; }
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    uiInitialized_ = false;
}

void VulkanDevice::DestroyViewportTarget() {
    // Note: vpImguiTex_ (ImGui descriptor set) is kept stable across resizes and
    // only freed at shutdown, so draw commands recorded before a resize stay valid.
    if (vpFramebuffer_) { vkDestroyFramebuffer(device_, vpFramebuffer_, nullptr); vpFramebuffer_ = VK_NULL_HANDLE; }
    if (vpColorView_) { vkDestroyImageView(device_, vpColorView_, nullptr); vpColorView_ = VK_NULL_HANDLE; }
    if (vpColor_) { vkDestroyImage(device_, vpColor_, nullptr); vpColor_ = VK_NULL_HANDLE; }
    if (vpColorMem_) { vkFreeMemory(device_, vpColorMem_, nullptr); vpColorMem_ = VK_NULL_HANDLE; }
    if (vpDepthView_) { vkDestroyImageView(device_, vpDepthView_, nullptr); vpDepthView_ = VK_NULL_HANDLE; }
    if (vpDepth_) { vkDestroyImage(device_, vpDepth_, nullptr); vpDepth_ = VK_NULL_HANDLE; }
    if (vpDepthMem_) { vkFreeMemory(device_, vpDepthMem_, nullptr); vpDepthMem_ = VK_NULL_HANDLE; }
    if (readbackBuffer_) { vkDestroyBuffer(device_, readbackBuffer_, nullptr); readbackBuffer_ = VK_NULL_HANDLE; }
    if (readbackMem_) { vkFreeMemory(device_, readbackMem_, nullptr); readbackMem_ = VK_NULL_HANDLE; }
    readbackSize_ = 0;
    viewportReady_ = false;
}

bool VulkanDevice::ReadbackViewportColor(std::vector<u8>& outRGBA, u32& w, u32& h) {
    if (!viewportReady_ || vpColor_ == VK_NULL_HANDLE) return false;
    w = vpW_;
    h = vpH_;
    const VkDeviceSize size = static_cast<VkDeviceSize>(w) * h * 4;

    // Reuse the staging buffer across frames (grow as needed) - a movie render calls
    // this every frame, so per-call alloc/free would churn device memory.
    if (readbackBuffer_ == VK_NULL_HANDLE || readbackSize_ < size) {
        if (readbackBuffer_) {
            vkDestroyBuffer(device_, readbackBuffer_, nullptr);
            vkFreeMemory(device_, readbackMem_, nullptr);
            readbackBuffer_ = VK_NULL_HANDLE;
        }
        if (!CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          readbackBuffer_, readbackMem_))
            return false;
        readbackSize_ = size;
    }
    VkBuffer buf = readbackBuffer_;
    VkDeviceMemory mem = readbackMem_;

    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = commandPool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &cbai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    const auto barrier = [&](VkImageLayout o, VkImageLayout n, VkAccessFlags sa, VkAccessFlags da,
                             VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = o;
        b.newLayout = n;
        b.srcAccessMask = sa;
        b.dstAccessMask = da;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = vpColor_;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    // The render pass leaves vpColor_ in SHADER_READ_ONLY_OPTIMAL.
    barrier(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {w, h, 1}; // tightly packed rows (bufferRowLength = 0)
    vkCmdCopyImageToBuffer(cmd, vpColor_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &r);
    barrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);
    vkFreeCommandBuffers(device_, commandPool_, 1, &cmd);

    void* p = nullptr;
    vkMapMemory(device_, mem, 0, size, 0, &p);
    outRGBA.resize(static_cast<usize>(w) * h * 4);
    const u8* s = static_cast<const u8*>(p);
    const bool bgra =
        (swapFormat_ == VK_FORMAT_B8G8R8A8_UNORM || swapFormat_ == VK_FORMAT_B8G8R8A8_SRGB);
    const usize px = static_cast<usize>(w) * h;
    for (usize i = 0; i < px; ++i) {
        outRGBA[i * 4 + 0] = bgra ? s[i * 4 + 2] : s[i * 4 + 0]; // R
        outRGBA[i * 4 + 1] = s[i * 4 + 1];                       // G
        outRGBA[i * 4 + 2] = bgra ? s[i * 4 + 0] : s[i * 4 + 2]; // B
        outRGBA[i * 4 + 3] = s[i * 4 + 3];                       // A
    }
    vkUnmapMemory(device_, mem);
    return true; // buffer is retained (readbackBuffer_) for reuse; freed in DestroyViewportTarget
}

bool VulkanDevice::CreateViewportTarget(u32 w, u32 h) {
    if (w == 0 || h == 0) return false;
    vkDeviceWaitIdle(device_);
    DestroyViewportTarget();

    // Color image (color attachment + sampled).
    {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = swapFormat_;
        ici.extent = {w, h, 1};
        ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        // TRANSFER_SRC_BIT: required so ReadbackViewportColor can copy this image to
        // a host buffer for the offline movie render.
        ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(device_, &ici, nullptr, &vpColor_), "vkCreateImage(vpColor)");
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device_, vpColor_, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkAllocateMemory(device_, &ai, nullptr, &vpColorMem_), "vkAllocateMemory(vpColor)");
        vkBindImageMemory(device_, vpColor_, vpColorMem_, 0);
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = vpColor_; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = swapFormat_;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &vpColorView_), "vkCreateImageView(vpColor)");
    }

    // Depth image.
    {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = depthFormat_;
        ici.extent = {w, h, 1};
        ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(device_, &ici, nullptr, &vpDepth_), "vkCreateImage(vpDepth)");
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device_, vpDepth_, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkAllocateMemory(device_, &ai, nullptr, &vpDepthMem_), "vkAllocateMemory(vpDepth)");
        vkBindImageMemory(device_, vpDepth_, vpDepthMem_, 0);
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = vpDepth_; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = depthFormat_;
        vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &vpDepthView_), "vkCreateImageView(vpDepth)");
    }

    // Offscreen render pass (created once; compatible with the mesh pipeline).
    if (!vpRenderPass_) {
        VkAttachmentDescription color{};
        color.format = swapFormat_;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentDescription depth{};
        depth.format = depthFormat_;
        depth.samples = VK_SAMPLE_COUNT_1_BIT;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;
        subpass.pDepthStencilAttachment = &depthRef;

        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass = 0;
        deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        deps[0].dstStageMask = deps[0].srcStageMask;
        deps[0].srcAccessMask = 0;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass = 0;
        deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        const VkAttachmentDescription attachments[2] = {color, depth};
        VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        ci.attachmentCount = 2; ci.pAttachments = attachments;
        ci.subpassCount = 1; ci.pSubpasses = &subpass;
        ci.dependencyCount = 2; ci.pDependencies = deps;
        VK_CHECK(vkCreateRenderPass(device_, &ci, nullptr, &vpRenderPass_), "vkCreateRenderPass(viewport)");
    }

    // Framebuffer.
    {
        const VkImageView attachments[2] = {vpColorView_, vpDepthView_};
        VkFramebufferCreateInfo ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        ci.renderPass = vpRenderPass_;
        ci.attachmentCount = 2; ci.pAttachments = attachments;
        ci.width = w; ci.height = h; ci.layers = 1;
        VK_CHECK(vkCreateFramebuffer(device_, &ci, nullptr, &vpFramebuffer_), "vkCreateFramebuffer(viewport)");
    }

    // Create the ImGui texture once; on resize, update the same descriptor set
    // in place so previously-recorded ImGui::Image commands remain valid.
    if (vpImguiTex_ == VK_NULL_HANDLE) {
        vpImguiTex_ = ImGui_ImplVulkan_AddTexture(bindlessSampler_, vpColorView_,
                                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    } else {
        VkDescriptorImageInfo ii{};
        ii.sampler = bindlessSampler_;
        ii.imageView = vpColorView_;
        ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet w0{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w0.dstSet = vpImguiTex_;
        w0.dstBinding = 0;
        w0.descriptorCount = 1;
        w0.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w0.pImageInfo = &ii;
        vkUpdateDescriptorSets(device_, 1, &w0, 0, nullptr);
    }

    vpW_ = w; vpH_ = h;
    viewportReady_ = true;
    return true;
}

void VulkanDevice::DestroyPreviewTargets() {
    const auto destroy = [&](PostTargetVk& t) {
        if (t.framebuffer) { vkDestroyFramebuffer(device_, t.framebuffer, nullptr); t.framebuffer = VK_NULL_HANDLE; }
        if (t.view) { vkDestroyImageView(device_, t.view, nullptr); t.view = VK_NULL_HANDLE; }
        if (t.image) { vkDestroyImage(device_, t.image, nullptr); t.image = VK_NULL_HANDLE; }
        if (t.memory) { vkFreeMemory(device_, t.memory, nullptr); t.memory = VK_NULL_HANDLE; }
        t.width = t.height = 0;
    };
    destroy(prevHdr_);
    destroy(prevLdr_);
    if (prevDepthView_) { vkDestroyImageView(device_, prevDepthView_, nullptr); prevDepthView_ = VK_NULL_HANDLE; }
    if (prevDepth_) { vkDestroyImage(device_, prevDepth_, nullptr); prevDepth_ = VK_NULL_HANDLE; }
    if (prevDepthMem_) { vkFreeMemory(device_, prevDepthMem_, nullptr); prevDepthMem_ = VK_NULL_HANDLE; }
    previewReady_ = false;
}

bool VulkanDevice::CreatePreviewTargets(u32 w, u32 h) {
    if (w == 0 || h == 0 || !postPipelinesReady_) return false;
    vkDeviceWaitIdle(device_);
    DestroyPreviewTargets();

    if (slotPrevHdr_ == 0) {
        if (bindlessNextSlot_ >= kMaxBindlessTextures) return false;
        slotPrevHdr_ = bindlessNextSlot_++;
    }

    // HDR color (framebuffer below pairs it with the preview depth).
    if (!CreatePostTarget(w, h, VK_FORMAT_R16G16B16A16_SFLOAT, VK_NULL_HANDLE,
                          slotPrevHdr_, prevHdr_)) {
        return false;
    }
    {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = depthFormat_;
        ici.extent = {w, h, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(device_, &ici, nullptr, &prevDepth_), "vkCreateImage(prevDepth)");
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device_, prevDepth_, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkAllocateMemory(device_, &mai, nullptr, &prevDepthMem_),
                 "vkAllocateMemory(prevDepth)");
        vkBindImageMemory(device_, prevDepth_, prevDepthMem_, 0);
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = prevDepth_;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = depthFormat_;
        vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &prevDepthView_),
                 "vkCreateImageView(prevDepth)");

        const VkImageView attachments[2] = {prevHdr_.view, prevDepthView_};
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = previewRenderPass_; // single colour + depth
        fci.attachmentCount = 2;
        fci.pAttachments = attachments;
        fci.width = w;
        fci.height = h;
        fci.layers = 1;
        VK_CHECK(vkCreateFramebuffer(device_, &fci, nullptr, &prevHdr_.framebuffer),
                 "vkCreateFramebuffer(prevHdr)");
    }

    // Tonemapped LDR shown by ImGui. Slot 0 is unused as a bindless input
    // here, but CreatePostTarget needs one: reuse the HDR slot? No - give it
    // none by writing to a scratch... it must register SOMEWHERE, so park it
    // on its own slot too (cheap, and keeps the helper simple).
    if (prevLdr_.image == VK_NULL_HANDLE) {
        static u32 ldrSlot = 0;
        if (ldrSlot == 0) {
            if (bindlessNextSlot_ >= kMaxBindlessTextures) return false;
            ldrSlot = bindlessNextSlot_++;
        }
        if (!CreatePostTarget(w, h, VK_FORMAT_R8G8B8A8_UNORM, postPass8_, ldrSlot,
                              prevLdr_)) {
            return false;
        }
    }

    if (prevImguiTex_ == VK_NULL_HANDLE) {
        prevImguiTex_ = ImGui_ImplVulkan_AddTexture(bindlessSampler_, prevLdr_.view,
                                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    } else {
        VkDescriptorImageInfo ii{};
        ii.sampler = bindlessSampler_;
        ii.imageView = prevLdr_.view;
        ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet w0{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w0.dstSet = prevImguiTex_;
        w0.dstBinding = 0;
        w0.descriptorCount = 1;
        w0.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w0.pImageInfo = &ii;
        vkUpdateDescriptorSets(device_, 1, &w0, 0, nullptr);
    }

    prevW_ = w;
    prevH_ = h;
    return true;
}

void VulkanDevice::DrawPreviewScene(const SceneView& view, const DrawItem* items, u32 count) {
    if (!frameActive_ || renderPassActive_ || !previewReady_ || !meshPipelineReady_ ||
        !postPipelinesReady_ || meshPipelineSingle_ == VK_NULL_HANDLE) {
        return;
    }
    VkCommandBuffer cmd = commandBuffers_[frameIndex_];

    // Preview frame constants (own UBO; the shared one belongs to the scene).
    {
        FrameUBO fcb{};
        fcb.viewProj = view.viewProj;
        fcb.cameraPos = view.cameraPos;
        fcb.exposure = view.exposure;
        fcb.lightDir = glm::normalize(-view.light.direction);
        fcb.lightIntensity = view.light.intensity;
        fcb.lightColor = view.light.color;
        fcb.ambient = view.ambientIntensity;
        fcb.irradianceIndex = view.irradianceIndex;
        fcb.prefilteredIndex = view.prefilteredIndex;
        fcb.brdfLUTIndex = view.brdfLUTIndex;
        fcb.prefilteredMaxLod = view.prefilteredMaxLod;
        fcb.invViewProj = view.invViewProj;
        fcb.prevViewProj = view.viewProj;
        fcb.skyIndex = view.skyIndex;
        fcb.outputLinear = 1; // tonemapped below
        fcb.punctualCount = std::min(view.punctualCount, kMaxPunctualLights);
        std::memcpy(fcb.punctualLights, view.punctualLights, sizeof(fcb.punctualLights));
        fcb.probeCount = std::min(view.probeCount, kMaxProbes);
        std::memcpy(fcb.probes, view.probes, sizeof(fcb.probes));
        fcb.decalCount = std::min(view.decalCount, kMaxDecals);
        std::memcpy(fcb.decals, view.decals, sizeof(fcb.decals));
        std::memcpy(fcb.waveA, view.waterWaveA, sizeof(fcb.waveA));
        std::memcpy(fcb.waveB, view.waterWaveB, sizeof(fcb.waveB));
        fcb.waterShallow = view.waterShallow;
        fcb.waterDeep = view.waterDeep;
        fcb.waterParams = view.waterParams;
        fcb.fftParams[0] = view.fftPatch;
        fcb.fftParams[1] = view.fftHeight;
        fcb.screenTexel[0] = sceneW_ ? 1.0f / static_cast<f32>(sceneW_) : 0.0f;
        fcb.screenTexel[1] = sceneH_ ? 1.0f / static_cast<f32>(sceneH_) : 0.0f;
        fcb.absorptionDepth = view.waterAbsorptionDepth;
        fcb.shorelineWidth = view.waterShorelineWidth;
        fcb.edgeFade = view.waterEdgeFade;
        fcb.rippleCount = std::min(view.rippleCount, kMaxRipples);
        std::memcpy(fcb.ripples, view.ripples, sizeof(fcb.ripples));
        fcb.giOrigin = view.giOrigin;
        fcb.giInvSpacing = view.giInvSpacing;
        fcb.giDims = view.giDims;
        fcb.weather = {view.cloudCoverage, view.cloudDensity, view.overcast, view.timeSeconds};
    fcb.weather1 = {view.windVelX, view.windVelZ, 0.0f, 0.0f};
        fcb.weather2 = {view.wetness, view.puddles, view.snowAmount, view.precipIntensity};
        fcb.weather3 = {view.puddleScale, view.snowScale, view.cloudVolumetric, view.cloudQuality};
        fcb.giShIndex = view.giShIndex;
        fcb.giDepthIndex = view.giDepthIndex;
        std::memcpy(previewFrameUBOMapped_[frameIndex_], &fcb, sizeof(fcb));
    }

    // Preview constants live at the object arena's TAIL: count object UBOs
    // plus one PostUBO, far away from the main scene's draws.
    const u32 maxDraws = static_cast<u32>(kObjectArenaSize / objectStride_);
    const u32 drawCount = std::min(count, std::min(maxDraws / 4, 256u));
    const u32 tailBase =
        static_cast<u32>(kObjectArenaSize - (drawCount + 1) * objectStride_);

    // --- HDR mini scene pass --------------------------------------------------
    VkClearValue clears[2]{};
    clears[0].color = {{0.10f, 0.105f, 0.12f, 1.0f}};
    clears[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = previewRenderPass_; // single colour + depth (no G-buffer)
    rp.framebuffer = prevHdr_.framebuffer;
    rp.renderArea.extent = {prevW_, prevH_};
    rp.clearValueCount = 2;
    rp.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{};
    vp.y = static_cast<f32>(prevH_);
    vp.width = static_cast<f32>(prevW_);
    vp.height = -static_cast<f32>(prevH_);
    vp.maxDepth = 1.0f;
    VkRect2D scissor{{0, 0}, {prevW_, prevH_}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshPipelineSingle_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 1, 1,
                            &bindlessSet_, 0, nullptr);

    for (u32 i = 0; i < drawCount; ++i) {
        const DrawItem& it = items[i];
        if (!it.mesh.IsValid() || it.mesh.id > meshes_.size()) continue;
        const GpuMeshVk& gm = meshes_[it.mesh.id - 1];

        ObjectUBO ocb{};
        ocb.model = it.transform;
        ocb.normalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(it.transform))));
        ocb.baseColor = it.baseColor;
        ocb.metallic = it.metallic;
        ocb.roughness = it.roughness;
        ocb.albedoIndex = it.albedoTexture.index;
        ocb.normalIndex = it.normalTexture.index;
        FillMorphUBO(ocb, it); // facial blendshapes (bindless delta atlas)
        ocb.mrIndex = it.mrTexture.index;
        ocb.aoIndex = it.aoTexture.index;
        ocb.flags = it.materialFlags;
        ocb.subsurfaceColor = it.subsurfaceColor;
        ocb.subsurfaceRadius = it.subsurfaceRadius;
        ocb.clearcoat = it.clearcoat;
        ocb.clearcoatRoughness = it.clearcoatRoughness;
        ocb.emissiveColor = it.emissiveColor;
        ocb.emissiveIntensity = it.emissiveIntensity;
        ocb.emissiveIndex = it.emissiveTexture.index;
        ocb.prevModel = it.transform; // preview needs no motion vectors

        const u32 dynOffset = tailBase + static_cast<u32>(i * objectStride_);
        std::memcpy(objectArenaMapped_[frameIndex_] + dynOffset, &ocb, sizeof(ocb));
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                                &previewSets_[frameIndex_], 1, &dynOffset);
        const VkDeviceSize voff = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &gm.vertexBuffer, &voff);
        vkCmdBindIndexBuffer(cmd, gm.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, gm.indexCount, 1, 0, 0, 0);
    }
    if (view.skyIndex != 0 && skyPipelineSingle_ != VK_NULL_HANDLE) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyPipelineSingle_);
        const u32 zeroOffset = tailBase; // sky reads only b0; any valid offset
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                                &previewSets_[frameIndex_], 1, &zeroOffset);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    vkCmdEndRenderPass(cmd);

    // --- Tonemap into the LDR preview (no bloom/AO/vignette) -------------------
    {
        PostUBO cb;
        cb.input0 = slotPrevHdr_;
        cb.outTexel = {1.0f / prevW_, 1.0f / prevH_};
        cb.inTexel = cb.outTexel;
        cb.params0 = {0.0f, 0.0f, 0.0f, 1.0f}; // no bloom, no AO, no vignette
        cb.params1 = {1.0f, 0.0f, 0.0f, 0.0f}; // unity contrast
        const u32 postOffset = tailBase + drawCount * static_cast<u32>(objectStride_);
        std::memcpy(objectArenaMapped_[frameIndex_] + postOffset, &cb, sizeof(cb));

        VkRenderPassBeginInfo rp2{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rp2.renderPass = postPass8_;
        rp2.framebuffer = prevLdr_.framebuffer;
        rp2.renderArea.extent = {prevLdr_.width, prevLdr_.height};
        vkCmdBeginRenderPass(cmd, &rp2, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport pvp{0.0f, 0.0f, static_cast<f32>(prevLdr_.width),
                       static_cast<f32>(prevLdr_.height), 0.0f, 1.0f};
        VkRect2D pscissor{{0, 0}, {prevLdr_.width, prevLdr_.height}};
        vkCmdSetViewport(cmd, 0, 1, &pvp);
        vkCmdSetScissor(cmd, 0, 1, &pscissor);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tonemapPipe_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                                &previewSets_[frameIndex_], 1, &postOffset);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }
}
#endif // HBE_EDITOR

VulkanDevice::~VulkanDevice() {
    if (!device_) {
        if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
        if (instance_) vkDestroyInstance(instance_, nullptr);
        return;
    }

    vkDeviceWaitIdle(device_);
#if HBE_EDITOR
    ShutdownUI();
#endif

    for (GpuMeshVk& m : meshes_) {
        if (m.vertexBuffer) vkDestroyBuffer(device_, m.vertexBuffer, nullptr);
        if (m.vertexMemory) vkFreeMemory(device_, m.vertexMemory, nullptr);
        if (m.indexBuffer) vkDestroyBuffer(device_, m.indexBuffer, nullptr);
        if (m.indexMemory) vkFreeMemory(device_, m.indexMemory, nullptr);
        if (m.strokeBuffer) vkDestroyBuffer(device_, m.strokeBuffer, nullptr);
        if (m.strokeMemory) vkFreeMemory(device_, m.strokeMemory, nullptr);
    }
    meshes_.clear();

    for (GpuTextureVk& t : textures_) {
        if (t.view) vkDestroyImageView(device_, t.view, nullptr);
        if (t.image) vkDestroyImage(device_, t.image, nullptr);
        if (t.memory) vkFreeMemory(device_, t.memory, nullptr);
    }
    textures_.clear();
    if (bindlessPool_) vkDestroyDescriptorPool(device_, bindlessPool_, nullptr);
    if (bindlessLayout_) vkDestroyDescriptorSetLayout(device_, bindlessLayout_, nullptr);
    if (bindlessSampler_) vkDestroySampler(device_, bindlessSampler_, nullptr);

    if (shadowPipeline_) vkDestroyPipeline(device_, shadowPipeline_, nullptr);
    if (shadowFramebuffer_) vkDestroyFramebuffer(device_, shadowFramebuffer_, nullptr);
    if (shadowRenderPass_) vkDestroyRenderPass(device_, shadowRenderPass_, nullptr);
    if (shadowView_) vkDestroyImageView(device_, shadowView_, nullptr);
    if (shadowImage_) vkDestroyImage(device_, shadowImage_, nullptr);
    if (shadowMemory_) vkFreeMemory(device_, shadowMemory_, nullptr);

    // HDR/post stack.
    DestroyPostTargets();
    if (ssaoPipe_) vkDestroyPipeline(device_, ssaoPipe_, nullptr);
    if (ssaoBlurPipe_) vkDestroyPipeline(device_, ssaoBlurPipe_, nullptr);
    if (bloomDownPipe_) vkDestroyPipeline(device_, bloomDownPipe_, nullptr);
    if (bloomUpPipe_) vkDestroyPipeline(device_, bloomUpPipe_, nullptr);
    if (tonemapPipe_) vkDestroyPipeline(device_, tonemapPipe_, nullptr);
    if (fxaaPipe_) vkDestroyPipeline(device_, fxaaPipe_, nullptr);
    if (taaPipe_) vkDestroyPipeline(device_, taaPipe_, nullptr);
    if (dofPipe_) vkDestroyPipeline(device_, dofPipe_, nullptr);
    if (motionBlurPipe_) vkDestroyPipeline(device_, motionBlurPipe_, nullptr);
    if (ssrPipe_) vkDestroyPipeline(device_, ssrPipe_, nullptr);
    if (exposurePipe_) vkDestroyPipeline(device_, exposurePipe_, nullptr);
    if (volPipe_) vkDestroyPipeline(device_, volPipe_, nullptr);
    if (volPartPipe_) vkDestroyPipeline(device_, volPartPipe_, nullptr); // raymarch pipeline (splat)
    if (volRaymarchPipe_) vkDestroyPipeline(device_, volRaymarchPipe_, nullptr); // NanoVDB raymarch

    // Volumetric compute-splat resources (EnsureVolumeResources). The volume image /
    // backing memory / sampled view live in textures_ (already freed above); only the
    // dedicated storage views + the compute pipeline/layout/pool + per-frame blob and
    // param buffers are separate and must be released here. Device is idle (5296).
    for (auto& [slot, view] : volumeStorageView_)
        if (view) vkDestroyImageView(device_, view, nullptr);
    volumeStorageView_.clear();
    for (u32 i = 0; i < framesInFlight_; ++i) {
        if (volParamsBuf_[i]) vkDestroyBuffer(device_, volParamsBuf_[i], nullptr);
        if (volParamsMem_[i]) vkFreeMemory(device_, volParamsMem_[i], nullptr); // implicitly unmaps
        if (volBlobBuf_[i]) vkDestroyBuffer(device_, volBlobBuf_[i], nullptr);
        if (volBlobMem_[i]) vkFreeMemory(device_, volBlobMem_[i], nullptr); // implicitly unmaps
        if (volGridBuf_[i]) vkDestroyBuffer(device_, volGridBuf_[i], nullptr); // NanoVDB grid SSBO
        if (volGridMem_[i]) vkFreeMemory(device_, volGridMem_[i], nullptr);    // implicitly unmaps
    }
    if (volDescPool_) vkDestroyDescriptorPool(device_, volDescPool_, nullptr);
    if (volSplatPipeline_) vkDestroyPipeline(device_, volSplatPipeline_, nullptr);
    if (volPipelineLayout_) vkDestroyPipelineLayout(device_, volPipelineLayout_, nullptr);
    if (volSetLayout_) vkDestroyDescriptorSetLayout(device_, volSetLayout_, nullptr);

    // General GPU buffers + compute pipelines (CreateGpuBuffer /
    // CreateComputePipeline). Vulkan has no RAII, so everything is released by
    // hand here - this site has NO D3D12 counterpart (its ComPtr members and the
    // gpuBuffers_/computePipes_ vectors release themselves). Device is idle.
    for (GpuBufferVk& gb : gpuBuffers_) {
        if (!gb.alive) continue;
        for (u32 i = 0; i < gb.slots; ++i) {
            if (gb.buf[i]) vkDestroyBuffer(device_, gb.buf[i], nullptr);
            if (gb.mem[i]) vkFreeMemory(device_, gb.mem[i], nullptr); // implicitly unmaps
        }
        gb.alive = false;
    }
    gpuBuffers_.clear();
    for (ComputePipelineVk& cp : computePipes_) {
        for (u32 i = 0; i < framesInFlight_; ++i) {
            if (cp.cb[i]) vkDestroyBuffer(device_, cp.cb[i], nullptr);
            if (cp.cbMem[i]) vkFreeMemory(device_, cp.cbMem[i], nullptr);
        }
        if (cp.pool) vkDestroyDescriptorPool(device_, cp.pool, nullptr);
        if (cp.pipeline) vkDestroyPipeline(device_, cp.pipeline, nullptr);
        if (cp.layout) vkDestroyPipelineLayout(device_, cp.layout, nullptr);
        if (cp.setLayout) vkDestroyDescriptorSetLayout(device_, cp.setLayout, nullptr);
    }
    computePipes_.clear();
    // vsSet_ descriptors are owned by this pool; destroying it frees them all.
    if (vsBufferPool_) vkDestroyDescriptorPool(device_, vsBufferPool_, nullptr);
    if (vsBufferLayout_) vkDestroyDescriptorSetLayout(device_, vsBufferLayout_, nullptr);

    if (ssgiPipe_) vkDestroyPipeline(device_, ssgiPipe_, nullptr);
    if (painterlyPipe_) vkDestroyPipeline(device_, painterlyPipe_, nullptr);
    if (brushStrokesPipe_) vkDestroyPipeline(device_, brushStrokesPipe_, nullptr);
    if (applyPipe_) vkDestroyPipeline(device_, applyPipe_, nullptr);
    if (compositePipe_) vkDestroyPipeline(device_, compositePipe_, nullptr);
    if (hdrRenderPass_) vkDestroyRenderPass(device_, hdrRenderPass_, nullptr);
    if (hdrWaterRenderPass_) vkDestroyRenderPass(device_, hdrWaterRenderPass_, nullptr);
    if (previewRenderPass_) vkDestroyRenderPass(device_, previewRenderPass_, nullptr);
    if (postPass16_) vkDestroyRenderPass(device_, postPass16_, nullptr);
    if (postPass16Load_) vkDestroyRenderPass(device_, postPass16Load_, nullptr);
    if (postPass8_) vkDestroyRenderPass(device_, postPass8_, nullptr);

    if (skyPipeline_) vkDestroyPipeline(device_, skyPipeline_, nullptr);
    if (meshPipeline_) vkDestroyPipeline(device_, meshPipeline_, nullptr);
    if (meshPipelineTransparent_) vkDestroyPipeline(device_, meshPipelineTransparent_, nullptr);
    if (waterPipeline_) vkDestroyPipeline(device_, waterPipeline_, nullptr);
    if (meshPipelineTransparentDepth_)
        vkDestroyPipeline(device_, meshPipelineTransparentDepth_, nullptr);
    if (skyPipelineSingle_) vkDestroyPipeline(device_, skyPipelineSingle_, nullptr);
    if (meshPipelineSingle_) vkDestroyPipeline(device_, meshPipelineSingle_, nullptr);
    if (pipelineLayout_) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    if (uiPipeline_) vkDestroyPipeline(device_, uiPipeline_, nullptr);
    if (particlePipeline_) vkDestroyPipeline(device_, particlePipeline_, nullptr);
    if (particlePipelineAdd_) vkDestroyPipeline(device_, particlePipelineAdd_, nullptr);
    // No RAII on this side: the D3D12 twin's ComPtrs release themselves, these do not.
    if (particleGpuPipeline_) vkDestroyPipeline(device_, particleGpuPipeline_, nullptr);
    if (particleGpuPipelineAdd_) vkDestroyPipeline(device_, particleGpuPipelineAdd_, nullptr);
    if (strokeSurfacePipe_) vkDestroyPipeline(device_, strokeSurfacePipe_, nullptr);

    // World-UI targets (their images/views are owned here, not by textures_).
    for (auto& [slot, t] : uiTargets_) {
        if (t.fb) vkDestroyFramebuffer(device_, t.fb, nullptr);
        if (t.sampleView) vkDestroyImageView(device_, t.sampleView, nullptr);
        if (t.attachView) vkDestroyImageView(device_, t.attachView, nullptr);
        if (t.img) vkDestroyImage(device_, t.img, nullptr);
        if (t.mem) vkFreeMemory(device_, t.mem, nullptr);
    }
    uiTargets_.clear();
    if (uiWorldPipeline_) vkDestroyPipeline(device_, uiWorldPipeline_, nullptr);
    if (worldUIRenderPass_) vkDestroyRenderPass(device_, worldUIRenderPass_, nullptr);

    for (u32 i = 0; i < framesInFlight_; ++i) {
        if (frameUBO_[i]) vkDestroyBuffer(device_, frameUBO_[i], nullptr);
        if (frameUBOMem_[i]) vkFreeMemory(device_, frameUBOMem_[i], nullptr);
        if (uiVertexBuffers_[i]) vkDestroyBuffer(device_, uiVertexBuffers_[i], nullptr);
        if (uiVertexMemory_[i]) vkFreeMemory(device_, uiVertexMemory_[i], nullptr);
        if (uiWorldVertexBuffers_[i]) vkDestroyBuffer(device_, uiWorldVertexBuffers_[i], nullptr);
        if (uiWorldVertexMemory_[i]) vkFreeMemory(device_, uiWorldVertexMemory_[i], nullptr);
        if (particleVertexBuffers_[i]) vkDestroyBuffer(device_, particleVertexBuffers_[i], nullptr);
        if (particleVertexMemory_[i]) vkFreeMemory(device_, particleVertexMemory_[i], nullptr);
        if (shadowFrameUBO_[i]) vkDestroyBuffer(device_, shadowFrameUBO_[i], nullptr);
        if (shadowFrameUBOMem_[i]) vkFreeMemory(device_, shadowFrameUBOMem_[i], nullptr);
        if (objectArena_[i]) vkDestroyBuffer(device_, objectArena_[i], nullptr);
        if (objectArenaMem_[i]) vkFreeMemory(device_, objectArenaMem_[i], nullptr);
        if (postArena_[i]) vkDestroyBuffer(device_, postArena_[i], nullptr);
        if (postArenaMem_[i]) vkFreeMemory(device_, postArenaMem_[i], nullptr);
        if (previewFrameUBO_[i]) vkDestroyBuffer(device_, previewFrameUBO_[i], nullptr);
        if (previewFrameUBOMem_[i]) vkFreeMemory(device_, previewFrameUBOMem_[i], nullptr);
        if (boneArena_[i]) vkDestroyBuffer(device_, boneArena_[i], nullptr);
        if (instanceArena_[i]) vkDestroyBuffer(device_, instanceArena_[i], nullptr);
        if (instanceArenaMem_[i]) vkFreeMemory(device_, instanceArenaMem_[i], nullptr);
        if (boneArenaMem_[i]) vkFreeMemory(device_, boneArenaMem_[i], nullptr);
    }
    if (descriptorPool_) vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
    if (descriptorLayout_) vkDestroyDescriptorSetLayout(device_, descriptorLayout_, nullptr);
    if (clampSampler_) vkDestroySampler(device_, clampSampler_, nullptr);

    DestroySwapchainDependents();
    if (renderPass_) vkDestroyRenderPass(device_, renderPass_, nullptr);

    for (u32 i = 0; i < framesInFlight_; ++i) {
        if (imageAvailable_[i]) vkDestroySemaphore(device_, imageAvailable_[i], nullptr);
        if (inFlight_[i]) vkDestroyFence(device_, inFlight_[i], nullptr);
        if (gpuPool_[i]) vkDestroyQueryPool(device_, gpuPool_[i], nullptr);
    }
    if (commandPool_) vkDestroyCommandPool(device_, commandPool_, nullptr);

    vkDestroyDevice(device_, nullptr);
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (debugMessenger_ && pfnDestroyDebugMessenger_) {
        pfnDestroyDebugMessenger_(instance_, debugMessenger_, nullptr);
    }
    if (instance_) vkDestroyInstance(instance_, nullptr);
}

} // namespace

std::unique_ptr<IRenderDevice> CreateVulkanDevice(const RenderDeviceDesc& desc) {
    auto dev = std::make_unique<VulkanDevice>();
    if (!dev->Initialize(desc)) {
        return nullptr;
    }
    return dev;
}

} // namespace hbe::rhi
