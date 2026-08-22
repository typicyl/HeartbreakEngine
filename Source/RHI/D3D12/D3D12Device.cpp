// RHI/D3D12/D3D12Device.cpp - Direct3D 12 implementation of IRenderDevice.
//
// Implements a complete, fenced, triple-buffered present loop: adapter
// selection, swapchain, RTV heap, per-frame command allocators, and a clear.
#include "RHI/D3D12/D3D12Device.h"
#include "Core/Platform.h"
#include "Vfx/EffekseerBackend.h" // Effekseer VFX runtime (DX12 backend owns it)
#include "Assets/Mesh.h"
#include "Assets/StrokeGen.h"
#include "Core/Log.h"
#include "RHI/GpuMaterial.h" // GpuSurfaceMaterialExt (appended OpenPBR params)

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#if HBE_EDITOR
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx12.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace hbe::rhi {
namespace {

// Human-readable name for the DXGI device-removal reasons. An 8-digit hex code
// in a log tells you nothing; the name says whether you are chasing a shader
// fault (HUNG), a driver crash (DRIVER_INTERNAL_ERROR) or a TDR (RESET).
const char* DxgiReasonName(HRESULT hr) {
    switch (static_cast<u32>(hr)) {
        case 0x887A0005u: return "DXGI_ERROR_DEVICE_REMOVED";
        case 0x887A0006u: return "DXGI_ERROR_DEVICE_HUNG (GPU fault/timeout - bad draw or shader)";
        case 0x887A0007u: return "DXGI_ERROR_DEVICE_RESET (TDR: driver reset the GPU)";
        case 0x887A0020u: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
        case 0x887A0001u: return "DXGI_ERROR_INVALID_CALL";
        case 0x887A000Cu: return "DXGI_ERROR_ACCESS_LOST";
        case 0x8007000Eu: return "E_OUTOFMEMORY";
        default: return "unknown";
    }
}

// True when the HRESULT means the device is gone (any further GPU call is futile).
bool IsDeviceLost(HRESULT hr) {
    const u32 c = static_cast<u32>(hr);
    return c == 0x887A0005u || c == 0x887A0006u || c == 0x887A0007u || c == 0x887A000Cu;
}

// Logs and returns false when an HRESULT indicates failure.
//
// Device loss gets special handling: once the device is gone EVERY subsequent
// call fails the same way, and callers that retry per frame (the editor's
// viewport resize did exactly this) turn one real error into an unbounded log
// flood that buries the FIRST failure - the only one that identifies the cause.
// So the loss is latched and reported once, with the decoded removal reason.
#define HR_CHECK(expr, what)                                                    \
    do {                                                                        \
        const HRESULT _hr = (expr);                                            \
        if (FAILED(_hr)) {                                                      \
            if (IsDeviceLost(_hr)) {                                            \
                ReportDeviceLost(what, _hr);                                    \
            } else {                                                            \
                HBE_ERROR("[D3D12] {} failed (hr=0x{:08X} {})", what,          \
                          static_cast<u32>(_hr), DxgiReasonName(_hr));          \
            }                                                                   \
            return false;                                                       \
        }                                                                       \
    } while (0)

DXGI_FORMAT ToDXGIFormat(Format f) {
    switch (f) {
        case Format::R8G8B8A8_UNORM:      return DXGI_FORMAT_R8G8B8A8_UNORM;
        case Format::R8G8B8A8_SRGB:       return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case Format::B8G8R8A8_UNORM:      return DXGI_FORMAT_B8G8R8A8_UNORM;
        case Format::B8G8R8A8_SRGB:       return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        case Format::R16G16B16A16_FLOAT:  return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case Format::R32G32B32A32_FLOAT:  return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case Format::D32_FLOAT:           return DXGI_FORMAT_D32_FLOAT;
        case Format::D24_UNORM_S8_UINT:   return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case Format::BC1_UNORM:           return DXGI_FORMAT_BC1_UNORM;
        case Format::BC1_SRGB:            return DXGI_FORMAT_BC1_UNORM_SRGB;
        case Format::BC3_UNORM:           return DXGI_FORMAT_BC3_UNORM;
        case Format::BC3_SRGB:            return DXGI_FORMAT_BC3_UNORM_SRGB;
        case Format::BC4_UNORM:           return DXGI_FORMAT_BC4_UNORM;
        case Format::BC5_UNORM:           return DXGI_FORMAT_BC5_UNORM;
        case Format::BC6H_UFLOAT:         return DXGI_FORMAT_BC6H_UF16;
        case Format::BC7_UNORM:           return DXGI_FORMAT_BC7_UNORM;
        case Format::BC7_SRGB:            return DXGI_FORMAT_BC7_UNORM_SRGB;
        default:                          return DXGI_FORMAT_UNKNOWN;
    }
}

// Note: flip-model swapchains require a non-SRGB back buffer format; SRGB is
// applied through the RTV instead. We keep it simple and present in UNORM.
DXGI_FORMAT ToSwapchainFormat(Format f) {
    switch (f) {
        case Format::R8G8B8A8_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case Format::B8G8R8A8_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        default:                    return ToDXGIFormat(f);
    }
}

D3D12_RESOURCE_BARRIER TransitionBarrier(ID3D12Resource* res,
                                         D3D12_RESOURCE_STATES before,
                                         D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return b;
}

std::string WideToUtf8(const wchar_t* w) {
    if (!w) return {};
    int len = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(static_cast<usize>(len - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
    return s;
}

// --- GPU helpers -----------------------------------------------------------

constexpr u64 AlignUp(u64 value, u64 alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES p{};
    p.Type = type;
    p.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    p.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    p.CreationNodeMask = 1;
    p.VisibleNodeMask = 1;
    return p;
}

// `flags` defaults to NONE so every pre-existing call site is byte-identical;
// D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS is what a compute-writable
// structured buffer needs (CreateGpuBuffer).
D3D12_RESOURCE_DESC BufferDesc(u64 size,
                               D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    return d;
}

// Creates an UPLOAD-heap buffer (CPU-writable, GPU-readable) of `size` bytes.
ComPtr<ID3D12Resource> CreateUploadBuffer(ID3D12Device* device, u64 size) {
    const D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC rd = BufferDesc(size);
    ComPtr<ID3D12Resource> res;
    device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                    IID_PPV_ARGS(&res));
    return res;
}

// Creates a DEFAULT-heap (device-local / VRAM) buffer in the given state.
ComPtr<ID3D12Resource> CreateDefaultBuffer(ID3D12Device* device, u64 size,
                                           D3D12_RESOURCE_STATES state,
                                           D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    const D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_RESOURCE_DESC rd = BufferDesc(size, flags);
    ComPtr<ID3D12Resource> res;
    device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr,
                                    IID_PPV_ARGS(&res));
    return res;
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

// Directory containing the running executable, with a trailing backslash.
std::wstring ExecutableDir() {
    const std::filesystem::path dir = hbe::platform::ExecutableDir();
    // The trailing separator is part of this function's contract - callers concatenate a
    // shader filename straight onto it.
    return dir.empty() ? std::wstring() : (dir.wstring() + L"\\");
}

// Constant buffers mirror the cbuffer layouts in Shaders/Common.hlsli. - TODO figure out what's causing the spike
struct FrameCB {
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
    glm::mat4 prevViewProj; // previous frame's jittered view-proj (TAA)
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
    // Opaque scene-colour SRV for OpenPBR transmission/refraction (P6). 0 = not bound -> the shader
    // uses the IBL environment as the transmitted background; P6b sets this to a resolved copy for true
    // screen-space refraction. Appended 16-byte row keeps the pre-P6 FrameCB byte-compatible.
    u32 sceneColorIndex = 0; u32 _padSc0 = 0; u32 _padSc1 = 0; u32 _padSc2 = 0;
};

// Per-pass constants of the post stack (Shaders/PostCommon.hlsli).
struct PostCB {
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
    // Volume raymarch color (appended at END; must match PostCommon.hlsli). Default albedo (1,1,1)
    // reproduces today's grey smoke; emission tint inert until a temperature grid is fed (P2).
    glm::vec4 volAlbedo{1.0f, 1.0f, 1.0f, 0.0f};   // (albedo.rgb, emissionMode)
    glm::vec4 volEmission{1.0f, 0.45f, 0.15f, 0.0f}; // (emissionColor.rgb, hasTemp)
};

struct ObjectCB {
    glm::mat4 model;
    glm::mat4 normalMatrix;
    glm::vec4 baseColor;
    f32 metallic; f32 roughness; u32 albedoIndex; u32 normalIndex;
    u32 mrIndex; u32 aoIndex; u32 flags; f32 _pad0;
    glm::vec3 subsurfaceColor; f32 _pad1;
    glm::vec3 emissiveColor; f32 emissiveIntensity;
    u32 emissiveIndex; u32 skinned; u32 boneOffset; u32 boneCount;
    glm::mat4 prevModel;       // previous-frame world matrix (motion vectors)
    u32 prevBoneOffset; u32 thicknessIndex; f32 subsurfaceRadius; f32 lodDither; // was _padObj
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
    // Appended OpenPBR Surface params (P2). Mirror of OpenPBRMaterialExt in Common.hlsli.
    GpuSurfaceMaterialExt matExt;
};

// Copies a DrawItem's blendshape fields into the object CB (bindless atlas index +
// up to 8 active target rows/weights). morphTexIndex 0 = no morphs.
inline void FillMorphCB(ObjectCB& ocb, const DrawItem& it) {
    if (it.morphTexture.index == 0u) return; // no morphs (the common case) -> zero-init defaults
    ocb.morphTexIndex = it.morphTexture.index;
    ocb.morphCount = it.morphCount < 8u ? it.morphCount : 8u;
    for (u32 m = 0; m < 8u; ++m) {
        ocb.morphTargets[m >> 2][m & 3] = it.morphTargets[m];
        ocb.morphWeights[m >> 2][m & 3] = it.morphWeights[m];
    }
}

// The one place a DrawItem becomes object constants. See the declaration above for why
// this must not be duplicated.
inline void FillObjectMaterial(ObjectCB& ocb, const DrawItem& it) {
    ocb.model = it.transform;
    ocb.normalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(it.transform))));
    ocb.baseColor = it.surface.base_color;
    ocb.metallic = it.surface.base_metalness;
    ocb.roughness = it.surface.specular_roughness;
    ocb.albedoIndex = it.albedoTexture.index;
    ocb.normalIndex = it.normalTexture.index;
    ocb.mrIndex = it.mrTexture.index;
    FillMorphCB(ocb, it); // facial blendshapes (bindless delta atlas)
    ocb.aoIndex = it.aoTexture.index;
    ocb.flags = it.materialFlags;
    ocb.subsurfaceColor = it.surface.subsurface_color;
    ocb.subsurfaceRadius = it.surface.subsurface_radius;
    ocb.lodDither = it.lodDither; // LOD cross-fade stipple (1.0 = opaque; shadow pass ignores it)
    ocb.thicknessIndex = it.thicknessTexture.index;
    ocb.clearcoat = it.surface.coat_weight;
    ocb.clearcoatRoughness = it.surface.coat_roughness;
    ocb.emissiveColor = it.surface.emission_color;
    ocb.emissiveIntensity = it.surface.emission_luminance;
    FillSurfaceMaterialExt(ocb.matExt, it.surface);
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
    // Motion vectors: the previous-frame world matrix. Filled here rather than in the
    // scene pass so the shadow pass's constants are COMPLETE and reusable.
    ocb.prevModel = it.prevTransform;
}

u32 BytesPerPixel(Format f) {
    switch (f) {
        case Format::R8G8B8A8_UNORM:
        case Format::R8G8B8A8_SRGB:
        case Format::B8G8R8A8_UNORM:
        case Format::B8G8R8A8_SRGB:      return 4;
        case Format::R16G16B16A16_FLOAT: return 8;
        case Format::R32G32B32A32_FLOAT: return 16;
        // Block-compressed formats have no bytes-PER-PIXEL: 0 is a sentinel so any caller that
        // wrongly treats a BC texture as linear allocates 0 (an obvious failure) instead of a
        // plausible-but-wrong size. The staging walk uses rhi::MipCopyRows (block-aware) instead.
        case Format::BC1_UNORM: case Format::BC1_SRGB:
        case Format::BC3_UNORM: case Format::BC3_SRGB:
        case Format::BC4_UNORM: case Format::BC5_UNORM:
        case Format::BC6H_UFLOAT:
        case Format::BC7_UNORM: case Format::BC7_SRGB: return 0;
        default:                         return 4;
    }
}

// GPU-resident mesh (UPLOAD-heap vertex + index buffers).
struct GpuMesh {
    ComPtr<ID3D12Resource> vertexBuffer;
    ComPtr<ID3D12Resource> indexBuffer;
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    D3D12_INDEX_BUFFER_VIEW  ibv{};
    // ALLOCATED capacity, which is NOT the same as the view size. UpdateMesh
    // rewrites vbv/ibv.SizeInBytes to the new (possibly smaller) contents, so
    // using the view as the capacity check made a shrink permanent: a later
    // update back to the original size was rejected even though the underlying
    // resource was still large enough. Mirrors GpuMeshVk::vbSize/ibSize, which
    // already got this right.
    u64 vbCapacity = 0;
    u64 ibCapacity = 0;
    u32 indexCount = 0;
    // 3D painterly: per-instance brush-stroke seeds scattered over this mesh's
    // surface (StrokeSurface.hlsl expands each into a lit world-space card).
    ComPtr<ID3D12Resource> strokeBuffer;
    D3D12_VERTEX_BUFFER_VIEW strokeVbv{};
    u32 strokeCount = 0;
};

class D3D12Device final : public IRenderDevice {
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
    bool UpdateMesh(MeshHandle handle, const hbe::MeshData& mesh) override;
    TextureHandle CreateTexture(const TextureDesc& desc) override;
    TextureHandle CreateVolumeTexture(const TextureDesc& desc) override;
    void SetVolumeGrid(const void* density, usize densitySize, const void* temperature, usize tempSize,
                       const VolumeRenderParams& params) override;
    bool SupportsGpuCompute() const override { return true; }
    GpuBufferHandle CreateGpuBuffer(const GpuBufferDesc& desc) override;
    void* MapGpuBuffer(GpuBufferHandle handle) override;
    bool ReadGpuBuffer(GpuBufferHandle handle, void* dst, u32 bytes) override;
    void DestroyGpuBuffer(GpuBufferHandle handle) override;
    void DestroyMesh(MeshHandle handle) override;
    void DestroyTexture(TextureHandle handle) override;
    bool SupportsResourceReclaim() const override { return true; }
    bool SupportsBlockCompression() const override { return true; } // BC is core in D3D12
    ComputePipelineHandle CreateComputePipeline(const ComputePipelineDesc& desc) override;
    void DestroyComputePipeline(ComputePipelineHandle handle) override;
    void QueueCompute(const ComputeDispatch& d) override;
    void SetVertexShaderBuffer(GpuBufferHandle handle, u32 firstElement) override;
    void UpdateTexture(TextureHandle handle, const TextureDesc& desc) override;
    void DrawShadowPass(const SceneView& view, const DrawItem* items, u32 count) override;
    void SetParticles(const ParticleVertex* alpha, u32 alphaCount,
                      const ParticleVertex* additive, u32 addCount) override;
    void SetGpuParticles(GpuBufferHandle records, const GpuParticleBatch* batches,
                         u32 count) override;

    // Effekseer VFX seam (see the base RHI declarations + Vfx/EffekseerBackend).
    bool VfxAvailable() const override { return effekseer_.Available(); }
    u32 VfxLoadEffect(const char* absPath) override {
        return EnsureEffekseer() ? effekseer_.LoadEffect(absPath ? absPath : "") : 0;
    }
    int VfxPlay(u32 effectId, const glm::vec3& worldPos) override {
        return EnsureEffekseer() ? effekseer_.Play(effectId, worldPos) : -1;
    }
    void VfxStop(int handle) override { effekseer_.Stop(handle); }
    void VfxStopAll() override { effekseer_.StopAll(); }
    void VfxSetLocation(int handle, const glm::vec3& p) override { effekseer_.SetLocation(handle, p); }
    bool VfxExists(int handle) const override { return effekseer_.Exists(handle); }
    void VfxUpdate(f32 dt) override {
        if (effekseer_.Available()) effekseer_.Update(dt);
    }
    int VfxLiveInstanceCount() const override { return effekseer_.LiveInstanceCount(); }

    void SetGrass(GpuBufferHandle blades, u32 bladeCount) override;
    void SetGrassIndirect(GpuBufferHandle blades, GpuBufferHandle args, u32 maxBlades) override;
    // Reflects the ACTUAL indirect grass pipeline, not a blanket capability: if the
    // GrassIndirect draw PSO / command signature failed to build, the caller must stay on
    // the fixed path (its compaction compute would produce a buffer nothing can draw).
    bool SupportsIndirectDraw() const override {
        return grassIndirectPSO_ != nullptr && grassDrawSig_ != nullptr;
    }
    void DrawScene(const SceneView& view, const DrawItem* items, u32 count) override;
    void DrawUIOverlay(const UIVertex* vertices, u32 count) override;
    TextureHandle CreateUITarget(u32 width, u32 height) override;
    void DrawUIToTexture(TextureHandle target, const UIVertex* vertices, u32 count) override;

    // GPU profiler toggle: MUST live OUTSIDE #if HBE_EDITOR - the per-pass breakdown is a
    // RUNTIME diagnostic (the shipped game is what the player profiles), so the override has
    // to exist in the runtime build or the call dispatches to the no-op base default.
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
    u64 GetViewportTextureId() override { return viewportReady_ ? offscreenSrvGpu_.ptr : 0; }
    bool ReadbackViewportColor(std::vector<u8>& outRGBA, u32& w, u32& h) override;
    u64 GetTextureUIHandle(TextureHandle handle) override;

    void ResizePreview(u32 width, u32 height) override {
        if (width > 0 && height > 0) { pendingPrevW_ = width; pendingPrevH_ = height; }
    }
    u64 GetPreviewTextureId() override { return previewReady_ ? prevSrvGpu_.ptr : 0; }
    void DrawPreviewScene(const SceneView& view, const DrawItem* items, u32 count) override;
#endif

    GraphicsAPI GetAPI() const override { return GraphicsAPI::D3D12; }
    const char* GetAdapterName() const override { return adapterName_.c_str(); }

    ~D3D12Device() override;

private:
    bool CreateRenderTargetViews();
    void MoveToNextFrame();

    // Latches device loss and reports it ONCE with the decoded removal reason
    // from GetDeviceRemovedReason(). `what` is the call that first noticed.
    void ReportDeviceLost(const char* what, HRESULT hr);
    bool DeviceLost() const { return deviceLost_; }
    bool deviceLost_ = false;

    // Scene-rendering setup.
    bool CreateDepthResources(u32 width, u32 height);
    bool CreateMeshPipeline();

    // Set only for the duration of one CreateMeshReserved call; 0 = allocate exactly.
    u64 reserveVertices_ = 0;
    u64 reserveIndices_ = 0;
    bool CreateConstantArenas();
    bool CreateBindlessResources();
    bool CreateShadowResources();
    // Bump-allocates `size` bytes from the current frame's CB arena (256-aligned).
    // Returns a CPU pointer and sets `outGpuAddr`; nullptr if the arena is full.
    void* AllocConstants(u64 size, D3D12_GPU_VIRTUAL_ADDRESS& outGpuAddr);
    // Passes and draws dropped this frame because the constant arena ran out. Reported
    // once per frame rather than per item: an exhausted arena starves hundreds of draws in
    // a row, and a per-item log would bury the one line that matters.
    u32 constantStarvedPasses_ = 0;
    u32 constantStarvedLastReported_ = 0;

    static constexpr u32 kMaxBackBuffers = 4;

    ComPtr<IDXGIFactory6>         factory_;
    ComPtr<ID3D12Device>          device_;
    ComPtr<ID3D12CommandQueue>    queue_;
    ComPtr<IDXGISwapChain3>       swapchain_;
    ComPtr<ID3D12DescriptorHeap>  rtvHeap_;
    ComPtr<ID3D12Resource>        backBuffers_[kMaxBackBuffers];
    ComPtr<ID3D12CommandAllocator> allocators_[kMaxBackBuffers];
    ComPtr<ID3D12GraphicsCommandList> cmdList_;

    ComPtr<ID3D12Fence> fence_;
    HANDLE  fenceEvent_ = nullptr;
    u64     fenceValues_[kMaxBackBuffers] = {};

    u32 backBufferCount_ = 3;
    u32 rtvDescriptorSize_ = 0;
    u32 frameIndex_ = 0;
    u32 width_ = 0;
    u32 height_ = 0;
    DXGI_FORMAT swapFormat_ = DXGI_FORMAT_R8G8B8A8_UNORM;

    // -- Per-pass GPU profiler (timestamp queries; opt-in via SetGpuProfileEnabled) --
    // A mark records the GPU clock at a pass boundary; consecutive marks' delta = that
    // pass's GPU time. Resolved into a per-frame readback buffer, read back a few frames
    // later (when the slot's fence has signalled), logged every ~2s. Vulkan twin: kMaxGpuMarks.
    static constexpr u32 kMaxGpuMarks = 40;
    void GpuMark(const char* name);
    ComPtr<ID3D12QueryHeap> gpuQueryHeap_;
    ComPtr<ID3D12Resource>  gpuReadback_[kMaxBackBuffers]; // READBACK heap, kMaxGpuMarks u64
    const char* gpuNames_[kMaxBackBuffers][kMaxGpuMarks]{};
    u32  gpuCount_ = 0;                       // marks written into the current frame
    u32  gpuCountSlot_[kMaxBackBuffers]{};    // marks resolved in each slot
    bool gpuValidSlot_[kMaxBackBuffers]{};    // slot has resolved data to read
    u64  gpuFreq_ = 0;                        // timestamp ticks per second (0 = unsupported)
    u32  gpuFrameCounter_ = 0;
    bool gpuProfile_ = false;                 // ACTIVE this run (marks written + read back)
    bool gpuProfileAvail_ = false;            // heap + buffers created
    bool gpuProfileRequested_ = false;        // runtime toggle target
    HWND hwnd_ = nullptr;
    bool vsync_ = true;            // Present interval 1 vs 0 (uncapped)
    bool tearingSupported_ = false; // DXGI_FEATURE_PRESENT_ALLOW_TEARING
    u32 swapchainFlags_ = 0;        // creation flags; ResizeBuffers MUST reuse them

    D3D12_CPU_DESCRIPTOR_HANDLE currentRtv_{};
    std::string adapterName_ = "Unknown D3D12 Adapter";

    // -- Scene rendering ----------------------------------------------------
    ComPtr<ID3D12DescriptorHeap> dsvHeap_;
    ComPtr<ID3D12Resource>       depthBuffer_;
    DXGI_FORMAT depthFormat_ = DXGI_FORMAT_D32_FLOAT;

    ComPtr<ID3D12RootSignature> meshRootSig_;
    // Opaque HDR pass (MRT: color+gbuffer+velocity), ONE PSO per curated OpenPBR shader variant
    // (P3). Indexed by ShaderVariant; the vertex shader is shared, only the (lobe-stripped) pixel
    // shader differs. meshPSO_[Full] is also the effective default before the opaque loop selects.
    ComPtr<ID3D12PipelineState> meshPSO_[static_cast<size_t>(ShaderVariant::Count)];
    // TWO-SIDED FOLIAGE clones of each opaque variant (CULL_MODE_NONE): alpha-cutout leaf cards
    // must show both faces or the canopy goes patchy from behind. Selected in the opaque loop for
    // draws with MaterialFlag_AlphaTest. Same shaders/state otherwise.
    ComPtr<ID3D12PipelineState> meshPSOFoliage_[static_cast<size_t>(ShaderVariant::Count)];
    ComPtr<ID3D12PipelineState> meshPSOTransparent_; // alpha-blended pass (depth-write off)
    ComPtr<ID3D12PipelineState> meshPSOTransparentDepth_; // solid transparent: depth+velocity write
    ComPtr<ID3D12PipelineState> skyPSO_;        // background pass (shares meshRootSig_)
    ComPtr<ID3D12PipelineState> meshPSOSingle_; // single-RT variant (editor preview)
    ComPtr<ID3D12PipelineState> skyPSOSingle_;  // single-RT sky (editor preview)
    bool meshPipelineReady_ = false;

    // --- Volumetric VFX compute (VV2) ------------------------------------------
    // Compute pipeline that splats a 3D density/temperature volume. Lazily created
    // the first frame it's enabled (off by default -> zero cost on low-end GPUs).
    // Enable is currently HBE_VOLTEST env scaffolding to verify the compute path;
    // VV5 wires the real per-emitter enable. VV4 adds the raymarch that reads it.

    // --- General GPU compute + GPU-writable structured buffers ----------------
    // The generalisation of the volumetric path above: buffers a compute kernel
    // writes and the vertex shader reads. Vulkan twin: GpuBufferVk /
    // ComputePipelineVk / ExecuteQueuedCompute in VulkanDevice.cpp.
    struct GpuBufferD3D12 {
        // slots == 1 for a device-local buffer; kMaxBackBuffers for CpuWrite
        // (per-frame ring, so the CPU can refill without racing the GPU).
        ComPtr<ID3D12Resource> res[kMaxBackBuffers];
        u8*  cpu[kMaxBackBuffers] = {};
        D3D12_RESOURCE_STATES state[kMaxBackBuffers] = {};
        u32  slots = 1;
        u32  stride = 0;
        u32  count = 0;
        u32  usage = 0;
        u64  bytes = 0;
        // Root SRVs take any GPU virtual address, so D3D12 needs no padding and no
        // bounded view. It still records the window Vulkan is forced to bound the
        // descriptor by, and clamps the draw to it, so the two backends cannot
        // disagree about the tail of an oversized batch.
        u32  maxBindElements = 0;
        bool alive = false;
    };
    struct ComputePipelineD3D12 {
        ComPtr<ID3D12RootSignature> rootSig;
        ComPtr<ID3D12PipelineState> pso;
        u32 constantBytes = 0;
        u32 uavCount = 0;
        u32 srvCount = 0;
        bool alive = false;
    };
    // Dispatch queued this frame, with its constants COPIED (the caller's pointer
    // does not have to outlive QueueCompute).
    struct QueuedComputeD3D12 {
        ComputeDispatch d;
        u8 constants[kMaxComputeConstantBytes] = {};
    };
    std::vector<GpuBufferD3D12>      gpuBuffers_;      // handle.id - 1
    std::vector<u32>                 gpuBufferFree_;   // recycled indices
    std::vector<ComputePipelineD3D12> computePipes_;   // handle.id - 1
    std::vector<u32>                 computePipeFree_; // recycled indices (DestroyComputePipeline)
    QueuedComputeD3D12 computeQueue_[kMaxQueuedComputeDispatches];
    u32 computeQueueCount_ = 0;
    // SetVertexShaderBuffer: applied at the top of DrawScene (root param 6).
    GpuBufferHandle vsBuffer_{};
    u32 vsBufferFirstElement_ = 0;
    GpuBufferD3D12* ResolveGpuBuffer(GpuBufferHandle h);
    // Buffers live in COMMON and are promoted implicitly, but a UAV->read change
    // needs a real barrier (promotion only happens FROM common).
    void TransitionGpuBuffer(GpuBufferD3D12& b, u32 slot, D3D12_RESOURCE_STATES to);
    void ExecuteQueuedCompute();

    // -- 3D painterly surface strokes (instanced cards, PBR-lit) ---------------
    ComPtr<ID3D12PipelineState> strokeSurfacePSO_; // depth-test LE, no write, alpha-over
    bool strokeSurfaceReady_ = false;
    ComPtr<ID3D12PipelineState> waterPSO_; // Gerstner water surface (alpha-over, depth-LE)
    bool waterReady_ = false;

    // -- In-game UI overlay (alpha-blended textured 2D triangles) -------------
    ComPtr<ID3D12PipelineState> uiPSO_; // uses meshRootSig_ (bindless table)
    static constexpr u64 kUIVertexBufferSize = 2u << 20; // 2 MB/frame (~37449 verts @ 56 B)
    ComPtr<ID3D12Resource> uiVertexBuffers_[kMaxBackBuffers];
    u8* uiVertexCpu_[kMaxBackBuffers] = {};

    // -- World-space UI (canvas -> texture -> lit quad in the scene) ----------
    // Same UI pipeline against an R8G8B8A8 target instead of the swapchain. The
    // targets are TYPELESS resources: UNORM RTV (raw UI shader output) + SRGB SRV
    // (decoded like an albedo PNG when the mesh pass samples the page).
    ComPtr<ID3D12PipelineState> uiWorldPSO_;
    static constexpr u32 kMaxUITargets = 8;
    ComPtr<ID3D12DescriptorHeap> uiTargetRtvHeap_; // created lazily (kMaxUITargets)
    struct UITarget {
        ComPtr<ID3D12Resource> tex;
        u32 rtvIndex = 0; // slot in uiTargetRtvHeap_
        u32 w = 0, h = 0;
        bool inSrvState = true; // resource-state tracking (PSR <-> RTV)
    };
    std::unordered_map<u32, UITarget> uiTargets_; // key = bindless slot
    // Own per-frame vertex buffers with a bump head: the overlay's buffers are
    // memcpy'd at offset 0 per call, so sharing them would alias (the GPU reads
    // upload heaps at execute time). Multiple canvases draw per frame.
    ComPtr<ID3D12Resource> uiWorldVertexBuffers_[kMaxBackBuffers];
    u8* uiWorldVertexCpu_[kMaxBackBuffers] = {};
    u64 uiWorldVertexHead_ = 0; // reset in BeginFrame

    // -- Particle billboards (world-space, drawn in the HDR pass) -------------
    ComPtr<ID3D12PipelineState> particlePSO_;    // alpha blend
    ComPtr<ID3D12PipelineState> particlePSOAdd_; // additive blend
    static constexpr u64 kParticleVertexBufferSize = 6u << 20; // 6 MB/frame
    ComPtr<ID3D12Resource> particleVertexBuffers_[kMaxBackBuffers];
    u8* particleVertexCpu_[kMaxBackBuffers] = {};
    const ParticleVertex* particleAlpha_ = nullptr;
    const ParticleVertex* particleAdd_ = nullptr;
    u32 particleAlphaCount_ = 0;
    u32 particleAddCount_ = 0;

    // Effekseer VFX runtime (this backend owns it - it needs the native device/queue). Lazily
    // created on first use; drawn in DrawScene's transparent slot. See Vfx/EffekseerBackend.
    hbe::vfx::EffekseerBackend effekseer_;
    bool effekseerTried_ = false; // init attempted (success or failure) - don't retry every frame
    bool EnsureEffekseer() {
        if (effekseer_.Available()) return true;
        if (effekseerTried_) return false;
        effekseerTried_ = true;
        return effekseer_.Init(device_.Get(), queue_.Get(),
                               static_cast<u32>(DXGI_FORMAT_R16G16B16A16_FLOAT),
                               static_cast<u32>(DXGI_FORMAT_D32_FLOAT), /*hasDepth=*/true,
                               static_cast<int>(backBufferCount_));
    }
    // GPU vertex expansion: no vertex buffer, no input layout - the VS builds the
    // quad from SV_VertexID out of the record buffer (Shaders/ParticleGpu.hlsl).
    // Vulkan twin: particleGpuPipeline_ / particleGpuPipelineAdd_.
    ComPtr<ID3D12PipelineState> particleGpuPSO_;    // alpha blend
    ComPtr<ID3D12PipelineState> particleGpuPSOAdd_; // additive blend
    // One group per record BUFFER (SetGpuParticles accumulates): the CpuWrite ring
    // the CPU-simulated emitters upload into, and the device-local buffer the GPU
    // simulation's compute pass writes. A batch carries only an element offset, so
    // the buffer identity has to live here. Vulkan twin: the same array.
    struct GpuParticleGroup {
        GpuBufferHandle buffer;
        const GpuParticleBatch* batches = nullptr;
        u32 count = 0;
    };
    GpuParticleGroup particleGpuGroups_[kMaxGpuParticleGroups]{};
    u32 particleGpuGroupCount_ = 0;
    void DrawGpuParticleBatches(bool additive);
    // GPU-DRIVEN GRASS: a compute-written blade buffer drawn OPAQUE (depth-write) with its
    // own lit PSO inside DrawScene's opaque pass. Reuses the main root signature so blades
    // are lit + shadowed. Vulkan twin: grassPipeline_ / grassBlades_ / DrawGrass.
    ComPtr<ID3D12PipelineState> grassPSO_;
    GpuBufferHandle grassBlades_{};
    u32 grassBladeCount_ = 0;
    void DrawGrass();
    // GPU-DRIVEN GRASS, INDIRECT/COMPACTED variant (opt-in): a separate PSO whose VS keys
    // each blade off SV_InstanceID, plus a DRAW command signature for ExecuteIndirect. When
    // grassArgs_ is set for a frame, DrawGrass issues the indirect draw instead of the fixed
    // one. Missing shaders leave grassIndirectPSO_ null and the caller falls back.
    ComPtr<ID3D12PipelineState> grassIndirectPSO_;
    ComPtr<ID3D12CommandSignature> grassDrawSig_;
    GpuBufferHandle grassArgs_{};      // 16-byte {vtx,inst,0,0}; valid = indirect this frame
    u32 grassIndirectMax_ = 0;         // compacted-region capacity (SRV bound size)
    // The groups have a ONE-FRAME lifetime: `batches` points into a vector the engine
    // rebuilds every frame, so a group that survives a frame is a dangling pointer
    // with a stale count. DrawScene has early returns, so the clear cannot live only
    // at its end. Vulkan's twin is the same method at the same call sites.
    void ClearGpuParticleGroups() {
        for (u32 g = 0; g < particleGpuGroupCount_; ++g) particleGpuGroups_[g] = {};
        particleGpuGroupCount_ = 0;
    }

