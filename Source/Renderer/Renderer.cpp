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
    // morphCount too: blendshape weights ride in the ObjectCB, and the run HEAD's
    // ObjectCB serves the whole group. An UNSKINNED morphing mesh passes the
    // boneCount test, so without this two blendshaped props with different
    // expressions instanced together and every one of them silently wore the first
    // one's face.
    return it.boneCount == 0 && it.morphCount == 0 && !it.paintColorTexture.IsValid() &&
           !(it.materialFlags &
             (rhi::MaterialFlag_Transparent | rhi::MaterialFlag_TerrainSplat));
}

// Looser instanceability for the SHADOW-ONLY tail. The depth-only shadow pass ignores
// material AND paint (it rasterizes geometry+transform only), so - unlike the scene-pass
// Instanceable above - a PAINTED caster CAN share a run. Only per-instance GEOMETRY
// deformation (skinning/morph) or the special transparent/splat passes block it. Grouping
// the tail by mesh.id alone (below) with this predicate collapses a cluster of the same
// mesh with per-object paint (e.g. hundreds of ground-decal planes) from N shadow draws
// into ONE - the single biggest driver of the shadow draw-call count on dense scenes.
bool ShadowInstanceable(const rhi::DrawItem& it) {
    return it.boneCount == 0 && it.morphCount == 0 &&
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

bool Renderer::UpdateMesh(rhi::MeshHandle handle, const MeshData& mesh) {
    if (!device_) return false;
    // GATED ON SUCCESS. This used to refresh unconditionally, so an upload the device
    // refused left the frustum culler holding bounds for geometry that was never on the
    // GPU - the mesh would cull against its new shape while drawing its old one.
    if (!device_->UpdateMesh(handle, mesh)) return false;
    if (handle.IsValid()) { // geometry changed (sculpting) -> refresh the cull bounds
        glm::vec3 bmin, bmax;
        ComputeBounds(mesh, bmin, bmax);
        meshBounds_[handle.id] = {(bmin + bmax) * 0.5f, (bmax - bmin) * 0.5f};
    }
    return true;
}

rhi::MeshHandle Renderer::UploadMeshReserved(const MeshData& mesh, u32 vertexCapacity,
                                             u32 indexCapacity) {
    if (!device_) return {};
    const rhi::MeshHandle h = device_->CreateMeshReserved(mesh, vertexCapacity, indexCapacity);
    if (h.IsValid()) {
        glm::vec3 bmin, bmax;
        ComputeBounds(mesh, bmin, bmax);
        meshBounds_[h.id] = {(bmin + bmax) * 0.5f, (bmax - bmin) * 0.5f};
    }
    return h;
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

void Renderer::DestroyMesh(rhi::MeshHandle handle) {
    if (!device_ || !handle.IsValid()) return;
    // A base mesh owns its distance-LOD handles: free them together, so a streaming reclaim of
    // the base leaves no orphaned LOD GPU buffers and no stale LOD-table entry (whose base id
    // could later be recycled by a different mesh and wrongly inherit these levels).
    if (const auto it = meshLods_.find(handle.id); it != meshLods_.end()) {
        for (const LodEntry& e : it->second) {
            if (e.handle.IsValid()) {
                device_->DestroyMesh(e.handle);
                meshBounds_.erase(e.handle.id);
            }
        }
        meshLods_.erase(it);
    }
    device_->DestroyMesh(handle);
    meshBounds_.erase(handle.id); // drop the culling bounds recorded for this id
}

void Renderer::RegisterMeshLods(rhi::MeshHandle base, const std::vector<rhi::MeshHandle>& lods) {
    if (!base.IsValid() || lods.empty()) return;
    // Thresholds on the FOV-normalized screen metric (see the selection block): the projected
    // fraction of the viewport half-height an object covers. LOD1 kicks in below kFirstSwitch,
    // each further level at half the previous. Resolution- AND fov-independent (a screen
    // fraction, so a zoomed/scoped narrow-fov view keeps near-full detail), honoring the
    // native-res "keep quality" mandate. 0.242 == the old radius/dist 0.14 preserved at the
    // default 60-deg fovY (x 1/tan(30deg) == 1.732), so default-fov behavior is unchanged.
    constexpr f32 kFirstSwitch = 0.242f;
    constexpr f32 kFalloff = 0.5f;
    std::vector<LodEntry> entries;
    entries.reserve(lods.size());
    f32 threshold = kFirstSwitch;
    for (rhi::MeshHandle h : lods) {
        if (!h.IsValid()) continue;
        entries.push_back({h, threshold});
        threshold *= kFalloff;
    }
    if (!entries.empty()) meshLods_[base.id] = std::move(entries);
}

void Renderer::DestroyTexture(rhi::TextureHandle handle) {
    if (device_) device_->DestroyTexture(handle);
}

bool Renderer::SupportsResourceReclaim() const {
    return device_ && device_->SupportsResourceReclaim();
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
        // view.timeSeconds is the scene's shared animation clock (advanced by water::Update,
        // written by MakeView) so the GPU waves/sky/ripples and CPU water buoyancy use ONE
        // time source and floating objects sit exactly on the rendered surface.
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

        // --- Distance LODs: swap each drawable to the coarsest level whose projected screen
        // coverage still justifies it. Runs AFTER the cull for two reasons: the cull sees stable
        // LOD0 bounds (near-identical to any LOD, and the full extent is the safe choice), and the
        // two halves get treated differently. The VISIBLE prefix [0,visibleCount) is what the main
        // AND shadow passes read at the SAME index, so it gets one handle (camera LOD) - the
        // index-coupling invariant above is preserved. The off-screen SUFFIX is shadow-only, so it
        // is floored one level finer than the coarsest, or a far off-screen caster would throw its
        // faceted low-poly silhouette into a near shadow. Near geometry keeps LOD0 (native-res
        // detail preserved). Skinned/morph and LOD-less draws are skipped -> cheap no-op when no
        // LODs exist. The metric is FOV-normalized (screen fraction, not raw angular size) so a
        // zoomed/scoped narrow-fov view keeps near-full detail.
        if (meshLodEnabled_ && !meshLods_.empty() && itemCount > 0) {
            const glm::vec3 camPos = view.cameraPos;
            const f32 invTanHalfFov = 1.0f / glm::max(glm::tan(0.5f * camera_.FovY()), 1e-4f);
            const auto pickLod = [&](rhi::DrawItem& it, bool shadowOnly) {
                if (it.boneCount > 0 || it.morphCount > 0) return;
                const auto lit = meshLods_.find(it.mesh.id);
                if (lit == meshLods_.end()) return;
                const auto bit = meshBounds_.find(it.mesh.id);
                if (bit == meshBounds_.end()) return;
                const glm::vec3 worldCenter =
                    glm::vec3(it.transform * glm::vec4(bit->second.center, 1.0f));
                const f32 scale = glm::max(glm::length(glm::vec3(it.transform[0])),
                                           glm::max(glm::length(glm::vec3(it.transform[1])),
                                                    glm::length(glm::vec3(it.transform[2]))));
                const f32 radius = glm::length(bit->second.extent) * scale;
                const f32 dist = glm::length(camPos - worldCenter);
                const f32 screen = (radius / glm::max(dist, 1e-3f)) * invTanHalfFov;
                const u32 total = static_cast<u32>(lit->second.size());
                const u32 n = (shadowOnly && total > 1) ? total - 1 : total;
                rhi::MeshHandle sel{};
                for (u32 k = 0; k < n; ++k) {
                    if (screen < lit->second[k].switchBelow) sel = lit->second[k].handle;
                    else break;
                }
                if (sel.IsValid()) it.mesh = sel;
            };
            for (u32 i = 0; i < visibleCount; ++i) pickLod(drawItems_[i], /*shadowOnly*/ false);
            for (u32 i = visibleCount; i < itemCount; ++i) pickLod(drawItems_[i], /*shadowOnly*/ true);
        }

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
            if (!ShadowInstanceable(drawItems_[i])) {
                ++i;
                continue;
            }
            // Group by MESH ALONE (not SameMaterial): the tail is shadow-only and the
            // depth pass is material/paint-agnostic, so same-mesh casters with different
            // materials/paint fold into ONE instanced shadow draw instead of N singles.
            u32 runEnd = i + 1;
            while (runEnd < itemCount && ShadowInstanceable(drawItems_[runEnd]) &&
                   drawItems_[runEnd].mesh.id == drawItems_[i].mesh.id)
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
