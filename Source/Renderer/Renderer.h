// Renderer/Renderer.h - high-level renderer that drives the RHI device.
#pragma once

#include "Core/Types.h"
#include "Renderer/Camera.h"
#include "RHI/RHI.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace hbe {

class Window;
class Scene;
struct MeshData;

class Renderer : public NonCopyable {
public:
    Renderer();
    ~Renderer();

    // Creates the RHI device for `api` and binds it to `window`. `vsync` false =
    // uncapped present (tearing/MAILBOX), for perf measurement + player choice.
    bool Initialize(const Window& window, rhi::GraphicsAPI api, bool enableValidation,
                    bool vsync = true);

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

    // -- World-space ("physical") UI: canvas -> texture -> lit quad -----------
    // Creates a per-canvas UI render target (invalid handle when unsupported).
    rhi::TextureHandle CreateUITarget(u32 width, u32 height);
    // One world-canvas draw for this frame: `verts` rendered into `target`
    // before the scene pass samples it. Pointers must stay valid through
    // RenderScene (the Engine owns the batch vectors). One frame only.
    struct WorldUIDraw {
        rhi::TextureHandle target;
        const rhi::UIVertex* verts = nullptr;
        u32 count = 0;
    };
    void SetWorldUI(const std::vector<WorldUIDraw>& draws) { worldUIDraws_ = &draws; }

    // -- Editor UI-authoring canvas (the dedicated `.hbui` editor panel) --------
    // ONE document rendered into its own UI target this frame. Its own slot rather
    // than an append to SetWorldUI because (a) worldUIDraws is a local of
    // Engine::Run that the editor cannot reach, and (b) an authoring page is not
    // world content and must not enter the world-UI list.
    //
    // Consumed and cleared by RenderScene (one frame only; `verts` must stay alive
    // until then - the editor owns the vector). Submitted in the SAME pre-shadow
    // block as the world-UI draws, which is what makes the panel same-frame rather
    // than one frame stale: the editor's BuildUI hook runs BEFORE RenderScene, and
    // DrawUIToTexture runs before the ImGui pass inside it.
    void SetEditorUICanvas(rhi::TextureHandle target, const rhi::UIVertex* verts, u32 count) {
        editorUITarget_ = target;
        editorUIVerts_ = verts;
        editorUICount_ = count;
    }

    // Particle billboards for this frame (world-space; alpha + additive lists).
    // Drawn inside RenderScene's HDR pass; cleared after one frame.
    void SetParticles(const std::vector<rhi::ParticleVertex>& alpha,
                      const std::vector<rhi::ParticleVertex>& additive) {
        particleAlpha_ = alpha.empty() ? nullptr : alpha.data();
        particleAlphaCount_ = static_cast<u32>(alpha.size());
        particleAdd_ = additive.empty() ? nullptr : additive.data();
        particleAddCount_ = static_cast<u32>(additive.size());
    }

    // GPU-EXPANDED particle batches for this frame. Deferred exactly like
    // SetParticles (forwarded inside RenderScene, cleared after one frame) so the
    // batch vector's lifetime rule is the same one callers already follow.
    //
    // ACCUMULATES, up to rhi::kMaxGpuParticleGroups: the CPU-simulated emitters live
    // in the per-frame upload ring and the GPU-simulated ones in the compute-written
    // buffer, and a batch carries only an element offset.
    void SetGpuParticles(rhi::GpuBufferHandle records,
                         const std::vector<rhi::GpuParticleBatch>& batches) {
        if (!records.IsValid() || batches.empty()) return;
        if (particleGpuGroupCount_ >= rhi::kMaxGpuParticleGroups) return;
        GpuParticleGroup& g = particleGpuGroups_[particleGpuGroupCount_++];
        g.buffer = records;
        g.batches = batches.data();
        g.count = static_cast<u32>(batches.size());
    }

    // Volumetric-VFX blobs for this frame (splatted into a 3D density volume; see
    // VolumeSplat.hlsl). Forwarded to the device IMMEDIATELY (not deferred like
    // particles) because the Vulkan splat runs in BeginFrame - so call this BEFORE
    // RenderScene. Empty = volumetrics off this frame (zero GPU cost).
    void SetVolumeParticles(const std::vector<rhi::VolumeBlob>& blobs,
                            const rhi::VolumeParams& params) {
        if (device_) {
            device_->SetVolumeParticles(blobs.empty() ? nullptr : blobs.data(),
                                        static_cast<u32>(blobs.size()), params);
        }
    }