    // -- Cascaded shadow maps (depth-only pass into a 2x2 atlas) -------------
    static constexpr u32 kShadowDim = 4096;     // atlas; one cascade per 2048 tile
    static constexpr u32 kShadowTileDim = kShadowDim / 2;
    ComPtr<ID3D12Resource>       shadowMap_;
    ComPtr<ID3D12DescriptorHeap> shadowDsvHeap_;
    ComPtr<ID3D12PipelineState>  shadowPSO_;
    u32  shadowSrvSlot_ = 0;     // bindless slot the PBR pass samples
    bool shadowReady_ = false;
    bool shadowInSrvState_ = false; // resource state tracking across frames
    bool shadowPassRun_ = false;    // a shadow map was rendered this frame

    // -- HDR pipeline + post-process stack ------------------------------------
    // Scene renders into an RGBA16F target with a sampleable depth, then:
    // SSAO (+blur) -> bloom down/up pyramid -> tonemap -> FXAA -> final target
    // (editor viewport texture or the swapchain). Post passes are fullscreen
    // triangles reading inputs through the bindless table.
    static constexpr u32 kBloomMaxMips = 6;
    bool CreatePostPipelines();
    bool CreatePostTargets(u32 width, u32 height);
    void RunPostStack(const SceneView& view);
    // One fullscreen pass: transitions `target` SRV->RT, draws, RT->SRV.
    void DrawPostPass(ID3D12PipelineState* pso, ID3D12Resource* target,
                      D3D12_CPU_DESCRIPTOR_HANDLE rtv, u32 w, u32 h, const PostCB& cb);

    bool postPipelinesReady_ = false; // PSOs + shaders present
    bool postReady_ = false;          // targets sized and usable this frame
    u32  sceneW_ = 0, sceneH_ = 0;    // size the post targets were built for
    DXGI_FORMAT sceneFormat_ = DXGI_FORMAT_R16G16B16A16_FLOAT;

    ComPtr<ID3D12Resource> hdrColor_, hdrDepth_;
    // Resolved opaque scene colour, copied from hdrColor_ after opaque+sky and before the transparent
    // pass, so transmissive materials can sample the geometry BEHIND them (screen-space refraction,
    // P6b). Copy-dest + SRV only (no RTV). Only produced when a transmissive transparent item exists.
    ComPtr<ID3D12Resource> sceneColorCopy_;
    ComPtr<ID3D12Resource> gbuffer_;            // RGBA16F: octN.rg, rough.b, metal.a
    ComPtr<ID3D12Resource> velocity_;           // RG16F: screen motion vectors
    ComPtr<ID3D12Resource> ssaoRaw_, ssaoBlur_; // half-res
    ComPtr<ID3D12Resource> bloom_[kBloomMaxMips];
    ComPtr<ID3D12Resource> ldr_;                // tonemapped, pre-FXAA
    ComPtr<ID3D12Resource> taaHistory_[2];      // TAA accumulation (ping-pong)
    ComPtr<ID3D12Resource> dof_;                // depth-of-field result (LDR)
    ComPtr<ID3D12Resource> motionBlur_;         // motion blur result (LDR)
    ComPtr<ID3D12Resource> ssr_;                // screen-space reflections (HDR)
    ComPtr<ID3D12Resource> ssgi_;               // screen-space GI composite (HDR, full-res)
    ComPtr<ID3D12Resource> ssgiHalf_;           // SSGI GI-only term at reduced res (upscaled into ssgi_)
    ComPtr<ID3D12Resource> volScatter_;         // volumetric fog composite (HDR, full-res)
    ComPtr<ID3D12Resource> volHalf_;            // fog (inscatter+transmittance) at reduced res
    ComPtr<ID3D12Resource> volPartScatter_;     // volumetric-particles composite (HDR, full-res)
    ComPtr<ID3D12Resource> volPartHalf_;        // volumetric-particles raymarch at half res
    ComPtr<ID3D12Resource> painterly_;          // painterly stroke pass (HDR)
    ComPtr<ID3D12Resource> painterlyComp_;      // painterly + dynamic-layer crisp composite (HDR)
    ComPtr<ID3D12Resource> adaptedLum_[2];      // auto-exposure adapted luminance (1x1, ping-pong)
    ComPtr<ID3D12DescriptorHeap> postRtvHeap_;  // hdr, ssaoRaw, ssaoBlur, ldr, bloom[N], taa[2], dof, mblur, ssr, lum[2]
    ComPtr<ID3D12DescriptorHeap> postDsvHeap_;  // hdr depth
    u32 postRtvSize_ = 0;
    u32 slotHdr_ = 0, slotDepth_ = 0, slotSsaoRaw_ = 0, slotSsaoBlur_ = 0, slotLdr_ = 0;
    u32 slotSceneColor_ = 0; // bindless SRV of sceneColorCopy_ (transmission/refraction, P6b)
    u32 slotGbuffer_ = 0, slotVelocity_ = 0;
    bool gbufInSrv_ = false; // G-buffer/velocity resource-state tracking
    u32 slotTaaHistory_[2] = {};
    u32 slotDof_ = 0;
    u32 slotMotionBlur_ = 0;
    u32 slotSsr_ = 0;
    u32 slotSsgi_ = 0;
    u32 slotSsgiHalf_ = 0;
    u32 slotVol_ = 0;
    u32 slotVolHalf_ = 0;
    u32 slotVolPart_ = 0;
    u32 slotVolPartHalf_ = 0;
    u32 slotPainterly_ = 0;
    u32 slotPainterlyComp_ = 0;
    u32 slotAdaptedLum_[2] = {};
    u32 slotBloom_[kBloomMaxMips] = {};
    u32 bloomW_[kBloomMaxMips] = {}, bloomH_[kBloomMaxMips] = {};
    u32 bloomCount_ = 0;
    u32 ssaoW_ = 0, ssaoH_ = 0;
    bool hdrDepthInSrv_ = false;
    // Read-only DSV for hdrDepth_ (postDsvHeap_ slot 1): lets the water pass depth-TEST while
    // hdrDepth_ is simultaneously PIXEL_SHADER_RESOURCE for the depth-graded water PS to sample.
    D3D12_CPU_DESCRIPTOR_HANDLE roDsvHandle_{};
    bool roDsvValid_ = false;

    ComPtr<ID3D12PipelineState> ssaoPSO_, ssaoBlurPSO_, bloomDownPSO_, bloomUpPSO_,
                                tonemapPSO_, fxaaPSO_, taaPSO_, dofPSO_, motionBlurPSO_,
                                ssrPSO_, exposurePSO_, volPSO_, ssgiPSO_, painterlyPSO_,
                                brushStrokesPSO_, compositePSO_, applyPSO_;

    // Temporal AA: jittered camera each frame + reprojected history accumulation.
    bool taaReady_ = false;          // TAA PSO built (optional; absent = no TAA)
    u32  taaHistoryIndex_ = 0;       // history target written this frame (ping-pong)
    bool taaHistoryValid_ = false;   // a prior frame's history exists (reset on resize)
    u64  taaFrame_ = 0;              // jitter sequence index
    glm::mat4 taaPrevViewProj_{1.0f}; // previous frame's jittered view-projection
    bool dofReady_ = false;          // DoF PSO built (optional; absent = no DoF)
    bool motionBlurReady_ = false;   // motion-blur PSO built
    bool ssrReady_ = false;          // SSR PSO built
    bool exposureReady_ = false;     // auto-exposure PSO built
    bool volReady_ = false;          // volumetric fog PSO built
    // NanoVDB baked-volume raymarch (the runtime volume path). The raw NanoVDB blob is uploaded
    // to a per-frame StructuredBuffer<uint> (volGridBuffer_ + its bindless SRV slotVolGrid_);
    // volRaymarchPSO_ (VolumeRaymarch.hlsl) PNanoVDB-samples it. Grows on demand.
    ComPtr<ID3D12PipelineState> volRaymarchPSO_;
    bool volRaymarchReady_ = false;
    ComPtr<ID3D12Resource> volGridBuffer_[kMaxBackBuffers];
    u8* volGridCpu_[kMaxBackBuffers] = {};
    u64 volGridCapacity_[kMaxBackBuffers] = {};
    u32 slotVolGrid_[kMaxBackBuffers] = {};
    const void* volGridBytes_ = nullptr;
    usize volGridSize_ = 0;
    // Optional SECOND grid (temperature -> emission glow); mirrors the density grid exactly, bound at
    // t1 space6. Fed together with density via SetVolumeGrid so both are the same frame/bounds.
    ComPtr<ID3D12Resource> volTempBuffer_[kMaxBackBuffers];
    u8* volTempCpu_[kMaxBackBuffers] = {};
    u64 volTempCapacity_[kMaxBackBuffers] = {};
    u32 slotVolTemp_[kMaxBackBuffers] = {};
    const void* volTempBytes_ = nullptr;
    usize volTempSize_ = 0;
    VolumeRenderParams volRenderParams_;
    bool EnsureVolumeGridBuffer(usize size); // (re)alloc this frame's density grid buffer + SRV
    bool EnsureVolumeTempBuffer(usize size); // (re)alloc this frame's temperature grid buffer + SRV
    bool ssgiReady_ = false;         // screen-space GI PSO built
    bool painterlyReady_ = false;    // painterly stroke PSO built
    bool brushStrokesReady_ = false; // brush-stroke splat PSO built
    u32  adaptIndex_ = 0;            // adapted-luminance target written this frame
    bool adaptValid_ = false;        // a prior adapted value exists (reset on resize)

    static constexpr u64 kConstantArenaSize = 1u << 22; // 4 MB / frame (~16k draws)
    ComPtr<ID3D12Resource> constantArenas_[kMaxBackBuffers];
    u8*  constantCpu_[kMaxBackBuffers] = {};
    u64  constantHead_ = 0;
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> shadowObjAddrs_; // scratch, per frame
    std::vector<u32> shadowInstanceCounts_; // scratch: instances per run head (1 = single)

    // -- Skinning: per-frame joint-palette arena (StructuredBuffer t0,space1) -
    static constexpr u64 kBoneArenaSize = 1u << 22; // 4 MB (~500 skinned draws)
    ComPtr<ID3D12Resource> boneArenas_[kMaxBackBuffers];
    u8*  boneCpu_[kMaxBackBuffers] = {};
    u64  boneHead_ = 0;
    std::vector<u32> shadowBoneOffsets_; // scratch: per-item palette offsets
    // Copies a palette into this frame's arena; returns the element offset
    // (in matrices) for ObjectCB::boneOffset, or 0 with `ok=false` when full.
    u32 AllocBones(const glm::mat4* mats, u32 count, bool& ok) {
        const u64 bytes = static_cast<u64>(count) * sizeof(glm::mat4);
        if (boneHead_ + bytes > kBoneArenaSize) {
            ok = false;
            return 0;
        }
        std::memcpy(boneCpu_[frameIndex_] + boneHead_, mats, bytes);
        const u32 offset = static_cast<u32>(boneHead_ / sizeof(glm::mat4));
        boneHead_ += bytes;
        ok = true;
        return offset;
    }

    // -- GPU instancing: per-frame instance-transform arena (t1, space1) -------
    // 3 matrices per instance (model, normalMatrix, prevModel); each pass appends
    // its runs independently (shadow first, then scene - no cross-pass coupling).
    static constexpr u64 kInstanceArenaSize = 1u << 22; // 4 MB (~21k instances)
    ComPtr<ID3D12Resource> instanceArenas_[kMaxBackBuffers];
    u8*  instanceCpu_[kMaxBackBuffers] = {};
    u64  instanceHead_ = 0;
    // Reserves `count` instances; returns the base INSTANCE index for
    // ObjectCB::instanceBase and a write pointer, or ok=false when full.
    glm::mat4* AllocInstances(u32 count, u32& outBase, bool& ok) {
        const u64 bytes = static_cast<u64>(count) * 3u * sizeof(glm::mat4);
        if (instanceHead_ + bytes > kInstanceArenaSize) {
            ok = false;
            outBase = 0;
            return nullptr;
        }
        outBase = static_cast<u32>(instanceHead_ / (3u * sizeof(glm::mat4)));
        glm::mat4* dst = reinterpret_cast<glm::mat4*>(instanceCpu_[frameIndex_] + instanceHead_);
        instanceHead_ += bytes;
        ok = true;
        return dst;
    }

    std::vector<GpuMesh> meshes_;

    // -- Bindless textures ---------------------------------------------------
    static constexpr u32 kMaxBindlessTextures = 4096;
    ComPtr<ID3D12DescriptorHeap> bindlessHeap_; // shader-visible CBV_SRV_UAV
    u32 bindlessDescSize_ = 0;
    u32 bindlessNextSlot_ = 0;
    std::vector<ComPtr<ID3D12Resource>> textures_; // keep resources alive
    // Per-bindless-slot info so editor thumbnails can re-view a texture.
    struct SlotTexture {
        ID3D12Resource* resource = nullptr;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        u32 mipCount = 1;
    };
    std::unordered_map<u32, SlotTexture> slotTextures_;
    // Volume textures (CreateVolumeTexture): SRV slot -> paired UAV bindless slot,
    // so the compute splat can bind the volume for writing (VV2). 0 = no UAV.
    std::unordered_map<u32, u32> volumeUav_;

    // --- VRAM RECLAIM (deferred, non-blocking) ------------------------------
    // Streaming despawn calls DestroyMesh/DestroyTexture. The GPU may still reference the
    // resource in in-flight frames, so it is NOT freed immediately (that would need a
    // WaitForGpuIdle, a per-despawn stall): it is parked here with the frame it becomes
    // safe to release (frameCounter_ + a full swapchain cycle of margin), then its bindless
    // slot / meshes_ index is recycled for a later Create. RetireFrees() runs each
    // BeginFrame. Only ever touched on the main thread (like every Create/Destroy here).
    u64 frameCounter_ = 0;
    struct PendingTexFree { ComPtr<ID3D12Resource> res; u32 slot; u64 freeFrame; };
    std::vector<PendingTexFree> pendingTexFree_;
    std::vector<u32> texSlotFree_; // reclaimed bindless slots, reused by CreateTexture
    struct PendingMeshFree { GpuMesh mesh; u32 meshId; u64 freeFrame; };
    std::vector<PendingMeshFree> pendingMeshFree_;
    std::vector<u32> meshFree_; // reclaimed meshes_ indices (0-based), reused by CreateMesh
    void RetireFrees();

    // Synchronous texture-upload command objects (Update* / buffer paths still block).
    ComPtr<ID3D12CommandAllocator>    uploadAlloc_;
    ComPtr<ID3D12GraphicsCommandList> uploadList_;
    ComPtr<ID3D12Fence> uploadFence_;
    HANDLE uploadEvent_ = nullptr;
    u64    uploadFenceValue_ = 0;

    // ASYNCHRONOUS (non-blocking) uploads for CreateMesh / CreateTexture - the two
    // uploads on the streaming finalize path. The old code blocked on the GPU after
    // EVERY resource, so a shard streaming in serialized dozens of CPU->GPU->CPU
    // round-trips into one ~10 ms frame stall. Now each upload submits on the graphics
    // queue and returns immediately; its staging buffers + command allocator are held
    // until the GPU passes the upload fence, then retired once per frame. Each batch
    // owns its OWN allocator so the still-synchronous sites above can keep resetting
    // the shared uploadAlloc_ without racing an in-flight async copy. Uploads happen on
    // the main thread only (the streamer stages on workers but finalizes here), so no
    // locking is needed. Reuses uploadFence_/uploadFenceValue_ (one graphics queue ->
    // fence values signal monotonically in submission order, so both paths share it).
    struct UploadBatch {
        ComPtr<ID3D12CommandAllocator>    alloc;
        ComPtr<ID3D12GraphicsCommandList> list;
        std::vector<ComPtr<ID3D12Resource>> staging; // alive until `fence` passes
        u64 fence = 0;
    };
    std::vector<UploadBatch> asyncUploadInFlight_;
    std::vector<UploadBatch> asyncUploadFree_;
    UploadBatch BeginAsyncUpload();           // an open command list, ready to record
    void        EndAsyncUpload(UploadBatch&& b); // Close + Execute + Signal, no wait
    void        RetireAsyncUploads();         // free staging for completed uploads

    // -- ImGui ---------------------------------------------------------------
    ComPtr<ID3D12DescriptorHeap> imguiSrvHeap_;
    u32 imguiSrvDescSize_ = 0;
    std::vector<u32> imguiSrvFreeList_; // free descriptor indices in imguiSrvHeap_
    std::unordered_map<u32, u64> uiTextureIds_; // bindless slot -> ImGui id
    bool uiInitialized_ = false;

#if HBE_EDITOR
    static void ImGuiSrvAlloc(ImGui_ImplDX12_InitInfo* info,
                              D3D12_CPU_DESCRIPTOR_HANDLE* outCpu,
                              D3D12_GPU_DESCRIPTOR_HANDLE* outGpu);
    static void ImGuiSrvFree(ImGui_ImplDX12_InitInfo* info,
                             D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                             D3D12_GPU_DESCRIPTOR_HANDLE gpu);
    bool CreateViewportTarget(u32 width, u32 height);
#endif

    // -- Editor viewport (offscreen scene target) ---------------------------

    ComPtr<ID3D12Resource>       offscreenColor_;
    ComPtr<ID3D12Resource>       offscreenDepth_;
    ComPtr<ID3D12DescriptorHeap> offscreenRtvHeap_;
    ComPtr<ID3D12DescriptorHeap> offscreenDsvHeap_;
    u32 vpW_ = 0, vpH_ = 0;
    u32 pendingVpW_ = 0, pendingVpH_ = 0;
    u32 offscreenSrvSlot_ = 0;
    bool offscreenSrvAllocated_ = false;
    D3D12_GPU_DESCRIPTOR_HANDLE offscreenSrvGpu_{};
    bool viewportReady_ = false;
    ComPtr<ID3D12Resource> readbackBuffer_; // CPU-readable copy of offscreenColor_ (movie render)
    u64 readbackSize_ = 0;
    u64 readbackRowPitch_ = 0; // pitch the buffer was sized for (differs between resolutions)

    // -- Editor asset preview (independent mini-scene: HDR pass + tonemap) ---
#if HBE_EDITOR
    bool CreatePreviewTargets(u32 width, u32 height);
#endif
    ComPtr<ID3D12Resource>       prevHdr_, prevDepth_, prevLdr_;
    ComPtr<ID3D12DescriptorHeap> prevRtvHeap_; // 0 = hdr, 1 = ldr
    ComPtr<ID3D12DescriptorHeap> prevDsvHeap_;
    u32 prevW_ = 0, prevH_ = 0;
    u32 pendingPrevW_ = 0, pendingPrevH_ = 0;
    u32 slotPrevHdr_ = 0;        // bindless input of the preview tonemap
    u32 prevSrvSlot_ = 0;        // ImGui heap slot for the LDR result
    bool prevSrvAllocated_ = false;
    D3D12_GPU_DESCRIPTOR_HANDLE prevSrvGpu_{};
    bool previewReady_ = false;
};

bool D3D12Device::Initialize(const RenderDeviceDesc& desc) {
    hwnd_ = static_cast<HWND>(desc.windowHandle);
    width_ = desc.width;
    height_ = desc.height;
    vsync_ = desc.vsync;
    backBufferCount_ = (desc.backBufferCount < 2) ? 2
                      : (desc.backBufferCount > kMaxBackBuffers ? kMaxBackBuffers
                                                                : desc.backBufferCount);
    swapFormat_ = ToSwapchainFormat(desc.backBufferFormat);

    u32 factoryFlags = 0;
    if (desc.enableValidation) {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
            HBE_INFO("[D3D12] Debug layer enabled");
            // GPU-based validation (HBE_GBV=1) instruments shaders to catch
            // out-of-bounds resource reads - e.g. a skinning fetch past the
            // bone palette - that the plain debug layer misses and that show
            // up only as a device hang.
            if (const char* gbv = std::getenv("HBE_GBV"); gbv && gbv[0] == '1') {
                ComPtr<ID3D12Debug1> debug1;
                if (SUCCEEDED(debug.As(&debug1))) {
                    debug1->SetEnableGPUBasedValidation(TRUE);
                    HBE_INFO("[D3D12] GPU-based validation enabled");
                }
            }
        }
    }

    HR_CHECK(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory_)), "CreateDXGIFactory2");

    // Pick the highest-performance adapter that supports D3D12 (FL 11_0).
    ComPtr<IDXGIAdapter1> adapter;
    for (u32 i = 0;
         factory_->EnumAdapterByGpuPreference(
             i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
         ++i) {
        DXGI_ADAPTER_DESC1 ad{};
        adapter->GetDesc1(&ad);
        if (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(&device_)))) {
            // The engine's shaders are Shader Model 6.5 and it binds textures through a
            // large bindless table (resource binding tier 2+). A GPU that makes an
            // FL 11_0 device but lacks these would CREATE fine here, then CRASH later at
            // PSO creation / the bindless heap - which on a bare machine looks like a
            // silent quit right after boot. Verify up front and skip the adapter if it
            // can't meet them, so the boot fallback moves on with a clear reason.
            D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_5};
            D3D12_FEATURE_DATA_D3D12_OPTIONS opt{};
            const bool smOk =
                SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) &&
                sm.HighestShaderModel >= D3D_SHADER_MODEL_6_5;
            const bool tierOk =
                SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opt, sizeof(opt))) &&
                opt.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_2;
            if (smOk && tierOk) {
                adapterName_ = WideToUtf8(ad.Description);
                break;
            }
            HBE_WARN("[D3D12] Adapter '{}' lacks required features (Shader Model 6.5: {}, "
                     "resource binding tier >= 2: {}); skipping.",
                     WideToUtf8(ad.Description), smOk, tierOk);
            device_.Reset();
        }
    }
    if (!device_) {
        HBE_ERROR("[D3D12] No Direct3D 12 adapter meets the engine's requirements "
                  "(Shader Model 6.5 + bindless resource binding tier 2). The GPU/driver "
                  "may be too old - update the graphics driver, or use --vulkan / --opengl.");
        return false;
    }
    HBE_INFO("[D3D12] Adapter: {}", adapterName_);

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    HR_CHECK(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_)), "CreateCommandQueue");

    // Tearing (uncapped present) support: required for Present(0) with
    // ALLOW_TEARING on flip-model swapchains. Borderless-only engine -> legal.
    {
        ComPtr<IDXGIFactory5> f5;
        BOOL allow = FALSE;
        if (SUCCEEDED(factory_.As(&f5)) &&
            SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow,
                                              sizeof(allow)))) {
            tearingSupported_ = allow == TRUE;
        }
        if (!vsync_ && !tearingSupported_)
            HBE_WARN("[D3D12] vsync off requested but tearing unsupported; presents "
                     "will still wait for vblank.");
    }

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = width_;
    scd.Height = height_;
    scd.Format = swapFormat_;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = backBufferCount_;
    scd.Scaling = DXGI_SCALING_STRETCH;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    // NOTE: swapchainFlags_ must ALSO be passed to every ResizeBuffers call, or
    // resize fails with E_INVALIDARG.
    swapchainFlags_ = tearingSupported_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    scd.Flags = swapchainFlags_;

    ComPtr<IDXGISwapChain1> sc1;
    HR_CHECK(factory_->CreateSwapChainForHwnd(queue_.Get(), hwnd_, &scd, nullptr, nullptr, &sc1),
             "CreateSwapChainForHwnd");
    HR_CHECK(sc1.As(&swapchain_), "SwapChain QueryInterface");
    factory_->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    frameIndex_ = swapchain_->GetCurrentBackBufferIndex();

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = backBufferCount_;
    rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HR_CHECK(device_->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap_)), "CreateDescriptorHeap");
    rtvDescriptorSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    if (!CreateRenderTargetViews()) return false;

    for (u32 i = 0; i < backBufferCount_; ++i) {
        HR_CHECK(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 IID_PPV_ARGS(&allocators_[i])),
                 "CreateCommandAllocator");
    }
    HR_CHECK(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        allocators_[frameIndex_].Get(), nullptr,
                                        IID_PPV_ARGS(&cmdList_)),
             "CreateCommandList");
    cmdList_->Close();

    HR_CHECK(device_->CreateFence(fenceValues_[frameIndex_], D3D12_FENCE_FLAG_NONE,
                                  IID_PPV_ARGS(&fence_)),
             "CreateFence");
    fenceValues_[frameIndex_]++;
    fenceEvent_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent_) {
        HBE_ERROR("[D3D12] CreateEvent failed");
        return false;
    }

    // Per-pass GPU profiler: a timestamp query heap + one READBACK buffer per frame slot.
    // Created whenever the queue can timestamp; ACTIVATION is a runtime toggle
    // (SetGpuProfileEnabled) so the marks/readback only cost their ~1-3 ms/frame when a
    // --gpuprofile / dev-menu diagnosis run asks for them. Mirrors the Vulkan profiler.
    if (SUCCEEDED(queue_->GetTimestampFrequency(&gpuFreq_)) && gpuFreq_ > 0) {
        D3D12_QUERY_HEAP_DESC qhd{};
        qhd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qhd.Count = kMaxGpuMarks;
        if (SUCCEEDED(device_->CreateQueryHeap(&qhd, IID_PPV_ARGS(&gpuQueryHeap_)))) {
            const D3D12_HEAP_PROPERTIES rb = HeapProps(D3D12_HEAP_TYPE_READBACK);
            const D3D12_RESOURCE_DESC rd = BufferDesc(kMaxGpuMarks * sizeof(u64));
            gpuProfileAvail_ = true;
            for (u32 i = 0; i < backBufferCount_; ++i) {
                if (FAILED(device_->CreateCommittedResource(
                        &rb, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                        nullptr, IID_PPV_ARGS(&gpuReadback_[i])))) {
                    gpuProfileAvail_ = false;
                    break;
                }
            }
            gpuProfile_ = gpuProfileAvail_ && gpuProfileRequested_; // honor a pre-init request
        }
    }

    // Scene-rendering resources. Failure here is non-fatal: the device falls
    // back to a clear-only loop (SupportsSceneRendering() stays false).
    if (!CreateDepthResources(width_, height_)) {
        HBE_WARN("[D3D12] Depth buffer creation failed; scene rendering disabled.");
    } else if (!CreateConstantArenas()) {
        HBE_WARN("[D3D12] Constant arena creation failed; scene rendering disabled.");
    } else if (!CreateBindlessResources()) {
        HBE_WARN("[D3D12] Bindless table creation failed; scene rendering disabled.");
    } else if (!CreateMeshPipeline()) {
        HBE_WARN("[D3D12] Mesh pipeline unavailable; scene rendering disabled.");
    } else {
        meshPipelineReady_ = true;
        if (!CreateShadowResources()) {
            HBE_WARN("[D3D12] Shadow resources unavailable; shadows disabled.");
        }
    }

    HBE_INFO("[D3D12] Device initialized ({} back buffers, {}x{}, scene={})",
             backBufferCount_, width_, height_, meshPipelineReady_ ? "on" : "off");
    return true;
}

bool D3D12Device::CreateRenderTargetViews() {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    for (u32 i = 0; i < backBufferCount_; ++i) {
        HR_CHECK(swapchain_->GetBuffer(i, IID_PPV_ARGS(&backBuffers_[i])), "GetBuffer");
        device_->CreateRenderTargetView(backBuffers_[i].Get(), nullptr, handle);
        handle.ptr += rtvDescriptorSize_;
    }
    return true;
}

// NOTE: deliberately OUTSIDE any #if HBE_EDITOR block - HR_CHECK expands in
// runtime-build code paths too, so the runtime link needs this definition.
void D3D12Device::ReportDeviceLost(const char* what, HRESULT hr) {
    if (deviceLost_) return; // already reported; stay silent (see HR_CHECK)
    deviceLost_ = true;
    // GetDeviceRemovedReason is the ONLY call that says why. The HRESULT the
    // failing call returned is almost always the generic DEVICE_REMOVED.
    const HRESULT reason = device_ ? device_->GetDeviceRemovedReason() : hr;
    HBE_ERROR("[D3D12] DEVICE LOST during '{}' (hr=0x{:08X}).", what, static_cast<u32>(hr));
    HBE_ERROR("[D3D12]   reason=0x{:08X} {}", static_cast<u32>(reason), DxgiReasonName(reason));
    HBE_ERROR("[D3D12]   Rendering is dead from here; further GPU errors are suppressed.");
    HBE_ERROR("[D3D12]   Diagnose with --validation (and set HBE_GBV=1 for GPU-based "
              "validation, which catches shader out-of-bounds).");
}

void D3D12Device::BeginFrame() {
    // Reclaim staging buffers from async uploads whose GPU copy has completed. Also
    // done inside BeginAsyncUpload, so a burst self-limits; this releases the last
    // batch's memory even on an idle frame with no new uploads.
    RetireAsyncUploads();
    // Advance the frame clock + release meshes/textures whose deferred-destroy window
    // has elapsed (VRAM reclaim), recycling their slots/indices.
    RetireFrees();
#if HBE_EDITOR
    // Apply a pending viewport (offscreen target) resize.
    //
    // The result MUST be honoured. This used to discard it, which meant a failed
    // re-create left viewportReady_ true with offscreenColor_ null - the frame
    // then ran TransitionBarrier() on a null resource - AND left vpW_/vpH_ stale,
    // so the mismatch that triggers the re-create never cleared and it retried
    // every single frame, flooding the log with the same error forever.
    if (viewportReady_ && pendingVpW_ > 0 && pendingVpH_ > 0 &&
        (pendingVpW_ != vpW_ || pendingVpH_ != vpH_)) {
        WaitForGpuIdle();
        if (!CreateViewportTarget(pendingVpW_, pendingVpH_)) {
            // Fall back to rendering straight to the swapchain rather than into a
            // target that no longer exists, and stop retrying this size.
            viewportReady_ = false;
            vpW_ = pendingVpW_;
            vpH_ = pendingVpH_;
            HBE_WARN("[D3D12] Offscreen viewport target unavailable at {}x{}; "
                     "rendering to the window instead.",
                     pendingVpW_, pendingVpH_);
        }
    }
#endif

#if HBE_EDITOR
    // Apply a pending asset-preview resize.
    if (pendingPrevW_ > 0 && pendingPrevH_ > 0 &&
        (pendingPrevW_ != prevW_ || pendingPrevH_ != prevH_)) {
        WaitForGpuIdle();
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
            WaitForGpuIdle();
            postReady_ = CreatePostTargets(w, h);
            if (!postReady_) {
                HBE_ERROR("[D3D12] Post target creation failed at {}x{}.", w, h);
            }
        }
    }

    allocators_[frameIndex_]->Reset();
    cmdList_->Reset(allocators_[frameIndex_].Get(), nullptr);
    // SAY IT ONCE, WITH A NUMBER. Silent starvation is how a frame quietly loses its post
    // stack or a shadow cascade and nobody can explain the picture.
    if (constantStarvedPasses_ > 0 && constantStarvedPasses_ != constantStarvedLastReported_) {
        HBE_WARN("[D3D12] The per-frame constant arena ran out: {} pass(es)/draw(s) were "
                 "skipped last frame rather than rendered with the wrong constants.",
                 constantStarvedPasses_);
        constantStarvedLastReported_ = constantStarvedPasses_;
    }
    constantStarvedPasses_ = 0;
    constantHead_ = 0;
    boneHead_ = 0;
    instanceHead_ = 0;
    if (gpuProfile_) { gpuCount_ = 0; GpuMark("start"); } // GPU profiler: frame origin
    uiWorldVertexHead_ = 0; // world-UI canvases bump-allocate here across the frame

    // Queued compute (GPU particle sim etc.). Runs here, at frame start, before
    // any render target is bound - the SAME point VulkanDevice::BeginFrame runs
    // its queue, where Vulkan's "no compute inside a render pass" rule forces it.
    // Must follow the constantHead_ reset above: dispatch constants come out of
    // this frame's arena.
    ExecuteQueuedCompute();

    if (!viewportReady_) {
        // Legacy direct-to-swapchain path (no editor viewport).
        auto toRT = TransitionBarrier(backBuffers_[frameIndex_].Get(),
                                      D3D12_RESOURCE_STATE_PRESENT,
                                      D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmdList_->ResourceBarrier(1, &toRT);
        currentRtv_ = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        currentRtv_.ptr += static_cast<SIZE_T>(frameIndex_) * rtvDescriptorSize_;
        if (dsvHeap_) {
            const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
            cmdList_->OMSetRenderTargets(1, &currentRtv_, FALSE, &dsv);
            cmdList_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        } else {
            cmdList_->OMSetRenderTargets(1, &currentRtv_, FALSE, nullptr);
        }
        D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<f32>(width_), static_cast<f32>(height_), 0.0f, 1.0f};
        D3D12_RECT scissor{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
        cmdList_->RSSetViewports(1, &vp);
        cmdList_->RSSetScissorRects(1, &scissor);
    }
}

