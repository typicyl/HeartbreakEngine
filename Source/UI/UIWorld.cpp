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
#include <glm/gtc/matrix_inverse.hpp>

#include <iterator>
#include <limits>
#include <string>
#include <unordered_map>
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

// Render targets already created for a world canvas, keyed on "<guid>@<w>x<h>".
//
// WHY THIS IS NOT THE SAME AS THE INTENTIONAL RESIZE LEAK. Recreating on a SIZE CHANGE
// is a one-off an author triggers by hand. A canvas that is DESTROYED AND RE-CREATED
// re-runs this path every time, and each re-creation used to mint a fresh render target
// that nothing frees (the RHI has no texture destroy) - so the churn grew VRAM without
// bound. Re-adopting the canvas's own target by its stable guid makes a respawn cost
// zero. Process-wide, because it outlives any one scene; cleared with the rest of the
// GPU caches on a project/device switch.
//
// WHAT ACTUALLY CHURNS: a document close/reopen and the editor's undo/redo and
// play-snapshot restore (RestoreSnapshot closes and reopens every open document), which
// is a real per-keystroke path in the UI editor. NOT tag streaming - a UICanvas can only
// exist on a `.hbui` document entity, and documents are resident, never streamed (see
// docs/NarrativeSystem.md). The cache is kept because the editor path alone justifies
// it, and because it is the correct behaviour if that restriction is ever lifted.
std::unordered_map<std::string, rhi::TextureHandle>& TargetCache() {
    static std::unordered_map<std::string, rhi::TextureHandle> c;
    return c;
}

} // namespace

void ClearWorldTargetCache() { TargetCache().clear(); }

rhi::TextureHandle AcquireUITarget(Renderer& renderer, const std::string& key, u32 width,
                                   u32 height) {
    if (width == 0 || height == 0) return {};
    if (!key.empty()) {
        const auto it = TargetCache().find(key);
        if (it != TargetCache().end()) return it->second; // may be the invalid sentinel
    }
    const rhi::TextureHandle rt = renderer.CreateUITarget(width, height);
    // A REFUSAL IS CACHED TOO. Callers ask every frame (the UI editor panel and the
    // world-surface pass both do), and neither backend can free a target - so once
    // the 8-target budget is spent, re-requesting only re-logged the same warning at
    // frame rate, burying every other line in the log. The caller already surfaces
    // the condition in its own note; an invalid handle under the key makes the
    // request happen exactly once per distinct size.
    if (!key.empty()) TargetCache().emplace(key, rt);
    return rt;
}

