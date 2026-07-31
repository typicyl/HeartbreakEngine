// Renderer/Renderer.cpp
#include "Renderer/Renderer.h"
#include "Assets/Mesh.h"
#include "Core/Log.h"
#include "Core/Window.h"
#include "RHI/RHIFactory.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace hbe {

namespace {

// Gribb-Hartmann frustum planes from a view-projection matrix, for the RH
// ZERO-TO-ONE clip space this engine uses (GLM_FORCE_DEPTH_ZERO_TO_ONE):
// left/right/bottom/top = row3 +/- row(i); near = row2 ALONE (z >= 0), far =
// row3 - row2. Planes point INWARD (positive half-space = inside).
struct Frustum {
    glm::vec4 planes[6];

    explicit Frustum(const glm::mat4& vp) {
        // glm is column-major: "row i" of the matrix = vec4(m[0][i], m[1][i], ...).
        const auto row = [&](int i) {
            return glm::vec4(vp[0][i], vp[1][i], vp[2][i], vp[3][i]);
        };
        const glm::vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
        planes[0] = r3 + r0; // left
        planes[1] = r3 - r0; // right
        planes[2] = r3 + r1; // bottom
        planes[3] = r3 - r1; // top
        planes[4] = r2;      // near (zero-to-one depth: z' >= 0)
        planes[5] = r3 - r2; // far
    }

    // Conservative world-AABB test (center + half-extent): outside ANY plane =
    // culled; intersecting/inside = visible.
    bool Intersects(const glm::vec3& c, const glm::vec3& e) const {
        for (const glm::vec4& p : planes) {
            const glm::vec3 n(p);
            const f32 r = e.x * std::abs(n.x) + e.y * std::abs(n.y) + e.z * std::abs(n.z);
            if (glm::dot(n, c) + p.w + r < 0.0f) return false;
        }
        return true;
    }
};

// Local center/extent -> world center/extent under an affine transform:
// world extent_i = sum_j |M[j][i]| * localExtent_j (no 8-corner loop).
void WorldAabb(const glm::mat4& m, const glm::vec3& localC, const glm::vec3& localE,
               glm::vec3& outC, glm::vec3& outE) {
    outC = glm::vec3(m * glm::vec4(localC, 1.0f));
    const glm::vec3 x = glm::vec3(m[0]) * localE.x;
    const glm::vec3 y = glm::vec3(m[1]) * localE.y;
    const glm::vec3 z = glm::vec3(m[2]) * localE.z;
    outE = glm::abs(x) + glm::abs(y) + glm::abs(z);
}

// True when an item is SAFE to fold into an instanced run: opaque, unskinned,
// unpainted, non-splat "simple" materials only (everything per-instance except
// the transforms must be identical - the run head's ObjectCB serves the group).
bool Instanceable(const rhi::DrawItem& it) {
    return it.boneCount == 0 && !it.paintColorTexture.IsValid() &&
           !(it.materialFlags &
             (rhi::MaterialFlag_Transparent | rhi::MaterialFlag_TerrainSplat));
}

// Material-key equality for run grouping: everything the ObjectCB carries
// besides the per-instance transforms.
bool SameMaterial(const rhi::DrawItem& a, const rhi::DrawItem& b) {
    return a.mesh.id == b.mesh.id && a.materialFlags == b.materialFlags &&
           a.baseColor == b.baseColor && a.metallic == b.metallic &&
           a.roughness == b.roughness &&
           a.albedoTexture.index == b.albedoTexture.index &&
           a.normalTexture.index == b.normalTexture.index &&
           a.mrTexture.index == b.mrTexture.index &&
           a.aoTexture.index == b.aoTexture.index &&
           a.emissiveTexture.index == b.emissiveTexture.index &&
           a.thicknessTexture.index == b.thicknessTexture.index &&
           a.emissiveColor == b.emissiveColor &&
           a.emissiveIntensity == b.emissiveIntensity &&
           a.subsurfaceColor == b.subsurfaceColor &&
           a.subsurfaceRadius == b.subsurfaceRadius &&
           a.surfaceStrokes == b.surfaceStrokes;
}

} // namespace

Renderer::Renderer() = default;
Renderer::~Renderer() { Shutdown(); }