void D3D12Device::ClearBackBuffer(f32 r, f32 g, f32 b, f32 a) {
    GpuMark("shadow"); // GPU profiler: delta start->here = the cascaded shadow pass
    const f32 color[4] = {r, g, b, a};
    if (postReady_) {
        // Begin the HDR scene pass; the post stack resolves to the final
        // target (viewport texture or swapchain) at the end of DrawScene. The
        // forward pass writes 3 targets: HDR colour + G-buffer + velocity.
        D3D12_RESOURCE_BARRIER barriers[4];
        u32 barrierCount = 0;
        barriers[barrierCount++] =
            TransitionBarrier(hdrColor_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                              D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (gbufInSrv_) {
            barriers[barrierCount++] =
                TransitionBarrier(gbuffer_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                  D3D12_RESOURCE_STATE_RENDER_TARGET);
            barriers[barrierCount++] =
                TransitionBarrier(velocity_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                  D3D12_RESOURCE_STATE_RENDER_TARGET);
            gbufInSrv_ = false;
        }
        if (hdrDepthInSrv_) {
            barriers[barrierCount++] =
                TransitionBarrier(hdrDepth_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                  D3D12_RESOURCE_STATE_DEPTH_WRITE);
            hdrDepthInSrv_ = false;
        }
        cmdList_->ResourceBarrier(barrierCount, barriers);

        const auto postRtv = [&](u32 index) {
            D3D12_CPU_DESCRIPTOR_HANDLE h = postRtvHeap_->GetCPUDescriptorHandleForHeapStart();
            h.ptr += static_cast<SIZE_T>(index) * postRtvSize_;
            return h;
        };
        const D3D12_CPU_DESCRIPTOR_HANDLE rtvs[3] = {
            postRtv(0), postRtv(11 + kBloomMaxMips), postRtv(12 + kBloomMaxMips)};
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = postDsvHeap_->GetCPUDescriptorHandleForHeapStart();
        cmdList_->OMSetRenderTargets(3, rtvs, FALSE, &dsv);
        const f32 zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        cmdList_->ClearRenderTargetView(rtvs[0], color, 0, nullptr);
        cmdList_->ClearRenderTargetView(rtvs[1], zero, 0, nullptr);
        cmdList_->ClearRenderTargetView(rtvs[2], zero, 0, nullptr);
        cmdList_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<f32>(sceneW_), static_cast<f32>(sceneH_),
                          0.0f, 1.0f};
        D3D12_RECT scissor{0, 0, static_cast<LONG>(sceneW_), static_cast<LONG>(sceneH_)};
        cmdList_->RSSetViewports(1, &vp);
        cmdList_->RSSetScissorRects(1, &scissor);
        return;
    }
    if (viewportReady_) {
        // Begin the offscreen scene pass.
        auto toRT = TransitionBarrier(offscreenColor_.Get(),
                                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                      D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmdList_->ResourceBarrier(1, &toRT);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = offscreenRtvHeap_->GetCPUDescriptorHandleForHeapStart();
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = offscreenDsvHeap_->GetCPUDescriptorHandleForHeapStart();
        cmdList_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        cmdList_->ClearRenderTargetView(rtv, color, 0, nullptr);
        cmdList_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<f32>(vpW_), static_cast<f32>(vpH_), 0.0f, 1.0f};
        D3D12_RECT scissor{0, 0, static_cast<LONG>(vpW_), static_cast<LONG>(vpH_)};
        cmdList_->RSSetViewports(1, &vp);
        cmdList_->RSSetScissorRects(1, &scissor);
    } else {
        cmdList_->ClearRenderTargetView(currentRtv_, color, 0, nullptr);
    }
}

void D3D12Device::EndFrame() {
    GpuMark("ui"); // GPU profiler: delta scene->here = the UI/ImGui overlay pass
    if (!viewportReady_) {
        auto toPresent = TransitionBarrier(backBuffers_[frameIndex_].Get(),
                                           D3D12_RESOURCE_STATE_RENDER_TARGET,
                                           D3D12_RESOURCE_STATE_PRESENT);
        cmdList_->ResourceBarrier(1, &toPresent);
    }
    // GPU profiler: resolve this frame's timestamps into its readback buffer (before the
    // command list closes). Read back a few frames later in MoveToNextFrame (fence-safe).
    if (gpuProfile_ && gpuCount_ >= 2) {
        cmdList_->ResolveQueryData(gpuQueryHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0,
                                   gpuCount_, gpuReadback_[frameIndex_].Get(), 0);
        gpuCountSlot_[frameIndex_] = gpuCount_;
        gpuValidSlot_[frameIndex_] = true;
    }
    // In viewport mode RenderUI already left the swapchain in PRESENT.
    cmdList_->Close();
    ID3D12CommandList* lists[] = {cmdList_.Get()};
    queue_->ExecuteCommandLists(1, lists);

    // Multi-viewport: render + present any ImGui panels the user dragged out into
    // their own OS windows (each gets its own swapchain on this queue). Draw data
    // for all viewports was finalized by ImGui::Render() in RenderUI; the main
    // viewport was recorded above, so this handles only the secondary windows.
    // (ImGui is editor-only, so this whole block is compiled out of the runtime.)
#if HBE_EDITOR
    if (uiInitialized_ && (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)) {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
#endif

    // Vsync on = interval 1; off = interval 0 + ALLOW_TEARING (when supported) so
    // presents never wait for vblank and real frame rates are measurable.
    const HRESULT pr = swapchain_->Present(
        vsync_ ? 1 : 0, (!vsync_ && tearingSupported_) ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (FAILED(pr)) {
        // A device removal/hang (e.g. a TDR after a bad draw) otherwise looks
        // like a freeze while the CPU loop spins at thousands of "FPS": shout
        // about it once so the cause is visible in the log.
        // Shares the one latch with HR_CHECK, so whichever call notices FIRST is
        // the one that gets reported - and it is reported exactly once.
        ReportDeviceLost("Present", pr);
    }
    MoveToNextFrame();
}

void D3D12Device::MoveToNextFrame() {
    const u64 currentValue = fenceValues_[frameIndex_];
    queue_->Signal(fence_.Get(), currentValue);

    frameIndex_ = swapchain_->GetCurrentBackBufferIndex();

    if (fence_->GetCompletedValue() < fenceValues_[frameIndex_]) {
        fence_->SetEventOnCompletion(fenceValues_[frameIndex_], fenceEvent_);
        ::WaitForSingleObjectEx(fenceEvent_, INFINITE, FALSE);
    }
    fenceValues_[frameIndex_] = currentValue + 1;

    // GPU profiler: this slot's fence has now signalled, so the timestamps it resolved
    // (framesInFlight ago) are readable. Map, compute per-pass deltas, log ~every 2s.
    if (gpuProfile_ && gpuValidSlot_[frameIndex_] && gpuCountSlot_[frameIndex_] >= 2 &&
        gpuFreq_ > 0 && gpuReadback_[frameIndex_]) {
        const u32 n = gpuCountSlot_[frameIndex_];
        const D3D12_RANGE rr{0, n * sizeof(u64)};
        void* mapped = nullptr;
        if (SUCCEEDED(gpuReadback_[frameIndex_]->Map(0, &rr, &mapped)) && mapped) {
            u64 t[kMaxGpuMarks];
            std::memcpy(t, mapped, n * sizeof(u64));
            const D3D12_RANGE wr{0, 0};
            gpuReadback_[frameIndex_]->Unmap(0, &wr);
            if (++gpuFrameCounter_ >= 180) { // ~2s at 90 FPS
                gpuFrameCounter_ = 0;
                const f64 toMs = 1000.0 / static_cast<f64>(gpuFreq_);
                char buf[512];
                int off = std::snprintf(buf, sizeof(buf), "[D3D12 GPU] total %.2f ms |",
                                        static_cast<f64>(t[n - 1] - t[0]) * toMs);
                for (u32 i = 1; i < n && off > 0 && off < static_cast<int>(sizeof(buf)) - 24;
                     ++i) {
                    const f64 ms = static_cast<f64>(t[i] - t[i - 1]) * toMs;
                    off += std::snprintf(buf + off, sizeof(buf) - off, " %s %.2f",
                                         gpuNames_[frameIndex_][i] ? gpuNames_[frameIndex_][i]
                                                                   : "?",
                                         ms);
                }
                HBE_INFO("{}", buf);
            }
        }
    }
}

void D3D12Device::WaitForGpuIdle() {
    if (!queue_ || !fence_) return;
    const u64 wait = fenceValues_[frameIndex_];
    queue_->Signal(fence_.Get(), wait);
    fence_->SetEventOnCompletion(wait, fenceEvent_);
    ::WaitForSingleObjectEx(fenceEvent_, INFINITE, FALSE);
    fenceValues_[frameIndex_] = wait + 1;
}

// Records the GPU clock at this point in the command stream (one timestamp query).
// The delta to the previous mark = the GPU time of the pass that just finished. No-op
// unless the profiler is active and there's room; names are kept per frame slot.
void D3D12Device::GpuMark(const char* name) {
    if (!gpuProfile_ || gpuCount_ >= kMaxGpuMarks || !cmdList_) return;
    gpuNames_[frameIndex_][gpuCount_] = name;
    cmdList_->EndQuery(gpuQueryHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, gpuCount_);
    ++gpuCount_;
}

void D3D12Device::Resize(u32 width, u32 height) {
    if (width == 0 || height == 0) return;
    if (width == width_ && height == height_) return;

    WaitForGpuIdle();

    for (u32 i = 0; i < backBufferCount_; ++i) {
        backBuffers_[i].Reset();
    }

    // Must pass the swapchain's CREATION flags (ALLOW_TEARING) or this fails
    // with E_INVALIDARG.
    HRESULT hr = swapchain_->ResizeBuffers(backBufferCount_, width, height, swapFormat_,
                                           swapchainFlags_);
    if (FAILED(hr)) {
        HBE_ERROR("[D3D12] ResizeBuffers failed (hr=0x{:08X})", static_cast<u32>(hr));
        return;
    }

    width_ = width;
    height_ = height;
    frameIndex_ = swapchain_->GetCurrentBackBufferIndex();

    // After a full idle, align all per-frame fence targets to the current one.
    for (u32 i = 0; i < backBufferCount_; ++i) {
        fenceValues_[i] = fenceValues_[frameIndex_];
    }
    CreateRenderTargetViews();
    if (dsvHeap_) {
        depthBuffer_.Reset();
        CreateDepthResources(width_, height_);
    }
    HBE_INFO("[D3D12] Resized to {}x{}", width_, height_);
}

bool D3D12Device::CreateDepthResources(u32 width, u32 height) {
    if (!dsvHeap_) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hd.NumDescriptors = 1;
        HR_CHECK(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvHeap_)),
                 "CreateDescriptorHeap(DSV)");
    }

    const D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = width;
    rd.Height = height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = depthFormat_;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = depthFormat_;
    clear.DepthStencil.Depth = 1.0f;

    HR_CHECK(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                              D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                                              IID_PPV_ARGS(&depthBuffer_)),
             "CreateCommittedResource(Depth)");

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = depthFormat_;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device_->CreateDepthStencilView(depthBuffer_.Get(), &dsv,
                                    dsvHeap_->GetCPUDescriptorHandleForHeapStart());
    return true;
}

bool D3D12Device::CreateConstantArenas() {
    for (u32 i = 0; i < backBufferCount_; ++i) {
        constantArenas_[i] = CreateUploadBuffer(device_.Get(), kConstantArenaSize);
        if (!constantArenas_[i]) {
            HBE_ERROR("[D3D12] Failed to create constant arena {}", i);
            return false;
        }
        D3D12_RANGE noRead{0, 0};
        void* mapped = nullptr;
        HR_CHECK(constantArenas_[i]->Map(0, &noRead, &mapped), "Map(ConstantArena)");
        constantCpu_[i] = static_cast<u8*>(mapped);

        boneArenas_[i] = CreateUploadBuffer(device_.Get(), kBoneArenaSize);
        if (!boneArenas_[i]) {
            HBE_ERROR("[D3D12] Failed to create bone arena {}", i);
            return false;
        }
        void* boneMapped = nullptr;
        HR_CHECK(boneArenas_[i]->Map(0, &noRead, &boneMapped), "Map(BoneArena)");
        boneCpu_[i] = static_cast<u8*>(boneMapped);

        instanceArenas_[i] = CreateUploadBuffer(device_.Get(), kInstanceArenaSize);
        if (!instanceArenas_[i]) {
            HBE_ERROR("[D3D12] Failed to create instance arena {}", i);
            return false;
        }
        void* instMapped = nullptr;
        HR_CHECK(instanceArenas_[i]->Map(0, &noRead, &instMapped), "Map(InstanceArena)");
        instanceCpu_[i] = static_cast<u8*>(instMapped);
    }
    return true;
}

bool D3D12Device::CreateBindlessResources() {
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kMaxBindlessTextures;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HR_CHECK(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&bindlessHeap_)),
             "CreateDescriptorHeap(bindless)");
    bindlessDescSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    HR_CHECK(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_PPV_ARGS(&uploadAlloc_)),
             "CreateCommandAllocator(upload)");
    HR_CHECK(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        uploadAlloc_.Get(), nullptr, IID_PPV_ARGS(&uploadList_)),
             "CreateCommandList(upload)");
    uploadList_->Close();
    HR_CHECK(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&uploadFence_)),
             "CreateFence(upload)");
    uploadEvent_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!uploadEvent_) {
        HBE_ERROR("[D3D12] upload event creation failed");
        return false;
    }

    // Slot 0 = a 1x1 white texture, so a default (index 0) material samples white.
    const u32 white = 0xFFFFFFFFu;
    TextureDesc desc;
    desc.width = 1;
    desc.height = 1;
    desc.format = Format::R8G8B8A8_UNORM;
    desc.pixels = &white;
    CreateTexture(desc); // consumes slot 0
    return true;
}

TextureHandle D3D12Device::CreateTexture(const TextureDesc& desc) {
    if (!bindlessHeap_ || !desc.pixels) return {};
    // Reuse a bindless slot reclaimed by DestroyTexture before growing the high-water mark,
    // so a long streaming session recycles slots instead of exhausting the 4096 cap.
    u32 slot;
    if (!texSlotFree_.empty()) {
        slot = texSlotFree_.back();
        texSlotFree_.pop_back();
    } else if (bindlessNextSlot_ < kMaxBindlessTextures) {
        slot = bindlessNextSlot_++;
    } else {
        return {};
    }
    const DXGI_FORMAT fmt = ToDXGIFormat(desc.format);
    const u32 mipCount = desc.mipCount < 1 ? 1 : desc.mipCount;

    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = desc.width;
    td.Height = desc.height;
    td.DepthOrArraySize = 1;
    td.MipLevels = static_cast<UINT16>(mipCount);
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    const D3D12_HEAP_PROPERTIES def = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> tex;
    if (const HRESULT hr = device_->CreateCommittedResource(
            &def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&tex)); FAILED(hr)) {
        HBE_ERROR("[D3D12] CreateTexture: CreateCommittedResource failed (fmt={}, {}x{} mip{}, hr=0x{:08x})",
                  static_cast<int>(desc.format), desc.width, desc.height, mipCount,
                  static_cast<u32>(hr));
        texSlotFree_.push_back(slot); // return the slot for reuse
        return {};
    }

    // Per-mip upload layout (handles the 256-byte row alignment automatically).
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(mipCount);
    std::vector<UINT> numRows(mipCount);
    std::vector<UINT64> rowSizes(mipCount);
    UINT64 totalBytes = 0;
    device_->GetCopyableFootprints(&td, 0, mipCount, 0, footprints.data(),
                                   numRows.data(), rowSizes.data(), &totalBytes);

    ComPtr<ID3D12Resource> staging = CreateUploadBuffer(device_.Get(), totalBytes);
    if (!staging) {
        HBE_ERROR("[D3D12] CreateTexture: staging alloc failed (fmt={}, {} bytes)",
                  static_cast<int>(desc.format), static_cast<u64>(totalBytes));
        texSlotFree_.push_back(slot);
        return {};
    }

    u8* mapped = nullptr;
    D3D12_RANGE noRead{0, 0};
    staging->Map(0, &noRead, reinterpret_cast<void**>(&mapped));
    const u8* src = static_cast<const u8*>(desc.pixels);
    for (u32 mip = 0; mip < mipCount; ++mip) {
        // GetCopyableFootprints reports the TIGHT source geometry for THIS format, block-aware:
        // rowSizes[mip] is the unpadded row size (blocksX*blockBytes for BC, w*bpp otherwise) and
        // numRows[mip] is the row (block-row) count. Using them means one code path serves both
        // linear and BC textures - no bytes-per-pixel assumption, no sub-4x4 tail-mip special case.
        const u64 srcRowBytes = rowSizes[mip];
        const u32 rows = numRows[mip];
        u8* dstBase = mapped + footprints[mip].Offset;
        for (u32 row = 0; row < rows; ++row) {
            std::memcpy(dstBase + static_cast<u64>(row) * footprints[mip].Footprint.RowPitch,
                        src + static_cast<u64>(row) * srcRowBytes,
                        static_cast<usize>(srcRowBytes));
        }
        src += srcRowBytes * rows; // next mip (tightly packed source), block-aware
    }
    staging->Unmap(0, nullptr);

    // Async upload: record the per-mip copies + the SRV transition, submit on the
    // graphics queue, and return WITHOUT blocking (the staging buffer rides the batch
    // until the fence passes). No per-texture round-trip stall while streaming.
    UploadBatch up = BeginAsyncUpload();
    for (u32 mip = 0; mip < mipCount; ++mip) {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = tex.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = mip;
        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = staging.Get();
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint = footprints[mip];
        up.list->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
    }
    auto toSRV = TransitionBarrier(tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    up.list->ResourceBarrier(1, &toSRV);
    up.staging.push_back(std::move(staging));
    EndAsyncUpload(std::move(up));

    D3D12_CPU_DESCRIPTOR_HANDLE h = bindlessHeap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot) * bindlessDescSize_;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = fmt;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = mipCount;
    device_->CreateShaderResourceView(tex.Get(), &sv, h);

    slotTextures_[slot] = SlotTexture{tex.Get(), fmt, mipCount};
    textures_.push_back(tex);
    return TextureHandle{slot};
}

TextureHandle D3D12Device::CreateVolumeTexture(const TextureDesc& desc) {
    // A compute-writable, sampled texture with NO initial upload. depth>1 => 3D.
    // Consumes 1 bindless slot for the SRV and (when storage) 1 more for the UAV.
    if (!bindlessHeap_ || bindlessNextSlot_ + 2 > kMaxBindlessTextures) return {};
    const DXGI_FORMAT fmt = ToDXGIFormat(desc.format);
    const u32 depth = (std::max)(1u, desc.depth);

    D3D12_RESOURCE_DESC td{};
    td.Dimension = (depth > 1) ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                               : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = desc.width;
    td.Height = desc.height;
    td.DepthOrArraySize = static_cast<UINT16>(depth);
    td.MipLevels = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (desc.storage) td.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    // Initial state matches first use: compute-written volumes start in UAV; a
    // plain sampled volume starts ready to read. The splat pass (VV2/VV3) manages
    // the per-frame UAV<->SRV transitions.
    const D3D12_RESOURCE_STATES init = desc.storage
                                           ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                           : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const D3D12_HEAP_PROPERTIES def = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> tex;
    if (FAILED(device_->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, init, nullptr,
                                                IID_PPV_ARGS(&tex)))) {
        return {};
    }

    const u32 srvSlot = bindlessNextSlot_++;
    D3D12_CPU_DESCRIPTOR_HANDLE hs = bindlessHeap_->GetCPUDescriptorHandleForHeapStart();
    hs.ptr += static_cast<SIZE_T>(srvSlot) * bindlessDescSize_;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = fmt;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (depth > 1) { sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D; sv.Texture3D.MipLevels = 1; }
    else { sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1; }
    device_->CreateShaderResourceView(tex.Get(), &sv, hs);

    u32 uavSlot = 0;
    if (desc.storage) {
        uavSlot = bindlessNextSlot_++;
        D3D12_CPU_DESCRIPTOR_HANDLE hu = bindlessHeap_->GetCPUDescriptorHandleForHeapStart();
        hu.ptr += static_cast<SIZE_T>(uavSlot) * bindlessDescSize_;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
        uv.Format = fmt;
        if (depth > 1) { uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D; uv.Texture3D.WSize = depth; }
        else { uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; }
        device_->CreateUnorderedAccessView(tex.Get(), nullptr, &uv, hu);
    }

    slotTextures_[srvSlot] = SlotTexture{tex.Get(), fmt, 1};
    textures_.push_back(tex);
    volumeUav_[srvSlot] = uavSlot;
    return TextureHandle{srvSlot};
}

