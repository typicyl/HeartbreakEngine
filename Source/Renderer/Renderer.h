// Renderer/Renderer.h - high-level renderer that drives the RHI device.
#pragma once

#include "Core/Types.h"
#include "Renderer/Camera.h"
#include "RHI/RHI.h"

#include <memory>
#include <vector>

namespace hbe {

class Window;
class Scene;
struct MeshData;

class Renderer : public NonCopyable {
public:
    Renderer();
    ~Renderer();

    // Creates the RHI device for `api` and binds it to `window`.
    bool Initialize(const Window& window, rhi::GraphicsAPI api, bool enableValidation);

    // Uploads a CPU mesh to the GPU; returns a handle used by MeshInstance.
    rhi::MeshHandle UploadMesh(const MeshData& mesh);

    // Re-uploads geometry into an existing mesh's GPU buffers in place (fixed
    // topology, e.g. terrain sculpting). Avoids leaking a buffer per edit.
    void UpdateMesh(rhi::MeshHandle handle, const MeshData& mesh);

    // Uploads a 2D texture into the bindless array; returns its index handle.
    rhi::TextureHandle UploadTexture(const rhi::TextureDesc& desc);

    // Re-uploads pixel data into an existing bindless texture in place (live
    // surface painting); the desc must match the texture's created layout.
    void UpdateTexture(rhi::TextureHandle handle, const rhi::TextureDesc& desc);

    // True if the active backend can render geometry (vs. clear-only).
    bool SupportsScene() const;

    // Advances simulation state (camera orbit when enabled).
    void Update(f32 dt);

    // Records and presents one frame of `scene`. `dt` is the delta in seconds.
    void RenderScene(const Scene& scene, f32 dt);

    // In-game UI overlay triangles for this frame (reference-canvas space);
    // drawn over the scene inside RenderScene. Cleared by the caller each
    // frame (an empty set draws nothing).
    void SetUIOverlay(const std::vector<rhi::UIVertex>& vertices) { uiVertices_ = &vertices; }

    // Particle billboards for this frame (world-space; alpha + additive lists).
    // Drawn inside RenderScene's HDR pass; cleared after one frame.
    void SetParticles(const std::vector<rhi::ParticleVertex>& alpha,
                      const std::vector<rhi::ParticleVertex>& additive) {
        particleAlpha_ = alpha.empty() ? nullptr : alpha.data();
        particleAlphaCount_ = static_cast<u32>(alpha.size());
        particleAdd_ = additive.empty() ? nullptr : additive.data();
        particleAddCount_ = static_cast<u32>(additive.size());
    }

    void Resize(u32 width, u32 height);
    void Shutdown();

    // -- Editor UI -----------------------------------------------------------
    bool SupportsUI() const;
    bool InitUI(void* nativeWindowHandle);
    void BeginUI(); // start an ImGui frame (before building widgets)

    // Editor viewport (scene rendered into an ImGui panel).
    void SetViewportSize(u32 width, u32 height);
    u64  ViewportTextureId();

    // Editor asset preview: an independent mini-scene (orbiting mesh preview)
    // rendered before the main scene each frame. The editor submits a view +
    // draw items during BuildUI; RenderScene consumes them (one frame only).
    void SetPreviewSize(u32 width, u32 height);
    u64  PreviewTextureId();
    void SetPreviewScene(const rhi::SceneView& view,
                         const std::vector<rhi::DrawItem>& items);
    // Pixel size of the surface the scene renders into this frame (the
    // offscreen viewport when active, else the window back buffer).
    glm::vec2 RenderTargetSize();
    // ImGui texture id for an uploaded texture (editor thumbnails; 0 = none).
    u64  TextureUIId(rhi::TextureHandle handle);

    void SetOrbitEnabled(bool enabled) { orbitEnabled_ = enabled; }
    bool IsOrbitEnabled() const { return orbitEnabled_; }

    // Frames the orbit camera on a bounding sphere (used after loading a model).
    void FocusOn(const glm::vec3& center, f32 radius);

    bool IsValid() const { return device_ != nullptr; }
    const char* AdapterName() const;
    rhi::GraphicsAPI API() const;

    Camera& GetCamera() { return camera_; }
    const Camera& GetCamera() const { return camera_; }

private:
    std::unique_ptr<rhi::IRenderDevice> device_;
    Camera camera_;
    std::vector<rhi::DrawItem> drawItems_; // reused each frame
    const std::vector<rhi::UIVertex>* uiVertices_ = nullptr; // set per frame
    const rhi::ParticleVertex* particleAlpha_ = nullptr;     // set per frame
    const rhi::ParticleVertex* particleAdd_ = nullptr;
    u32 particleAlphaCount_ = 0, particleAddCount_ = 0;
    rhi::SceneView previewView_;                  // editor asset preview
    std::vector<rhi::DrawItem> previewItems_;     // (consumed each frame)
    bool previewPending_ = false;
    bool orbitEnabled_ = true;
    glm::vec2 viewportSize_{0.0f};      // last SetViewportSize request
    glm::vec2 windowSize_{1280, 720};   // swapchain size (Initialize/Resize)
    glm::vec3 orbitTarget_{0.0f};
    f32 orbitRadius_ = 9.5f;
    f32 orbitTime_ = 0.0f;
    f32 time_ = 0.0f;
};

} // namespace hbe
