// UI/UIWorld.cpp - world-space UI surface upkeep (render target + lit quad).
#include "UI/UIWorld.h"

#include "Assets/MeshGenerator.h"
#include "Core/Log.h"
#include "Renderer/Camera.h"
#include "Renderer/Renderer.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "UI/FontAtlas.h"

#include <entt/entt.hpp>

#include <vector>

namespace hbe::ui {

namespace {

// The shared unit page quad (1m x 1m in local XZ, +Y normal), uploaded once per
// run. The surface entity's Transform scales it to worldWidth x derived height.
rhi::MeshHandle SharedPageMesh(Renderer& renderer) {
    static rhi::MeshHandle s_mesh; // one per process (device outlives scenes)
    if (!s_mesh.IsValid()) {
        MeshData plane = mesh::GeneratePlane(1.0f, 1);
        plane.name = "__uiPage";
        s_mesh = renderer.UploadMesh(plane);
    }
    return s_mesh;
}

// Requested render-target size for a canvas (explicit rt size, else ref size),
// clamped to sane texture bounds.
glm::uvec2 TargetSize(const UICanvas& c) {
    const u32 w = c.rtWidth ? c.rtWidth : static_cast<u32>(glm::max(c.refWidth, 64.0f));
    const u32 h = c.rtHeight ? c.rtHeight : static_cast<u32>(glm::max(c.refHeight, 64.0f));
    return {glm::clamp(w, 64u, 4096u), glm::clamp(h, 64u, 4096u)};
}

} // namespace

void UpdateWorldSurfaces(Scene& scene, Renderer& renderer) {
    auto& reg = scene.Registry();

    // Prune surfaces whose canvas died or stopped being world-space (component
    // removed, worldSpace unchecked, canvas destroyed by a scene swap).
    {
        std::vector<entt::entity> kill;
        for (const entt::entity e : reg.view<UISurface>()) {
            const UISurface& s = reg.get<UISurface>(e);
            const UICanvas* c =
                reg.valid(s.canvas) ? reg.try_get<UICanvas>(s.canvas) : nullptr;
            if (!c || !c->worldSpace) kill.push_back(e);
        }
        for (const entt::entity e : kill) {
            if (reg.valid(e)) reg.destroy(e);
        }
    }

    for (const entt::entity e : reg.view<UICanvas>()) {
        UICanvas& c = reg.get<UICanvas>(e);
        if (!c.worldSpace) continue;

        // (a) Render target at the requested size. Recreate on size change; the
        // old bindless slot is intentionally leaked (engine-wide no-free policy).
        const glm::uvec2 want = TargetSize(c);
        if (!c.rtTexture.IsValid() || c.rtTexW != want.x || c.rtTexH != want.y) {
            const rhi::TextureHandle rt = renderer.CreateUITarget(want.x, want.y);
            if (!rt.IsValid()) continue; // unsupported backend (GL) or out of slots
            c.rtTexture = rt;
            c.rtTexW = want.x;
            c.rtTexH = want.y;
        }

        // (b) The hidden lit quad that displays it, parented under the canvas so
        // it follows the canvas entity's (or its notebook parent's) transform.
        if (c.surface == entt::null || !reg.valid(c.surface) ||
            !reg.all_of<UISurface>(c.surface)) {
            const entt::entity surf = scene.CreateEntity("__uiSurface");
            reg.emplace<UISurface>(surf, UISurface{e});
            reg.emplace<Parent>(surf, Parent{e});
            reg.emplace<Transform>(surf); // identity; scaled below
            MeshInstance mi;
            mi.mesh = SharedPageMesh(renderer);
            c.surface = surf;
            reg.emplace<MeshInstance>(surf, mi);
            // The canvas may live in the PERSISTENT UI scene; its page must
            // survive scene swaps with it or it dangles for a frame.
            if (reg.all_of<Persistent>(e)) reg.emplace<Persistent>(surf);
        }

        // Keep the page's material + size in sync with the canvas each frame
        // (cheap; all plain member writes).
        if (MeshInstance* mi = reg.try_get<MeshInstance>(c.surface)) {
            mi->baseColor = {1.0f, 1.0f, 1.0f, 1.0f};
            mi->metallic = 0.0f;
            mi->roughness = 0.9f; // matte paper
            mi->albedoTexture = c.rtTexture; // LIT like a printed page
            // Optional readability floor in dark scenes; 0 keeps pure paper.
            mi->emissiveTexture = c.emissive > 0.0f ? c.rtTexture : rhi::TextureHandle{};
            mi->emissiveColor = glm::vec3(c.emissive);
            // OPAQUE + PainterlyExempt, deliberately NOT Transparent: the opaque
            // pass encodes the painterly/censor exempt mask in alpha (MeshPBR
            // mbits), which MaterialFlag_Transparent would override with coverage
            // - a transparent page gets Kuwahara-smeared and/or censor-blacked.
            // Crisp UI text needs the exemption; empty canvas regions therefore
            // render black, so author a full-bleed background (paper) on the
            // canvas. NoShadow avoids near-coplanar acne with the notebook.
            mi->materialFlags =
                rhi::MaterialFlag_PainterlyExempt | rhi::MaterialFlag_NoShadow;
        }
        if (Transform* t = reg.try_get<Transform>(c.surface)) {
            const f32 refW = glm::max(c.refWidth, 64.0f);
            const f32 refH = glm::max(c.refHeight, 64.0f);
            const f32 width = glm::max(c.worldWidth, 0.01f);
            t->scale = {width, 1.0f, width * refH / refW};
        }
    }
}

// Shared ray-pick body; `ctx` (optional) caches per-surface inverse matrices so
// a static page skips the glm::inverse each frame.
static void ComputeWorldPointersImpl(Scene& scene, const Camera& camera,
                                     glm::vec2 pointerNorm, PointerState& out,
                                     UIContext* ctx) {
    out.screenNorm = pointerNorm;
    out.worldCanvasPx.clear();
    if (pointerNorm.x < 0.0f || pointerNorm.y < 0.0f) return;

    glm::vec3 origin(0.0f), dir(0.0f);
    camera.ScreenRay(pointerNorm, origin, dir);

    auto& reg = scene.Registry();
    for (const entt::entity e : reg.view<UISurface>()) {
        const UISurface& s = reg.get<UISurface>(e);
        const UICanvas* c = reg.valid(s.canvas) ? reg.try_get<UICanvas>(s.canvas) : nullptr;
        if (!c || !c->worldSpace || !c->visible) continue;

        // The page is the unit XZ plane under the surface's world matrix (its
        // Transform.scale carries worldWidth x height). Front face = local +Y.
        const glm::mat4 M = scene.WorldMatrix(e);
        glm::mat4 invM;
        if (ctx) {
            UIContext::SurfaceXf& xf = ctx->surfaceInv[static_cast<u32>(e)];
            if (xf.world != M) { // recompute the inverse only on movement
                xf.world = M;
                xf.inv = glm::inverse(M);
            }
            invM = xf.inv;
        } else {
            invM = glm::inverse(M);
        }
        const glm::vec3 n = glm::normalize(glm::vec3(M * glm::vec4(0, 1, 0, 0)));
        const glm::vec3 p0 = glm::vec3(M[3]);
        const f32 denom = glm::dot(dir, n);
        if (denom > -1e-6f) continue; // back side / edge-on: the page is inert
        const f32 t = glm::dot(p0 - origin, n) / denom;
        if (t <= 0.0f) continue; // behind the camera

        // Hit -> canvas-local unit square -> canvas pixels. Same u=fx / v=fz
        // convention as the page mesh's UVs (flip BOTH if the page is authored
        // mirrored - see the plane UVs in SharedPageMesh).
        const glm::vec3 local = glm::vec3(invM * glm::vec4(origin + dir * t, 1.0f));
        const f32 u = local.x + 0.5f;
        const f32 v = local.z + 0.5f;
        if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) continue; // missed the page
        const f32 refW = glm::max(c->refWidth, 64.0f);
        const f32 refH = glm::max(c->refHeight, 64.0f);
        out.worldCanvasPx[static_cast<u32>(s.canvas)] = {u * refW, v * refH};
    }
}