void D3D12Device::UpdateTexture(TextureHandle handle, const TextureDesc& desc) {
    if (!handle.IsValid() || !desc.pixels) return;
    const auto it = slotTextures_.find(handle.index);
    if (it == slotTextures_.end() || !it->second.resource) return;
    ID3D12Resource* tex = it->second.resource;
    const u32 mipCount = desc.mipCount < 1 ? 1 : desc.mipCount;

    // The in-place path requires the new data to match the created texture's
    // layout (paint canvases keep a fixed resolution/format); bail otherwise.
    D3D12_RESOURCE_DESC td = tex->GetDesc();
    if (td.Width != desc.width || td.Height != desc.height ||
        td.MipLevels != mipCount || td.Format != ToDXGIFormat(desc.format))
        return;

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(mipCount);
    std::vector<UINT> numRows(mipCount);
    std::vector<UINT64> rowSizes(mipCount);
    UINT64 totalBytes = 0;
    device_->GetCopyableFootprints(&td, 0, mipCount, 0, footprints.data(),
                                   numRows.data(), rowSizes.data(), &totalBytes);

    ComPtr<ID3D12Resource> staging = CreateUploadBuffer(device_.Get(), totalBytes);
    if (!staging) return;
    u8* mapped = nullptr;
    D3D12_RANGE noRead{0, 0};
    staging->Map(0, &noRead, reinterpret_cast<void**>(&mapped));
    const u8* src = static_cast<const u8*>(desc.pixels);
    for (u32 mip = 0; mip < mipCount; ++mip) {
        // Block-aware tight source geometry from the footprint (see CreateTexture).
        const u64 srcRowBytes = rowSizes[mip];
        const u32 rows = numRows[mip];
        u8* dstBase = mapped + footprints[mip].Offset;
        for (u32 row = 0; row < rows; ++row) {
            std::memcpy(dstBase + static_cast<u64>(row) * footprints[mip].Footprint.RowPitch,
                        src + static_cast<u64>(row) * srcRowBytes,
                        static_cast<usize>(srcRowBytes));
        }
        src += srcRowBytes * rows;
    }
    staging->Unmap(0, nullptr);

    // Async in-place update: record on a pooled command list and submit WITHOUT blocking;
    // the staging buffer rides the batch until the fence passes (same as CreateTexture).
    // This removes the per-texture GPU round-trip that made a despawn/respawn of a painted
    // shard (re-adopted texture handles -> UpdateTexture) serialize dozens of stalls.
    UploadBatch up = BeginAsyncUpload();
    auto toCopy = TransitionBarrier(tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                    D3D12_RESOURCE_STATE_COPY_DEST);
    up.list->ResourceBarrier(1, &toCopy);
    for (u32 mip = 0; mip < mipCount; ++mip) {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = tex;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = mip;
        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = staging.Get();
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint = footprints[mip];
        up.list->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
    }
    auto toSRV = TransitionBarrier(tex, D3D12_RESOURCE_STATE_COPY_DEST,
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    up.list->ResourceBarrier(1, &toSRV);
    up.staging.push_back(std::move(staging));
    EndAsyncUpload(std::move(up));
}

bool D3D12Device::CreateMeshPipeline() {
    const std::wstring dir = ExecutableDir() + L"shaders\\";
    const std::vector<u8> vs = ReadBinaryFile(dir + L"MeshPBR.vs.dxil");
    const std::vector<u8> ps = ReadBinaryFile(dir + L"MeshPBR.ps.dxil");
    if (vs.empty() || ps.empty()) {
        HBE_WARN("[D3D12] MeshPBR DXIL not found next to the executable.");
        return false;
    }

    // Root signature: two root CBVs (b0 = frame, b1 = object) plus an unbounded
    // SRV table (t0, space0) for the bindless texture array. Root-sig 1.0
    // descriptor tables are implicitly volatile, which is what bindless needs.
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = UINT_MAX; // unbounded
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = 0;

    // SRV table (t0, space6) for the volumetric 3D volume (Texture3D can't live in
    // the Texture2D[] bindless array). Only the volumetric raymarch pass sets it;
    // every other pass leaves it unbound (their shaders don't reference it).
    D3D12_DESCRIPTOR_RANGE volRange{};
    volRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    volRange.NumDescriptors = 1;
    volRange.BaseShaderRegister = 0;
    volRange.RegisterSpace = 6;
    volRange.OffsetInDescriptorsFromTableStart = 0;
    // Second SRV table (t1 space6) for the OPTIONAL temperature grid (emission glow). A SEPARATE root
    // param, NOT a 2-wide table off volRange: slotVolGrid_[] are allocated consecutively across FRAMES,
    // so a 2-wide table based at the density slot would read the NEXT frame's density as temperature.
    D3D12_DESCRIPTOR_RANGE volRange2 = volRange;
    volRange2.BaseShaderRegister = 1; // t1 space6

    D3D12_ROOT_PARAMETER params[8] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &srvRange;
    // ALL (not just PIXEL): the BrushStrokes pass samples the bindless HDR in its
    // VERTEX shader (per-stroke colour + Sobel flow). PIXEL-only made the VS reads
    // return nothing -> strokes never rendered on D3D12 (they worked on Vulkan once its
    // descriptor stage flags were widened to VERTEX; this is the same fix, root-sig side).
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // Joint palettes (StructuredBuffer<float4x4> gBones, t0 space1).
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[3].Descriptor.ShaderRegister = 0;
    params[3].Descriptor.RegisterSpace = 1;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    // Instance transforms (StructuredBuffer<float4x4> gInstances, t1 space1).
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[4].Descriptor.ShaderRegister = 1;
    params[4].Descriptor.RegisterSpace = 1;
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    // Volumetric 3D volume SRV table (t0 space6, pixel-only) - the raymarch pass.
    params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[5].DescriptorTable.NumDescriptorRanges = 1;
    params[5].DescriptorTable.pDescriptorRanges = &volRange;
    params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // General VS-visible structured buffer (StructuredBuffer<T> gVfxRecords,
    // t2 space1) - SetVertexShaderBuffer. A ROOT SRV, like the bone (param 3) and
    // instance (param 4) palettes: its GPU virtual address can be offset freely
    // per draw, which is how a per-batch record base is expressed WITHOUT a
    // firstInstance (Vulkan's twin is a dynamic storage-buffer offset on set 2).
    // Only shaders that declare t2 space1 read it; every other pass leaves it
    // unbound, exactly like the volume SRV table above.
    params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[6].Descriptor.ShaderRegister = 2;
    params[6].Descriptor.RegisterSpace = 1;
    params[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    // Volume TEMPERATURE grid SRV table (t1 space6, pixel-only) - the raymarch emission read. Bound to
    // a valid buffer every frame the pass runs (fallback = the density SRV); hasTemp gates the sample,
    // so the static shader reference always resolves. Unbound/unused by every other pass.
    params[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[7].DescriptorTable.NumDescriptorRanges = 1;
    params[7].DescriptorTable.pDescriptorRanges = &volRange2;
    params[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Static samplers: s0 = anisotropic wrap (materials), s1 = linear clamp
    // (post-process sampling; wrap would bleed opposite screen edges).
    D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
    samplers[0].Filter = D3D12_FILTER_ANISOTROPIC;
    samplers[0].MaxAnisotropy = 8;
    samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[0].ShaderRegister = 0;
    samplers[0].RegisterSpace = 0;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samplers[1] = samplers[0];
    samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[1].MaxAnisotropy = 0;
    samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].ShaderRegister = 1;
    // ALL: the BrushStrokes VERTEX shader samples through this clamp sampler (s1).
    samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 8;
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = 2;
    rsDesc.pStaticSamplers = samplers;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sig, err;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (FAILED(hr)) {
        if (err) HBE_ERROR("[D3D12] RootSig: {}", static_cast<const char*>(err->GetBufferPointer()));
        return false;
    }
    HR_CHECK(device_->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                          IID_PPV_ARGS(&meshRootSig_)),
             "CreateRootSignature");

    // HDR post stack: when the post shaders are present the scene renders into
    // an RGBA16F target (tonemapped later); otherwise fall back to the legacy
    // direct-to-LDR path (shaders tonemap inline via gOutputLinear == 0).
    postPipelinesReady_ = CreatePostPipelines();
    sceneFormat_ = postPipelinesReady_ ? DXGI_FORMAT_R16G16B16A16_FLOAT : swapFormat_;
    if (!postPipelinesReady_) {
        HBE_WARN("[D3D12] Post-process pipelines unavailable; HDR stack disabled.");
    }

    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BLENDINDICES", 0, DXGI_FORMAT_R16G16B16A16_UINT,  0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BLENDWEIGHT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 56, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = meshRootSig_.Get();
    pso.VS = {vs.data(), vs.size()};
    pso.PS = {ps.data(), ps.size()};
    pso.InputLayout = {layout, _countof(layout)};
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.SampleMask = UINT_MAX;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = sceneFormat_;
    pso.DSVFormat = depthFormat_;
    pso.SampleDesc.Count = 1;

    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    // Back-face cull opaque meshes. Empirically, front faces are COUNTER-clockwise
    // in window space here (FrontCounterClockwise = TRUE) - culling with FALSE
    // showed the inside of objects. Matches the Vulkan side.
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    pso.RasterizerState.FrontCounterClockwise = TRUE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    // Opaque blend. All enum fields must hold valid (non-zero) enum values even
    // when blending is disabled, or the debug layer rejects the PSO.
    D3D12_RENDER_TARGET_BLEND_DESC& rt = pso.BlendState.RenderTarget[0];
    rt.BlendEnable = FALSE;
    rt.LogicOpEnable = FALSE;
    rt.SrcBlend = D3D12_BLEND_ONE;
    rt.DestBlend = D3D12_BLEND_ZERO;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlendAlpha = D3D12_BLEND_ZERO;
    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.LogicOp = D3D12_LOGIC_OP_NOOP;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // HDR scene pass writes a thin G-buffer (RGBA16F: octN/rough/metal) and a
    // velocity buffer (RG16F) alongside the colour. The legacy non-post path
    // stays single-target. Every bound RT needs a valid blend desc or the debug
    // layer rejects the PSO, so replicate RT[0]'s opaque blend.
    if (postPipelinesReady_) {
        pso.NumRenderTargets = 3;
        pso.RTVFormats[0] = sceneFormat_; // RGBA16F
        pso.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT; // G-buffer
        pso.RTVFormats[2] = DXGI_FORMAT_R16G16_FLOAT;       // velocity
        pso.BlendState.RenderTarget[1] = rt;
        pso.BlendState.RenderTarget[2] = rt;
    }

    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    pso.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    const D3D12_DEPTH_STENCILOP_DESC stencilOp{
        D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
        D3D12_COMPARISON_FUNC_ALWAYS};
    pso.DepthStencilState.FrontFace = stencilOp;
    pso.DepthStencilState.BackFace = stencilOp;

    // OpenPBR shader specialization (P3): one opaque PSO per curated variant. Only the pixel shader
    // differs (stripped lobes); the shared VS above is reused. A missing/failed variant binary falls
    // back to the FULL pixel shader (`ps`) so the material still renders correctly. Order MUST match
    // rhi::ShaderVariant / cmake MeshPBR_<VAR>.ps.
    const wchar_t* kVariantPs[static_cast<size_t>(ShaderVariant::Count)] = {
        L"MeshPBR_STD.ps.dxil", L"MeshPBR_COAT.ps.dxil", L"MeshPBR_SSS.ps.dxil",
        L"MeshPBR_FUZZ.ps.dxil", L"MeshPBR_HAIR.ps.dxil", L"MeshPBR_EYE.ps.dxil",
        L"MeshPBR.ps.dxil", // Full (== ps, already loaded)
    };
    for (u32 v = 0; v < static_cast<u32>(ShaderVariant::Count); ++v) {
        std::vector<u8> pv =
            (v == static_cast<u32>(ShaderVariant::Full)) ? ps : ReadBinaryFile(dir + kVariantPs[v]);
        if (pv.empty()) pv = ps; // variant binary missing -> FULL (correct, just unspecialized)
        pso.PS = {pv.data(), pv.size()};
        HRESULT psoHr = device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&meshPSO_[v]));
        if (SUCCEEDED(psoHr)) {
            // Two-sided foliage clone: same PSO, cull off (leaf cards need both faces).
            D3D12_GRAPHICS_PIPELINE_STATE_DESC fp = pso;
            fp.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            if (FAILED(device_->CreateGraphicsPipelineState(&fp, IID_PPV_ARGS(&meshPSOFoliage_[v]))))
                meshPSOFoliage_[v] = meshPSO_[v]; // fall back to the culled PSO if the clone fails
        }
        if (FAILED(psoHr)) {
            HBE_ERROR("[D3D12] CreateGraphicsPipelineState (mesh variant {}) failed (hr=0x{:08X})",
                      v, static_cast<u32>(psoHr));
            // Surface the debug-layer explanation if available.
            ComPtr<ID3D12InfoQueue> iq;
            if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&iq)))) {
                const u64 n = iq->GetNumStoredMessages();
                for (u64 i = 0; i < n; ++i) {
                    SIZE_T len = 0;
                    iq->GetMessage(i, nullptr, &len);
                    std::vector<u8> buf(len);
                    auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
                    if (SUCCEEDED(iq->GetMessage(i, msg, &len)) && msg->pDescription) {
                        HBE_ERROR("[D3D12][dbg] {}", msg->pDescription);
                    }
                }
            }
            return false;
        }
    }
    pso.PS = {ps.data(), ps.size()}; // restore FULL PS for the transparent/preview clones below
    HBE_INFO("[D3D12] Mesh PBR pipelines created ({} OpenPBR variants).",
             static_cast<u32>(ShaderVariant::Count));

    // Transparent variant: straight-alpha blend on the colour target, depth test
    // LESS_EQUAL with writes OFF, and no writes to the G-buffer/velocity (so glass
    // / painterly stroke decals don't pollute SSR/AO/TAA). Drawn back-to-front
    // after opaques. Proper alpha blending - no dithering, no TAA dependency.
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC tp = pso;
        tp.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // transparent stays double-sided
        D3D12_RENDER_TARGET_BLEND_DESC& trt = tp.BlendState.RenderTarget[0];
        trt.BlendEnable = TRUE;
        trt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        trt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        trt.BlendOp = D3D12_BLEND_OP_ADD;
        trt.SrcBlendAlpha = D3D12_BLEND_ONE;
        trt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        trt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        if (postPipelinesReady_) {
            tp.BlendState.IndependentBlendEnable = TRUE;
            tp.BlendState.RenderTarget[1].RenderTargetWriteMask = 0; // keep G-buffer
            tp.BlendState.RenderTarget[2].RenderTargetWriteMask = 0; // keep velocity
        }
        tp.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        tp.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        if (FAILED(device_->CreateGraphicsPipelineState(&tp, IID_PPV_ARGS(&meshPSOTransparent_)))) {
            HBE_WARN("[D3D12] Transparent mesh pipeline failed; transparency disabled.");
            meshPSOTransparent_.Reset();
        }
        // "Solid transparent" variant (MaterialFlag_DepthWrite, e.g. paint strokes):
        // identical alpha blend but WRITES depth (+ velocity for TAA) so DoF/TAA treat
        // the stroke as a real in-focus surface instead of the far background. The
        // G-buffer stays masked (kept out of SSR/AO).
        tp.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        if (postPipelinesReady_)
            tp.BlendState.RenderTarget[2].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device_->CreateGraphicsPipelineState(
                &tp, IID_PPV_ARGS(&meshPSOTransparentDepth_)))) {
            meshPSOTransparentDepth_.Reset();
        }
    }

    // 3D painterly surface strokes: instanced camera-facing cards (one per stroke
    // seed), PBR-lit, drawn over the lit mesh. Same alpha-over + depth-LE-no-write
    // + G-buffer-preserving setup as the transparent pass, but with the stroke
    // shaders and a PER-INSTANCE vertex layout (the quad's 6 corners come from
    // SV_VertexID; the StrokeInstance stream steps once per instance).
    {
        const std::vector<u8> svs = ReadBinaryFile(dir + L"StrokeSurface.vs.dxil");
        const std::vector<u8> sps = ReadBinaryFile(dir + L"StrokeSurface.ps.dxil");
        if (!svs.empty() && !sps.empty()) {
            const D3D12_INPUT_ELEMENT_DESC sl[] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
                 D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
                {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
                 D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
                {"TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24,
                 D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
                {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 36,
                 D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
                {"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, 44,
                 D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
            };
            D3D12_GRAPHICS_PIPELINE_STATE_DESC sp = pso; // inherits MRT formats + depth
            sp.VS = {svs.data(), svs.size()};
            sp.PS = {sps.data(), sps.size()};
            sp.InputLayout = {sl, _countof(sl)};
            sp.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            D3D12_RENDER_TARGET_BLEND_DESC& srt = sp.BlendState.RenderTarget[0];
            srt.BlendEnable = TRUE;
            srt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            srt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            srt.BlendOp = D3D12_BLEND_OP_ADD;
            srt.SrcBlendAlpha = D3D12_BLEND_ONE;
            srt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            srt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            if (postPipelinesReady_) {
                sp.BlendState.IndependentBlendEnable = TRUE;
                sp.BlendState.RenderTarget[1].RenderTargetWriteMask = 0; // keep G-buffer
                sp.BlendState.RenderTarget[2].RenderTargetWriteMask = 0; // keep velocity
            }
            sp.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            sp.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
            const HRESULT shr =
                device_->CreateGraphicsPipelineState(&sp, IID_PPV_ARGS(&strokeSurfacePSO_));
            strokeSurfaceReady_ = SUCCEEDED(shr);
            if (!strokeSurfaceReady_) {
                HBE_WARN("[D3D12] Stroke-surface pipeline failed (hr=0x{:08X}).",
                         static_cast<u32>(shr));
                ComPtr<ID3D12InfoQueue> iq;
                if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&iq)))) {
                    const u64 n = iq->GetNumStoredMessages();
                    for (u64 i = 0; i < n; ++i) {
                        SIZE_T len = 0;
                        iq->GetMessage(i, nullptr, &len);
                        std::vector<u8> buf(len);
                        auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
                        if (SUCCEEDED(iq->GetMessage(i, msg, &len)) && msg->pDescription)
                            HBE_WARN("[D3D12][dbg] {}", msg->pDescription);
                    }
                }
            }
        }
    }

    // Gerstner water surface: reflective, alpha-blended, VS-displaced. Same MRT/depth
    // base as the mesh PSO (keeps the standard vertex input layout) + transparent blend +
    // depth-LE-no-write, G-buffer/velocity preserved. Two-sided (waves can face away).
    {
        const std::vector<u8> wvs = ReadBinaryFile(dir + L"Water.vs.dxil");
        const std::vector<u8> wps = ReadBinaryFile(dir + L"Water.ps.dxil");
        if (!wvs.empty() && !wps.empty()) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC wp = pso;
            wp.VS = {wvs.data(), wvs.size()};
            wp.PS = {wps.data(), wps.size()};
            wp.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            D3D12_RENDER_TARGET_BLEND_DESC& wrt = wp.BlendState.RenderTarget[0];
            wrt.BlendEnable = TRUE;
            wrt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            wrt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            wrt.BlendOp = D3D12_BLEND_OP_ADD;
            wrt.SrcBlendAlpha = D3D12_BLEND_ONE;
            wrt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            wrt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            if (postPipelinesReady_) {
                wp.BlendState.IndependentBlendEnable = TRUE;
                wp.BlendState.RenderTarget[1].RenderTargetWriteMask = 0; // keep G-buffer
                wp.BlendState.RenderTarget[2].RenderTargetWriteMask = 0; // keep velocity
            }
            wp.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            wp.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
            waterReady_ =
                SUCCEEDED(device_->CreateGraphicsPipelineState(&wp, IID_PPV_ARGS(&waterPSO_)));
            if (!waterReady_) HBE_WARN("[D3D12] Water pipeline failed to create.");
        }
    }

    // Single-RT variant for the editor preview / asset-viewer mini-pass, which
    // renders into one HDR target (no G-buffer/velocity). The shared PS still
    // outputs SV_Target1/2; D3D12 discards outputs past NumRenderTargets.
    if (postPipelinesReady_) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC single = pso;
        single.NumRenderTargets = 1;
        single.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
        single.RTVFormats[2] = DXGI_FORMAT_UNKNOWN;
        if (FAILED(device_->CreateGraphicsPipelineState(&single, IID_PPV_ARGS(&meshPSOSingle_)))) {
            HBE_WARN("[D3D12] Single-RT mesh pipeline failed; preview may be disabled.");
            meshPSOSingle_.Reset();
        }
    }

    // Sky background pipeline: fullscreen triangle (no vertex input) at the far
    // plane; depth test LESS_EQUAL with writes off so it fills only uncovered
    // pixels. Shares the mesh root signature (frame CBV + bindless table).
    const std::vector<u8> skyVs = ReadBinaryFile(dir + L"Sky.vs.dxil");
    const std::vector<u8> skyPs = ReadBinaryFile(dir + L"Sky.ps.dxil");
    if (!skyVs.empty() && !skyPs.empty()) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC sky = pso; // inherit state, then adjust
        sky.VS = {skyVs.data(), skyVs.size()};
        sky.PS = {skyPs.data(), skyPs.size()};
        sky.InputLayout = {nullptr, 0};
        // Fullscreen triangle: never cull (the inherited mesh state back-face
        // culls, and this triangle's NDC winding would be dropped by it).
        sky.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        sky.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        sky.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        // The sky only writes colour; leave the G-buffer + velocity at their
        // cleared "no surface" values so SSR/AO/GI treat sky pixels correctly.
        if (postPipelinesReady_) {
            sky.BlendState.IndependentBlendEnable = TRUE;
            sky.BlendState.RenderTarget[1].RenderTargetWriteMask = 0;
            sky.BlendState.RenderTarget[2].RenderTargetWriteMask = 0;
        }
        if (FAILED(device_->CreateGraphicsPipelineState(&sky, IID_PPV_ARGS(&skyPSO_)))) {
            HBE_WARN("[D3D12] Sky pipeline creation failed; no background pass.");
            skyPSO_.Reset();
        }
        // Single-RT sky for the preview mini-pass.
        if (postPipelinesReady_ && skyPSO_) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC skySingle = sky;
            skySingle.NumRenderTargets = 1;
            skySingle.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
            skySingle.RTVFormats[2] = DXGI_FORMAT_UNKNOWN;
            skySingle.BlendState.IndependentBlendEnable = FALSE;
            if (FAILED(device_->CreateGraphicsPipelineState(&skySingle,
                                                            IID_PPV_ARGS(&skyPSOSingle_)))) {
                skyPSOSingle_.Reset();
            }
        }
    } else {
        HBE_WARN("[D3D12] Sky DXIL not found; no background pass.");
    }

    // In-game UI overlay pipeline: textured 2D triangles, alpha blend, no
    // depth. Shares the mesh root signature (bindless texture table + static
    // sampler); vertices come from a small per-frame upload buffer.
    {
        const std::vector<u8> uiVs = ReadBinaryFile(dir + L"UI.vs.dxil");
        const std::vector<u8> uiPs = ReadBinaryFile(dir + L"UI.ps.dxil");
        bool ok = !uiVs.empty() && !uiPs.empty();
        if (ok) {
            const D3D12_INPUT_ELEMENT_DESC uiLayout[] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 1, DXGI_FORMAT_R32_UINT, 0, 32,
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 36,
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}, // NDC clip rect
                {"TEXCOORD", 3, DXGI_FORMAT_R32_UINT, 0, 52,
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}, // effect id (P7)
            };
            D3D12_GRAPHICS_PIPELINE_STATE_DESC ui = pso; // inherit, then adjust
            ui.pRootSignature = meshRootSig_.Get();
            ui.VS = {uiVs.data(), uiVs.size()};
            ui.PS = {uiPs.data(), uiPs.size()};
            ui.InputLayout = {uiLayout, _countof(uiLayout)};
            // The UI overlay draws into the post-FXAA final target (the
            // viewport texture / swapchain), which stays LDR.
            ui.RTVFormats[0] = swapFormat_;
            // UI is screen-space 2D: never cull (the mesh PSO this inherits from
            // now back-face culls, which would drop the oppositely-wound quads).
            ui.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            ui.DepthStencilState.DepthEnable = FALSE;
            ui.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            ui.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
            D3D12_RENDER_TARGET_BLEND_DESC& uiRt = ui.BlendState.RenderTarget[0];
            uiRt.BlendEnable = TRUE;
            uiRt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            uiRt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            uiRt.SrcBlendAlpha = D3D12_BLEND_ONE;
            uiRt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            ok = SUCCEEDED(device_->CreateGraphicsPipelineState(&ui, IID_PPV_ARGS(&uiPSO_)));

            // World-UI variant: the SAME UI pipeline against an R8G8B8A8_UNORM
            // canvas texture instead of the swapchain (world canvases render to
            // texture, then a lit quad in the scene shows it). Failure here only
            // disables world-space canvases, never the overlay.
            if (ok) {
                D3D12_GRAPHICS_PIPELINE_STATE_DESC uiw = ui;
                uiw.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
                if (FAILED(device_->CreateGraphicsPipelineState(
                        &uiw, IID_PPV_ARGS(&uiWorldPSO_)))) {
                    HBE_WARN("[D3D12] world-UI pipeline unavailable.");
                    uiWorldPSO_.Reset();
                }
            }
        }
        for (u32 i = 0; ok && i < backBufferCount_; ++i) {
            uiVertexBuffers_[i] = CreateUploadBuffer(device_.Get(), kUIVertexBufferSize);
            ok = uiVertexBuffers_[i] != nullptr;
            if (ok) {
                D3D12_RANGE noRead{0, 0};
                void* mapped = nullptr;
                ok = SUCCEEDED(uiVertexBuffers_[i]->Map(0, &noRead, &mapped));
                uiVertexCpu_[i] = static_cast<u8*>(mapped);
            }
        }
        // World-UI vertex buffers: SEPARATE from the overlay's (which memcpy at
        // offset 0 per call and would alias) - bump-allocated across the frame.
        for (u32 i = 0; uiWorldPSO_ && i < backBufferCount_; ++i) {
            uiWorldVertexBuffers_[i] = CreateUploadBuffer(device_.Get(), kUIVertexBufferSize);
            bool wok = uiWorldVertexBuffers_[i] != nullptr;
            if (wok) {
                D3D12_RANGE noRead{0, 0};
                void* mapped = nullptr;
                wok = SUCCEEDED(uiWorldVertexBuffers_[i]->Map(0, &noRead, &mapped));
                uiWorldVertexCpu_[i] = static_cast<u8*>(mapped);
            }
            if (!wok) {
                HBE_WARN("[D3D12] world-UI vertex buffers unavailable.");
                uiWorldPSO_.Reset();
                break;
            }
        }
        if (!ok) {
            HBE_WARN("[D3D12] UI overlay pipeline unavailable.");
            uiPSO_.Reset();
        }
    }

    // In-scene particle billboards: world-space quads drawn in the HDR pass after
    // transparents, depth-TESTED against the scene (no write) so geometry occludes
    // them; alpha + additive variants. Shares the mesh root sig (frame CBV + bindless).
    {
        const std::vector<u8> pvs = ReadBinaryFile(dir + L"Particle.vs.dxil");
        const std::vector<u8> pps = ReadBinaryFile(dir + L"Particle.ps.dxil");
        bool ok = !pvs.empty() && !pps.empty();
        if (ok) {
            const D3D12_INPUT_ELEMENT_DESC pl[] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12,
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 20,
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 1, DXGI_FORMAT_R32_UINT, 0, 36,
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            };
            const auto makeParticlePSO = [&](bool additive, ComPtr<ID3D12PipelineState>& out) {
                D3D12_GRAPHICS_PIPELINE_STATE_DESC pp = pso; // inherit HDR RTV/DSV formats
                pp.VS = {pvs.data(), pvs.size()};
                pp.PS = {pps.data(), pps.size()};
                pp.InputLayout = {pl, _countof(pl)};
                pp.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // billboards
                pp.DepthStencilState.DepthEnable = TRUE;
                pp.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
                pp.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
                D3D12_RENDER_TARGET_BLEND_DESC& rt = pp.BlendState.RenderTarget[0];
                rt.BlendEnable = TRUE;
                rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
                rt.DestBlend = additive ? D3D12_BLEND_ONE : D3D12_BLEND_INV_SRC_ALPHA;
                rt.BlendOp = D3D12_BLEND_OP_ADD;
                rt.SrcBlendAlpha = D3D12_BLEND_ONE;
                rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
                rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                // Only touch the HDR colour; leave the G-buffer + velocity targets.
                if (postPipelinesReady_) {
                    pp.BlendState.IndependentBlendEnable = TRUE;
                    pp.BlendState.RenderTarget[1].RenderTargetWriteMask = 0;
                    pp.BlendState.RenderTarget[2].RenderTargetWriteMask = 0;
                }
                return SUCCEEDED(device_->CreateGraphicsPipelineState(&pp, IID_PPV_ARGS(&out)));
            };
            ok = makeParticlePSO(false, particlePSO_) && makeParticlePSO(true, particlePSOAdd_);

            // GPU-expanded variant: same blend/depth/RT state, but NO input layout
            // and no vertex buffer - the VS reads the record buffer through root
            // param 6 and builds the quad from SV_VertexID. Failing to build it
            // leaves the CPU path fully intact (it is opt-in per emitter), so this
            // is a warning, not a failure of the whole particle block.
            const std::vector<u8> gvs = ReadBinaryFile(dir + L"ParticleGpu.vs.dxil");
            const std::vector<u8> gps = ReadBinaryFile(dir + L"ParticleGpu.ps.dxil");
            if (ok && !gvs.empty() && !gps.empty()) {
                const auto makeGpuPSO = [&](bool additive, ComPtr<ID3D12PipelineState>& out) {
                    D3D12_GRAPHICS_PIPELINE_STATE_DESC pp = pso;
                    pp.VS = {gvs.data(), gvs.size()};
                    pp.PS = {gps.data(), gps.size()};
                    pp.InputLayout = {nullptr, 0}; // SV_VertexID only
                    pp.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                    pp.DepthStencilState.DepthEnable = TRUE;
                    pp.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
                    pp.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
                    D3D12_RENDER_TARGET_BLEND_DESC& rt = pp.BlendState.RenderTarget[0];
                    rt.BlendEnable = TRUE;
                    rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
                    rt.DestBlend = additive ? D3D12_BLEND_ONE : D3D12_BLEND_INV_SRC_ALPHA;
                    rt.BlendOp = D3D12_BLEND_OP_ADD;
                    rt.SrcBlendAlpha = D3D12_BLEND_ONE;
                    rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
                    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                    if (postPipelinesReady_) {
                        pp.BlendState.IndependentBlendEnable = TRUE;
                        pp.BlendState.RenderTarget[1].RenderTargetWriteMask = 0;
                        pp.BlendState.RenderTarget[2].RenderTargetWriteMask = 0;
                    }
                    return SUCCEEDED(device_->CreateGraphicsPipelineState(&pp, IID_PPV_ARGS(&out)));
                };
                if (!makeGpuPSO(false, particleGpuPSO_) || !makeGpuPSO(true, particleGpuPSOAdd_)) {
                    HBE_WARN("[D3D12] GPU particle expansion pipeline unavailable; "
                             "emitters with gpuExpand fall back to nothing (CPU path is "
                             "unaffected).");
                    particleGpuPSO_.Reset();
                    particleGpuPSOAdd_.Reset();
                }
            } else if (ok) {
                HBE_WARN("[D3D12] ParticleGpu shaders missing; GPU particle expansion off.");
            }

            // GPU-DRIVEN GRASS PSO: like the GPU particle PSO (no input layout, SV_VertexID),
            // but OPAQUE - depth WRITE on, no blend, cull NONE (blades are two-sided). Reuses
            // the main root signature, so the grass PS is fully lit + shadowed. Missing shaders
            // just leave grass off (opt-in per scene), so it warns rather than failing.
            const std::vector<u8> grvs = ReadBinaryFile(dir + L"Grass.vs.dxil");
            const std::vector<u8> grps = ReadBinaryFile(dir + L"Grass.ps.dxil");
            if (ok && !grvs.empty() && !grps.empty()) {
                D3D12_GRAPHICS_PIPELINE_STATE_DESC pp = pso;
                pp.VS = {grvs.data(), grvs.size()};
                pp.PS = {grps.data(), grps.size()};
                pp.InputLayout = {nullptr, 0};
                pp.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                pp.DepthStencilState.DepthEnable = TRUE;
                pp.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
                pp.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
                pp.BlendState.RenderTarget[0].BlendEnable = FALSE;
                if (postPipelinesReady_) {
                    // Opaque grass writes colour only; leave the G-buffer velocity/normal
                    // targets untouched (grass gets no SSAO/velocity in this first cut).
                    pp.BlendState.IndependentBlendEnable = TRUE;
                    pp.BlendState.RenderTarget[1].RenderTargetWriteMask = 0;
                    pp.BlendState.RenderTarget[2].RenderTargetWriteMask = 0;
                }
                if (!SUCCEEDED(device_->CreateGraphicsPipelineState(&pp, IID_PPV_ARGS(&grassPSO_)))) {
                    HBE_WARN("[D3D12] Grass pipeline unavailable; GPU grass off.");
                    grassPSO_.Reset();
                }
            } else if (ok) {
                HBE_WARN("[D3D12] Grass shaders missing; GPU grass off.");
            }

            // INDIRECT/COMPACTED grass PSO (opt-in): identical opaque state, but the VS keys
            // each blade off SV_InstanceID (drawn as 6 verts x instanceCount via ExecuteIndirect).
            // Also build the DRAW command signature once. Missing shaders just leave the indirect
            // path unavailable (SetGrassIndirect no-ops and the caller falls back to SetGrass).
            const std::vector<u8> givs = ReadBinaryFile(dir + L"GrassIndirect.vs.dxil");
            const std::vector<u8> gips = ReadBinaryFile(dir + L"GrassIndirect.ps.dxil");
            if (ok && grassPSO_ && !givs.empty() && !gips.empty()) {
                D3D12_GRAPHICS_PIPELINE_STATE_DESC pp = pso;
                pp.VS = {givs.data(), givs.size()};
                pp.PS = {gips.data(), gips.size()};
                pp.InputLayout = {nullptr, 0};
                pp.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                pp.DepthStencilState.DepthEnable = TRUE;
                pp.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
                pp.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
                pp.BlendState.RenderTarget[0].BlendEnable = FALSE;
                if (postPipelinesReady_) {
                    pp.BlendState.IndependentBlendEnable = TRUE;
                    pp.BlendState.RenderTarget[1].RenderTargetWriteMask = 0;
                    pp.BlendState.RenderTarget[2].RenderTargetWriteMask = 0;
                }
                bool giOk = SUCCEEDED(
                    device_->CreateGraphicsPipelineState(&pp, IID_PPV_ARGS(&grassIndirectPSO_)));
                if (giOk) {
                    // A DRAW-only command signature needs no root-signature (it changes no
                    // root arguments); stride = sizeof(D3D12_DRAW_ARGUMENTS) = 16 bytes.
                    D3D12_INDIRECT_ARGUMENT_DESC arg{};
                    arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
                    D3D12_COMMAND_SIGNATURE_DESC csd{};
                    csd.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
                    csd.NumArgumentDescs = 1;
                    csd.pArgumentDescs = &arg;
                    giOk = SUCCEEDED(device_->CreateCommandSignature(
                        &csd, nullptr, IID_PPV_ARGS(&grassDrawSig_)));
                }
                if (!giOk) {
                    HBE_WARN("[D3D12] Indirect grass pipeline unavailable; falls back to fixed grass.");
                    grassIndirectPSO_.Reset();
                    grassDrawSig_.Reset();
                }
            } else if (ok && grassPSO_) {
                HBE_WARN("[D3D12] GrassIndirect shaders missing; indirect grass path off.");
            }
        }
        for (u32 i = 0; ok && i < backBufferCount_; ++i) {
            particleVertexBuffers_[i] = CreateUploadBuffer(device_.Get(), kParticleVertexBufferSize);
            ok = particleVertexBuffers_[i] != nullptr;
            if (ok) {
                D3D12_RANGE noRead{0, 0};
                void* mapped = nullptr;
                ok = SUCCEEDED(particleVertexBuffers_[i]->Map(0, &noRead, &mapped));
                particleVertexCpu_[i] = static_cast<u8*>(mapped);
            }
        }
        if (!ok) {
            HBE_WARN("[D3D12] Particle pipeline unavailable.");
            particlePSO_.Reset();
            particlePSOAdd_.Reset();
        }
    }
    return true;
}

bool D3D12Device::CreateShadowResources() {
    // Depth-only target sampled by the PBR pass through the bindless table
    // (SRV format R32_FLOAT over the D32 resource).
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = kShadowDim;
    rd.Height = kShadowDim;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R32_TYPELESS;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1.0f;

    const D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    if (FAILED(device_->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
            IID_PPV_ARGS(&shadowMap_)))) {
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC dh{};
    dh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dh.NumDescriptors = 1;
    if (FAILED(device_->CreateDescriptorHeap(&dh, IID_PPV_ARGS(&shadowDsvHeap_)))) {
        return false;
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
    dv.Format = DXGI_FORMAT_D32_FLOAT;
    dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device_->CreateDepthStencilView(shadowMap_.Get(), &dv,
                                    shadowDsvHeap_->GetCPUDescriptorHandleForHeapStart());

    // Reserve a bindless slot and view the depth as R32_FLOAT.
    if (bindlessNextSlot_ >= kMaxBindlessTextures) return false;
    shadowSrvSlot_ = bindlessNextSlot_++;
    D3D12_CPU_DESCRIPTOR_HANDLE h = bindlessHeap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(shadowSrvSlot_) * bindlessDescSize_;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R32_FLOAT;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(shadowMap_.Get(), &sv, h);

    // Depth-only PSO: reuses the MeshPBR vertex shader (gViewProj carries the
    // light matrix during the pass), no pixel shader, no render targets.
    const std::wstring dir = ExecutableDir() + L"shaders\\";
    const std::vector<u8> vs = ReadBinaryFile(dir + L"MeshPBR.vs.dxil");
    if (vs.empty()) return false;

    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BLENDINDICES", 0, DXGI_FORMAT_R16G16B16A16_UINT,  0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BLENDWEIGHT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 56, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = meshRootSig_.Get();
    pso.VS = {vs.data(), vs.size()};
    pso.InputLayout = {layout, _countof(layout)};
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.SampleMask = UINT_MAX;
    pso.NumRenderTargets = 0;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;

    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    // Rasterizer bias fights acne; the shader adds a slope-scaled receiver bias.
    pso.RasterizerState.DepthBias = 100;
    pso.RasterizerState.SlopeScaledDepthBias = 2.0f;
    pso.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    D3D12_RENDER_TARGET_BLEND_DESC& rt = pso.BlendState.RenderTarget[0];
    rt.SrcBlend = D3D12_BLEND_ONE;
    rt.DestBlend = D3D12_BLEND_ZERO;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlendAlpha = D3D12_BLEND_ZERO;
    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.LogicOp = D3D12_LOGIC_OP_NOOP;

    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pso.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    pso.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    const D3D12_DEPTH_STENCILOP_DESC stencilOp{
        D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
        D3D12_COMPARISON_FUNC_ALWAYS};
    pso.DepthStencilState.FrontFace = stencilOp;
    pso.DepthStencilState.BackFace = stencilOp;

    if (FAILED(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&shadowPSO_)))) {
        return false;
    }

    shadowReady_ = true;
    HBE_INFO("[D3D12] Cascaded shadow atlas ready ({0}x{0}, {1} tiles, bindless slot {2}).",
             kShadowDim, kMaxShadowCascades, shadowSrvSlot_);
    return true;
}

bool D3D12Device::CreatePostPipelines() {
    const std::wstring dir = ExecutableDir() + L"shaders\\";

    // Template state shared by every post pass: fullscreen triangle, no vertex
    // input, no depth, opaque blend, one render target.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC base{};
    base.pRootSignature = meshRootSig_.Get();
    base.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    base.SampleMask = UINT_MAX;
    base.NumRenderTargets = 1;
    base.SampleDesc.Count = 1;
    base.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    base.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    base.RasterizerState.DepthClipEnable = TRUE;
    base.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    D3D12_RENDER_TARGET_BLEND_DESC& rt = base.BlendState.RenderTarget[0];
    rt.BlendEnable = FALSE;
    rt.LogicOpEnable = FALSE;
    rt.SrcBlend = D3D12_BLEND_ONE;
    rt.DestBlend = D3D12_BLEND_ZERO;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlendAlpha = D3D12_BLEND_ZERO;
    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.LogicOp = D3D12_LOGIC_OP_NOOP;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    base.DepthStencilState.DepthEnable = FALSE;
    base.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    base.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    base.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    base.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    const D3D12_DEPTH_STENCILOP_DESC stencilOp{
        D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
        D3D12_COMPARISON_FUNC_ALWAYS};
    base.DepthStencilState.FrontFace = stencilOp;
    base.DepthStencilState.BackFace = stencilOp;

    // Shader bytecode must stay alive until each PSO is created.
    std::vector<std::vector<u8>> blobs;
    const auto makePso = [&](const wchar_t* name, DXGI_FORMAT rtvFormat, bool additive,
                             DXGI_FORMAT dsvFormat,
                             ComPtr<ID3D12PipelineState>& out) -> bool {
        const std::vector<u8> vs = ReadBinaryFile(dir + name + std::wstring(L".vs.dxil"));
        const std::vector<u8> ps = ReadBinaryFile(dir + name + std::wstring(L".ps.dxil"));
        if (vs.empty() || ps.empty()) return false;
        blobs.push_back(vs);
        blobs.push_back(ps);
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = base;
        d.VS = {blobs[blobs.size() - 2].data(), blobs[blobs.size() - 2].size()};
        d.PS = {blobs.back().data(), blobs.back().size()};
        d.RTVFormats[0] = rtvFormat;
        d.DSVFormat = dsvFormat;
        if (additive) {
            D3D12_RENDER_TARGET_BLEND_DESC& b = d.BlendState.RenderTarget[0];
            b.BlendEnable = TRUE;
            b.SrcBlend = D3D12_BLEND_ONE;
            b.DestBlend = D3D12_BLEND_ONE;
            b.SrcBlendAlpha = D3D12_BLEND_ONE;
            b.DestBlendAlpha = D3D12_BLEND_ONE;
        }
        return SUCCEEDED(device_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&out)));
    };

    bool ok = true;
    ok = ok && makePso(L"SSAO", DXGI_FORMAT_R8G8B8A8_UNORM, false, DXGI_FORMAT_UNKNOWN, ssaoPSO_);
    ok = ok && makePso(L"SSAOBlur", DXGI_FORMAT_R8G8B8A8_UNORM, false, DXGI_FORMAT_UNKNOWN, ssaoBlurPSO_);
    ok = ok && makePso(L"BloomDown", DXGI_FORMAT_R16G16B16A16_FLOAT, false, DXGI_FORMAT_UNKNOWN, bloomDownPSO_);
    ok = ok && makePso(L"BloomUp", DXGI_FORMAT_R16G16B16A16_FLOAT, true, DXGI_FORMAT_UNKNOWN, bloomUpPSO_);
    ok = ok && makePso(L"Tonemap", swapFormat_, false, DXGI_FORMAT_UNKNOWN, tonemapPSO_);
    // FXAA renders into the final target with the legacy depth bound (unused,
    // but the UI overlay PSO that follows expects a matching DSV).
    ok = ok && makePso(L"FXAA", swapFormat_, false, depthFormat_, fxaaPSO_);
    // TAA is optional: if its shader is missing, temporal AA is simply disabled
    // (the rest of the post stack still runs).
    taaReady_ = makePso(L"TAA", swapFormat_, false, DXGI_FORMAT_UNKNOWN, taaPSO_);
    dofReady_ = makePso(L"DoF", swapFormat_, false, DXGI_FORMAT_UNKNOWN, dofPSO_);
    motionBlurReady_ =
        makePso(L"MotionBlur", swapFormat_, false, DXGI_FORMAT_UNKNOWN, motionBlurPSO_);
    ssrReady_ = makePso(L"SSR", DXGI_FORMAT_R16G16B16A16_FLOAT, false, DXGI_FORMAT_UNKNOWN,
                        ssrPSO_); // SSR composites in HDR
    exposureReady_ = makePso(L"Exposure", DXGI_FORMAT_R16G16B16A16_FLOAT, false,
                             DXGI_FORMAT_UNKNOWN, exposurePSO_);
    volReady_ = makePso(L"VolumetricFog", DXGI_FORMAT_R16G16B16A16_FLOAT, false,
                        DXGI_FORMAT_UNKNOWN, volPSO_); // composites in HDR
    // NanoVDB raymarch PSO (the runtime volume path): samples a StructuredBuffer<uint> grid
    // via PNanoVDB, raymarch -> half-res target, then ApplyHalfRes composite.
    volRaymarchReady_ = makePso(L"VolumeRaymarch", DXGI_FORMAT_R16G16B16A16_FLOAT, false,
                                DXGI_FORMAT_UNKNOWN, volRaymarchPSO_);
    ssgiReady_ = makePso(L"SSGI", DXGI_FORMAT_R16G16B16A16_FLOAT, false,
                         DXGI_FORMAT_UNKNOWN, ssgiPSO_); // composites in HDR
    painterlyReady_ = makePso(L"Painterly", DXGI_FORMAT_R16G16B16A16_FLOAT, false,
                              DXGI_FORMAT_UNKNOWN, painterlyPSO_); // repaints HDR
    // Dynamic-layer composite: lerp painterly vs crisp lit colour by the HDR-alpha mask.
    // REQUIRED, not optional: the painterly chain binds compositePSO_ unconditionally
    // (no ready flag), so dropping this result on the floor would bind a null PSO if the
    // shader were ever missing. Gate `ok` like every other required pass - matching the
    // Vulkan backend, which already does, so a broken shader disables post coherently.
    ok = ok && makePso(L"PainterlyComposite", DXGI_FORMAT_R16G16B16A16_FLOAT, false,
                       DXGI_FORMAT_UNKNOWN, compositePSO_);
    // Reduced-res effect composite (SSGI GI / fog) upscaled back over the full-res HDR.
    ok = ok && makePso(L"ApplyHalfRes", DXGI_FORMAT_R16G16B16A16_FLOAT, false,
                       DXGI_FORMAT_UNKNOWN, applyPSO_);

    // Brush-stroke splat: instanced oriented quads, straight-alpha blended over
    // the painterly base. Custom VS (no fullscreen triangle) so it needs its own
    // PSO with SRC_ALPHA/INV_SRC_ALPHA blending rather than the makePso template.
    {
        const std::vector<u8> vs = ReadBinaryFile(dir + L"BrushStrokes.vs.dxil");
        const std::vector<u8> ps = ReadBinaryFile(dir + L"BrushStrokes.ps.dxil");
        if (!vs.empty() && !ps.empty()) {
            blobs.push_back(vs);
            blobs.push_back(ps);
            D3D12_GRAPHICS_PIPELINE_STATE_DESC d = base;
            d.VS = {blobs[blobs.size() - 2].data(), blobs[blobs.size() - 2].size()};
            d.PS = {blobs.back().data(), blobs.back().size()};
            d.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
            d.DSVFormat = DXGI_FORMAT_UNKNOWN;
            D3D12_RENDER_TARGET_BLEND_DESC& b = d.BlendState.RenderTarget[0];
            b.BlendEnable = TRUE;
            b.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            b.BlendOp = D3D12_BLEND_OP_ADD;
            b.SrcBlendAlpha = D3D12_BLEND_ONE;
            b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            brushStrokesReady_ =
                SUCCEEDED(device_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&brushStrokesPSO_)));
        }
    }
    return ok;
}

