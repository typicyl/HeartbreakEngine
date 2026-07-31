// UI/UIWorld.h - world-space ("physical") UI canvases: the surface manager.
//
// A UICanvas with worldSpace set renders to a per-canvas texture, displayed on
// a hidden auto-managed LIT quad entity (UISurface) parented under the canvas -
// paper-like: shaded by scene lighting, occluded by geometry, tone-mapped with
// the world. The page lies in the canvas entity's local XZ plane facing +Y;
// rotate the canvas entity to mount it, parent it under an object (a notebook)
// to follow it. All 2D authoring (panels, widgets, animators, tokens, the
// UIManager) works unchanged - only the destination differs.
#pragma once

#include "Core/Types.h"
#include "UI/UISystem.h" // PointerState

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <string>

namespace hbe {

class Scene;
class Renderer;
class Camera;

namespace ui {

// Per-frame upkeep: for every visible worldSpace UICanvas ensure (a) its render
// target exists at the requested size (rtWidth/rtHeight, 0 = refWidth/refHeight;
// recreated when the request changes - the old bindless slot is intentionally
// leaked, matching the engine-wide no-free bindless policy) and (b) its hidden
// lit quad surface entity exists and tracks worldWidth/aspect/emissive. Prunes
// surfaces whose canvas died or stopped being worldSpace. Call before
// BuildVertices (the batches reference canvas.rtTexture).
void UpdateWorldSurfaces(Scene& scene, Renderer& renderer);

// Acquires (or RE-ADOPTS) a UI render target under an arbitrary cache key.
// Invalid handle = unsupported backend (GL has no CreateUITarget) or the RHI's
// per-process target budget is exhausted.
//
// This is the ONE place UI render targets are minted, and it is exposed because a
// second consumer exists: the dedicated `.hbui` editor's authoring canvas. The
// budget matters - BOTH backends cap at kMaxUITargets (8) and neither can FREE a
// target (the engine-wide no-free bindless policy), so every distinct key costs a
// permanent slot. Keys are "<guid>@<w>x<h>" for world canvases and
// "uiedit@<w>x<h>" for the editor canvas, so the whole budget is visible in one
// map. Callers must NOT key on a size that follows a resizable window.
rhi::TextureHandle AcquireUITarget(Renderer& renderer, const std::string& key,
                                  u32 width, u32 height);

// Number of distinct UI targets this process has minted through AcquireUITarget
// (the editor shows it against the RHI's cap of 8). Not the RHI's own count: a
// target created before a ClearWorldTargetCache still occupies its slot.
u32 UITargetCacheCount();

// Drops the process-wide world-canvas render-target cache (the map that lets a canvas
// respawned by tag streaming re-adopt its own target instead of minting a new one that
// the RHI can never free). Call on a project switch / device recreate, alongside
// scene::ClearInstantiateCaches - the handles it holds belong to the old device.
void ClearWorldTargetCache();

// The single winning world page under a ray. `hit` false = the ray touched no
// page (or every page it touched was occluded / out of range / back-facing).
struct PagePick {
    bool hit = false;
    entt::entity canvas = entt::null;  // the UICanvas the page belongs to
    entt::entity surface = entt::null; // its UISurface quad
    glm::vec2 px{0.0f};                // hit point in that canvas's pixel space
    f32 distance = 0.0f;               // along the ray, from `origin`
    glm::vec3 point{0.0f};             // world-space hit point
};

// THE world-page picker. Casts `origin + dir * t` against every visible
// world-space UISurface and returns the NEAREST front-facing hit - exactly one
// winner, never a set.
//
// Correctness contract (each clause is a bug this replaced):
//   * NEAREST WINS, and a TIE IS DETERMINISTIC. Overlapping pages no longer all
//     receive the pointer; two pages at the SAME distance (an overlay bolted to
//     the same wall plane as its base page) break on UICanvas::sortOrder, then on
//     the lower canvas entity id - never on entt pool order, which changes on a
//     document reopen, an undo, or a streamed respawn.
//   * OCCLUDED PAGES LOSE. `maxT` is the caller's occlusion horizon (the distance
//     to the closest solid world collider along the same ray, plus a skin); a page
//     farther than that is not a candidate. A canvas with `occlude` cleared ignores
//     `maxT` and is picked through walls on purpose. Passing FLT_MAX = no occlusion.
//     `wallEntity` is WHAT the caller's horizon hit: a page's OWN housing must not
//     occlude it, so a canvas whose entity (or surface, or any ancestor) IS that
//     body ignores `maxT`. Without it, mounting a page on a prop that carries a
//     collider - a wall terminal, a handheld tablet, a screen recessed behind a
//     bezel - made it permanently, silently unclickable.
//   * A canvas under an INACTIVE UIPanel is inert, exactly like the widgets that
//     screen owns (ui::CanvasAncestryActive).
//   * The plane normal is the INVERSE-TRANSPOSE normal, flipped on a negative
//     determinant, so a page under a sheared (non-uniformly scaled + rotated)
//     parent picks where it is drawn, and a mirrored page's VISIBLE face is the
//     interactive one.
//   * `dir` must be normalised: `distance` is in meters and is compared against
//     occlusion distances and UICanvas::interactRange.
// `ctx` (optional) caches per-surface inverse matrices, recomputed only when the
// world matrix actually changes, and prunes entries for pages that went away.
PagePick PickWorldPage(Scene& scene, const glm::vec3& origin, const glm::vec3& dir,
                       f32 maxT, UIContext* ctx,
                       entt::entity wallEntity = entt::null);

// Ray-picks the world canvases under `pointerNorm` (0..1, y-down; outside that
// range = no pointer) and writes AT MOST ONE entry into `out.worldCanvasPx` - the
// nearest front-facing page. NO occlusion (this entry point has no physics): use
// interact::Pick for the occluding, unified pass. Call before UpdateInteraction.
void ComputeWorldPointers(Scene& scene, const Camera& camera, glm::vec2 pointerNorm,
                          PointerState& out);
// Cached variant: per-surface inverse matrices recomputed only when the world
// transform actually changes (ctx.surfaceInv).
void ComputeWorldPointers(Scene& scene, const Camera& camera, glm::vec2 pointerNorm,
                          PointerState& out, UIContext& ctx);

// 3D text objects (WorldText components): appends unlit glyph quads to the
// frame's alpha-particle batch (depth-tested, tone-mapped with the scene).
// `camRight/camUp` = the camera basis billboarded text faces. Call right after
// particle::BuildVertices, before Renderer::SetParticles.
void AppendWorldText(Scene& scene, Renderer& renderer,
                     const std::filesystem::path& assetsDir, glm::vec3 camRight,
                     glm::vec3 camUp, std::vector<rhi::ParticleVertex>& outAlpha);

} // namespace ui
} // namespace hbe