    // -- GPU compute + GPU-writable structured buffers -----------------------
    // Thin forwarders (the SetVolumeParticles pattern). QueueCompute must be
    // called BEFORE RenderScene: both backends execute the queue in their
    // BeginFrame, because Vulkan cannot record compute inside a render pass.
    bool SupportsGpuCompute() const { return device_ && device_->SupportsGpuCompute(); }
    rhi::GpuBufferHandle CreateGpuBuffer(const rhi::GpuBufferDesc& desc) {
        return device_ ? device_->CreateGpuBuffer(desc) : rhi::GpuBufferHandle{};
    }
    void* MapGpuBuffer(rhi::GpuBufferHandle h) {
        return device_ ? device_->MapGpuBuffer(h) : nullptr;
    }
    bool ReadGpuBuffer(rhi::GpuBufferHandle h, void* dst, u32 bytes) {
        return device_ && device_->ReadGpuBuffer(h, dst, bytes);
    }
    void DestroyGpuBuffer(rhi::GpuBufferHandle h) {
        if (device_) device_->DestroyGpuBuffer(h);
    }
    rhi::ComputePipelineHandle CreateComputePipeline(const rhi::ComputePipelineDesc& desc) {
        return device_ ? device_->CreateComputePipeline(desc) : rhi::ComputePipelineHandle{};
    }
    void QueueCompute(const rhi::ComputeDispatch& d) {
        if (device_) device_->QueueCompute(d);
    }
    void SetVertexShaderBuffer(rhi::GpuBufferHandle h, u32 firstElement) {
        if (device_) device_->SetVertexShaderBuffer(h, firstElement);
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
    // Reads the offscreen viewport color back to CPU as canonical RGBA8 (top row
    // first). Blocking; editor-only. Returns false when unsupported / not ready.
    // The offline movie render captures each frame through this.
    bool ReadbackViewportColor(std::vector<u8>& outRGBA, u32& w, u32& h);

    // Per-pass GPU profiler: enable to log a per-pass GPU-time breakdown every ~2s
    // (--gpuprofile / dev menu). ~1-3 ms/frame while active. See IRenderDevice.
    void SetGpuProfileEnabled(bool enable);
    bool GpuProfileActive() const;

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

    // Player brightness (Settings menu): a multiplier composed onto the view's
    // exposure at render time - the authored scene/volume exposure is never
    // modified. 1.0 = neutral. Sticky until changed (not per-frame).
    void SetUserExposureScale(f32 scale) { userExposureScale_ = scale; }

    // -- Frustum culling ------------------------------------------------------
    // Per-frame draw statistics (filled by RenderScene).
    struct FrameStats {
        u32 total = 0;  // draw items collected from the scene
        u32 drawn = 0;  // submitted to the main pass after frustum culling
        u32 culled = 0; // rejected (off-screen); shadows still see the full list
        u32 instancedDraws = 0;  // instanced run heads this frame
        u32 totalInstances = 0;  // items covered by those runs
        // Shadow submission after per-cascade culling. `shadowDraws` counts
        // (item, cascade) pairs actually recorded; `shadowCulled` counts the pairs
        // skipped. Before per-cascade culling this was always items x cascades.
        u32 shadowDraws = 0;
        u32 shadowCulled = 0;
    };
    const FrameStats& Stats() const { return stats_; }
    // Kill-switch (--nocull) for A/B comparison; culling is on by default.
    void SetCullingEnabled(bool enabled) { cullingEnabled_ = enabled; }

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
    const std::vector<WorldUIDraw>* worldUIDraws_ = nullptr; // set per frame
    rhi::TextureHandle editorUITarget_;                       // set per frame (editor canvas)
    const rhi::UIVertex* editorUIVerts_ = nullptr;
    u32 editorUICount_ = 0;
    const rhi::ParticleVertex* particleAlpha_ = nullptr;     // set per frame
    const rhi::ParticleVertex* particleAdd_ = nullptr;
    u32 particleAlphaCount_ = 0, particleAddCount_ = 0;
    struct GpuParticleGroup {                                        // set per frame
        rhi::GpuBufferHandle buffer;
        const rhi::GpuParticleBatch* batches = nullptr;
        u32 count = 0;
    };
    GpuParticleGroup particleGpuGroups_[rhi::kMaxGpuParticleGroups]{};
    u32 particleGpuGroupCount_ = 0;
    rhi::SceneView previewView_;                  // editor asset preview
    std::vector<rhi::DrawItem> previewItems_;     // (consumed each frame)
    bool previewPending_ = false;
    f32 userExposureScale_ = 1.0f; // player brightness (view-level, sticky)
    // Local-space bounds per MeshHandle id, cached at Upload/UpdateMesh - the
    // frustum cull's data source (missing entry = never culled). Renderer is the
    // only upload path, so coverage is universal.
    struct MeshBounds {
        glm::vec3 center{0.0f};
        glm::vec3 extent{0.0f}; // half-size
    };
    std::unordered_map<u32, MeshBounds> meshBounds_;
    FrameStats stats_;
    bool cullingEnabled_ = true;
    bool orbitEnabled_ = true;
    glm::vec2 viewportSize_{0.0f};      // last SetViewportSize request
    glm::vec2 windowSize_{1280, 720};   // swapchain size (Initialize/Resize)
    glm::vec3 orbitTarget_{0.0f};
    f32 orbitRadius_ = 9.5f;
    f32 orbitTime_ = 0.0f;
    f32 time_ = 0.0f;
};

} // namespace hbe
