// Renderer/Renderer.cpp
#include "Renderer/Renderer.h"
#include "Assets/Mesh.h"
#include "Core/Log.h"
#include "Core/Window.h"
#include "RHI/RHIFactory.h"
#include "Scene/Scene.h"

#include <cmath>
#include <vector>

namespace hbe {

Renderer::Renderer() = default;
Renderer::~Renderer() { Shutdown(); }

bool Renderer::Initialize(const Window& window, rhi::GraphicsAPI api, bool enableValidation) {
    const NativeWindowHandle native = window.GetNativeHandle();

    rhi::RenderDeviceDesc desc;
    desc.api = api;
    desc.windowHandle = native.hwnd;
    desc.windowInstance = native.hinstance;
    desc.width = window.Width();
    desc.height = window.Height();
    desc.backBufferCount = 3;
    desc.backBufferFormat = rhi::Format::R8G8B8A8_UNORM;
    desc.enableValidation = enableValidation;

    device_ = rhi::CreateRenderDevice(desc);
    if (!device_) {
        HBE_ERROR("Renderer: failed to create {} device.", rhi::ToString(api));
        return false;
    }

    windowSize_ = {static_cast<f32>(window.Width()), static_cast<f32>(window.Height())};
    const f32 aspect = window.Height() > 0
        ? static_cast<f32>(window.Width()) / static_cast<f32>(window.Height())
        : 1.0f;
    camera_.SetPerspective(60.0f, aspect, 0.1f, 1000.0f);
    camera_.LookAt({0, 1, 3}, {0, 0, 0});

    HBE_INFO("Renderer ready: {} on '{}' (scene rendering {})",
             rhi::ToString(api), device_->GetAdapterName(),
             device_->SupportsSceneRendering() ? "supported" : "unsupported");
    return true;
}

rhi::MeshHandle Renderer::UploadMesh(const MeshData& mesh) {
    return device_ ? device_->CreateMesh(mesh) : rhi::MeshHandle{};
}

void Renderer::UpdateMesh(rhi::MeshHandle handle, const MeshData& mesh) {
    if (device_) device_->UpdateMesh(handle, mesh);
}

rhi::TextureHandle Renderer::UploadTexture(const rhi::TextureDesc& desc) {
    return device_ ? device_->CreateTexture(desc) : rhi::TextureHandle{};
}

void Renderer::UpdateTexture(rhi::TextureHandle handle, const rhi::TextureDesc& desc) {
    if (device_) device_->UpdateTexture(handle, desc);
}

bool Renderer::SupportsScene() const {
    return device_ && device_->SupportsSceneRendering();
}

void Renderer::Update(f32 dt) {
    if (orbitEnabled_) {
        orbitTime_ += dt;
        const f32 a = orbitTime_ * 0.3f;
        const glm::vec3 eye = orbitTarget_ + glm::vec3(std::sin(a) * orbitRadius_,
                                                       orbitRadius_ * 0.25f,
                                                       std::cos(a) * orbitRadius_);
        camera_.LookAt(eye, orbitTarget_);
    }
}

void Renderer::FocusOn(const glm::vec3& center, f32 radius) {
    orbitTarget_ = center;
    orbitRadius_ = glm::max(radius * 2.2f, 1.0f);
    orbitEnabled_ = true;
    // Scale the clip planes to the model so it is neither clipped nor z-fighting.
    camera_.SetClipPlanes(glm::max(radius * 0.002f, 0.02f), glm::max(radius * 20.0f, 1000.0f));
    HBE_INFO("Renderer: focused on model (radius {:.1f}).", radius);
}

void Renderer::RenderScene(const Scene& scene, f32 dt) {
    if (!device_) return;
    time_ += dt;

    device_->BeginFrame();

    if (device_->SupportsSceneRendering()) {
        // Editor asset preview: its own mini-scene, before the main passes.
        if (previewPending_) {
            previewPending_ = false;
            device_->DrawPreviewScene(previewView_, previewItems_.data(),
                                      static_cast<u32>(previewItems_.size()));
        }

        rhi::SceneView view = scene.MakeView(camera_);
        view.deltaTime = dt; // for temporal post effects (auto-exposure)
        static f32 s_skyTime = 0.0f; // accumulated time for sky animation
        s_skyTime += dt;
        view.timeSeconds = s_skyTime;
        drawItems_.clear();
        scene.CollectDrawItems(drawItems_);
        const u32 itemCount = static_cast<u32>(drawItems_.size());
        // Particle billboards for this frame (drawn inside DrawScene's HDR pass).
        device_->SetParticles(particleAlpha_, particleAlphaCount_, particleAdd_,
                              particleAddCount_);
        particleAlpha_ = particleAdd_ = nullptr; // one frame only
        particleAlphaCount_ = particleAddCount_ = 0;
        // Shadow map first: it must record before the main pass begins.
        device_->DrawShadowPass(view, drawItems_.data(), itemCount);
        device_->ClearBackBuffer(0.018f, 0.018f, 0.022f, 1.0f);
        device_->DrawScene(view, drawItems_.data(), itemCount);
        if (uiVertices_ && !uiVertices_->empty()) {
            device_->DrawUIOverlay(uiVertices_->data(),
                                   static_cast<u32>(uiVertices_->size()));
        }
        uiVertices_ = nullptr; // one frame only
    } else {
        // Fallback (backend without geometry support): animated gradient clear.
        const f32 r = 0.5f + 0.5f * std::sin(time_ * 0.6f);
        const f32 g = 0.5f + 0.5f * std::sin(time_ * 0.6f + 2.094f);
        const f32 b = 0.5f + 0.5f * std::sin(time_ * 0.6f + 4.188f);
        device_->ClearBackBuffer(r * 0.25f, g * 0.25f, b * 0.30f, 1.0f);
    }

    // Editor overlay records into the same frame, after the scene.
    device_->RenderUI();
    device_->EndFrame();
}

bool Renderer::SupportsUI() const { return device_ && device_->SupportsUI(); }

bool Renderer::InitUI(void* nativeWindowHandle) {
    return device_ && device_->InitUI(nativeWindowHandle);
}

void Renderer::BeginUI() {
    if (device_) device_->BeginUIFrame();
}

void Renderer::SetViewportSize(u32 width, u32 height) {
    viewportSize_ = {static_cast<f32>(width), static_cast<f32>(height)};
    if (device_) device_->ResizeViewport(width, height);
}

glm::vec2 Renderer::RenderTargetSize() {
    const bool offscreen = device_ && device_->GetViewportTextureId() != 0;
    if (offscreen && viewportSize_.x > 0.0f && viewportSize_.y > 0.0f) {
        return viewportSize_;
    }
    return windowSize_;
}

u64 Renderer::ViewportTextureId() {
    return device_ ? device_->GetViewportTextureId() : 0;
}

void Renderer::SetPreviewSize(u32 width, u32 height) {
    if (device_) device_->ResizePreview(width, height);
}

u64 Renderer::PreviewTextureId() {
    return device_ ? device_->GetPreviewTextureId() : 0;
}

void Renderer::SetPreviewScene(const rhi::SceneView& view,
                               const std::vector<rhi::DrawItem>& items) {
    previewView_ = view;
    previewItems_ = items;
    previewPending_ = true;
}

u64 Renderer::TextureUIId(rhi::TextureHandle handle) {
    return device_ ? device_->GetTextureUIHandle(handle) : 0;
}

void Renderer::Resize(u32 width, u32 height) {
    if (!device_) return;
    windowSize_ = {static_cast<f32>(width), static_cast<f32>(height)};
    device_->Resize(width, height);
    if (height > 0) {
        camera_.SetAspect(static_cast<f32>(width) / static_cast<f32>(height));
    }
}

void Renderer::Shutdown() {
    if (device_) {
        device_->WaitForGpuIdle();
        device_.reset();
    }
}

const char* Renderer::AdapterName() const {
    return device_ ? device_->GetAdapterName() : "<no device>";
}

rhi::GraphicsAPI Renderer::API() const {
    return device_ ? device_->GetAPI() : rhi::GraphicsAPI::D3D12;
}

} // namespace hbe