bool D3D12Device::CreatePostTargets(u32 width, u32 height) {
    if (width == 0 || height == 0) return false;

    // RTV heap layout: 0=hdr, 1=ssaoRaw, 2=ssaoBlur, 3=ldr, 4+i=bloom[i].
    if (!postRtvHeap_) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        // base 4 + bloom + 2 TAA history + DoF + motion blur + SSR + 2 lum +
        // G-buffer + velocity + volumetric + SSGI + painterly.
        hd.NumDescriptors = 21 + kBloomMaxMips; // +volumetric-particles scatter + half RTVs
        HR_CHECK(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&postRtvHeap_)),
                 "CreateDescriptorHeap(postRTV)");
        postRtvSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }
    if (!postDsvHeap_) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hd.NumDescriptors = 2; // slot 0 = read/write DSV, slot 1 = read-only DSV (water pass)
        HR_CHECK(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&postDsvHeap_)),
                 "CreateDescriptorHeap(postDSV)");
    }
    // Reserve the bindless slots once; resizes rewrite the same descriptors so
    // shader-visible indices stay stable.
    if (slotHdr_ == 0) {
        if (bindlessNextSlot_ + 23 + 2 * kMaxBackBuffers + kBloomMaxMips > kMaxBindlessTextures)
            return false; // 2*kMaxBackBuffers = density + temperature grid SRVs, one pair per frame
        slotHdr_ = bindlessNextSlot_++;
        slotDepth_ = bindlessNextSlot_++;
        slotGbuffer_ = bindlessNextSlot_++;
        slotVelocity_ = bindlessNextSlot_++;
        slotSceneColor_ = bindlessNextSlot_++; // opaque scene-colour copy for transmission (P6b)
        slotVol_ = bindlessNextSlot_++;
        slotVolHalf_ = bindlessNextSlot_++;
        slotVolPart_ = bindlessNextSlot_++;
        slotVolPartHalf_ = bindlessNextSlot_++;
        // NanoVDB grid StructuredBuffer<uint> SRV, one per frame-in-flight (the blob differs
        // per frame). The SRV is (re)created when the per-frame buffer grows.
        for (u32 i = 0; i < kMaxBackBuffers; ++i) slotVolGrid_[i] = bindlessNextSlot_++;
        // Temperature grid SRV, one per frame-in-flight (mirrors slotVolGrid_; the emission grid).
        for (u32 i = 0; i < kMaxBackBuffers; ++i) slotVolTemp_[i] = bindlessNextSlot_++;
        slotSsgi_ = bindlessNextSlot_++;
        slotSsgiHalf_ = bindlessNextSlot_++;
        slotPainterly_ = bindlessNextSlot_++;
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

    const D3D12_HEAP_PROPERTIES def = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    const auto makeColor = [&](u32 w, u32 h, DXGI_FORMAT fmt, u32 rtvIndex, u32 srvSlot,
                               const f32* clearColor,
                               ComPtr<ID3D12Resource>& out) -> bool {
        out.Reset();
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = fmt; rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = fmt;
        if (clearColor) std::memcpy(cv.Color, clearColor, sizeof(cv.Color));
        if (FAILED(device_->CreateCommittedResource(
                &def, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                clearColor ? &cv : nullptr, IID_PPV_ARGS(&out)))) {
            return false;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = postRtvHeap_->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(rtvIndex) * postRtvSize_;
        device_->CreateRenderTargetView(out.Get(), nullptr, rtv);
        D3D12_CPU_DESCRIPTOR_HANDLE srv = bindlessHeap_->GetCPUDescriptorHandleForHeapStart();
        srv.ptr += static_cast<SIZE_T>(srvSlot) * bindlessDescSize_;
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = fmt;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(out.Get(), &sv, srv);
        return true;
    };

    const f32 hdrClear[4] = {0.018f, 0.018f, 0.022f, 1.0f};
    if (!makeColor(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, slotHdr_, hdrClear, hdrColor_))
        return false;

    // Scene-colour copy target for screen-space refraction (P6b): a plain R16F texture with only a
    // bindless SRV (no RTV). Filled by CopyResource(hdrColor_) between opaque+sky and the transparent
    // pass; its slot (slotSceneColor_) stays stable across resizes like the others.
    {
        sceneColorCopy_.Reset();
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = width; rd.Height = height; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags = D3D12_RESOURCE_FLAG_NONE; // copy dest + SRV only (never a render target)
        if (FAILED(device_->CreateCommittedResource(
                &def, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                nullptr, IID_PPV_ARGS(&sceneColorCopy_))))
            return false;
        D3D12_CPU_DESCRIPTOR_HANDLE srv = bindlessHeap_->GetCPUDescriptorHandleForHeapStart();
        srv.ptr += static_cast<SIZE_T>(slotSceneColor_) * bindlessDescSize_;
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(sceneColorCopy_.Get(), &sv, srv);
    }

    // Sampleable scene depth (R32_TYPELESS resource, D32 DSV, R32F SRV).
    {
        hdrDepth_.Reset();
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = width; rd.Height = height; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R32_TYPELESS;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = DXGI_FORMAT_D32_FLOAT;
        cv.DepthStencil.Depth = 1.0f;
        HR_CHECK(device_->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &rd,
                                                  D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                                                  IID_PPV_ARGS(&hdrDepth_)),
                 "CreateCommittedResource(hdrDepth)");
        D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = DXGI_FORMAT_D32_FLOAT;
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device_->CreateDepthStencilView(hdrDepth_.Get(), &dv,
                                        postDsvHeap_->GetCPUDescriptorHandleForHeapStart());
        // Read-only DSV at slot 1: identical, plus READ_ONLY_DEPTH so the same image can be a
        // depth attachment (for the water depth-test) AND a shader resource (sampled) at once.
        dv.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
        const u32 dsvInc = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        roDsvHandle_ = postDsvHeap_->GetCPUDescriptorHandleForHeapStart();
        roDsvHandle_.ptr += static_cast<SIZE_T>(dsvInc);
        device_->CreateDepthStencilView(hdrDepth_.Get(), &dv, roDsvHandle_);
        roDsvValid_ = true;
        D3D12_CPU_DESCRIPTOR_HANDLE srv = bindlessHeap_->GetCPUDescriptorHandleForHeapStart();
        srv.ptr += static_cast<SIZE_T>(slotDepth_) * bindlessDescSize_;
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R32_FLOAT;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(hdrDepth_.Get(), &sv, srv);
        hdrDepthInSrv_ = false;
    }

    // Thin G-buffer (octahedral world normal + roughness/metalness) and screen
    // velocity, written by the forward pass alongside the HDR colour. Cleared to
    // 0 each frame; sky/empty pixels stay 0 (callers gate on depth == 1).
    const f32 zeroClear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (!makeColor(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, 11 + kBloomMaxMips,
                   slotGbuffer_, zeroClear, gbuffer_))
        return false;
    if (!makeColor(width, height, DXGI_FORMAT_R16G16_FLOAT, 12 + kBloomMaxMips,
                   slotVelocity_, zeroClear, velocity_))
        return false;
    gbufInSrv_ = true; // created in PIXEL_SHADER_RESOURCE state

    // Half-resolution SSAO targets.
    ssaoW_ = (std::max)(1u, width / 2);
    ssaoH_ = (std::max)(1u, height / 2);
    if (!makeColor(ssaoW_, ssaoH_, DXGI_FORMAT_R8G8B8A8_UNORM, 1, slotSsaoRaw_, nullptr, ssaoRaw_))
        return false;
    if (!makeColor(ssaoW_, ssaoH_, DXGI_FORMAT_R8G8B8A8_UNORM, 2, slotSsaoBlur_, nullptr, ssaoBlur_))
        return false;

    // Bloom pyramid from half resolution down (stop above 8px).
    bloomCount_ = 0;
    for (u32 i = 0; i < kBloomMaxMips; ++i) {
        const u32 w = (std::max)(1u, width >> (i + 1));
        const u32 h = (std::max)(1u, height >> (i + 1));
        if (w < 8 || h < 8) break;
        bloomW_[i] = w;
        bloomH_[i] = h;
        if (!makeColor(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, 4 + i, slotBloom_[i], nullptr,
                       bloom_[i]))
            return false;
        ++bloomCount_;
    }

    // Tonemapped LDR (pre-FXAA).
    if (!makeColor(width, height, swapFormat_, 3, slotLdr_, nullptr, ldr_)) return false;

    // TAA history (ping-pong), same format as the LDR resolve.
    if (taaReady_) {
        const u32 r0 = 4 + kBloomMaxMips; // RTV indices past the bloom mips
        if (!makeColor(width, height, swapFormat_, r0, slotTaaHistory_[0], nullptr,
                       taaHistory_[0]))
            return false;
        if (!makeColor(width, height, swapFormat_, r0 + 1, slotTaaHistory_[1], nullptr,
                       taaHistory_[1]))
            return false;
        taaHistoryValid_ = false; // history is stale after a (re)size
    }

    // Depth-of-field result (LDR), one RTV past the TAA history targets.
    if (dofReady_) {
        if (!makeColor(width, height, swapFormat_, 6 + kBloomMaxMips, slotDof_, nullptr, dof_))
            return false;
    }
    if (motionBlurReady_) {
        if (!makeColor(width, height, swapFormat_, 7 + kBloomMaxMips, slotMotionBlur_, nullptr,
                       motionBlur_))
            return false;
    }
    if (ssrReady_) { // HDR (composites reflections before tonemap)
        if (!makeColor(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, 8 + kBloomMaxMips,
                       slotSsr_, nullptr, ssr_))
            return false;
    }
    if (volReady_) { // HDR (volumetric fog composited before bloom)
        if (!makeColor(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, 13 + kBloomMaxMips,
                       slotVol_, nullptr, volScatter_))
            return false;
        // Fog (inscatter+transmittance) at QUARTER res - fog is very low-frequency, so
        // HALF-res (was quarter): the march's per-step sun-shadow is world-scale
        // blocky at grazing sun angles; quarter-res made the blocks obvious.
        // Half-res + the bilateral upscale reads smooth, and the fog march is
        // cheap enough that 4x the pixels is still a small pass.
        const u32 hw = (width + 1u) / 2u, hh = (height + 1u) / 2u;
        if (!makeColor(hw, hh, DXGI_FORMAT_R16G16B16A16_FLOAT, 18 + kBloomMaxMips, slotVolHalf_,
                       nullptr, volHalf_))
            return false;
    }
    if (volRaymarchReady_) { // NanoVDB volume raymarch: full-res composite + half-res raymarch
        if (!makeColor(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, 19 + kBloomMaxMips,
                       slotVolPart_, nullptr, volPartScatter_))
            return false;
        const u32 hw = (width + 1u) / 2u, hh = (height + 1u) / 2u;
        if (!makeColor(hw, hh, DXGI_FORMAT_R16G16B16A16_FLOAT, 20 + kBloomMaxMips, slotVolPartHalf_,
                       nullptr, volPartHalf_))
            return false;
    }
    if (ssgiReady_) { // HDR (indirect bounce composited before bloom)
        if (!makeColor(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, 14 + kBloomMaxMips,
                       slotSsgi_, nullptr, ssgi_))
            return false;
        // GI-only term at QUARTER res (GI is very low-frequency + TAA-denoised, so a
        // 1/4-res gather + bilinear upscale is near-lossless and ~16x cheaper).
        const u32 qw = (width + 3u) / 4u, qh = (height + 3u) / 4u;
        if (!makeColor(qw, qh, DXGI_FORMAT_R16G16B16A16_FLOAT, 17 + kBloomMaxMips, slotSsgiHalf_,
                       nullptr, ssgiHalf_))
            return false;
    }
    if (painterlyReady_) { // HDR (brush-stroke repaint before bloom/tonemap)
        if (!makeColor(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, 15 + kBloomMaxMips,
                       slotPainterly_, nullptr, painterly_))
            return false;
        // Dynamic-layer composite target (painterly + crisp objects by the HDR mask).
        if (!makeColor(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, 16 + kBloomMaxMips,
                       slotPainterlyComp_, nullptr, painterlyComp_))
            return false;
    }
    if (exposureReady_) { // 1x1 adapted-luminance ping-pong (auto-exposure)
        if (!makeColor(1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 9 + kBloomMaxMips,
                       slotAdaptedLum_[0], nullptr, adaptedLum_[0]))
            return false;
        if (!makeColor(1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 10 + kBloomMaxMips,
                       slotAdaptedLum_[1], nullptr, adaptedLum_[1]))
            return false;
        adaptValid_ = false; // no adapted history after a (re)size
    }

    sceneW_ = width;
    sceneH_ = height;
    HBE_INFO("[D3D12] HDR post targets ready ({}x{}, {} bloom mips).", width, height,
             bloomCount_);
    return true;
}

