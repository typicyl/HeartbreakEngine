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

#include <glm/glm.hpp>

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

// Ray-picks every world canvas: casts the camera ray under `pointerNorm` (0..1,
// y-down; negative = no pointer) against each UISurface page (FRONT face only -
// pointing at the page's back does nothing), converting hits to canvas pixels in
// `out.worldCanvasPx`. Misses are simply absent. Call before UpdateInteraction.
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
