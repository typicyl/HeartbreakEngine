// Vfx/EffekseerBackend.h - the Effekseer VFX runtime, wrapped behind a Heartbreak-facing interface.
//
// This is the BACKEND half of the VFX abstraction the brief asked for: gameplay/editor talk to the
// Heartbreak seam (game::SpawnEffect / VfxWorld); this owns the actual Effekseer Manager + its DX12
// renderer and knows nothing about Heartbreak's scene. It is deliberately opaque - no Effekseer or
// D3D12 type appears in this header - so only EffekseerBackend.cpp pulls in the Effekseer headers,
// and the rest of the engine stays decoupled from the dependency.
//
// Windows/DX12 only for now (Effekseer's Vulkan renderer needs its glslang submodule + a second
// backend pass; tracked as follow-up). The whole thing is gated by HBE_HAVE_EFFEKSEER: without the
// third_party/Effekseer checkout the methods are inert (Available() == false) and the engine falls
// back to its native particle system.
#pragma once

#include "Core/Types.h"

#include <glm/glm.hpp>

#include <string>

namespace hbe::vfx {

class EffekseerBackend {
public:
    EffekseerBackend();
    ~EffekseerBackend();
    EffekseerBackend(const EffekseerBackend&) = delete;
    EffekseerBackend& operator=(const EffekseerBackend&) = delete;

    // Create the Effekseer manager + DX12 renderer from the RHI's native handles. `d3d12Device` and
    // `d3d12CommandQueue` are `ID3D12Device*` / `ID3D12CommandQueue*` passed as void* (the header
    // stays D3D12-free). `rtFormatDXGI` / `depthFormatDXGI` are DXGI_FORMAT values of the HDR target
    // + scene depth so Effekseer's pipelines match the pass it draws into. Returns false (and stays
    // unavailable) if Effekseer is not compiled in or creation fails.
    bool Init(void* d3d12Device, void* d3d12CommandQueue, u32 rtFormatDXGI, u32 depthFormatDXGI,
              bool hasDepth, int swapBufferCount);
    // Vulkan variant: handles are VkPhysicalDevice / VkDevice / VkQueue / VkCommandPool as void*.
    // `rtFormatsVk` points at `colorAttachmentCount` VkFormat values - the EXACT per-attachment
    // formats of the MRT pass Effekseer draws into (Heartbreak's HDR pass is colour+G-buffer+velocity,
    // and velocity is NOT the same format), so Effekseer's pipelines are render-pass compatible.
    // `depthFormatVk` is the depth VkFormat. Inert unless HBE_EFFEKSEER_VK was compiled in.
    bool InitVulkan(void* physicalDevice, void* device, void* queue, void* commandPool,
                    const u32* rtFormatsVk, int colorAttachmentCount, u32 depthFormatVk,
                    int swapBufferCount);
    void Shutdown();
    bool Available() const;

    // Load an effect file (`.efk` / `.efkefc`), cached by path. Returns an effect id (0 = failure).
    u32 LoadEffect(const std::string& path);
    // Spawn a loaded effect at a world position. Returns an Effekseer handle (-1 = failure).
    int Play(u32 effectId, const glm::vec3& pos);
    void Stop(int handle);
    void StopAll();
    void SetLocation(int handle, const glm::vec3& pos);
    bool Exists(int handle) const;

    // Advance the simulation by `dt` seconds (Effekseer counts in 1/60s frames internally).
    void Update(f32 dt);
    // Record the effect draw into `d3d12CommandList` (an `ID3D12GraphicsCommandList*` as void*),
    // using the camera view + projection. Call inside the HDR forward pass, with the scene depth
    // bound, so particles depth-test against geometry.
    void Draw(void* d3d12CommandList, const glm::mat4& view, const glm::mat4& proj);

    // Live instance count (for stats / tests).
    int LiveInstanceCount() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace hbe::vfx