u32 UITargetCacheCount() {
    // Only the ones that really exist: the map also holds negative entries.
    u32 n = 0;
    for (const auto& [k, h] : TargetCache())
        if (h.IsValid()) ++n;
    return n;
}

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
            // A canvas that has had this exact target before (a shard respawn, an
            // undo/redo) re-adopts it rather than minting one that nothing can free.
            const u64 g = reg.all_of<Guid>(e) ? reg.get<Guid>(e).value : 0ull;
            const std::string key = g != 0 ? std::to_string(g) + "@" + std::to_string(want.x) +
                                                 "x" + std::to_string(want.y)
                                           : std::string();
            const rhi::TextureHandle rt = AcquireUITarget(renderer, key, want.x, want.y);
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
            // Same rule for a canvas inside an open `.hbui` document. Without
            // this the canvas is spared by the Replace sweep and its page is
            // not: the quad is destroyed, recreated one frame later by the loop
            // above, and the old bindless render-target slot is leaked each time
            // (see the intentional-leak note on recreate). The membership also
            // makes DocumentSet::Close reap the page with its canvas.
            if (const UIDocMember* m = reg.try_get<UIDocMember>(e))
                reg.emplace<UIDocMember>(surf, *m);
        }

        // Keep the page's material + size in sync with the canvas each frame
        // (cheap; all plain member writes).
        // A page whose SCREEN is closed must not keep drawing. Layout and picking
        // are already gated on the same test; without this the quad stayed in the
        // draw list showing a render target nothing writes any more (a black
        // rectangle floating where a popped screen's diegetic page used to be).
        const bool shown =
            c.visible && CanvasAncestryActive(reg, e, scene.EditorView() &&
                                                          scene.UIAuthoringView());
        if (MeshInstance* mi = reg.try_get<MeshInstance>(c.surface)) {
            mi->mesh = shown ? SharedPageMesh(renderer) : rhi::MeshHandle{};
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

PagePick PickWorldPage(Scene& scene, const glm::vec3& origin, const glm::vec3& dir,
                       f32 maxT, UIContext* ctx, entt::entity wallEntity) {
    PagePick best;
    auto& reg = scene.Registry();
    // Tie-break state (see the contract in UIWorld.h): a page at the SAME distance
    // as the current best wins only by a higher sortOrder, then by a lower entity
    // id. Nothing here may depend on the order the UISurface view happens to yield.
    int bestSort = 0;
    const bool editorForced = scene.EditorView() && scene.UIAuthoringView();

    // "Is the thing the occlusion ray hit this page's OWN housing?" A page mounted
    // on a prop that has a collider (a terminal, a tablet, a bezel) is BEHIND that
    // prop's front face by construction, so without this the page can never be
    // picked. Mirrors the candidate self-exemption the object half already has.
    const auto isOwnHousing = [&](entt::entity canvas, entt::entity surface) {
        if (wallEntity == entt::null || !reg.valid(wallEntity)) return false;
        if (wallEntity == canvas || wallEntity == surface) return true;
        int depth = 0;
        const Parent* p0 = reg.try_get<Parent>(canvas);
        for (entt::entity cur = (p0 && reg.valid(p0->entity)) ? p0->entity : entt::null;
             cur != entt::null && depth < 64; ++depth) {
            if (cur == wallEntity) return true;
            const Parent* pp = reg.try_get<Parent>(cur);
            cur = (pp && reg.valid(pp->entity)) ? pp->entity : entt::null;
        }
        return false;
    };

    // Cache bookkeeping: stamp every surface we touch this call so entries for
    // pages that went away can be dropped WITHOUT the blunt "clear everything"
    // that used to re-invert every live page on any UI structure change.
    u64 stamp = 0;
    u32 seen = 0;
    if (ctx) stamp = ++ctx->surfaceInvStamp;

    for (const entt::entity e : reg.view<UISurface>()) {
        const UISurface& s = reg.get<UISurface>(e);
        const UICanvas* c = reg.valid(s.canvas) ? reg.try_get<UICanvas>(s.canvas) : nullptr;
        if (!c || !c->worldSpace || !c->visible) continue;
        // A page whose SCREEN was popped is inert, like every widget that screen
        // owns. The canvas walk starts AT the canvas, so nothing above it was
        // gated before this - a diegetic button under the Settings root stayed
        // clickable with Settings closed.
        if (!CanvasAncestryActive(reg, s.canvas, editorForced)) continue;

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
            xf.stamp = stamp;
            ++seen;
            invM = xf.inv;
        } else {
            invM = glm::inverse(M);
        }

        // PLANE NORMAL, done properly. `M * (0,1,0,0)` is only parallel to the
        // page normal when the composed matrix has no shear; a page mounted on a
        // non-uniformly scaled, rotated parent solved a DIFFERENT plane and picked
        // offset from where it was drawn. The inverse-transpose is the normal
        // transform; the determinant flip restores the GEOMETRIC (visible) facing
        // when an odd number of scale axes is negative, so a mirrored page's front
        // face is the one you can see rather than the one you cannot.
        glm::vec3 n = glm::transpose(glm::mat3(invM)) * glm::vec3(0.0f, 1.0f, 0.0f);
        const f32 nlen2 = glm::dot(n, n);
        if (nlen2 < 1e-20f) continue; // degenerate (zero-scaled) page
        n *= glm::inversesqrt(nlen2);
        if (glm::determinant(glm::mat3(M)) < 0.0f) n = -n;

        const glm::vec3 p0 = glm::vec3(M[3]);
        const f32 denom = glm::dot(dir, n);
        if (denom > -1e-6f) continue; // back side / edge-on: the page is inert
        const f32 t = glm::dot(p0 - origin, n) / denom;
        if (t <= 0.0f) continue; // behind the camera
        // Nearest wins, with a DETERMINISTIC tie. Coplanar pages (an overlay bolted
        // to the same wall plane as its base) land on the same `t` to well within
        // float noise; picking "whichever the view yielded first" made the winner
        // depend on creation order, which flips on a document reopen, an undo, or a
        // streamed respawn. Authored sortOrder decides, then the lower entity id.
        constexpr f32 kTieEps = 1e-4f;
        if (best.hit) {
            if (t > best.distance + kTieEps) continue; // strictly farther
            if (t >= best.distance - kTieEps) {        // a tie
                if (c->sortOrder < bestSort) continue;
                if (c->sortOrder == bestSort && s.canvas > best.canvas) continue;
            }
        }
        if (c->interactRange > 0.0f && t > c->interactRange) continue;
        // Occluded by solid world geometry (the caller's horizon). A canvas with
        // `occlude` cleared is deliberately clickable through walls, and a page is
        // never occluded by the prop it is MOUNTED ON.
        if (c->occlude && t > maxT && !isOwnHousing(s.canvas, e)) continue;

        // Hit -> canvas-local unit square -> canvas pixels. Same u=fx / v=fz
        // convention as the page mesh's UVs (a mirrored page mirrors both the
        // rendered texture and this mapping, so they stay in agreement).
        const glm::vec3 hitP = origin + dir * t;
        const glm::vec3 local = glm::vec3(invM * glm::vec4(hitP, 1.0f));
        const f32 u = local.x + 0.5f;
        const f32 v = local.z + 0.5f;
        if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) continue; // missed the page
        const f32 refW = glm::max(c->refWidth, 64.0f);
        const f32 refH = glm::max(c->refHeight, 64.0f);
        best.hit = true;
        best.canvas = s.canvas;
        best.surface = e;
        best.px = {u * refW, v * refH};
        best.distance = t;
        best.point = hitP;
        bestSort = c->sortOrder;
    }

    // Drop cache entries for surfaces that no longer exist. Bounded and cheap:
    // only when the map has grown past twice the live page count.
    if (ctx && ctx->surfaceInv.size() > static_cast<usize>(seen) * 2 + 16) {
        for (auto it = ctx->surfaceInv.begin(); it != ctx->surfaceInv.end();)
            it = (it->second.stamp != stamp) ? ctx->surfaceInv.erase(it) : std::next(it);
    }
    return best;
}