void D3D12Device::DrawPostPass(ID3D12PipelineState* pso, ID3D12Resource* target,
                               D3D12_CPU_DESCRIPTOR_HANDLE rtv, u32 w, u32 h,
                               const PostCB& cb) {
    auto toRT = TransitionBarrier(target, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                  D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmdList_->ResourceBarrier(1, &toRT);
    cmdList_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<f32>(w), static_cast<f32>(h), 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
    cmdList_->RSSetViewports(1, &vp);
    cmdList_->RSSetScissorRects(1, &scissor);
    D3D12_GPU_VIRTUAL_ADDRESS addr = 0;
    if (void* dst = AllocConstants(sizeof(PostCB), addr)) {
        std::memcpy(dst, &cb, sizeof(cb));
        cmdList_->SetGraphicsRootConstantBufferView(1, addr);
    } else {
        // No constants -> the root CBV still points at the PREVIOUS pass's PostCB, so this
        // pass would run with another effect's parameters. Skip it: a missing effect is a
        // visible, explicable result; a pass running on the wrong constants is not.
        ++constantStarvedPasses_;
        return;
    }
    cmdList_->SetPipelineState(pso);
    cmdList_->DrawInstanced(3, 1, 0, 0);
    auto toSRV = TransitionBarrier(target, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList_->ResourceBarrier(1, &toSRV);
}

void D3D12Device::RunPostStack(const SceneView& view) {
    const PostSettings& ps = view.post;
    const glm::vec2 sceneTexel(1.0f / sceneW_, 1.0f / sceneH_);

    // The scene pass is done: make HDR color + depth + G-buffer + velocity
    // sampleable for the post passes.
    D3D12_RESOURCE_BARRIER toRead[4] = {
        TransitionBarrier(hdrColor_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        TransitionBarrier(hdrDepth_.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        TransitionBarrier(gbuffer_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        TransitionBarrier(velocity_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
    };
    cmdList_->ResourceBarrier(4, toRead);
    hdrDepthInSrv_ = true;
    gbufInSrv_ = true;

    const auto rtvAt = [&](u32 index) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = postRtvHeap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(index) * postRtvSize_;
        return h;
    };

    // --- Screen-space reflections (HDR; composited before bloom/tonemap) ----
    u32 hdrInput = slotHdr_; // what bloom + tonemap read (SSR output when on)
    if (view.post.ssrEnabled && ssrReady_) {
        PostCB cb;
        cb.input0 = slotHdr_;
        cb.input1 = slotGbuffer_; // per-pixel normal + roughness
        cb.input2 = slotDepth_;
        cb.outTexel = sceneTexel;
        cb.inTexel = sceneTexel;
        cb.params0 = {ps.ssrIntensity, ps.ssrMaxDistance, 1.0f, 0.0f};
        DrawPostPass(ssrPSO_.Get(), ssr_.Get(), rtvAt(8 + kBloomMaxMips), sceneW_, sceneH_, cb);
        hdrInput = slotSsr_;
    }
    GpuMark("ssr");

    // --- Screen-space GI: gathered at REDUCED res, then upscaled + added over the
    //     full-res HDR. The SSGI shader outputs the GI term only (a=1); ApplyHalfRes
    //     does scene + GI. GI is low-frequency + TAA-denoised, so ~16x cheaper is
    //     near-lossless. -------------------------------------------------------------
    if (view.post.ssgiEnabled && ssgiReady_) {
        const u32 qw = (sceneW_ + 3u) / 4u, qh = (sceneH_ + 3u) / 4u;
        const glm::vec2 giTexel(1.0f / qw, 1.0f / qh);
        PostCB cb;
        cb.input0 = hdrInput;   // full-res lit HDR = the ray-march gather source
        cb.input1 = slotGbuffer_;
        cb.input2 = slotDepth_;
        cb.outTexel = giTexel;  // rasterize GI at reduced res
        cb.inTexel = sceneTexel;
        cb.params0 = {ps.ssgiIntensity, ps.ssgiRadius, static_cast<f32>(ps.ssgiSamples), 1.0f};
        DrawPostPass(ssgiPSO_.Get(), ssgiHalf_.Get(), rtvAt(17 + kBloomMaxMips), qw, qh, cb);
        // Upscale + add: scene + GI -> full-res HDR.
        PostCB ap;
        ap.input0 = hdrInput;
        ap.input1 = slotSsgiHalf_;
        ap.outTexel = sceneTexel;
        ap.inTexel = sceneTexel;
        DrawPostPass(applyPSO_.Get(), ssgi_.Get(), rtvAt(14 + kBloomMaxMips), sceneW_, sceneH_, ap);
        hdrInput = slotSsgi_;
    }
    GpuMark("ssgi");

    // --- Volumetric fog: marched at HALF res (inscatter+transmittance), then upscaled
    //     + applied over the full-res HDR (scene*transmittance + inscatter). Fog is
    //     smooth, so half-res is near-lossless and ~4x cheaper. ---------------------
    if (view.post.fogEnabled && volReady_) {
        const u32 hw = (sceneW_ + 1u) / 2u, hh = (sceneH_ + 1u) / 2u; // half res (matches volHalf_)
        const glm::vec2 fogTexel(1.0f / hw, 1.0f / hh);
        PostCB cb;
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
        DrawPostPass(volPSO_.Get(), volHalf_.Get(), rtvAt(18 + kBloomMaxMips), hw, hh, cb);
        // Upscale + apply: scene * transmittance + inscatter -> full-res HDR. Low-pass
        // the fog buffer here (inTexel = fog's quarter-res texel, params0.x = radius) so
        // the world-scale sun-shadow blocks smooth out instead of reading as cubes.
        PostCB ap;
        ap.input0 = hdrInput;
        ap.input1 = slotVolHalf_;
        ap.input2 = slotDepth_;     // depth for the bilateral (no silhouette bleed)
        ap.outTexel = sceneTexel;
        ap.inTexel = fogTexel;      // blur offsets in fog (half-res) texels
        ap.params0 = {1.5f, 0.0f, 0.0f, 0.0f}; // fog blur radius (texels); 0 = crisp (SSGI)
        DrawPostPass(applyPSO_.Get(), volScatter_.Get(), rtvAt(13 + kBloomMaxMips), sceneW_, sceneH_,
                     ap);
        hdrInput = slotVol_;
    }
    GpuMark("fog");

    // --- NanoVDB baked volume: the RUNTIME volume path. Upload the raw NanoVDB blob to a
    //     per-frame StructuredBuffer<uint>, bind it at the volume SRV table (root param 5), and
    //     PNanoVDB-raymarch it (half res) + composite over the HDR - same shape as fog/splat.
    //     PNanoVDB-raymarch it (half res) + composite over the HDR. ----------------------------
    if (volGridSize_ > 0 && volGridBytes_ && volRaymarchReady_ &&
        EnsureVolumeGridBuffer(volGridSize_)) {
        std::memcpy(volGridCpu_[frameIndex_], volGridBytes_, volGridSize_);
        D3D12_GPU_DESCRIPTOR_HANDLE gridSrv = bindlessHeap_->GetGPUDescriptorHandleForHeapStart();
        gridSrv.ptr += static_cast<UINT64>(slotVolGrid_[frameIndex_]) * bindlessDescSize_;
        cmdList_->SetGraphicsRootDescriptorTable(5, gridSrv); // StructuredBuffer<uint> t0 space6
        // Temperature grid (emission). Bind a VALID SRV every frame (fallback = the density SRV);
        // hasTemp gates the sample, so the static t1 space6 reference always resolves.
        bool hasTemp = false;
        if (volTempSize_ > 0 && volTempBytes_ && EnsureVolumeTempBuffer(volTempSize_)) {
            std::memcpy(volTempCpu_[frameIndex_], volTempBytes_, volTempSize_);
            hasTemp = true;
        }
        D3D12_GPU_DESCRIPTOR_HANDLE tempSrv = bindlessHeap_->GetGPUDescriptorHandleForHeapStart();
        tempSrv.ptr += static_cast<UINT64>(hasTemp ? slotVolTemp_[frameIndex_]
                                                   : slotVolGrid_[frameIndex_]) * bindlessDescSize_;
        cmdList_->SetGraphicsRootDescriptorTable(7, tempSrv); // StructuredBuffer<uint> t1 space6
        const u32 hw = (sceneW_ + 1u) / 2u, hh = (sceneH_ + 1u) / 2u;
        const glm::vec2 vTexel(1.0f / hw, 1.0f / hh);
        const VolumeRenderParams& rp = volRenderParams_;
        PostCB cb;
        cb.input2 = slotDepth_;
        cb.outTexel = vTexel;
        cb.inTexel = sceneTexel;
        cb.params0 = {rp.boundsMin.x, rp.boundsMin.y, rp.boundsMin.z, static_cast<f32>(rp.stepCount)};
        cb.params1 = {rp.boundsMax.x, rp.boundsMax.y, rp.boundsMax.z, rp.densityScale};
        cb.params2 = {rp.emission, static_cast<f32>(rp.shadowSteps), rp.extinction,
                      ps.taaEnabled ? glm::mod(glm::floor(view.timeSeconds * 120.0f), 64.0f) : 0.0f};
        cb.params3 = {rp.worldOffset.x, rp.worldOffset.y, rp.worldOffset.z, 0.0f}; // volume placement
        cb.volAlbedo = {rp.albedo.x, rp.albedo.y, rp.albedo.z, static_cast<f32>(rp.emissionMode)};
        cb.volEmission = {rp.emissionColor.x, rp.emissionColor.y, rp.emissionColor.z,
                          hasTemp ? 1.0f : 0.0f}; // .w = hasTemp gate
        DrawPostPass(volRaymarchPSO_.Get(), volPartHalf_.Get(), rtvAt(20 + kBloomMaxMips), hw, hh, cb);
        PostCB ap;
        ap.input0 = hdrInput;
        ap.input1 = slotVolPartHalf_;
        ap.input2 = slotDepth_;
        ap.outTexel = sceneTexel;
        ap.inTexel = vTexel;
        ap.params0 = {1.0f, 0.0f, 0.0f, 0.0f};
        DrawPostPass(applyPSO_.Get(), volPartScatter_.Get(), rtvAt(19 + kBloomMaxMips), sceneW_,
                     sceneH_, ap);
        hdrInput = slotVolPart_;
    }
    GpuMark("volume");

    // --- Painterly: repaint the lit HDR as edge-aware brush strokes ----------
    // Skipped when the true 3D surface-stroke renderer is active (no double-paint).
    const u32 paintColorSrc = hdrInput; // pre-painterly lit HDR (crisp stroke colour)
    if (ps.painterlyEnabled && !ps.painterly3D && painterlyReady_) {
        PostCB cb;
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
        // into mush (the filter's whole value is crisp region boundaries), so it runs at
        // full res. Painterly is the art style; spend the ms on it, save perf elsewhere.
        DrawPostPass(painterlyPSO_.Get(), painterly_.Get(), rtvAt(15 + kBloomMaxMips), sceneW_,
                     sceneH_, cb);
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
            auto toRT = TransitionBarrier(painterly_.Get(),
                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                          D3D12_RESOURCE_STATE_RENDER_TARGET);
            cmdList_->ResourceBarrier(1, &toRT);
            const D3D12_CPU_DESCRIPTOR_HANDLE srtv = rtvAt(15 + kBloomMaxMips);
            cmdList_->OMSetRenderTargets(1, &srtv, FALSE, nullptr);
            D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<f32>(sceneW_), static_cast<f32>(sceneH_),
                              0.0f, 1.0f};
            D3D12_RECT scissor{0, 0, static_cast<LONG>(sceneW_), static_cast<LONG>(sceneH_)};
            cmdList_->RSSetViewports(1, &vp);
            cmdList_->RSSetScissorRects(1, &scissor);
            cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            cmdList_->SetPipelineState(brushStrokesPSO_.Get());

            const f32 density = std::max(ps.painterlyStrokeDensity, 0.1f);
            const f32 baseLen = std::max(ps.painterlyRadius * 6.0f * ps.painterlyStrokeLength, 6.0f);
            // Stop-motion "boil": fold a TIME-QUANTIZED step into the per-stroke seed
            // so the strokes repaint in discrete jumps at painterlyStrokeBoil fps and
            // hold in between (instead of recomputing every frame). Wrapped to a small
            // range so the growing time can't blow up Hash21 (precision -> stripes).
            const f32 boilPhase =
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
            const auto drawLayer = [&](f32 lenPx, f32 widthFrac, f32 spacingFac, f32 seed) {
                const f32 spacing = std::max(3.0f, lenPx * spacingFac / density);
                const u32 cols = std::max(1u, static_cast<u32>(std::ceil(sceneW_ / spacing)));
                const u32 rows = std::max(1u, static_cast<u32>(std::ceil(sceneH_ / spacing)));
                PostCB cb2;
                cb2.input0 = paintColorSrc;
                cb2.input1 = slotGbuffer_;
                cb2.input2 = slotDepth_; // depth: world anchoring + the 3D censor test
                cb2.input3 = slotHdr_;   // forward HDR alpha = the per-pixel censored flag
                cb2.outTexel = sceneTexel;
                cb2.inTexel = sceneTexel;
                cb2.params0 = {lenPx, widthFrac, 0.35f, ps.painterlyStrength};
                cb2.params1 = {ps.painterlyStrokeSharp, ps.painterlyEdge, 0.30f,
                               std::max(ps.painterlyStrokeDetail * 2.0f, 0.25f)};
                cb2.params2 = {static_cast<f32>(cols), static_cast<f32>(rows),
                               ps.painterlyStrokeFlow, seed};
                cb2.params3 = strokeRect;
                for (u32 ci = 0; ci < kMaxCensors; ++ci) cb2.censors[ci] = censorArr[ci];
                cb2.censorStrength = censorStrength;
                cb2.censorFeather = censorFeather;
                cb2.censorCount = glm::uvec4(nCensor, 0u, 0u, 0u);
                D3D12_GPU_VIRTUAL_ADDRESS addr = 0;
                if (void* dst = AllocConstants(sizeof(PostCB), addr)) {
                    std::memcpy(dst, &cb2, sizeof(cb2));
                    cmdList_->SetGraphicsRootConstantBufferView(1, addr);
                    cmdList_->DrawInstanced(6, cols * rows, 0, 0);
                } else {
                    ++constantStarvedPasses_;
                }
            };
            drawLayer(baseLen * 1.8f, 0.55f, 0.62f, 11.0f + boilPhase); // coarse block-in
            drawLayer(baseLen, 0.42f, 0.46f, 37.0f + boilPhase);        // finer detail

            auto toSRV = TransitionBarrier(painterly_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cmdList_->ResourceBarrier(1, &toSRV);
        }
        GpuMark("strokes");

        // Dynamic-layer composite: restore crisp lit colour over the painterly where the
        // forward HDR alpha mask = 1 (player / NPCs / interactables), so dynamic objects
        // stand out against the painted static world. paintColorSrc = pre-painterly lit
        // HDR (fog/GI integrated); slotHdr_ = untouched forward HDR carrying the mask.
        PostCB comp;
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
        DrawPostPass(compositePSO_.Get(), painterlyComp_.Get(), rtvAt(16 + kBloomMaxMips),
                     sceneW_, sceneH_, comp);
        hdrInput = slotPainterlyComp_;
        GpuMark("composite");
    }

    // --- SSAO + blur (half res) --------------------------------------------
    u32 aoSlot = 0; // bindless white: no occlusion
    if (ps.ssaoEnabled) {
        const glm::vec2 ssaoTexel(1.0f / ssaoW_, 1.0f / ssaoH_);
        PostCB cb;
        cb.input0 = slotDepth_;
        cb.input1 = slotGbuffer_; // GTAO uses the shading normal
        cb.outTexel = ssaoTexel;
        cb.inTexel = sceneTexel;
        cb.params0 = {ps.ssaoRadius, ps.ssaoIntensity, 0.0005f, 0.0f};
        DrawPostPass(ssaoPSO_.Get(), ssaoRaw_.Get(), rtvAt(1), ssaoW_, ssaoH_, cb);

        PostCB blur;
        blur.input0 = slotSsaoRaw_;
        blur.outTexel = ssaoTexel;
        blur.inTexel = ssaoTexel;
        DrawPostPass(ssaoBlurPSO_.Get(), ssaoBlur_.Get(), rtvAt(2), ssaoW_, ssaoH_, blur);
        aoSlot = slotSsaoBlur_;
    }
    GpuMark("ssao");

    // --- Bloom pyramid -------------------------------------------------------
    f32 bloomMix = 0.0f;
    if (ps.bloomEnabled && bloomCount_ > 0) {
        PostCB cb;
        cb.input0 = hdrInput;
        cb.inTexel = sceneTexel;
        cb.outTexel = {1.0f / bloomW_[0], 1.0f / bloomH_[0]};
        cb.params0 = {ps.bloomThreshold, ps.bloomThreshold * 0.5f + 1e-4f, 1.0f, 0.0f};
        DrawPostPass(bloomDownPSO_.Get(), bloom_[0].Get(), rtvAt(4), bloomW_[0], bloomH_[0], cb);
        for (u32 i = 1; i < bloomCount_; ++i) {
            PostCB down;
            down.input0 = slotBloom_[i - 1];
            down.inTexel = {1.0f / bloomW_[i - 1], 1.0f / bloomH_[i - 1]};
            down.outTexel = {1.0f / bloomW_[i], 1.0f / bloomH_[i]};
            DrawPostPass(bloomDownPSO_.Get(), bloom_[i].Get(), rtvAt(4 + i), bloomW_[i],
                         bloomH_[i], down);
        }
        for (u32 i = bloomCount_ - 1; i-- > 0;) {
            PostCB up;
            up.input0 = slotBloom_[i + 1];
            up.inTexel = {1.0f / bloomW_[i + 1], 1.0f / bloomH_[i + 1]};
            up.outTexel = {1.0f / bloomW_[i], 1.0f / bloomH_[i]};
            up.params0 = {1.0f, 0.0f, 0.0f, 0.0f};
            DrawPostPass(bloomUpPSO_.Get(), bloom_[i].Get(), rtvAt(4 + i), bloomW_[i],
                         bloomH_[i], up);
        }
        bloomMix = ps.bloomIntensity;
    }
    GpuMark("bloom");

    // --- Auto-exposure: average luminance + temporal adaptation (1x1) --------
    u32 lumSlot = 0; // 0 = off; tonemap then uses manual exposure only
    if (view.post.autoExposureEnabled && exposureReady_) {
        const u32 cur = adaptIndex_;
        const u32 prev = cur ^ 1u;
        PostCB cb;
        cb.input0 = hdrInput;
        cb.input1 = slotAdaptedLum_[prev];
        cb.outTexel = {1.0f, 1.0f};
        cb.inTexel = sceneTexel;
        cb.params0 = {ps.autoExposureSpeed, view.deltaTime, adaptValid_ ? 1.0f : 0.0f, 0.0f};
        DrawPostPass(exposurePSO_.Get(), adaptedLum_[cur].Get(), rtvAt(9 + kBloomMaxMips + cur),
                     1, 1, cb);
        lumSlot = slotAdaptedLum_[cur];
        adaptIndex_ = prev;
        adaptValid_ = true;
    }
    GpuMark("exposure");

    // --- Tonemap composite -> LDR -------------------------------------------
    {
        PostCB cb;
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
        DrawPostPass(tonemapPSO_.Get(), ldr_.Get(), rtvAt(3), sceneW_, sceneH_, cb);
    }
    GpuMark("tonemap");

    // --- TAA resolve -> history ---------------------------------------------
    // Reproject last frame's accumulation into this frame (via depth + the
    // previous view-proj) and blend a small slice of the current frame, turning
    // the per-frame camera jitter into temporal supersampling. The final pass
    // reads the freshly written history.
    u32 finalInput = slotLdr_;
    if (view.post.taaEnabled && taaReady_) {
        const u32 cur = taaHistoryIndex_;
        const u32 prev = cur ^ 1u;
        PostCB cb;
        cb.input0 = slotLdr_;               // current tonemapped frame
        cb.input1 = slotTaaHistory_[prev];  // previous accumulation
        cb.input2 = slotDepth_;             // depth (kept for reference)
        cb.input3 = slotVelocity_;          // per-object motion vectors
        cb.outTexel = sceneTexel;
        cb.inTexel = sceneTexel;
        cb.params0 = {0.1f, taaHistoryValid_ ? 1.0f : 0.0f, 0.0f, 0.0f};
        DrawPostPass(taaPSO_.Get(), taaHistory_[cur].Get(),
                     rtvAt(4 + kBloomMaxMips + cur), sceneW_, sceneH_, cb);
        finalInput = slotTaaHistory_[cur];
        taaHistoryIndex_ = prev; // next frame writes the other target
        taaHistoryValid_ = true;
    }
    GpuMark("taa");

    // --- Depth of field -> dof_ ---------------------------------------------
    // Gathers a depth-driven bokeh disk over the resolved colour.
    if (view.post.dofEnabled && dofReady_) {
        PostCB cb;
        cb.input0 = finalInput;  // resolved colour (TAA output or tonemapped LDR)
        cb.input2 = slotDepth_;  // reconstruct distance for the circle of confusion
        cb.outTexel = sceneTexel;
        cb.inTexel = sceneTexel;
        cb.params0 = {ps.dofFocusDistance, ps.dofFocusRange, ps.dofMaxBlur, 1.0f};
        DrawPostPass(dofPSO_.Get(), dof_.Get(), rtvAt(6 + kBloomMaxMips), sceneW_, sceneH_, cb);
        finalInput = slotDof_;
    }
    GpuMark("dof");

    // --- Motion blur -> motionBlur_ -----------------------------------------
    if (view.post.motionBlurEnabled && motionBlurReady_) {
        PostCB cb;
        cb.input0 = finalInput;
        cb.input3 = slotVelocity_; // per-object motion vectors
        cb.outTexel = sceneTexel;
        cb.inTexel = sceneTexel;
        cb.params0 = {ps.motionBlurIntensity, ps.motionBlurMaxRadius, 1.0f, 0.0f};
        DrawPostPass(motionBlurPSO_.Get(), motionBlur_.Get(), rtvAt(7 + kBloomMaxMips), sceneW_,
                     sceneH_, cb);
        finalInput = slotMotionBlur_;
    }
    GpuMark("mblur");

    // --- FXAA -> final target (viewport texture or swapchain) ---------------
    {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
        D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
#if HBE_EDITOR
        if (viewportReady_) {
            auto toRT = TransitionBarrier(offscreenColor_.Get(),
                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                          D3D12_RESOURCE_STATE_RENDER_TARGET);
            cmdList_->ResourceBarrier(1, &toRT);
            rtv = offscreenRtvHeap_->GetCPUDescriptorHandleForHeapStart();
            dsv = offscreenDsvHeap_->GetCPUDescriptorHandleForHeapStart();
        } else
#endif
        {
            rtv = currentRtv_; // back buffer, already in RENDER_TARGET
            dsv = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        }
        cmdList_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<f32>(sceneW_), static_cast<f32>(sceneH_),
                          0.0f, 1.0f};
        D3D12_RECT scissor{0, 0, static_cast<LONG>(sceneW_), static_cast<LONG>(sceneH_)};
        cmdList_->RSSetViewports(1, &vp);
        cmdList_->RSSetScissorRects(1, &scissor);

        PostCB cb;
        cb.input0 = finalInput; // TAA output when enabled, else the tonemapped LDR
        cb.outTexel = sceneTexel;
        cb.inTexel = sceneTexel;
        cb.params0 = {ps.fxaaEnabled ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
        D3D12_GPU_VIRTUAL_ADDRESS addr = 0;
        bool fxaaConstantsOk = false;
        if (void* dst = AllocConstants(sizeof(PostCB), addr)) {
            std::memcpy(dst, &cb, sizeof(cb));
            cmdList_->SetGraphicsRootConstantBufferView(1, addr);
            fxaaConstantsOk = true;
        } else {
            ++constantStarvedPasses_;
        }
        if (!fxaaConstantsOk) return;
        cmdList_->SetPipelineState(fxaaPSO_.Get());
        cmdList_->DrawInstanced(3, 1, 0, 0);
        // The final target stays bound: the UI overlay and (in viewport mode)
        // RenderUI's RT->SRV transition both expect exactly this state.
    }
    GpuMark("fxaa");
}

void D3D12Device::DrawShadowPass(const SceneView& view, const DrawItem* items, u32 count) {
    shadowPassRun_ = false;
    if (!shadowReady_ || !view.shadowsEnabled || !items || count == 0) return;

    if (shadowInSrvState_) {
        auto toDepth = TransitionBarrier(shadowMap_.Get(),
                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                         D3D12_RESOURCE_STATE_DEPTH_WRITE);
        cmdList_->ResourceBarrier(1, &toDepth);
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = shadowDsvHeap_->GetCPUDescriptorHandleForHeapStart();
    cmdList_->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
    cmdList_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    cmdList_->SetGraphicsRootSignature(meshRootSig_.Get());
    cmdList_->SetPipelineState(shadowPSO_.Get());
    cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Object constants are identical for every cascade: allocate them once and
    // reuse the GPU addresses, so cascades don't multiply arena consumption.
    // Skinned items upload their joint palette here too (reused by DrawScene).
    cmdList_->SetGraphicsRootShaderResourceView(
        3, boneArenas_[frameIndex_]->GetGPUVirtualAddress());
    cmdList_->SetGraphicsRootShaderResourceView(
        4, instanceArenas_[frameIndex_]->GetGPUVirtualAddress());
    shadowObjAddrs_.clear();
    shadowObjAddrs_.resize(count, 0);
    shadowBoneOffsets_.clear();
    shadowBoneOffsets_.resize(count, UINT32_MAX); // MAX = not uploaded
    shadowInstanceCounts_.clear();
    shadowInstanceCounts_.resize(count, 1);
    for (u32 i = 0; i < count; ++i) {
        const DrawItem& it = items[i];
        if (it.instanceRun == 0) continue; // consumed by a run head (no CB = skipped)
        if (!it.mesh.IsValid() || it.mesh.id > meshes_.size()) continue;
        if (it.materialFlags & MaterialFlag_NoShadow) continue; // doesn't cast a shadow
        D3D12_GPU_VIRTUAL_ADDRESS objAddr = 0;
        void* shadowDst = AllocConstants(sizeof(ObjectCB), objAddr);
        // A shadow caster that cannot get constants must be DROPPED, not drawn with the
        // previous caster's transform - that projects a shadow from the wrong place.
        if (!shadowDst) continue;
        {
            void* dst = shadowDst;
            // COMPLETE constants, not just the transform. The depth-only pass reads almost
            // none of this - but filling it here lets DrawScene REUSE this allocation
            // instead of making a second one, which is what removes the 2:1 arena cost on
            // every object that both casts a shadow and is visible.
            ObjectCB ocb{};
            FillObjectMaterial(ocb, it);
            if (it.bones && it.boneCount > 0) {
                bool ok = false;
                const u32 offset = AllocBones(it.bones, it.boneCount, ok);
                if (ok) {
                    ocb.skinned = 1;
                    ocb.boneOffset = offset;
                    ocb.boneCount = it.boneCount;
                    shadowBoneOffsets_[i] = offset;
                }
            }
            // Previous pose, for skinned motion vectors. The shadow pass does not need it,
            // but the scene pass reading these same constants does - and uploading it here
            // is what lets that reuse happen at all.
            ocb.prevBoneOffset = ocb.boneOffset;
            if (ocb.skinned && it.prevBones) {
                bool pok = false;
                const u32 prevOff = AllocBones(it.prevBones, it.boneCount, pok);
                if (pok) ocb.prevBoneOffset = prevOff;
            }
            // Instanced run head: upload the whole run once, with the REAL normal and
            // previous matrices. The depth pass ignores both, but writing model into all
            // three slots (as this used to) would have made the run unusable by the scene
            // pass - and it costs exactly the same three matrices either way.
            if (it.instanceRun > 1) {
                bool iok = false;
                u32 base = 0;
                if (glm::mat4* inst = AllocInstances(it.instanceRun, base, iok); iok) {
                    for (u32 k = 0; k < it.instanceRun; ++k) {
                        const DrawItem& run = items[i + k];
                        inst[k * 3 + 0] = run.transform;
                        inst[k * 3 + 1] = glm::mat4(
                            glm::transpose(glm::inverse(glm::mat3(run.transform))));
                        inst[k * 3 + 2] = run.prevTransform;
                    }
                    ocb.instanced = 1;
                    ocb.instanceBase = base;
                    shadowInstanceCounts_[i] = it.instanceRun;
                }
            }
            std::memcpy(dst, &ocb, sizeof(ocb));
            shadowObjAddrs_[i] = objAddr;
        }
    }

    // One depth-only pass per cascade into its 2x2-atlas tile. The reused
    // MeshPBR vertex shader sees viewProj = that cascade's light matrix.
    const u32 cascadeCount = std::min(view.cascadeCount, kMaxShadowCascades);
    for (u32 c = 0; c < cascadeCount; ++c) {
        const f32 tx = static_cast<f32>((c & 1) * kShadowTileDim);
        const f32 ty = static_cast<f32>((c >> 1) * kShadowTileDim);
        D3D12_VIEWPORT vp{tx, ty, static_cast<f32>(kShadowTileDim),
                          static_cast<f32>(kShadowTileDim), 0.0f, 1.0f};
        D3D12_RECT scissor{static_cast<LONG>(tx), static_cast<LONG>(ty),
                           static_cast<LONG>(tx) + static_cast<LONG>(kShadowTileDim),
                           static_cast<LONG>(ty) + static_cast<LONG>(kShadowTileDim)};
        cmdList_->RSSetViewports(1, &vp);
        cmdList_->RSSetScissorRects(1, &scissor);

        D3D12_GPU_VIRTUAL_ADDRESS frameAddr = 0;
        void* cascadeDst = AllocConstants(sizeof(FrameCB), frameAddr);
        if (!cascadeDst) {
            // Without this cascade's light matrix the pass would render it from the
            // PREVIOUS cascade's viewpoint, writing a plausible-looking but wrong shadow
            // map. An absent cascade is recoverable; a wrong one is not.
            ++constantStarvedPasses_;
            continue;
        }
        {
            void* dst = cascadeDst;
            FrameCB fcb{};
            fcb.viewProj = view.cascadeViewProj[c];
            std::memcpy(dst, &fcb, sizeof(fcb));
            cmdList_->SetGraphicsRootConstantBufferView(0, frameAddr);
        }

        const u8 cascadeBit = static_cast<u8>(1u << c);
        u32 lastMesh = 0; // consecutive same-mesh draws skip the IA rebinds
        for (u32 i = 0; i < count; ++i) {
            const DrawItem& it = items[i];
            if (!shadowObjAddrs_[i]) continue; // no CB = NoShadow or run-consumed
            // Per-cascade culling: this caster cannot affect this cascade's slice.
            // The object CB above is still uploaded once and shared, so skipping
            // here costs nothing and removes the draw entirely.
            if (!(it.cascadeMask & cascadeBit)) continue;
            const GpuMesh& gm = meshes_[it.mesh.id - 1];
            cmdList_->SetGraphicsRootConstantBufferView(1, shadowObjAddrs_[i]);
            if (it.mesh.id != lastMesh) {
                cmdList_->IASetVertexBuffers(0, 1, &gm.vbv);
                cmdList_->IASetIndexBuffer(&gm.ibv);
                lastMesh = it.mesh.id;
            }
            cmdList_->DrawIndexedInstanced(gm.indexCount, shadowInstanceCounts_[i], 0, 0, 0);
        }
    }

    auto toSrv = TransitionBarrier(shadowMap_.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList_->ResourceBarrier(1, &toSrv);
    shadowInSrvState_ = true;
    shadowPassRun_ = true;

    // Restore the main targets for the direct-to-swapchain path (the editor
    // viewport path re-binds its own targets in ClearBackBuffer).
    if (!viewportReady_) {
        D3D12_VIEWPORT mainVp{0.0f, 0.0f, static_cast<f32>(width_), static_cast<f32>(height_),
                              0.0f, 1.0f};
        D3D12_RECT mainScissor{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
        if (dsvHeap_) {
            const D3D12_CPU_DESCRIPTOR_HANDLE mainDsv =
                dsvHeap_->GetCPUDescriptorHandleForHeapStart();
            cmdList_->OMSetRenderTargets(1, &currentRtv_, FALSE, &mainDsv);
        } else {
            cmdList_->OMSetRenderTargets(1, &currentRtv_, FALSE, nullptr);
        }
        cmdList_->RSSetViewports(1, &mainVp);
        cmdList_->RSSetScissorRects(1, &mainScissor);
    }
}

void* D3D12Device::AllocConstants(u64 size, D3D12_GPU_VIRTUAL_ADDRESS& outGpuAddr) {
    const u64 aligned = AlignUp(constantHead_, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    if (aligned + size > kConstantArenaSize) {
        outGpuAddr = 0;
        return nullptr; // arena exhausted this frame
    }
    constantHead_ = aligned + size;
    outGpuAddr = constantArenas_[frameIndex_]->GetGPUVirtualAddress() + aligned;
    return constantCpu_[frameIndex_] + aligned;
}

// Move any async uploads whose GPU work has completed back to the free pool,
// releasing their staging buffers. Cheap (one fence read); safe to call any frame.
void D3D12Device::RetireAsyncUploads() {
    if (!uploadFence_) return;
    const u64 done = uploadFence_->GetCompletedValue();
    for (usize i = 0; i < asyncUploadInFlight_.size();) {
        if (asyncUploadInFlight_[i].fence <= done) {
            asyncUploadInFlight_[i].staging.clear(); // GPU is done reading these
            asyncUploadFree_.push_back(std::move(asyncUploadInFlight_[i]));
            asyncUploadInFlight_[i] = std::move(asyncUploadInFlight_.back());
            asyncUploadInFlight_.pop_back();
        } else {
            ++i;
        }
    }
}

// Acquire a fresh, OPEN command list for a non-blocking upload. Reuses a retired
// batch when one is free (its allocator's GPU work is complete, so Reset is legal),
// else allocates a new one. The pool self-limits to the in-flight upload depth.
D3D12Device::UploadBatch D3D12Device::BeginAsyncUpload() {
    RetireAsyncUploads();
    UploadBatch b;
    if (!asyncUploadFree_.empty()) {
        b = std::move(asyncUploadFree_.back());
        asyncUploadFree_.pop_back();
        b.staging.clear();
        b.alloc->Reset();
        b.list->Reset(b.alloc.Get(), nullptr);
    } else {
        device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        IID_PPV_ARGS(&b.alloc));
        device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, b.alloc.Get(),
                                   nullptr, IID_PPV_ARGS(&b.list)); // starts OPEN
    }
    return b;
}

// Close the caller-recorded list, submit it on the graphics queue, and defer the
// batch (staging + allocator) for retirement once the upload fence passes. Does NOT
// block. The resource is ready when first sampled because the recorder ends the list
// with the COPY_DEST -> read-state transition barrier (which flushes the copy and
// establishes the dependency), and the list is executed before any later draw list on
// the same queue - exactly as the old blocking path did, minus the CPU stall.
void D3D12Device::EndAsyncUpload(UploadBatch&& b) {
    b.list->Close();
    ID3D12CommandList* lists[] = {b.list.Get()};
    queue_->ExecuteCommandLists(1, lists);
    b.fence = ++uploadFenceValue_;
    queue_->Signal(uploadFence_.Get(), b.fence);
    asyncUploadInFlight_.push_back(std::move(b));
}

MeshHandle D3D12Device::CreateMesh(const hbe::MeshData& mesh) {
    if (mesh.Empty()) return {};

    GpuMesh gm;
    const u64 vbSize = static_cast<u64>(mesh.vertices.size()) * sizeof(hbe::Vertex);
    const u64 ibSize = static_cast<u64>(mesh.indices.size()) * sizeof(u32);
    // A pending CreateMeshReserved widens the ALLOCATION without changing what is
    // uploaded or what the views describe.
    const u64 vbAlloc = std::max(vbSize, reserveVertices_ * sizeof(hbe::Vertex));
    const u64 ibAlloc = std::max(ibSize, reserveIndices_ * sizeof(u32));

    // Device-local (VRAM) vertex/index buffers so the GPU fetches geometry from
    // VRAM (~450 GB/s) instead of host memory over PCIe. Data is uploaded via a
    // temporary staging buffer + a copy.
    gm.vertexBuffer = CreateDefaultBuffer(device_.Get(), vbAlloc, D3D12_RESOURCE_STATE_COPY_DEST);
    gm.indexBuffer  = CreateDefaultBuffer(device_.Get(), ibAlloc, D3D12_RESOURCE_STATE_COPY_DEST);
    ComPtr<ID3D12Resource> vStage = CreateUploadBuffer(device_.Get(), vbSize);
    ComPtr<ID3D12Resource> iStage = CreateUploadBuffer(device_.Get(), ibSize);
    if (!gm.vertexBuffer || !gm.indexBuffer || !vStage || !iStage) {
        HBE_ERROR("[D3D12] Failed to allocate GPU buffers for mesh '{}'", mesh.name);
        return {};
    }

    D3D12_RANGE noRead{0, 0};
    void* p = nullptr;
    vStage->Map(0, &noRead, &p);
    std::memcpy(p, mesh.vertices.data(), vbSize);
    vStage->Unmap(0, nullptr);
    iStage->Map(0, &noRead, &p);
    std::memcpy(p, mesh.indices.data(), ibSize);
    iStage->Unmap(0, nullptr);

    // Async upload: record the copies + state transitions, submit on the graphics
    // queue, and return WITHOUT blocking. The staging buffers ride the batch until the
    // GPU passes the fence, so there is no per-mesh round-trip stall while streaming.
    UploadBatch up = BeginAsyncUpload();
    up.list->CopyBufferRegion(gm.vertexBuffer.Get(), 0, vStage.Get(), 0, vbSize);
    up.list->CopyBufferRegion(gm.indexBuffer.Get(), 0, iStage.Get(), 0, ibSize);
    D3D12_RESOURCE_BARRIER barriers[2] = {
        TransitionBarrier(gm.vertexBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                          D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER),
        TransitionBarrier(gm.indexBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                          D3D12_RESOURCE_STATE_INDEX_BUFFER),
    };
    up.list->ResourceBarrier(2, barriers);
    up.staging.push_back(std::move(vStage));
    up.staging.push_back(std::move(iStage));
    EndAsyncUpload(std::move(up));

    gm.vbv.BufferLocation = gm.vertexBuffer->GetGPUVirtualAddress();
    gm.vbv.SizeInBytes = static_cast<UINT>(vbSize);
    gm.vbv.StrideInBytes = sizeof(hbe::Vertex);
    gm.ibv.BufferLocation = gm.indexBuffer->GetGPUVirtualAddress();
    gm.ibv.SizeInBytes = static_cast<UINT>(ibSize);
    gm.ibv.Format = DXGI_FORMAT_R32_UINT;
    // Record the true allocation size once, at creation. UpdateMesh checks
    // against THIS, never the view size (which tracks the live contents).
    // ALLOCATED, not uploaded. Recording the upload size here made CreateMeshReserved's
    // headroom unreachable: the buffers really were allocated larger, but UpdateMesh tests
    // against these fields, so every growth was refused on the default backend while the
    // same call succeeded on Vulkan (which records the allocation). A reserved mesh that
    // cannot grow is worse than no reservation at all - it looks like it worked.
    gm.vbCapacity = vbAlloc;
    gm.ibCapacity = ibAlloc;
    gm.indexCount = mesh.IndexCount();
    // Reuse a meshes_ slot reclaimed by DestroyMesh before growing the vector, so a long
    // streaming session recycles indices instead of leaking one per despawned mesh.
    if (!meshFree_.empty()) {
        const u32 idx = meshFree_.back();
        meshFree_.pop_back();
        meshes_[idx] = std::move(gm);
        return MeshHandle{idx + 1}; // 1-based id
    }
    meshes_.push_back(std::move(gm));
    return MeshHandle{static_cast<u32>(meshes_.size())}; // 1-based id
}

bool D3D12Device::UpdateMesh(MeshHandle handle, const hbe::MeshData& mesh) {
    if (!handle.IsValid() || handle.id > meshes_.size() || mesh.Empty()) return false;
    GpuMesh& gm = meshes_[handle.id - 1];
    const u64 vbSize = static_cast<u64>(mesh.vertices.size()) * sizeof(hbe::Vertex);
    const u64 ibSize = static_cast<u64>(mesh.indices.size()) * sizeof(u32);
    // In-place path needs the new data to fit the ALLOCATION - not the current
    // view size, which shrinks with the contents (see GpuMesh::vbCapacity).
    if (vbSize > gm.vbCapacity || ibSize > gm.ibCapacity) {
        HBE_WARN("[D3D12] UpdateMesh refused: {} vertex + {} index bytes exceed the "
                 "{}/{} reserved for mesh {}. Use CreateMeshReserved.",
                 vbSize, ibSize, gm.vbCapacity, gm.ibCapacity, handle.id);
        return false;
    }

    ComPtr<ID3D12Resource> vStage = CreateUploadBuffer(device_.Get(), vbSize);
    ComPtr<ID3D12Resource> iStage = CreateUploadBuffer(device_.Get(), ibSize);
    if (!vStage || !iStage) return false;
    D3D12_RANGE noRead{0, 0};
    void* p = nullptr;
    vStage->Map(0, &noRead, &p);
    std::memcpy(p, mesh.vertices.data(), vbSize);
    vStage->Unmap(0, nullptr);
    iStage->Map(0, &noRead, &p);
    std::memcpy(p, mesh.indices.data(), ibSize);
    iStage->Unmap(0, nullptr);

    uploadAlloc_->Reset();
    uploadList_->Reset(uploadAlloc_.Get(), nullptr);
    D3D12_RESOURCE_BARRIER toCopy[2] = {
        TransitionBarrier(gm.vertexBuffer.Get(),
                          D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                          D3D12_RESOURCE_STATE_COPY_DEST),
        TransitionBarrier(gm.indexBuffer.Get(), D3D12_RESOURCE_STATE_INDEX_BUFFER,
                          D3D12_RESOURCE_STATE_COPY_DEST),
    };
    uploadList_->ResourceBarrier(2, toCopy);
    uploadList_->CopyBufferRegion(gm.vertexBuffer.Get(), 0, vStage.Get(), 0, vbSize);
    uploadList_->CopyBufferRegion(gm.indexBuffer.Get(), 0, iStage.Get(), 0, ibSize);
    D3D12_RESOURCE_BARRIER toRead[2] = {
        TransitionBarrier(gm.vertexBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                          D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER),
        TransitionBarrier(gm.indexBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                          D3D12_RESOURCE_STATE_INDEX_BUFFER),
    };
    uploadList_->ResourceBarrier(2, toRead);
    uploadList_->Close();
    ID3D12CommandList* lists[] = {uploadList_.Get()};
    queue_->ExecuteCommandLists(1, lists); // same queue as draws -> serialized
    const u64 fv = ++uploadFenceValue_;
    queue_->Signal(uploadFence_.Get(), fv);
    if (uploadFence_->GetCompletedValue() < fv) {
        uploadFence_->SetEventOnCompletion(fv, uploadEvent_);
        ::WaitForSingleObjectEx(uploadEvent_, INFINITE, FALSE);
    }
    gm.vbv.SizeInBytes = static_cast<UINT>(vbSize);
    gm.ibv.SizeInBytes = static_cast<UINT>(ibSize);
    gm.indexCount = mesh.IndexCount();
    return true;
}

MeshHandle D3D12Device::CreateMeshReserved(const hbe::MeshData& initial, u32 vertexCapacity,
                                           u32 indexCapacity) {
    // Allocate for the larger of "what we were handed" and "what was asked for", then
    // record THAT as the capacity. CreateMesh already stores capacity separately from
    // the view size, so nothing in UpdateMesh has to change for this to work.
    reserveVertices_ = std::max<u64>(vertexCapacity, initial.vertices.size());
    reserveIndices_ = std::max<u64>(indexCapacity, initial.indices.size());
    const MeshHandle h = CreateMesh(initial);
    reserveVertices_ = reserveIndices_ = 0;
    return h;
}

void D3D12Device::SetVolumeGrid(const void* density, usize densitySize,
                                const void* temperature, usize tempSize,
                                const VolumeRenderParams& params) {
    // Store the pointers + sizes; the upload + SRVs happen in RunPostStack (deferred to the post pass
    // so the bindless slots exist). Pointers must stay valid through the frame.
    volGridBytes_ = (density && densitySize > 0) ? density : nullptr;
    volGridSize_ = volGridBytes_ ? densitySize : 0u;
    volTempBytes_ = (temperature && tempSize > 0) ? temperature : nullptr;
    volTempSize_ = volTempBytes_ ? tempSize : 0u;
    volRenderParams_ = params;
}

// (Re)allocate this frame's NanoVDB upload buffer to hold `size` bytes and (re)create its
// StructuredBuffer<uint> SRV in the bindless heap. Grows on demand; the SRV covers the whole
// capacity (PNanoVDB only reads addresses within the grid, so trailing slack is never touched).
bool D3D12Device::EnsureVolumeGridBuffer(usize size) {
    if (slotHdr_ == 0) return false; // post targets (and thus the bindless slots) not set up
    if (volGridBuffer_[frameIndex_] && volGridCapacity_[frameIndex_] >= size) return true;
    const u64 cap = (static_cast<u64>(size) + 0xFFFFFull) & ~0xFFFFFull; // round up to 1 MiB
    volGridBuffer_[frameIndex_] = CreateUploadBuffer(device_.Get(), cap);
    if (!volGridBuffer_[frameIndex_]) return false;
    void* m = nullptr;
    D3D12_RANGE noRead{0, 0};
    volGridBuffer_[frameIndex_]->Map(0, &noRead, &m);
    volGridCpu_[frameIndex_] = static_cast<u8*>(m);
    volGridCapacity_[frameIndex_] = cap;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_UNKNOWN;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.FirstElement = 0;
    srv.Buffer.NumElements = static_cast<UINT>(cap / 4u);
    srv.Buffer.StructureByteStride = 4u; // StructuredBuffer<uint>
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    D3D12_CPU_DESCRIPTOR_HANDLE h = bindlessHeap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slotVolGrid_[frameIndex_]) * bindlessDescSize_;
    device_->CreateShaderResourceView(volGridBuffer_[frameIndex_].Get(), &srv, h);
    return true;
}

// Twin of EnsureVolumeGridBuffer for the TEMPERATURE grid (its own per-frame buffer + SRV slot).
bool D3D12Device::EnsureVolumeTempBuffer(usize size) {
    if (slotHdr_ == 0) return false;
    if (volTempBuffer_[frameIndex_] && volTempCapacity_[frameIndex_] >= size) return true;
    const u64 cap = (static_cast<u64>(size) + 0xFFFFFull) & ~0xFFFFFull; // round up to 1 MiB
    volTempBuffer_[frameIndex_] = CreateUploadBuffer(device_.Get(), cap);
    if (!volTempBuffer_[frameIndex_]) return false;
    void* m = nullptr;
    D3D12_RANGE noRead{0, 0};
    volTempBuffer_[frameIndex_]->Map(0, &noRead, &m);
    volTempCpu_[frameIndex_] = static_cast<u8*>(m);
    volTempCapacity_[frameIndex_] = cap;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_UNKNOWN;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.FirstElement = 0;
    srv.Buffer.NumElements = static_cast<UINT>(cap / 4u);
    srv.Buffer.StructureByteStride = 4u; // StructuredBuffer<uint>
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    D3D12_CPU_DESCRIPTOR_HANDLE h = bindlessHeap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slotVolTemp_[frameIndex_]) * bindlessDescSize_;
    device_->CreateShaderResourceView(volTempBuffer_[frameIndex_].Get(), &srv, h);
    return true;
}

// ---------------------------------------------------------------------------
// General GPU compute + GPU-writable structured buffers
// ---------------------------------------------------------------------------

D3D12Device::GpuBufferD3D12* D3D12Device::ResolveGpuBuffer(GpuBufferHandle h) {
    if (!h.IsValid() || h.id > gpuBuffers_.size()) return nullptr;
    GpuBufferD3D12& b = gpuBuffers_[h.id - 1];
    return b.alive ? &b : nullptr;
}

void D3D12Device::TransitionGpuBuffer(GpuBufferD3D12& b, u32 slot,
                                      D3D12_RESOURCE_STATES to) {
    // UPLOAD-heap buffers are permanently GENERIC_READ - transitioning one is
    // invalid, so CpuWrite buffers are simply never transitioned.
    if (b.usage & GpuBufferUsage::CpuWrite) return;
    if (b.state[slot] == to || !b.res[slot]) return;
    auto bar = TransitionBarrier(b.res[slot].Get(), b.state[slot], to);
    cmdList_->ResourceBarrier(1, &bar);
    b.state[slot] = to;
}

GpuBufferHandle D3D12Device::CreateGpuBuffer(const GpuBufferDesc& desc) {
    if (desc.elementCount == 0 || desc.elementStride == 0) {
        HBE_ERROR("[D3D12] CreateGpuBuffer: zero elementCount/elementStride.");
        return {};
    }
    if ((desc.usage & GpuBufferUsage::ShaderWrite) && (desc.usage & GpuBufferUsage::CpuWrite)) {
        // D3D12 forbids ALLOW_UNORDERED_ACCESS on the UPLOAD heap; a GPU-written
        // buffer must be device-local. Rejected on both backends identically.
        HBE_ERROR("[D3D12] CreateGpuBuffer: ShaderWrite|CpuWrite is not a legal combination.");
        return {};
    }
    GpuBufferD3D12 b{};
    b.stride = desc.elementStride;
    b.count = desc.elementCount;
    b.usage = desc.usage;
    b.bytes = static_cast<u64>(desc.elementCount) * desc.elementStride;
    b.maxBindElements = std::min(desc.maxBindElements, desc.elementCount);
    b.slots = (desc.usage & GpuBufferUsage::CpuWrite) ? backBufferCount_ : 1u;
    b.alive = true;

    for (u32 i = 0; i < b.slots; ++i) {
        if (desc.usage & GpuBufferUsage::CpuWrite) {
            b.res[i] = CreateUploadBuffer(device_.Get(), b.bytes);
            if (!b.res[i]) { HBE_ERROR("[D3D12] CreateGpuBuffer: upload alloc failed."); return {}; }
            void* m = nullptr;
            D3D12_RANGE noRead{0, 0};
            b.res[i]->Map(0, &noRead, &m);
            b.cpu[i] = static_cast<u8*>(m);
            b.state[i] = D3D12_RESOURCE_STATE_GENERIC_READ;
        } else {
            const D3D12_RESOURCE_FLAGS flags =
                (desc.usage & GpuBufferUsage::ShaderWrite)
                    ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                    : D3D12_RESOURCE_FLAG_NONE;
            b.res[i] = CreateDefaultBuffer(device_.Get(), b.bytes,
                                           D3D12_RESOURCE_STATE_COMMON, flags);
            if (!b.res[i]) { HBE_ERROR("[D3D12] CreateGpuBuffer: default alloc failed."); return {}; }
            b.state[i] = D3D12_RESOURCE_STATE_COMMON;
        }
        if (desc.debugName) {
            const std::wstring wn(desc.debugName, desc.debugName + std::strlen(desc.debugName));
            b.res[i]->SetName(wn.c_str());
        }
    }

    u32 index;
    if (!gpuBufferFree_.empty()) {
        index = gpuBufferFree_.back();
        gpuBufferFree_.pop_back();
        gpuBuffers_[index] = std::move(b);
    } else {
        index = static_cast<u32>(gpuBuffers_.size());
        gpuBuffers_.push_back(std::move(b));
    }
    return GpuBufferHandle{index + 1};
}

void* D3D12Device::MapGpuBuffer(GpuBufferHandle handle) {
    GpuBufferD3D12* b = ResolveGpuBuffer(handle);
    if (!b || !(b->usage & GpuBufferUsage::CpuWrite)) return nullptr;
    return b->cpu[frameIndex_ % b->slots];
}

bool D3D12Device::ReadGpuBuffer(GpuBufferHandle handle, void* dst, u32 bytes) {
    GpuBufferD3D12* b = ResolveGpuBuffer(handle);
    if (!b || !dst || bytes == 0 || bytes > b->bytes) return false;
    const u32 slot = frameIndex_ % b->slots;
    if (b->cpu[slot]) { std::memcpy(dst, b->cpu[slot], bytes); return true; }

    // Device-local: flush, copy into a READBACK buffer on the synchronous upload
    // list, wait, map. Debug/validation only (see the RHI comment).
    WaitForGpuIdle();
    const D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_READBACK);
    const D3D12_RESOURCE_DESC rd = BufferDesc(bytes);
    ComPtr<ID3D12Resource> staging;
    if (FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&staging)))) {
        return false;
    }
    uploadAlloc_->Reset();
    uploadList_->Reset(uploadAlloc_.Get(), nullptr);
    const D3D12_RESOURCE_STATES prev = b->state[slot];
    if (prev != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        auto toSrc = TransitionBarrier(b->res[slot].Get(), prev,
                                       D3D12_RESOURCE_STATE_COPY_SOURCE);
        uploadList_->ResourceBarrier(1, &toSrc);
    }
    uploadList_->CopyBufferRegion(staging.Get(), 0, b->res[slot].Get(), 0, bytes);
    if (prev != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        auto back = TransitionBarrier(b->res[slot].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, prev);
        uploadList_->ResourceBarrier(1, &back);
    }
    uploadList_->Close();
    ID3D12CommandList* lists[] = {uploadList_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    const u64 fv = ++uploadFenceValue_;
    queue_->Signal(uploadFence_.Get(), fv);
    if (uploadFence_->GetCompletedValue() < fv) {
        uploadFence_->SetEventOnCompletion(fv, uploadEvent_);
        ::WaitForSingleObjectEx(uploadEvent_, INFINITE, FALSE);
    }
    void* mapped = nullptr;
    D3D12_RANGE readRange{0, bytes};
    if (FAILED(staging->Map(0, &readRange, &mapped)) || !mapped) return false;
    std::memcpy(dst, mapped, bytes);
    D3D12_RANGE noWrite{0, 0};
    staging->Unmap(0, &noWrite);
    return true;
}

void D3D12Device::DestroyGpuBuffer(GpuBufferHandle handle) {
    GpuBufferD3D12* b = ResolveGpuBuffer(handle);
    if (!b) return;
    WaitForGpuIdle(); // may still be referenced by in-flight command lists
    if (vsBuffer_.id == handle.id) vsBuffer_ = {};
    for (u32 g = 0; g < particleGpuGroupCount_; ++g) {
        if (particleGpuGroups_[g].buffer.id == handle.id) particleGpuGroups_[g] = {};
    }
    for (u32 i = 0; i < b->slots; ++i) {
        if (b->cpu[i] && b->res[i]) b->res[i]->Unmap(0, nullptr);
        b->cpu[i] = nullptr;
        b->res[i].Reset();
    }
    b->alive = false;
    gpuBufferFree_.push_back(handle.id - 1);
}

// Runs once per BeginFrame. Advances the frame clock and releases anything whose deferral
// window has passed (no in-flight frame can still reference it), recycling its slot/index.
void D3D12Device::RetireFrees() {
    ++frameCounter_;
    for (usize i = 0; i < pendingTexFree_.size();) {
        if (pendingTexFree_[i].freeFrame <= frameCounter_) {
            texSlotFree_.push_back(pendingTexFree_[i].slot); // recycle the bindless slot
            pendingTexFree_[i] = std::move(pendingTexFree_.back()); // releases the ComPtr -> VRAM freed
            pendingTexFree_.pop_back();
        } else {
            ++i;
        }
    }
    for (usize i = 0; i < pendingMeshFree_.size();) {
        if (pendingMeshFree_[i].freeFrame <= frameCounter_) {
            meshFree_.push_back(pendingMeshFree_[i].meshId); // recycle the meshes_ index
            pendingMeshFree_[i] = std::move(pendingMeshFree_.back()); // releases the GpuMesh
            pendingMeshFree_.pop_back();
        } else {
            ++i;
        }
    }
}

void D3D12Device::DestroyTexture(TextureHandle handle) {
    if (!handle.IsValid()) return;
    const u32 slot = handle.index;
    const auto it = slotTextures_.find(slot);
    if (it == slotTextures_.end()) return; // not a reclaimable slot texture, or already gone
    ID3D12Resource* res = it->second.resource;
    // Detach the owning ComPtr out of the keep-alive vector (swap-erase).
    ComPtr<ID3D12Resource> owned;
    for (usize k = 0; k < textures_.size(); ++k) {
        if (textures_[k].Get() == res) {
            owned = std::move(textures_[k]);
            textures_[k] = std::move(textures_.back());
            textures_.pop_back();
            break;
        }
    }
    slotTextures_.erase(it);
    volumeUav_.erase(slot);
    // Defer: an in-flight frame may still sample it, and the bindless descriptor at `slot`
    // must not be overwritten by a reuse until the GPU is done with the old resource.
    pendingTexFree_.push_back({std::move(owned), slot, frameCounter_ + kMaxBackBuffers + 1});
}

void D3D12Device::DestroyMesh(MeshHandle handle) {
    if (!handle.IsValid() || handle.id == 0 || handle.id > meshes_.size()) return;
    const u32 idx = handle.id - 1;
    GpuMesh& gm = meshes_[idx];
    if (!gm.vertexBuffer && !gm.indexBuffer) return; // already freed
    // Park the whole GpuMesh until no in-flight frame can reference it, then recycle the
    // index. Clearing meshes_[idx] here leaves an empty slot CreateMesh reuses.
    pendingMeshFree_.push_back({std::move(gm), idx, frameCounter_ + kMaxBackBuffers + 1});
    meshes_[idx] = GpuMesh{};
}

ComputePipelineHandle D3D12Device::CreateComputePipeline(const ComputePipelineDesc& desc) {
    if (!desc.shaderName || desc.uavCount > kMaxComputeUavs || desc.srvCount > kMaxComputeSrvs ||
        desc.constantBytes > kMaxComputeConstantBytes) {
        HBE_ERROR("[D3D12] CreateComputePipeline: invalid desc.");
        return {};
    }
    ComputePipelineD3D12 p{};
    p.constantBytes = desc.constantBytes;
    p.uavCount = desc.uavCount;
    p.srvCount = desc.srvCount;

    // Root signature, in the order the binding convention documents:
    // [0] root CBV b0, [1 .. uavCount] root UAVs u0.., then root SRVs t0...
    // Root descriptors (not tables) mean no descriptor heap is needed and each
    // buffer's GPU virtual address can be offset freely - the same trick the
    // bone/instance palettes use on the graphics side.
    D3D12_ROOT_PARAMETER cp[1 + kMaxComputeUavs + kMaxComputeSrvs]{};
    u32 n = 0;
    cp[n].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    cp[n].Descriptor.ShaderRegister = 0; // b0
    cp[n].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    ++n;
    for (u32 i = 0; i < desc.uavCount; ++i, ++n) {
        cp[n].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        cp[n].Descriptor.ShaderRegister = i; // u<i>
        cp[n].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    for (u32 i = 0; i < desc.srvCount; ++i, ++n) {
        cp[n].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        cp[n].Descriptor.ShaderRegister = i; // t<i>
        cp[n].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = n;
    rd.pParameters = cp;
    rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE; // compute: no input assembler
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
        if (err) HBE_ERROR("[D3D12] Compute RootSig ({}): {}", desc.shaderName,
                           static_cast<const char*>(err->GetBufferPointer()));
        return {};
    }
    if (FAILED(device_->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                            IID_PPV_ARGS(&p.rootSig)))) {
        return {};
    }

    const std::string name(desc.shaderName);
    const std::wstring wname(name.begin(), name.end());
    const std::wstring dir = ExecutableDir() + L"shaders\\";
    const std::vector<u8> cs = ReadBinaryFile(dir + wname + L".cs.dxil");
    if (cs.empty()) {
        // The fog/ssgi precedent: a kernel missing from cmake/ShaderCompile.cmake
        // produces no file at all. Fail loudly rather than silently dormant.
        HBE_ERROR("[D3D12] {}.cs.dxil missing - is it registered in cmake/ShaderCompile.cmake?",
                  name);
        return {};
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC cpso{};
    cpso.pRootSignature = p.rootSig.Get();
    cpso.CS = {cs.data(), cs.size()};
    if (FAILED(device_->CreateComputePipelineState(&cpso, IID_PPV_ARGS(&p.pso)))) {
        HBE_ERROR("[D3D12] CreateComputePipelineState failed for {}.", name);
        return {};
    }
    p.alive = true;
    // Reuse a slot freed by DestroyComputePipeline so re-compiling a kernel at runtime (the editor
    // material-graph GPU preview) does not grow the vector unbounded.
    u32 idx;
    if (!computePipeFree_.empty()) {
        idx = computePipeFree_.back();
        computePipeFree_.pop_back();
        computePipes_[idx] = std::move(p);
    } else {
        idx = static_cast<u32>(computePipes_.size());
        computePipes_.push_back(std::move(p));
    }
    HBE_INFO("[D3D12] Compute pipeline '{}' ready ({} UAV, {} SRV, {} B constants).", name,
             desc.uavCount, desc.srvCount, desc.constantBytes);
    return ComputePipelineHandle{idx + 1};
}

void D3D12Device::DestroyComputePipeline(ComputePipelineHandle handle) {
    if (!handle.IsValid() || handle.id > computePipes_.size()) return;
    ComputePipelineD3D12& p = computePipes_[handle.id - 1];
    if (!p.alive) return;
    WaitForGpuIdle(); // the PSO/root sig may still be referenced by in-flight command lists
    p.pso.Reset();
    p.rootSig.Reset();
    p.alive = false;
    computePipeFree_.push_back(handle.id - 1);
}

void D3D12Device::QueueCompute(const ComputeDispatch& d) {
    if (!d.pipeline.IsValid() || d.pipeline.id > computePipes_.size()) return;
    if (computeQueueCount_ >= kMaxQueuedComputeDispatches) {
        HBE_WARN("[D3D12] QueueCompute: more than {} dispatches this frame; dropping.",
                 kMaxQueuedComputeDispatches);
        return;
    }
    QueuedComputeD3D12& q = computeQueue_[computeQueueCount_++];
    q.d = d;
    q.d.constantBytes = std::min(d.constantBytes, kMaxComputeConstantBytes);
    if (d.constants && q.d.constantBytes) std::memcpy(q.constants, d.constants, q.d.constantBytes);
    q.d.constants = nullptr; // the copy above is what the dispatch reads
}

// Runs every dispatch queued since the last frame. Called from BeginFrame, at the
// same point in the frame as the Vulkan twin, so both backends' compute work sits
// before any render pass and their GPU timelines stay comparable.
void D3D12Device::ExecuteQueuedCompute() {
    if (computeQueueCount_ == 0) return;
    for (u32 qi = 0; qi < computeQueueCount_; ++qi) {
        const QueuedComputeD3D12& q = computeQueue_[qi];
        ComputePipelineD3D12& p = computePipes_[q.d.pipeline.id - 1];
        if (!p.alive) continue;

        // Transition every bound buffer BEFORE the root descriptors are set:
        // barriers are queue operations, root descriptors are per-draw state.
        GpuBufferD3D12* uav[kMaxComputeUavs] = {};
        GpuBufferD3D12* srv[kMaxComputeSrvs] = {};
        const u32 uavN = std::min(q.d.uavCount, p.uavCount);
        const u32 srvN = std::min(q.d.srvCount, p.srvCount);
        bool ok = true;
        for (u32 i = 0; i < uavN; ++i) {
            uav[i] = ResolveGpuBuffer(q.d.uavs[i]);
            if (!uav[i]) { ok = false; break; }
            TransitionGpuBuffer(*uav[i], frameIndex_ % uav[i]->slots,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        for (u32 i = 0; ok && i < srvN; ++i) {
            srv[i] = ResolveGpuBuffer(q.d.srvs[i]);
            if (!srv[i]) { ok = false; break; }
            TransitionGpuBuffer(*srv[i], frameIndex_ % srv[i]->slots,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        if (!ok) continue;

        cmdList_->SetComputeRootSignature(p.rootSig.Get());
        cmdList_->SetPipelineState(p.pso.Get());
        u32 rp = 0;
        if (p.constantBytes) {
            D3D12_GPU_VIRTUAL_ADDRESS gpu{};
            void* cpu = AllocConstants(p.constantBytes, gpu);
            if (!cpu) continue; // constant arena full this frame
            // The allocation is sized by the PIPELINE's declared block; the copy is
            // sized by the DISPATCH's. QueueCompute only clamps the latter against
            // kMaxComputeConstantBytes, so a dispatch that passes more than its
            // pipeline declared would run off the end of the frame constant arena.
            std::memcpy(cpu, q.constants, std::min(q.d.constantBytes, p.constantBytes));
            cmdList_->SetComputeRootConstantBufferView(rp, gpu);
        }
        ++rp;
        for (u32 i = 0; i < uavN; ++i, ++rp) {
            const u32 s = frameIndex_ % uav[i]->slots;
            cmdList_->SetComputeRootUnorderedAccessView(
                rp, uav[i]->res[s]->GetGPUVirtualAddress());
        }
        // The root signature reserves p.uavCount UAV params; skip any the caller
        // left unbound so the SRV params stay at their declared indices.
        rp += (p.uavCount - uavN);
        for (u32 i = 0; i < srvN; ++i, ++rp) {
            const u32 s = frameIndex_ % srv[i]->slots;
            cmdList_->SetComputeRootShaderResourceView(
                rp, srv[i]->res[s]->GetGPUVirtualAddress());
        }
        cmdList_->Dispatch(std::max(1u, q.d.groupsX), std::max(1u, q.d.groupsY),
                           std::max(1u, q.d.groupsZ));

        // UAV barriers so a following dispatch (or this frame's draws) observes
        // the writes. The Vulkan twin does this with one VkMemoryBarrier.
        D3D12_RESOURCE_BARRIER ub[kMaxComputeUavs]{};
        u32 ubN = 0;
        for (u32 i = 0; i < uavN; ++i) {
            const u32 s = frameIndex_ % uav[i]->slots;
            ub[ubN].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            ub[ubN].UAV.pResource = uav[i]->res[s].Get();
            ++ubN;
        }
        if (ubN) cmdList_->ResourceBarrier(ubN, ub);
    }
    computeQueueCount_ = 0; // one frame only, like SetParticles
}

void D3D12Device::SetVertexShaderBuffer(GpuBufferHandle handle, u32 firstElement) {
    vsBuffer_ = handle;
    vsBufferFirstElement_ = firstElement;
}

void D3D12Device::DrawScene(const SceneView& view, const DrawItem* items, u32 count) {
    // Every return from here drops this frame's GPU-particle groups. They point into
    // an engine-owned vector that is rebuilt each frame, so carrying one over would
    // dangle - and the group array would fill up and warn-and-drop permanently.
    if (!meshPipelineReady_) {
        ClearGpuParticleGroups();
        return;
    }
    const bool drawSky = (view.skyIndex != 0) && skyPSO_;
    // P6b screen-space refraction: does any transparent item need the opaque scene colour behind it
    // (OpenPBR transmission)? Only then do we pay for the scene-colour copy + bind it; otherwise the
    // pass flow is byte-identical to before, so non-transmissive scenes are completely unaffected.
    bool needSceneColor = false;
    if (items && sceneColorCopy_)
        for (u32 i = 0; i < count; ++i)
            if ((items[i].materialFlags & MaterialFlag_Transparent) &&
                items[i].surface.transmission_weight > 0.0f) {
                needSceneColor = true;
                break;
            }
    // Without the post stack there is nothing to resolve, so an empty scene
    // can skip the pass entirely (legacy behavior).
    if ((count == 0 || !items) && !drawSky && !postReady_) {
        ClearGpuParticleGroups();
        return;
    }

    cmdList_->SetGraphicsRootSignature(meshRootSig_.Get());
    cmdList_->SetPipelineState(meshPSO_[static_cast<size_t>(ShaderVariant::Full)].Get());
    cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Bind the bindless texture heap + table (root param 2) and this frame's
    // joint-palette arena (root param 3).
    ID3D12DescriptorHeap* heaps[] = {bindlessHeap_.Get()};
    cmdList_->SetDescriptorHeaps(1, heaps);
    cmdList_->SetGraphicsRootDescriptorTable(2, bindlessHeap_->GetGPUDescriptorHandleForHeapStart());
    cmdList_->SetGraphicsRootShaderResourceView(
        3, boneArenas_[frameIndex_]->GetGPUVirtualAddress());
    cmdList_->SetGraphicsRootShaderResourceView(
        4, instanceArenas_[frameIndex_]->GetGPUVirtualAddress());

    // General VS-visible structured buffer (root param 6, t2 space1). The
    // per-batch base is folded into the GPU virtual address, NOT into a
    // firstInstance - see the SetVertexShaderBuffer contract in RHI.h.
    // Root param 6 is bound UNCONDITIONALLY: the water VS (Water.hlsl gOceanDisp) statically
    // references it, so a root SRV left unbound is undefined and the debug layer flags it even
    // on the Gerstner path where the uniform-false branch never reads it. Fall back to an
    // always-valid structured buffer (the instance arena) as a never-read placeholder.
    D3D12_GPU_VIRTUAL_ADDRESS vsAddr = instanceArenas_[frameIndex_]->GetGPUVirtualAddress();
    if (GpuBufferD3D12* vb = ResolveGpuBuffer(vsBuffer_)) {
        const u32 s = frameIndex_ % vb->slots;
        const u64 off = static_cast<u64>(vsBufferFirstElement_) * vb->stride;
        if (off < vb->bytes) {
            TransitionGpuBuffer(*vb, s, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            vsAddr = vb->res[s]->GetGPUVirtualAddress() + off;
        }
    }
    cmdList_->SetGraphicsRootShaderResourceView(6, vsAddr);
    vsBuffer_ = {}; // one frame only, like SetParticles

    // Temporal AA: jitter the camera sub-pixel each frame so successive frames
    // sample different positions; the TAA resolve reprojects + accumulates them.
    // Jittering the matrices the device writes keeps this entirely in the backend
    // (the renderer is unaware). gInvViewProj stays consistent with the jittered
    // depth, so SSAO and the TAA reprojection both reconstruct correctly.
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
        jitter[3][0] = jx; // clip-space translation -> NDC offset (scales with w)
        jitter[3][1] = jy;
        curVP = jitter * view.viewProj;
        curInvVP = glm::inverse(curVP);
        taaPrevViewProj_ = curVP; // becomes next frame's reprojection basis (prevVP)
        ++taaFrame_;
    }

    // Per-frame constants (b0); the post passes reuse this allocation.
    D3D12_GPU_VIRTUAL_ADDRESS frameAddr = 0;
    if (void* dst = AllocConstants(sizeof(FrameCB), frameAddr)) {
        FrameCB fcb;
        fcb.viewProj = curVP;
        fcb.cameraPos = view.cameraPos;
        fcb.exposure = view.exposure;
        fcb.lightDir = glm::normalize(-view.light.direction); // shader wants dir TO light
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
        // Depth-graded water reads scene depth via slotDepth_; only valid when the read-only DSV
        // exists (the water loop below re-exposes hdrDepth_ read-only). 0 = skip depth grading.
        fcb.sceneDepthIndex = roDsvValid_ ? slotDepth_ : 0u;
        fcb.sceneColorIndex = (postReady_ && needSceneColor) ? slotSceneColor_ : 0u; // transmission (P6b)
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
        std::memcpy(dst, &fcb, sizeof(fcb));
        cmdList_->SetGraphicsRootConstantBufferView(0, frameAddr);
    }

    u32 lastSceneMesh = 0; // consecutive same-mesh draws skip the IA rebinds
    const auto drawItem = [&](u32 i) {
        const DrawItem& it = items[i];
        if (!it.mesh.IsValid() || it.mesh.id > meshes_.size()) return;
        const GpuMesh& gm = meshes_[it.mesh.id - 1];
        u32 instances = 1; // >1 when this is an instanced run head

        D3D12_GPU_VIRTUAL_ADDRESS objAddr = 0;
        // REUSE THE SHADOW PASS'S CONSTANTS. They are byte-identical to what this pass
        // would write - both come from FillObjectMaterial plus the same bone and instance
        // offsets - and the shadow pass has already paid for the arena slot. Allocating a
        // second one halved the draw ceiling for every object that both casts a shadow and
        // is visible: 768 bytes, twice, for one object.
        //
        // Index alignment is what makes this legal. The renderer hands DrawShadowPass the
        // FULL item list and DrawScene a PREFIX of the same array (Renderer.cpp), so item i
        // is the same object in both. A zero address means the shadow pass skipped it -
        // NoShadow, a run follower, or its own arena failure - and this pass allocates
        // normally, so correctness never depends on the shadow pass having succeeded.
        const bool reuseShadowCb = shadowPassRun_ && i < shadowObjAddrs_.size() &&
                                   shadowObjAddrs_[i] != 0;
        if (reuseShadowCb) {
            objAddr = shadowObjAddrs_[i];
            cmdList_->SetGraphicsRootConstantBufferView(1, objAddr);
            if (i < shadowInstanceCounts_.size()) instances = shadowInstanceCounts_[i];
        } else if (void* dst = AllocConstants(sizeof(ObjectCB), objAddr)) {
            // Zero-init: the skinning fields (skinned/boneOffset/boneCount) are
            // only written for skinned items; uninitialized garbage here made a
            // non-skinned mesh skin against a wild bone offset -> GPU hang.
            ObjectCB ocb{};
            ocb.model = it.transform;
            ocb.normalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(it.transform))));
            ocb.baseColor = it.surface.base_color;
            ocb.metallic = it.surface.base_metalness;
            ocb.roughness = it.surface.specular_roughness;
            ocb.albedoIndex = it.albedoTexture.index;
            ocb.normalIndex = it.normalTexture.index;
            ocb.mrIndex = it.mrTexture.index;
            FillMorphCB(ocb, it); // facial blendshapes (bindless delta atlas)
            ocb.aoIndex = it.aoTexture.index;
            ocb.flags = it.materialFlags;
            ocb.subsurfaceColor = it.surface.subsurface_color;
            ocb.subsurfaceRadius = it.surface.subsurface_radius;
    ocb.lodDither = it.lodDither; // LOD cross-fade stipple (1.0 = opaque; shadow pass ignores it)
            ocb.thicknessIndex = it.thicknessTexture.index;
            ocb.clearcoat = it.surface.coat_weight;
            ocb.clearcoatRoughness = it.surface.coat_roughness;
            ocb.emissiveColor = it.surface.emission_color;
            ocb.emissiveIntensity = it.surface.emission_luminance;
            FillSurfaceMaterialExt(ocb.matExt, it.surface);
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
                // Reuse the palette the shadow pass uploaded this frame.
                u32 offset = (shadowPassRun_ && i < shadowBoneOffsets_.size())
                                 ? shadowBoneOffsets_[i]
                                 : UINT32_MAX;
                if (offset == UINT32_MAX) {
                    bool ok = false;
                    offset = AllocBones(it.bones, it.boneCount, ok);
                    if (!ok) offset = UINT32_MAX;
                }
                if (offset != UINT32_MAX) {
                    ocb.skinned = 1;
                    ocb.boneOffset = offset;
                    ocb.boneCount = it.boneCount;
                }
            }

            // Motion vectors: previous-frame world matrix (camera-only velocity
            // when the renderer hasn't tracked this entity yet, i.e.
            // prevTransform == transform) and the previous joint palette for
            // skinned motion (falls back to the current pose -> no pose motion).
            ocb.prevModel = it.prevTransform;
            ocb.prevBoneOffset = ocb.boneOffset;
            if (ocb.skinned && it.prevBones) {
                bool pok = false;
                const u32 prevOff = AllocBones(it.prevBones, it.boneCount, pok);
                if (pok) ocb.prevBoneOffset = prevOff;
            }

            // Instanced run head: upload the whole run's transforms and draw the
            // group in ONE call (followers were skipped by the loop below).
            if (it.instanceRun > 1) {
                bool iok = false;
                u32 base = 0;
                if (glm::mat4* inst = AllocInstances(it.instanceRun, base, iok); iok) {
                    for (u32 k = 0; k < it.instanceRun; ++k) {
                        const DrawItem& run = items[i + k];
                        inst[k * 3 + 0] = run.transform;
                        inst[k * 3 + 1] = glm::mat4(
                            glm::transpose(glm::inverse(glm::mat3(run.transform))));
                        inst[k * 3 + 2] = run.prevTransform;
                    }
                    ocb.instanced = 1;
                    ocb.instanceBase = base;
                    instances = it.instanceRun;
                }
            }

            std::memcpy(dst, &ocb, sizeof(ocb));
            cmdList_->SetGraphicsRootConstantBufferView(1, objAddr);
        } else {
            // THE ARENA IS FULL. Falling through here left the ROOT CBV pointing at the
            // PREVIOUS item's constants, so this mesh drew with another object's transform
            // and material - geometry teleported and took the wrong textures with it, with
            // nothing logged. Vulkan already bounds its loop and drops the item; do the
            // same rather than render something that is definitely wrong. (This is the
            // per-item draw LAMBDA, so the early out is a return, not a continue.)
            return;
        }

        if (it.mesh.id != lastSceneMesh) {
            cmdList_->IASetVertexBuffers(0, 1, &gm.vbv);
            cmdList_->IASetIndexBuffer(&gm.ibv);
            lastSceneMesh = it.mesh.id;
        }
        cmdList_->DrawIndexedInstanced(gm.indexCount, instances, 0, 0, 0);
    };

    // Opaque pass (transparent items deferred to the blended pass below; items consumed by an
    // instanced run head are skipped - the head draws them). P3: bind the per-item OpenPBR shader
    // variant PSO, switching only when it changes (material-sorted items keep switches rare).
    ShaderVariant curVariant = ShaderVariant::Count; // != any real variant -> forces the first bind
    bool curFoliage = false;
    for (u32 i = 0; i < count; ++i) {
        if (items[i].instanceRun == 0 || (items[i].materialFlags & MaterialFlag_Transparent))
            continue;
        const bool foliage = (items[i].materialFlags & MaterialFlag_AlphaTest) != 0;
        if (items[i].shaderVariant != curVariant || foliage != curFoliage) {
            curVariant = items[i].shaderVariant;
            curFoliage = foliage;
            const size_t vi = static_cast<size_t>(curVariant);
            cmdList_->SetPipelineState((foliage && meshPSOFoliage_[vi]) ? meshPSOFoliage_[vi].Get()
                                                                       : meshPSO_[vi].Get());
        }
        drawItem(i);
    }

    // GPU-DRIVEN GRASS: opaque, drawn with the opaque geometry (depth-write on, shadow map
    // + FrameConstants still bound) so blades are lit, shadowed, and occlude correctly.
    DrawGrass();

    // Sky background: after opaques so the depth test rejects covered pixels.
    if (drawSky) {
        cmdList_->SetPipelineState(skyPSO_.Get());
        cmdList_->DrawInstanced(3, 1, 0, 0);
    }

    // P6b screen-space refraction: copy the resolved opaque+sky HDR into sceneColorCopy_ so the
    // transparent pass can sample the geometry BEHIND transmissive surfaces. Guarded on needSceneColor
    // so non-transmissive scenes never pay for it or change behaviour. hdrColor_ is a bound RTV, so
    // transition it to COPY_SOURCE for the copy then back, and re-bind the MRTs before the draws.
    if (needSceneColor && postReady_ && sceneColorCopy_) {
        D3D12_RESOURCE_BARRIER pre[2] = {
            TransitionBarrier(hdrColor_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                              D3D12_RESOURCE_STATE_COPY_SOURCE),
            TransitionBarrier(sceneColorCopy_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                              D3D12_RESOURCE_STATE_COPY_DEST),
        };
        cmdList_->ResourceBarrier(2, pre);
        cmdList_->CopyResource(sceneColorCopy_.Get(), hdrColor_.Get());
        D3D12_RESOURCE_BARRIER post[2] = {
            TransitionBarrier(hdrColor_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                              D3D12_RESOURCE_STATE_RENDER_TARGET),
            TransitionBarrier(sceneColorCopy_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        };
        cmdList_->ResourceBarrier(2, post);
        const auto pRtvCopy = [&](u32 index) {
            D3D12_CPU_DESCRIPTOR_HANDLE h = postRtvHeap_->GetCPUDescriptorHandleForHeapStart();
            h.ptr += static_cast<SIZE_T>(index) * postRtvSize_;
            return h;
        };
        const D3D12_CPU_DESCRIPTOR_HANDLE rtvsCopy[3] = {
            pRtvCopy(0), pRtvCopy(11 + kBloomMaxMips), pRtvCopy(12 + kBloomMaxMips)};
        const D3D12_CPU_DESCRIPTOR_HANDLE dsvCopy = postDsvHeap_->GetCPUDescriptorHandleForHeapStart();
        cmdList_->OMSetRenderTargets(3, rtvsCopy, FALSE, &dsvCopy);
    }

    // Transparent pass: alpha-blended, back-to-front, over opaques + sky.
    if (meshPSOTransparent_) {
        std::vector<u32> tlist;
        for (u32 i = 0; i < count; ++i)
            if ((items[i].materialFlags & MaterialFlag_Transparent) &&
                !(items[i].materialFlags & MaterialFlag_Water))
                tlist.push_back(i);
        if (!tlist.empty()) {
            std::sort(tlist.begin(), tlist.end(), [&](u32 a, u32 b) {
                const f32 da = glm::distance(glm::vec3(items[a].transform[3]), view.cameraPos);
                const f32 db = glm::distance(glm::vec3(items[b].transform[3]), view.cameraPos);
                return da > db; // farthest first
            });
            ID3D12PipelineState* cur = meshPSOTransparent_.Get();
            cmdList_->SetPipelineState(cur);
            for (u32 idx : tlist) {
                // Solid transparents (paint strokes) use the depth-writing variant so
                // DoF/TAA keep them sharp where they float off a surface.
                ID3D12PipelineState* want =
                    (meshPSOTransparentDepth_ && (items[idx].materialFlags & MaterialFlag_DepthWrite))
                        ? meshPSOTransparentDepth_.Get()
                        : meshPSOTransparent_.Get();
                if (want != cur) {
                    cmdList_->SetPipelineState(want);
                    cur = want;
                }
                drawItem(idx);
            }
        }
    }

    // Water surfaces: Gerstner-wave reflective water (own PSO), drawn after the transparent
    // meshes. The frame CB already carries the wave/ripple/colour params; each item's b1
    // ObjectCB supplies its plane transform via the shared drawItem path.
    bool depthReadOnlyForWater = false;
    if (waterReady_) {
        bool waterBound = false;
        for (u32 i = 0; i < count; ++i) {
            if (!(items[i].materialFlags & MaterialFlag_Water)) continue;
            if (!waterBound) {
                // First water item: expose hdrDepth_ read-only so the water PS can SAMPLE the
                // scene depth (floor) for depth-graded absorption/foam/soft edges while still
                // depth-TESTING against it. Rebind the 3 HDR RTVs with the READ-ONLY DSV. Depth
                // is restored to DEPTH_WRITE after the particle draw, so RunPostStack's own
                // DEPTH_WRITE->SRV transition (and the frame-start reset) stay untouched.
                if (roDsvValid_) {
                    auto b = TransitionBarrier(
                        hdrDepth_.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                        D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    cmdList_->ResourceBarrier(1, &b);
                    const auto pRtv = [&](u32 index) {
                        D3D12_CPU_DESCRIPTOR_HANDLE h =
                            postRtvHeap_->GetCPUDescriptorHandleForHeapStart();
                        h.ptr += static_cast<SIZE_T>(index) * postRtvSize_;
                        return h;
                    };
                    const D3D12_CPU_DESCRIPTOR_HANDLE rtvs[3] = {
                        pRtv(0), pRtv(11 + kBloomMaxMips), pRtv(12 + kBloomMaxMips)};
                    cmdList_->OMSetRenderTargets(3, rtvs, FALSE, &roDsvHandle_);
                    depthReadOnlyForWater = true;
                }
                cmdList_->SetPipelineState(waterPSO_.Get());
                waterBound = true;
            }
            drawItem(i);
        }
    }

    GpuMark("scene"); // GPU profiler: delta shadow->here = the HDR forward pass (geo+sky+transparent)

    // Particle billboards: depth-tested against the scene (no write), into the HDR
    // colour, alpha first then additive. Root sig / frame CBV / bindless from above
    // stay bound; only the PSO + VB change.
    if (particlePSO_ && (particleAlphaCount_ + particleAddCount_) > 0) {
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
        D3D12_VERTEX_BUFFER_VIEW vbv{};
        vbv.BufferLocation = particleVertexBuffers_[frameIndex_]->GetGPUVirtualAddress();
        vbv.SizeInBytes = (aN + addN) * sizeof(ParticleVertex);
        vbv.StrideInBytes = sizeof(ParticleVertex);
        cmdList_->IASetVertexBuffers(0, 1, &vbv);
        cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (aN) {
            cmdList_->SetPipelineState(particlePSO_.Get());
            cmdList_->DrawInstanced(aN, 1, 0, 0);
        }
        // GPU-expanded ALPHA batches ride between the two CPU draws so the overall
        // order stays "all alpha, then all additive" - the same blend ordering the
        // single-batch CPU path has always produced.
        DrawGpuParticleBatches(false);
        if (addN) {
            cmdList_->SetPipelineState(particlePSOAdd_.Get());
            cmdList_->DrawInstanced(addN, 1, aN, 0);
        }
        DrawGpuParticleBatches(true);
    } else {
        // No CPU billboards this frame, but there may still be GPU-expanded ones.
        cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        DrawGpuParticleBatches(false);
        DrawGpuParticleBatches(true);
    }

    // Effekseer VFX: draws into the SAME HDR target + scene depth (bound above), right after the
    // native billboards, using the frame's view/proj. Effekseer manages its own PSOs/root sig/
    // descriptor heaps inside Begin/EndCommandList; the post stack that follows rebinds everything,
    // so its state changes don't leak. No-op unless the runtime is up and effects are alive.
    if (effekseer_.Available()) effekseer_.Draw(cmdList_.Get(), view.view, view.proj);

    // One frame only, like SetParticles. Cleared here (not in the block above) so a
    // frame with batches but no CPU verts still drops them.
    ClearGpuParticleGroups();

    // GPU profiler: delta scene->here = the particle billboard draws ONLY (alpha + additive).
    // Split out of "scene" so VFX cost is measurable and regression-testable on its own.
    // Vulkan emits the same label at the same point in its own DrawScene, so the two
    // backends' "particles" buckets measure the same work and are comparable.
    GpuMark("particles");
    // Restore hdrDepth_ to DEPTH_WRITE so RunPostStack's DEPTH_WRITE->SRV transition and the
    // frame-start reset stay unchanged - the read-only exposure above was local to water+particles.
    if (depthReadOnlyForWater) {
        auto b = TransitionBarrier(
            hdrDepth_.Get(),
            D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_DEPTH_WRITE);
        cmdList_->ResourceBarrier(1, &b);
    }
    // HDR resolve: SSAO -> bloom -> tonemap -> FXAA into the final target.
    // (Per-pass GpuMarks inside break the post cost down: ssr/ssgi/fog/kuwahara/...)
    if (postReady_) {
        RunPostStack(view);
    }
}

void D3D12Device::SetParticles(const ParticleVertex* alpha, u32 alphaCount,
                               const ParticleVertex* additive, u32 addCount) {
    particleAlpha_ = alpha;
    particleAlphaCount_ = alphaCount;
    particleAdd_ = additive;
    particleAddCount_ = addCount;
}

// APPENDS a group rather than replacing one - see the contract in RHI.h. Vulkan's
// twin is character-for-character the same body.
void D3D12Device::SetGpuParticles(GpuBufferHandle records, const GpuParticleBatch* batches,
                                  u32 count) {
    if (!records.IsValid() || !batches || count == 0) return;
    if (particleGpuGroupCount_ >= kMaxGpuParticleGroups) {
        HBE_WARN("[D3D12] SetGpuParticles: more than {} record buffers this frame; dropping.",
                 kMaxGpuParticleGroups);
        return;
    }
    GpuParticleGroup& g = particleGpuGroups_[particleGpuGroupCount_++];
    g.buffer = records;
    g.batches = batches;
    g.count = count;
}

// One draw per emitter. The per-batch base is folded into root param 6's GPU
// VIRTUAL ADDRESS - never into a firstInstance, which is 0 here as it is on the
// Vulkan side (see the SV_InstanceID / gl_InstanceIndex note in RHI.h). Vulkan's
// twin expresses the identical offset as a dynamic storage-buffer offset on set 2.
void D3D12Device::DrawGpuParticleBatches(bool additive) {
    if (!particleGpuPSO_ || particleGpuGroupCount_ == 0) return;

    bool psoSet = false;
    for (u32 g = 0; g < particleGpuGroupCount_; ++g) {
        const GpuParticleGroup& grp = particleGpuGroups_[g];
        if (!grp.batches || grp.count == 0) continue;
        GpuBufferD3D12* rb = ResolveGpuBuffer(grp.buffer);
        if (!rb || rb->stride == 0) continue;

        const u32 slot = frameIndex_ % rb->slots;
        // The simulation buffer arrives from compute in UNORDERED_ACCESS; this is
        // the transition that makes the VS's read of the very same bytes legal.
        TransitionGpuBuffer(*rb, slot, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const D3D12_GPU_VIRTUAL_ADDRESS base = rb->res[slot]->GetGPUVirtualAddress();

        // Clamp identically to Vulkan, where the batch physically cannot exceed the
        // descriptor's bind window. D3D12 has no such limit, so without this the two
        // backends would draw different particle counts for an oversized batch.
        const u32 maxCount = rb->maxBindElements > kGpuParticleEmitterElements
                                 ? rb->maxBindElements - kGpuParticleEmitterElements
                                 : 0u;
        for (u32 i = 0; i < grp.count; ++i) {
            const GpuParticleBatch& b = grp.batches[i];
            if (b.count == 0 || (b.additive != 0) != additive) continue;
            const u32 count = maxCount ? std::min(b.count, maxCount) : b.count;
            const u64 off = static_cast<u64>(b.recordFirst) * rb->stride;
            // The record block plus its particles must fit; a malformed batch is
            // skipped rather than read out of bounds.
            const u64 need =
                static_cast<u64>(kGpuParticleEmitterElements + count) * rb->stride;
            if (off + need > rb->bytes) continue;
            if (!psoSet) {
                cmdList_->SetPipelineState(additive ? particleGpuPSOAdd_.Get()
                                                    : particleGpuPSO_.Get());
                psoSet = true;
            }
            cmdList_->SetGraphicsRootShaderResourceView(6, base + off);
            cmdList_->DrawInstanced(count * 6u, 1, 0, 0);
        }
    }
}

void D3D12Device::SetGrass(GpuBufferHandle blades, u32 bladeCount) {
    grassBlades_ = blades;
    grassBladeCount_ = bladeCount;
    grassArgs_ = {}; // fixed-count path for this frame
}

void D3D12Device::SetGrassIndirect(GpuBufferHandle blades, GpuBufferHandle args, u32 maxBlades) {
    if (!grassIndirectPSO_ || !grassDrawSig_) return; // no indirect PSO -> caller falls back
    grassBlades_ = blades;
    grassArgs_ = args;
    grassIndirectMax_ = maxBlades;
    grassBladeCount_ = maxBlades; // unused by the indirect draw, but keeps DrawGrass' guard happy
}

// Draws the compute-generated grass field. Two paths share this: the DEFAULT fixed-count path
// binds the blade buffer at root param 6 and issues DrawInstanced(6*N,1,0,0) (culled blades
// collapse to a point in the VS); the opt-in INDIRECT path (grassArgs_ valid) binds the
// COMPACTED buffer and issues ExecuteIndirect against a {6,instanceCount,0,0} arg buffer the
// compaction compute filled - only visible blades reach the VS.
void D3D12Device::DrawGrass() {
    if (!grassBlades_.IsValid()) return;
    GpuBufferD3D12* rb = ResolveGpuBuffer(grassBlades_);
    if (!rb || rb->stride == 0) return;
    const u32 slot = frameIndex_ % rb->slots;
    // The compute pass left the blade buffer in UNORDERED_ACCESS; make the VS read legal.
    TransitionGpuBuffer(*rb, slot, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const D3D12_GPU_VIRTUAL_ADDRESS base = rb->res[slot]->GetGPUVirtualAddress();

    // INDIRECT/COMPACTED path.
    if (grassArgs_.IsValid() && grassIndirectPSO_ && grassDrawSig_) {
        GpuBufferD3D12* ab = ResolveGpuBuffer(grassArgs_);
        if (ab && ab->res[0]) {
            const u32 aslot = frameIndex_ % ab->slots;
            TransitionGpuBuffer(*ab, aslot, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            cmdList_->SetPipelineState(grassIndirectPSO_.Get());
            cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            cmdList_->SetGraphicsRootShaderResourceView(6, base);
            cmdList_->ExecuteIndirect(grassDrawSig_.Get(), 1, ab->res[aslot].Get(), 0, nullptr, 0);
        }
        return;
    }

    if (!grassPSO_ || grassBladeCount_ == 0) return;
    u32 blades = grassBladeCount_;
    if (static_cast<u64>(blades) * rb->stride > rb->bytes)
        blades = static_cast<u32>(rb->bytes / rb->stride);
    if (blades == 0) return;
    cmdList_->SetPipelineState(grassPSO_.Get());
    cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList_->SetGraphicsRootShaderResourceView(6, base);
    cmdList_->DrawInstanced(blades * 6u, 1, 0, 0);
}

void D3D12Device::DrawUIOverlay(const UIVertex* vertices, u32 count) {
    if (!uiPSO_ || !vertices || count == 0) return;
    const u32 maxVerts = static_cast<u32>(kUIVertexBufferSize / sizeof(UIVertex));
    count = std::min(count, maxVerts - maxVerts % 3);
    std::memcpy(uiVertexCpu_[frameIndex_], vertices, count * sizeof(UIVertex));

    cmdList_->SetGraphicsRootSignature(meshRootSig_.Get());
    ID3D12DescriptorHeap* heaps[] = {bindlessHeap_.Get()};
    cmdList_->SetDescriptorHeaps(1, heaps);
    cmdList_->SetGraphicsRootDescriptorTable(
        2, bindlessHeap_->GetGPUDescriptorHandleForHeapStart());
    cmdList_->SetPipelineState(uiPSO_.Get());
    cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    vbv.BufferLocation = uiVertexBuffers_[frameIndex_]->GetGPUVirtualAddress();
    vbv.SizeInBytes = count * sizeof(UIVertex);
    vbv.StrideInBytes = sizeof(UIVertex);
    cmdList_->IASetVertexBuffers(0, 1, &vbv);
    cmdList_->DrawInstanced(count, 1, 0, 0);
}

TextureHandle D3D12Device::CreateUITarget(u32 width, u32 height) {
    if (!uiWorldPSO_ || !bindlessHeap_ || bindlessNextSlot_ >= kMaxBindlessTextures ||
        width == 0 || height == 0)
        return {};
    if (uiTargets_.size() >= kMaxUITargets) {
        HBE_WARN("[D3D12] world-UI target limit ({}) reached.", kMaxUITargets);
        return {};
    }
    // RTV descriptor heap for the targets, created on first use.
    if (!uiTargetRtvHeap_) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = kMaxUITargets;
        if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&uiTargetRtvHeap_))))
            return {};
    }

    // TYPELESS resource: UNORM RTV (the UI shader's raw display-space output) +
    // UNORM_SRGB SRV (hardware sRGB decode when the mesh pass samples the page,
    // exactly like an albedo PNG). Starts (and is tracked) in the SRV state.
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = width;
    td.Height = height;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    const D3D12_HEAP_PROPERTIES def = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> tex;
    if (FAILED(device_->CreateCommittedResource(
            &def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &clear, IID_PPV_ARGS(&tex))))
        return {};

    const u32 slot = bindlessNextSlot_++;
    D3D12_CPU_DESCRIPTOR_HANDLE sh = bindlessHeap_->GetCPUDescriptorHandleForHeapStart();
    sh.ptr += static_cast<SIZE_T>(slot) * bindlessDescSize_;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(tex.Get(), &sv, sh);

    const u32 rtvIndex = static_cast<u32>(uiTargets_.size());
    D3D12_CPU_DESCRIPTOR_HANDLE rh = uiTargetRtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rh.ptr += static_cast<SIZE_T>(rtvIndex) *
              device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_RENDER_TARGET_VIEW_DESC rv{};
    rv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device_->CreateRenderTargetView(tex.Get(), &rv, rh);

    uiTargets_[slot] = UITarget{tex, rtvIndex, width, height, true};
    // slotTextures_ is what GetTextureUIHandle builds ImGui's OWN SRV from, and for
    // a UI target it has no other consumer (UpdateTexture and the volume-UAV paths
    // never see one). Record it UNORM, NOT the bindless SRV's _SRGB: the target
    // holds the UI shader's raw display-space output, and the swapchain the editor
    // presents to is non-sRGB, so an sRGB read would DECODE once with no re-encode
    // and the authoring canvas would render measurably darker than the same
    // document in the Game tab. The bindless SRV above keeps _SRGB - the lit world
    // page IS sampled as an albedo map and must decode.
    slotTextures_[slot] = SlotTexture{tex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 1};
    textures_.push_back(tex);
    HBE_INFO("[D3D12] world-UI target {}x{} (slot {}).", width, height, slot);
    return TextureHandle{slot};
}

void D3D12Device::DrawUIToTexture(TextureHandle target, const UIVertex* vertices, u32 count) {
    if (!uiWorldPSO_) return;
    const auto it = uiTargets_.find(target.index);
    if (it == uiTargets_.end()) return;
    UITarget& t = it->second;

    // Bump-allocate this canvas's verts in the frame's world-UI buffer (several
    // canvases share it; DrawUIOverlay's buffer is NOT reusable - offset-0 memcpy).
    // count == 0 (or a null vertex pointer) still CLEARS the target below - a
    // fresh/hidden page must read transparent, never uninitialized garbage.
    if (!vertices) count = 0;
    const u64 remaining = kUIVertexBufferSize - uiWorldVertexHead_;
    const u32 maxVerts = static_cast<u32>(remaining / sizeof(UIVertex));
    count = std::min(count, maxVerts - maxVerts % 3);
    if (count > 0) {
        std::memcpy(uiWorldVertexCpu_[frameIndex_] + uiWorldVertexHead_, vertices,
                    count * sizeof(UIVertex));
    }

    if (t.inSrvState) {
        auto toRt = TransitionBarrier(t.tex.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                      D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmdList_->ResourceBarrier(1, &toRt);
        t.inSrvState = false;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = uiTargetRtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(t.rtvIndex) *
               device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const f32 clear[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // transparent page background
    cmdList_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cmdList_->ClearRenderTargetView(rtv, clear, 0, nullptr);
    D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<f32>(t.w), static_cast<f32>(t.h), 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(t.w), static_cast<LONG>(t.h)};
    cmdList_->RSSetViewports(1, &vp);
    cmdList_->RSSetScissorRects(1, &scissor);

    if (count > 0) {
        cmdList_->SetGraphicsRootSignature(meshRootSig_.Get());
        ID3D12DescriptorHeap* heaps[] = {bindlessHeap_.Get()};
        cmdList_->SetDescriptorHeaps(1, heaps);
        cmdList_->SetGraphicsRootDescriptorTable(
            2, bindlessHeap_->GetGPUDescriptorHandleForHeapStart());
        cmdList_->SetPipelineState(uiWorldPSO_.Get());
        cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D12_VERTEX_BUFFER_VIEW vbv{};
        vbv.BufferLocation =
            uiWorldVertexBuffers_[frameIndex_]->GetGPUVirtualAddress() + uiWorldVertexHead_;
        vbv.SizeInBytes = count * sizeof(UIVertex);
        vbv.StrideInBytes = sizeof(UIVertex);
        cmdList_->IASetVertexBuffers(0, 1, &vbv);
        cmdList_->DrawInstanced(count, 1, 0, 0);
        uiWorldVertexHead_ += static_cast<u64>(count) * sizeof(UIVertex);
    }

    // Back to SRV: the scene pass samples the page this same frame.
    auto toSrv = TransitionBarrier(t.tex.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList_->ResourceBarrier(1, &toSrv);
    t.inSrvState = true;

    // Restore the main targets for the direct-to-swapchain path (the editor
    // viewport path re-binds its own targets in ClearBackBuffer).
    if (!viewportReady_) {
        D3D12_VIEWPORT mainVp{0.0f, 0.0f, static_cast<f32>(width_), static_cast<f32>(height_),
                              0.0f, 1.0f};
        D3D12_RECT mainScissor{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
        if (dsvHeap_) {
            const D3D12_CPU_DESCRIPTOR_HANDLE mainDsv =
                dsvHeap_->GetCPUDescriptorHandleForHeapStart();
            cmdList_->OMSetRenderTargets(1, &currentRtv_, FALSE, &mainDsv);
        } else {
            cmdList_->OMSetRenderTargets(1, &currentRtv_, FALSE, nullptr);
        }
        cmdList_->RSSetViewports(1, &mainVp);
        cmdList_->RSSetScissorRects(1, &mainScissor);
    }
}

#if HBE_EDITOR
void D3D12Device::ImGuiSrvAlloc(ImGui_ImplDX12_InitInfo* info,
                                D3D12_CPU_DESCRIPTOR_HANDLE* outCpu,
                                D3D12_GPU_DESCRIPTOR_HANDLE* outGpu) {
    auto* self = static_cast<D3D12Device*>(info->UserData);
    const u32 idx = self->imguiSrvFreeList_.back();
    self->imguiSrvFreeList_.pop_back();
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = self->imguiSrvHeap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = self->imguiSrvHeap_->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(idx) * self->imguiSrvDescSize_;
    gpu.ptr += static_cast<u64>(idx) * self->imguiSrvDescSize_;
    *outCpu = cpu;
    *outGpu = gpu;
}

void D3D12Device::ImGuiSrvFree(ImGui_ImplDX12_InitInfo* info,
                               D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                               D3D12_GPU_DESCRIPTOR_HANDLE /*gpu*/) {
    auto* self = static_cast<D3D12Device*>(info->UserData);
    const D3D12_CPU_DESCRIPTOR_HANDLE start = self->imguiSrvHeap_->GetCPUDescriptorHandleForHeapStart();
    const u32 idx = static_cast<u32>((cpu.ptr - start.ptr) / self->imguiSrvDescSize_);
    self->imguiSrvFreeList_.push_back(idx);
}

u64 D3D12Device::GetTextureUIHandle(TextureHandle handle) {
    if (!uiInitialized_ || handle.index == 0) return 0;
    if (auto it = uiTextureIds_.find(handle.index); it != uiTextureIds_.end()) {
        return it->second;
    }
    const auto st = slotTextures_.find(handle.index);
    if (st == slotTextures_.end() || imguiSrvFreeList_.empty()) return 0;

    // ImGui binds its own SRV heap while drawing, so the texture needs a
    // descriptor there (the bindless heap isn't visible to ImGui's pipeline).
    const u32 idx = imguiSrvFreeList_.back();
    imguiSrvFreeList_.pop_back();
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = imguiSrvHeap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = imguiSrvHeap_->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(idx) * imguiSrvDescSize_;
    gpu.ptr += static_cast<u64>(idx) * imguiSrvDescSize_;

    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = st->second.format;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = st->second.mipCount;
    device_->CreateShaderResourceView(st->second.resource, &sv, cpu);

    uiTextureIds_[handle.index] = gpu.ptr;
    return gpu.ptr;
}

bool D3D12Device::InitUI(void* nativeWindowHandle) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // Multi-viewport: panels can be dragged OUT of the main window into their own
    // OS windows (dock freely across monitors). Must be set BEFORE the backends'
    // Init so they install the platform/renderer viewport interfaces.
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.IniFilename = nullptr; // don't persist an imgui.ini
    ImGui::StyleColorsDark();
    // Detached platform windows read as normal OS windows (opaque, un-rounded).
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    if (!ImGui_ImplWin32_Init(nativeWindowHandle)) {
        HBE_ERROR("[D3D12] ImGui_ImplWin32_Init failed");
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 256; // font + viewport + asset-browser thumbnails
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&imguiSrvHeap_)))) {
        HBE_ERROR("[D3D12] ImGui SRV heap creation failed");
        return false;
    }
    imguiSrvDescSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    imguiSrvFreeList_.clear();
    for (u32 i = hd.NumDescriptors; i-- > 0;) imguiSrvFreeList_.push_back(i);

    ImGui_ImplDX12_InitInfo init{};
    init.Device = device_.Get();
    init.CommandQueue = queue_.Get();
    init.NumFramesInFlight = static_cast<int>(backBufferCount_);
    init.RTVFormat = swapFormat_;
    init.DSVFormat = depthFormat_;
    init.SrvDescriptorHeap = imguiSrvHeap_.Get();
    init.SrvDescriptorAllocFn = &ImGuiSrvAlloc;
    init.SrvDescriptorFreeFn = &ImGuiSrvFree;
    init.UserData = this;
    if (!ImGui_ImplDX12_Init(&init)) {
        HBE_ERROR("[D3D12] ImGui_ImplDX12_Init failed");
        return false;
    }
    uiInitialized_ = true;

    // Offscreen viewport target so the editor can display the scene in a panel.
    if (!CreateViewportTarget(width_, height_)) {
        HBE_WARN("[D3D12] Viewport target unavailable; scene renders to the window.");
    }

    HBE_INFO("[D3D12] ImGui initialized.");
    return true;
}

void D3D12Device::BeginUIFrame() {
    if (!uiInitialized_) return;
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void D3D12Device::RenderUI() {
    if (viewportReady_) {
        // Finish the offscreen scene pass, make its color sampleable.
        auto toSRV = TransitionBarrier(offscreenColor_.Get(),
                                       D3D12_RESOURCE_STATE_RENDER_TARGET,
                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList_->ResourceBarrier(1, &toSRV);

        // Begin the swapchain UI pass.
        auto toRT = TransitionBarrier(backBuffers_[frameIndex_].Get(),
                                      D3D12_RESOURCE_STATE_PRESENT,
                                      D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmdList_->ResourceBarrier(1, &toRT);
        D3D12_CPU_DESCRIPTOR_HANDLE scRtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        scRtv.ptr += static_cast<SIZE_T>(frameIndex_) * rtvDescriptorSize_;
        cmdList_->OMSetRenderTargets(1, &scRtv, FALSE, nullptr);
        const f32 dark[4] = {0.05f, 0.05f, 0.06f, 1.0f};
        cmdList_->ClearRenderTargetView(scRtv, dark, 0, nullptr);
        D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<f32>(width_), static_cast<f32>(height_), 0.0f, 1.0f};
        D3D12_RECT scissor{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
        cmdList_->RSSetViewports(1, &vp);
        cmdList_->RSSetScissorRects(1, &scissor);

        if (uiInitialized_) {
            ImGui::Render();
            ID3D12DescriptorHeap* heaps[] = {imguiSrvHeap_.Get()};
            cmdList_->SetDescriptorHeaps(1, heaps);
            ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmdList_.Get());
        }

        auto toPresent = TransitionBarrier(backBuffers_[frameIndex_].Get(),
                                           D3D12_RESOURCE_STATE_RENDER_TARGET,
                                           D3D12_RESOURCE_STATE_PRESENT);
        cmdList_->ResourceBarrier(1, &toPresent);
        return;
    }

    if (!uiInitialized_) return;
    ImGui::Render();
    ID3D12DescriptorHeap* heaps[] = {imguiSrvHeap_.Get()};
    cmdList_->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmdList_.Get());
}

void D3D12Device::ShutdownUI() {
    if (!uiInitialized_) return;
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    uiInitialized_ = false;
}

bool D3D12Device::CreateViewportTarget(u32 w, u32 h) {
    if (w == 0 || h == 0 || !imguiSrvHeap_) return false;
    if (deviceLost_) return false; // nothing can succeed; don't re-log every frame
    const D3D12_HEAP_PROPERTIES def = HeapProps(D3D12_HEAP_TYPE_DEFAULT);

    // Color target (render target + shader resource).
    offscreenColor_.Reset();
    D3D12_RESOURCE_DESC cd{};
    cd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    cd.Width = w; cd.Height = h; cd.DepthOrArraySize = 1; cd.MipLevels = 1;
    cd.Format = swapFormat_; cd.SampleDesc.Count = 1;
    cd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    cd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv{};
    cv.Format = swapFormat_;
    cv.Color[0] = 0.018f; cv.Color[1] = 0.018f; cv.Color[2] = 0.022f; cv.Color[3] = 1.0f;
    HR_CHECK(device_->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &cd,
                                              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv,
                                              IID_PPV_ARGS(&offscreenColor_)),
             "CreateCommittedResource(offscreenColor)");

    if (!offscreenRtvHeap_) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
        HR_CHECK(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&offscreenRtvHeap_)), "OffscreenRtvHeap");
    }
    device_->CreateRenderTargetView(offscreenColor_.Get(), nullptr,
                                    offscreenRtvHeap_->GetCPUDescriptorHandleForHeapStart());

    // Depth target.
    offscreenDepth_.Reset();
    D3D12_RESOURCE_DESC dd{};
    dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    dd.Width = w; dd.Height = h; dd.DepthOrArraySize = 1; dd.MipLevels = 1;
    dd.Format = depthFormat_; dd.SampleDesc.Count = 1;
    dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE dcv{};
    dcv.Format = depthFormat_; dcv.DepthStencil.Depth = 1.0f;
    HR_CHECK(device_->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &dd,
                                              D3D12_RESOURCE_STATE_DEPTH_WRITE, &dcv,
                                              IID_PPV_ARGS(&offscreenDepth_)),
             "CreateCommittedResource(offscreenDepth)");
    if (!offscreenDsvHeap_) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; hd.NumDescriptors = 1;
        HR_CHECK(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&offscreenDsvHeap_)), "OffscreenDsvHeap");
    }
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = depthFormat_; dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device_->CreateDepthStencilView(offscreenDepth_.Get(), &dsv,
                                        offscreenDsvHeap_->GetCPUDescriptorHandleForHeapStart());
    }

    // SRV in ImGui's heap so ImGui::Image can display the color (allocate once).
    if (!offscreenSrvAllocated_) {
        offscreenSrvSlot_ = imguiSrvFreeList_.back();
        imguiSrvFreeList_.pop_back();
        offscreenSrvAllocated_ = true;
        offscreenSrvGpu_ = imguiSrvHeap_->GetGPUDescriptorHandleForHeapStart();
        offscreenSrvGpu_.ptr += static_cast<u64>(offscreenSrvSlot_) * imguiSrvDescSize_;
    }
    {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = imguiSrvHeap_->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(offscreenSrvSlot_) * imguiSrvDescSize_;
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = swapFormat_;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(offscreenColor_.Get(), &sv, cpu);
    }

    vpW_ = w; vpH_ = h;
    viewportReady_ = true;
    return true;
}