void ComputeWorldPointers(Scene& scene, const Camera& camera, glm::vec2 pointerNorm,
                          PointerState& out) {
    ComputeWorldPointersImpl(scene, camera, pointerNorm, out, nullptr);
}

void ComputeWorldPointers(Scene& scene, const Camera& camera, glm::vec2 pointerNorm,
                          PointerState& out, UIContext& ctx) {
    ComputeWorldPointersImpl(scene, camera, pointerNorm, out, &ctx);
}

void AppendWorldText(Scene& scene, Renderer& renderer,
                     const std::filesystem::path& assetsDir, glm::vec3 camRight,
                     glm::vec3 camUp, std::vector<rhi::ParticleVertex>& outAlpha) {
    auto& reg = scene.Registry();
    // Layout resolution: glyphs are laid out at this pixel size and scaled to
    // meters, so text quality tracks the atlas bake regardless of world size.
    constexpr f32 kLayoutPx = 64.0f;

    static thread_local std::vector<GlyphQuad> quads;
    for (const entt::entity e : reg.view<WorldText>()) {
        const WorldText& wt = reg.get<WorldText>(e);
        if (wt.text.empty() || wt.size <= 0.0f || wt.color.a <= 0.0f) continue;

        FontAtlas& font = ResolveFont(renderer, assetsDir, wt.font);
        if (!font.Ready()) font.Initialize(renderer); // idempotent (system font)
        if (!font.Ready()) continue;

        quads.clear();
        f32 w = 0.0f, h = 0.0f;
        font.Layout(wt.text, kLayoutPx, quads, w, h);
        if (quads.empty()) continue;

        const f32 s = wt.size / kLayoutPx; // layout px -> meters
        const glm::mat4 M = scene.WorldMatrix(e);
        const glm::vec3 origin = glm::vec3(M[3]);

        // Layout is y-down from a top-left origin; center the block and flip y
        // up. Oriented text lives in the entity's local XY plane facing +Z (a
        // standing sign); billboard text uses the camera basis at the position.
        const auto place = [&](f32 px, f32 py) -> glm::vec3 {
            const f32 lx = (px - w * 0.5f) * s;
            const f32 ly = (h * 0.5f - py) * s;
            if (wt.billboard) return origin + camRight * lx + camUp * ly;
            return glm::vec3(M * glm::vec4(lx, ly, 0.0f, 1.0f));
        };
        const auto push = [&](glm::vec3 p, f32 u, f32 v) {
            rhi::ParticleVertex pv;
            pv.x = p.x; pv.y = p.y; pv.z = p.z;
            pv.u = u; pv.v = v;
            pv.r = wt.color.r; pv.g = wt.color.g; pv.b = wt.color.b; pv.a = wt.color.a;
            pv.texIndex = font.TextureIndex();
            outAlpha.push_back(pv);
        };
        for (const GlyphQuad& q : quads) {
            const glm::vec3 tl = place(q.x0, q.y0), tr = place(q.x1, q.y0);
            const glm::vec3 bl = place(q.x0, q.y1), br = place(q.x1, q.y1);
            push(tl, q.u0, q.v0); push(tr, q.u1, q.v0); push(br, q.u1, q.v1);
            push(tl, q.u0, q.v0); push(br, q.u1, q.v1); push(bl, q.u0, q.v1);
        }
    }
}

} // namespace hbe::ui