// Shared ray-pick body; `ctx` (optional) caches per-surface inverse matrices so
// a static page skips the glm::inverse each frame.
static void ComputeWorldPointersImpl(Scene& scene, const Camera& camera,
                                     glm::vec2 pointerNorm, PointerState& out,
                                     UIContext* ctx) {
    out.worldCanvasPx.clear();
    // BOTH sides are guarded. Only the negative sentinel used to be: a pointer
    // past 1.0 (the cursor outside the editor's letterboxed Game image, or a stale
    // external value) unprojected happily and picked pages off-screen.
    if (pointerNorm.x < 0.0f || pointerNorm.y < 0.0f || pointerNorm.x > 1.0f ||
        pointerNorm.y > 1.0f)
        return;

    glm::vec3 origin(0.0f), dir(0.0f);
    camera.ScreenRay(pointerNorm, origin, dir);

    const PagePick p = PickWorldPage(scene, origin, dir, std::numeric_limits<f32>::max(), ctx);
    if (p.hit) out.worldCanvasPx[static_cast<u32>(p.canvas)] = p.px;
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
        const auto push = [&](glm::vec3 p, f32 u, f32 v, u32 tex) {
            rhi::ParticleVertex pv;
            pv.x = p.x; pv.y = p.y; pv.z = p.z;
            pv.u = u; pv.v = v;
            pv.r = wt.color.r; pv.g = wt.color.g; pv.b = wt.color.b; pv.a = wt.color.a;
            pv.texIndex = tex; // per-glyph atlas page (paged/fallback glyph atlas)
            outAlpha.push_back(pv);
        };
        for (const GlyphQuad& q : quads) {
            const glm::vec3 tl = place(q.x0, q.y0), tr = place(q.x1, q.y0);
            const glm::vec3 bl = place(q.x0, q.y1), br = place(q.x1, q.y1);
            push(tl, q.u0, q.v0, q.atlas); push(tr, q.u1, q.v0, q.atlas); push(br, q.u1, q.v1, q.atlas);
            push(tl, q.u0, q.v0, q.atlas); push(br, q.u1, q.v1, q.atlas); push(bl, q.u0, q.v1, q.atlas);
        }
    }
}

} // namespace hbe::ui