bool D3D12Device::ReadbackViewportColor(std::vector<u8>& outRGBA, u32& w, u32& h) {
    if (!viewportReady_ || !offscreenColor_) return false;
    w = vpW_;
    h = vpH_;

    // 256B-aligned row footprint of the offscreen color (subresource 0).
    const D3D12_RESOURCE_DESC rd = offscreenColor_->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT numRows = 0;
    UINT64 rowSize = 0, total = 0;
    device_->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &numRows, &rowSize, &total);

    // A READBACK-heap buffer sized to the footprint (grown as needed, reused). Also
    // reallocate when the 256B-aligned row pitch changes (some resolutions share a
    // total but differ in pitch - reusing would de-pad at the wrong stride).
    if (!readbackBuffer_ || readbackSize_ < total || readbackRowPitch_ != fp.Footprint.RowPitch) {
        readbackBuffer_.Reset();
        const D3D12_HEAP_PROPERTIES rb = HeapProps(D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device_->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&readbackBuffer_))))
            return false;
        readbackSize_ = total;
        readbackRowPitch_ = fp.Footprint.RowPitch;
    }

    // Copy offscreenColor_ -> readback buffer on the shared upload list (serialized
    // after this frame's draws on queue_); blocking wait (offline, slow is fine).
    uploadAlloc_->Reset();
    uploadList_->Reset(uploadAlloc_.Get(), nullptr);
    auto toCopy = TransitionBarrier(offscreenColor_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                    D3D12_RESOURCE_STATE_COPY_SOURCE);
    uploadList_->ResourceBarrier(1, &toCopy);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readbackBuffer_.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = offscreenColor_.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    uploadList_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    auto toSRV = TransitionBarrier(offscreenColor_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    uploadList_->ResourceBarrier(1, &toSRV);
    uploadList_->Close();
    ID3D12CommandList* lists[] = {uploadList_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    const u64 fv = ++uploadFenceValue_;
    queue_->Signal(uploadFence_.Get(), fv);
    if (uploadFence_->GetCompletedValue() < fv) {
        uploadFence_->SetEventOnCompletion(fv, uploadEvent_);
        ::WaitForSingleObjectEx(uploadEvent_, INFINITE, FALSE);
    }

    // Map + de-pad rows (RowPitch is 256B-aligned, != w*4 at odd widths).
    void* mapped = nullptr;
    D3D12_RANGE readRange{0, static_cast<SIZE_T>(total)};
    if (FAILED(readbackBuffer_->Map(0, &readRange, &mapped))) return false;
    outRGBA.resize(static_cast<usize>(w) * h * 4);
    const u8* srcBytes = static_cast<const u8*>(mapped);
    const u64 pitch = fp.Footprint.RowPitch;
    const bool bgra = rd.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                      rd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    for (u32 y = 0; y < h; ++y) {
        const u8* srow = srcBytes + y * pitch;
        u8* drow = outRGBA.data() + static_cast<usize>(y) * w * 4;
        if (bgra) {
            for (u32 x = 0; x < w; ++x) {
                drow[x * 4 + 0] = srow[x * 4 + 2]; // R <- B
                drow[x * 4 + 1] = srow[x * 4 + 1];
                drow[x * 4 + 2] = srow[x * 4 + 0]; // B <- R
                drow[x * 4 + 3] = srow[x * 4 + 3];
            }
        } else {
            std::memcpy(drow, srow, static_cast<usize>(w) * 4); // R8G8B8A8 -> canonical RGBA
        }
    }
    const D3D12_RANGE noWrite{0, 0};
    readbackBuffer_->Unmap(0, &noWrite);
    return true;
}

bool D3D12Device::CreatePreviewTargets(u32 w, u32 h) {
    if (w == 0 || h == 0 || !imguiSrvHeap_ || !postPipelinesReady_) return false;
    const D3D12_HEAP_PROPERTIES def = HeapProps(D3D12_HEAP_TYPE_DEFAULT);

    if (!prevRtvHeap_) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 2;
        HR_CHECK(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&prevRtvHeap_)),
                 "CreateDescriptorHeap(prevRTV)");
    }
    if (!prevDsvHeap_) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hd.NumDescriptors = 1;
        HR_CHECK(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&prevDsvHeap_)),
                 "CreateDescriptorHeap(prevDSV)");
    }
    if (slotPrevHdr_ == 0) {
        if (bindlessNextSlot_ >= kMaxBindlessTextures) return false;
        slotPrevHdr_ = bindlessNextSlot_++;
    }

    const auto makeTarget = [&](DXGI_FORMAT fmt, u32 rtvIndex,
                                ComPtr<ID3D12Resource>& out) -> bool {
        out.Reset();
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = fmt; rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (FAILED(device_->CreateCommittedResource(
                &def, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&out)))) {
            return false;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = prevRtvHeap_->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(rtvIndex) *
                   device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        device_->CreateRenderTargetView(out.Get(), nullptr, rtv);
        return true;
    };

    if (!makeTarget(DXGI_FORMAT_R16G16B16A16_FLOAT, 0, prevHdr_)) return false;
    if (!makeTarget(swapFormat_, 1, prevLdr_)) return false;

    // Bindless SRV over the HDR color (the preview tonemap samples it).
    {
        D3D12_CPU_DESCRIPTOR_HANDLE srv = bindlessHeap_->GetCPUDescriptorHandleForHeapStart();
        srv.ptr += static_cast<SIZE_T>(slotPrevHdr_) * bindlessDescSize_;
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(prevHdr_.Get(), &sv, srv);
    }

    // Depth.
    {
        prevDepth_.Reset();
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = depthFormat_; rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = depthFormat_; cv.DepthStencil.Depth = 1.0f;
        HR_CHECK(device_->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &rd,
                                                  D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                                                  IID_PPV_ARGS(&prevDepth_)),
                 "CreateCommittedResource(prevDepth)");
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = depthFormat_; dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device_->CreateDepthStencilView(prevDepth_.Get(), &dsv,
                                        prevDsvHeap_->GetCPUDescriptorHandleForHeapStart());
    }

    // Stable ImGui SRV over the LDR result.
    if (!prevSrvAllocated_) {
        if (imguiSrvFreeList_.empty()) return false;
        prevSrvSlot_ = imguiSrvFreeList_.back();
        imguiSrvFreeList_.pop_back();
        prevSrvAllocated_ = true;
        prevSrvGpu_ = imguiSrvHeap_->GetGPUDescriptorHandleForHeapStart();
        prevSrvGpu_.ptr += static_cast<u64>(prevSrvSlot_) * imguiSrvDescSize_;
    }
    {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = imguiSrvHeap_->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(prevSrvSlot_) * imguiSrvDescSize_;
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = swapFormat_;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(prevLdr_.Get(), &sv, cpu);
    }

    prevW_ = w;
    prevH_ = h;
    return true;
}