bool Renderer::Initialize(const Window& window, rhi::GraphicsAPI api, bool enableValidation,
                          bool vsync) {
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
    desc.vsync = vsync;

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
    const rhi::MeshHandle handle = device_ ? device_->CreateMesh(mesh) : rhi::MeshHandle{};
    if (handle.IsValid()) {
        glm::vec3 bmin, bmax;
        ComputeBounds(mesh, bmin, bmax);
        meshBounds_[handle.id] = {(bmin + bmax) * 0.5f, (bmax - bmin) * 0.5f};
    }
    return handle;
}

void Renderer::UpdateMesh(rhi::MeshHandle handle, const MeshData& mesh) {
    if (!device_) return;
    device_->UpdateMesh(handle, mesh);
    if (handle.IsValid()) { // geometry changed (sculpting) -> refresh the cull bounds
        glm::vec3 bmin, bmax;
        ComputeBounds(mesh, bmin, bmax);
        meshBounds_[handle.id] = {(bmin + bmax) * 0.5f, (bmax - bmin) * 0.5f};
    }
}

rhi::TextureHandle Renderer::UploadTexture(const rhi::TextureDesc& desc) {
    return device_ ? device_->CreateTexture(desc) : rhi::TextureHandle{};
}

rhi::TextureHandle Renderer::CreateUITarget(u32 width, u32 height) {
    return device_ ? device_->CreateUITarget(width, height) : rhi::TextureHandle{};
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

        // World-space UI canvases render into their textures FIRST so the scene
        // pass below samples this frame's page content (shadow-map precedent).
        // count == 0 still submits: the backend clears the target (a fresh or
        // just-hidden page shows transparent, never stale/garbage content).
        if (worldUIDraws_) {
            for (const WorldUIDraw& d : *worldUIDraws_) {
                if (d.target.IsValid())
                    device_->DrawUIToTexture(d.target, d.verts, d.count);
            }
            worldUIDraws_ = nullptr; // one frame only
        }

        // The editor's `.hbui` authoring canvas: same pre-shadow slot, so the ImGui
        // pass later in THIS frame samples what was just written (no staleness).
        // count == 0 still submits - the backend clears, so an empty document reads
        // transparent rather than showing the previous document's page.
        if (editorUITarget_.IsValid()) {
            device_->DrawUIToTexture(editorUITarget_, editorUIVerts_, editorUICount_);
            editorUITarget_ = {};
            editorUIVerts_ = nullptr;
            editorUICount_ = 0;
        }

        rhi::SceneView view = scene.MakeView(camera_);
        view.exposure *= userExposureScale_; // player brightness (view-level only)
        view.deltaTime = dt; // for temporal post effects (auto-exposure)
        static f32 s_skyTime = 0.0f; // accumulated time for sky animation
        s_skyTime += dt;
        view.timeSeconds = s_skyTime;
        drawItems_.clear();
        scene.CollectDrawItems(drawItems_);
        const u32 itemCount = static_cast<u32>(drawItems_.size());

        // --- Frustum culling: reorder drawItems_ IN PLACE to a visible-first
        // prefix. INVARIANT: the shadow pass gets the FULL list (off-screen
        // casters still shadow) and the main pass gets only the prefix; both
        // backends couple shadow->scene per-item state BY INDEX (D3D12
        // shadowObjAddrs_[i], Vulkan's shared object-UBO arena), which stays
        // valid because indices < visibleCount are identical for both passes.
        // All reordering happens strictly BEFORE DrawShadowPass. Skinned items
        // (boneCount > 0) are never culled (bind-pose bounds would lie).
        u32 visibleCount = itemCount;
        if (cullingEnabled_ && itemCount > 0) {
            const Frustum frustum(view.viewProj);
            const auto visible = [&](const rhi::DrawItem& it) {
                if (it.boneCount > 0) return true; // skinned: bounds unreliable
                const auto bit = meshBounds_.find(it.mesh.id);
                if (bit == meshBounds_.end()) return true; // unknown: never cull
                glm::vec3 c, e;
                WorldAabb(it.transform, bit->second.center, bit->second.extent, c, e);
                return frustum.Intersects(c, e);
            };
            const auto mid = std::stable_partition(drawItems_.begin(), drawItems_.end(),
                                                   visible);
            visibleCount = static_cast<u32>(std::distance(drawItems_.begin(), mid));
        }
        stats_.total = itemCount;
        stats_.drawn = visibleCount;
        stats_.culled = itemCount - visibleCount;

        // Sort the VISIBLE prefix for submission coherence: opaque first (matches
        // the backends' two-pass split), grouped by mesh (enables the IA-rebind
        // skip + instancing runs), then front-to-back within a mesh (early-Z).
        // The backends' own transparent back-to-front sort still runs after this.
        if (visibleCount > 1) {
            const glm::vec3 camPos = view.cameraPos;
            std::sort(drawItems_.begin(), drawItems_.begin() + visibleCount,
                      [&](const rhi::DrawItem& a, const rhi::DrawItem& b) {
                          const bool ta = (a.materialFlags & rhi::MaterialFlag_Transparent) != 0;
                          const bool tb = (b.materialFlags & rhi::MaterialFlag_Transparent) != 0;
                          if (ta != tb) return !ta; // opaque before transparent
                          if (a.mesh.id != b.mesh.id) return a.mesh.id < b.mesh.id;
                          const glm::vec3 va = glm::vec3(a.transform[3]) - camPos;
                          const glm::vec3 vb = glm::vec3(b.transform[3]) - camPos;
                          return glm::dot(va, va) < glm::dot(vb, vb); // front-to-back
                      });
        }

        // Run-builder: fold consecutive identical-material items in the sorted
        // VISIBLE prefix into instanced runs (head.instanceRun = N, followers 0;
        // singles stay 1). Runs never cross the cull boundary - the unsorted
        // culled tail (shadow-only) stays single draws. Both passes consume the
        // same markers, so the shadow<->scene coupling stays per-run consistent.
        stats_.instancedDraws = 0;
        stats_.totalInstances = 0;
        for (u32 i = 0; i < visibleCount;) {
            drawItems_[i].instanceRun = 1;
            if (!Instanceable(drawItems_[i])) {
                ++i;
                continue;
            }
            u32 runEnd = i + 1;
            while (runEnd < visibleCount && Instanceable(drawItems_[runEnd]) &&
                   SameMaterial(drawItems_[i], drawItems_[runEnd]))
                ++runEnd;
            const u32 runLen = runEnd - i;
            if (runLen > 1) {
                drawItems_[i].instanceRun = runLen;
                for (u32 j = i + 1; j < runEnd; ++j) drawItems_[j].instanceRun = 0;
                ++stats_.instancedDraws;
                stats_.totalInstances += runLen;
            }
            i = runEnd;
        }
        // The SHADOW-ONLY TAIL (culled from view, still casting) gets the same
        // treatment. It used to stay unsorted singles, so a scene of repeated
        // meshes - foliage, props, modular kit pieces, i.e. every real complex
        // scene - submitted one shadow draw per instance per cascade. Sorting and
        // run-building the tail is SAFE: the shadow<->scene index coupling only
        // constrains indices < visibleCount, and DrawScene never reads the tail.
        if (itemCount > visibleCount + 1) {
            std::sort(drawItems_.begin() + visibleCount, drawItems_.begin() + itemCount,
                      [](const rhi::DrawItem& a, const rhi::DrawItem& b) {
                          return a.mesh.id < b.mesh.id;
                      });
        }
        for (u32 i = visibleCount; i < itemCount;) {
            drawItems_[i].instanceRun = 1;
            if (!Instanceable(drawItems_[i])) {
                ++i;
                continue;
            }
            u32 runEnd = i + 1;
            while (runEnd < itemCount && Instanceable(drawItems_[runEnd]) &&
                   SameMaterial(drawItems_[i], drawItems_[runEnd]))
                ++runEnd;
            const u32 runLen = runEnd - i;
            if (runLen > 1) {
                drawItems_[i].instanceRun = runLen;
                for (u32 j = i + 1; j < runEnd; ++j) drawItems_[j].instanceRun = 0;
                ++stats_.instancedDraws;
                stats_.totalInstances += runLen;
            }
            i = runEnd;
        }

        // --- Per-cascade shadow culling -------------------------------------
        // The shadow pass gets the FULL list (off-screen casters still shadow),
        // but it was re-rasterizing every caster into EVERY cascade: measured at
        // 58% of total GPU time (5.6 of 9.7 ms at 2000 casters), and the cost
        // barely moved with cascade count because the per-object constants are
        // shared - so it is per-DRAW overhead, which is exactly what culling
        // removes. A near cascade covers a tiny world volume; most casters cannot
        // affect it.
        //
        // Correctness: each cascade's ortho near plane is already pulled back to
        // the scene bounds in Scene::MakeView ("casters behind the slice still
        // occlude"), so the frustum contains everything that could cast into the
        // slice - a plain AABB test is conservative, not an approximation.
        //
        // Ordering: this runs AFTER the run-builder, because an instanced run head
        // draws its whole run in one call and must therefore carry the UNION of
        // its followers' masks.
        stats_.shadowDraws = 0;
        stats_.shadowCulled = 0;
        if (view.shadowsEnabled && itemCount > 0) {
            const u32 cascades = glm::min(view.cascadeCount, rhi::kMaxShadowCascades);
            if (cascades == 0 || !cullingEnabled_) {
                for (u32 i = 0; i < itemCount; ++i) drawItems_[i].cascadeMask = 0xFF;
            } else {
                std::vector<Frustum> cascadeFrusta;
                cascadeFrusta.reserve(cascades);
                for (u32 c = 0; c < cascades; ++c)
                    cascadeFrusta.emplace_back(view.cascadeViewProj[c]);
                const u8 allBits = static_cast<u8>((1u << cascades) - 1u);

                for (u32 i = 0; i < itemCount; ++i) {
                    rhi::DrawItem& it = drawItems_[i];
                    if (it.instanceRun == 0) { it.cascadeMask = 0; continue; } // run follower
                    if (it.materialFlags & rhi::MaterialFlag_NoShadow) {
                        it.cascadeMask = 0;
                        continue;
                    }
                    const auto bit = meshBounds_.find(it.mesh.id);
                    // Skinned poses and unknown bounds are never culled: a bind-pose
                    // AABB does not describe an animated mesh.
                    if (it.boneCount > 0 || bit == meshBounds_.end()) {
                        it.cascadeMask = allBits;
                        stats_.shadowDraws += cascades;
                        continue;
                    }
                    u8 mask = 0;
                    const u32 runLen = glm::max(it.instanceRun, 1u);
                    for (u32 k = 0; k < runLen; ++k) { // union over the instanced run
                        glm::vec3 wc, we;
                        WorldAabb(drawItems_[i + k].transform, bit->second.center,
                                  bit->second.extent, wc, we);
                        for (u32 c = 0; c < cascades; ++c) {
                            const u8 b = static_cast<u8>(1u << c);
                            if (mask & b) continue; // already needed
                            if (cascadeFrusta[c].Intersects(wc, we)) mask |= b;
                        }
                        if (mask == allBits) break; // cannot narrow further
                    }
                    it.cascadeMask = mask;
                    for (u32 c = 0; c < cascades; ++c) {
                        if (mask & (1u << c)) ++stats_.shadowDraws;
                        else ++stats_.shadowCulled;
                    }
                }
            }
        }

        // Particle billboards for this frame (drawn inside DrawScene's HDR pass).
        device_->SetParticles(particleAlpha_, particleAlphaCount_, particleAdd_,
                              particleAddCount_);
        particleAlpha_ = particleAdd_ = nullptr; // one frame only
        particleAlphaCount_ = particleAddCount_ = 0;
        // GPU-expanded batches (opt-in emitters) ride the same HDR pass. One
        // forward per record buffer - the device accumulates them too.
        for (u32 g = 0; g < particleGpuGroupCount_; ++g) {
            const GpuParticleGroup& grp = particleGpuGroups_[g];
            device_->SetGpuParticles(grp.buffer, grp.batches, grp.count);
            particleGpuGroups_[g] = {};
        }
        particleGpuGroupCount_ = 0;
        // Shadow map first: it must record before the main pass begins. FULL list
        // (see the culling invariant above); the main pass draws the prefix only.
        device_->DrawShadowPass(view, drawItems_.data(), itemCount);
        device_->ClearBackBuffer(0.018f, 0.018f, 0.022f, 1.0f);
        device_->DrawScene(view, drawItems_.data(), visibleCount);
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

bool Renderer::ReadbackViewportColor(std::vector<u8>& outRGBA, u32& w, u32& h) {
    return device_ && device_->ReadbackViewportColor(outRGBA, w, h);
}

void Renderer::SetGpuProfileEnabled(bool enable) {
    if (device_) device_->SetGpuProfileEnabled(enable);
}
bool Renderer::GpuProfileActive() const {
    return device_ && device_->GpuProfileActive();
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