void D3D12Device::DrawPreviewScene(const SceneView& view, const DrawItem* items, u32 count) {
    if (!previewReady_ || !meshPipelineReady_ || !postPipelinesReady_ || !meshPSOSingle_) return;

    // --- HDR mini scene pass -------------------------------------------------
    auto toRT = TransitionBarrier(prevHdr_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                  D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmdList_->ResourceBarrier(1, &toRT);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = prevRtvHeap_->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = prevDsvHeap_->GetCPUDescriptorHandleForHeapStart();
    cmdList_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    const f32 clear[4] = {0.10f, 0.105f, 0.12f, 1.0f};
    cmdList_->ClearRenderTargetView(rtv, clear, 0, nullptr);
    cmdList_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<f32>(prevW_), static_cast<f32>(prevH_),
                      0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(prevW_), static_cast<LONG>(prevH_)};
    cmdList_->RSSetViewports(1, &vp);
    cmdList_->RSSetScissorRects(1, &scissor);

    cmdList_->SetGraphicsRootSignature(meshRootSig_.Get());
    ID3D12DescriptorHeap* heaps[] = {bindlessHeap_.Get()};
    cmdList_->SetDescriptorHeaps(1, heaps);
    cmdList_->SetGraphicsRootDescriptorTable(2,
                                             bindlessHeap_->GetGPUDescriptorHandleForHeapStart());
    // Bind the joint-palette buffer (root param 3) even though the preview
    // never skins: a root SRV left unbound is read as undefined and faults the
    // GPU the moment a skinned vertex buffer is drawn here (the Asset Viewer
    // previewing a skinned mesh).
    cmdList_->SetGraphicsRootShaderResourceView(
        3, boneArenas_[frameIndex_]->GetGPUVirtualAddress());
    cmdList_->SetGraphicsRootShaderResourceView(
        4, instanceArenas_[frameIndex_]->GetGPUVirtualAddress());
    cmdList_->SetPipelineState(meshPSOSingle_.Get()); // single-RT (no G-buffer)
    cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_GPU_VIRTUAL_ADDRESS frameAddr = 0;
    if (void* dst = AllocConstants(sizeof(FrameCB), frameAddr)) {
        FrameCB fcb{};
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
        // gScreenTexel = 1 / RENDER size (sceneW_/sceneH_), NOT the swapchain (width_): the
        // editor 3D viewport renders smaller than the window, and Water.hlsl samples scene depth
        // at SV_Position * gScreenTexel, so a swapchain-scaled UV mis-taps depth. (W2 depth water)
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
        std::memcpy(dst, &fcb, sizeof(fcb));
        cmdList_->SetGraphicsRootConstantBufferView(0, frameAddr);
    }

    for (u32 i = 0; i < count; ++i) {
        const DrawItem& it = items[i];
        if (!it.mesh.IsValid() || it.mesh.id > meshes_.size()) continue;
        const GpuMesh& gm = meshes_[it.mesh.id - 1];
        D3D12_GPU_VIRTUAL_ADDRESS objAddr = 0;
        if (void* dst = AllocConstants(sizeof(ObjectCB), objAddr)) {
            ObjectCB ocb{};
            ocb.model = it.transform;
            ocb.normalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(it.transform))));
            ocb.baseColor = it.surface.base_color;
            ocb.metallic = it.surface.base_metalness;
            ocb.roughness = it.surface.specular_roughness;
            ocb.albedoIndex = it.albedoTexture.index;
            ocb.normalIndex = it.normalTexture.index;
            ocb.mrIndex = it.mrTexture.index;
            FillMorphCB(ocb, it); // facial blendshapes (bindless delta atlas)
            ocb.aoIndex = it.aoTexture.index;
            ocb.flags = it.materialFlags;
            ocb.subsurfaceColor = it.surface.subsurface_color;
            ocb.subsurfaceRadius = it.surface.subsurface_radius;
    ocb.lodDither = it.lodDither; // LOD cross-fade stipple (1.0 = opaque; shadow pass ignores it)
            ocb.clearcoat = it.surface.coat_weight;
            ocb.clearcoatRoughness = it.surface.coat_roughness;
            ocb.emissiveColor = it.surface.emission_color;
            ocb.emissiveIntensity = it.surface.emission_luminance;
            FillSurfaceMaterialExt(ocb.matExt, it.surface);
            ocb.emissiveIndex = it.emissiveTexture.index;
            ocb.prevModel = it.transform; // preview needs no motion vectors
            std::memcpy(dst, &ocb, sizeof(ocb));
            cmdList_->SetGraphicsRootConstantBufferView(1, objAddr);
        } else {
            // THE THIRD COPY of the same defect. The scene and shadow paths were fixed to
            // drop an item whose constants could not be allocated; this one still fell
            // through with the root CBV pointing at the PREVIOUS item, so an asset preview
            // would draw one submesh wearing another's transform and material. Same rule
            // everywhere: never draw with constants that belong to a different object.
            continue;
        }
        cmdList_->IASetVertexBuffers(0, 1, &gm.vbv);
        cmdList_->IASetIndexBuffer(&gm.ibv);
        cmdList_->DrawIndexedInstanced(gm.indexCount, 1, 0, 0, 0);
    }
    if (view.skyIndex != 0 && skyPSOSingle_) {
        cmdList_->SetPipelineState(skyPSOSingle_.Get());
        cmdList_->DrawInstanced(3, 1, 0, 0);
    }

    auto toSRV = TransitionBarrier(prevHdr_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList_->ResourceBarrier(1, &toSRV);

    // --- Tonemap into the LDR preview (no bloom/AO/vignette) ------------------
    {
        D3D12_CPU_DESCRIPTOR_HANDLE ldrRtv = prevRtvHeap_->GetCPUDescriptorHandleForHeapStart();
        ldrRtv.ptr += device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        PostCB cb;
        cb.input0 = slotPrevHdr_;
        cb.outTexel = {1.0f / prevW_, 1.0f / prevH_};
        cb.inTexel = cb.outTexel;
        cb.params0 = {0.0f, 0.0f, 0.0f, 1.0f}; // no bloom, no AO, no vignette
        cb.params1 = {1.0f, 0.0f, 0.0f, 0.0f}; // unity contrast
        DrawPostPass(tonemapPSO_.Get(), prevLdr_.Get(), ldrRtv, prevW_, prevH_, cb);
    }
}
#endif // HBE_EDITOR

D3D12Device::~D3D12Device() {
    WaitForGpuIdle();
#if HBE_EDITOR
    ShutdownUI();
#endif
    if (fenceEvent_) {
        ::CloseHandle(fenceEvent_);
        fenceEvent_ = nullptr;
    }
    if (uploadEvent_) {
        ::CloseHandle(uploadEvent_);
        uploadEvent_ = nullptr;
    }
}

} // namespace

std::unique_ptr<IRenderDevice> CreateD3D12Device(const RenderDeviceDesc& desc) {
    auto dev = std::make_unique<D3D12Device>();
    if (!dev->Initialize(desc)) {
        return nullptr;
    }
    return dev;
}

} // namespace hbe::rhi
