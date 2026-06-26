// UI/UISystem.h - in-game UI (Unity-style canvases + RectTransform layout).
//
// Entities with a UICanvas component root UI trees; UIElement entities lay
// out hierarchically inside them (Parent component) with Unity RectTransform
// semantics (anchorMin/anchorMax/pivot). Elements without a canvas ancestor
// use the project-wide canvas configuration (legacy scenes keep working).
// Everything renders through the RHI's UI overlay pass (textured,
// alpha-blended triangles): TTF text via the shared FontAtlas, images/panels
// via bindless textures, buttons with hover/click, and progress bars (linear
// or radial). Each frame the engine:
//   1. UpdateInteraction - BEFORE scripts: hit-tests buttons against the
//      pointer so scripts read fresh hovered/clicked state.
//   2. BuildVertices - AFTER scripts: emits the overlay triangles.
#pragma once

#include "Core/Types.h"
#include "RHI/RHI.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <filesystem>
#include <vector>

namespace hbe {

class Scene;
class Input;
class Renderer;
struct UIElement;

namespace ui {

// How the reference canvas maps to the output (Unity CanvasScaler-style).
enum class ScaleMode : u8 {
    Stretch = 0,     // canvas fills the target (distorts on aspect mismatch)
    MatchHeight = 1, // canvas height = reference; width follows target aspect
    PixelPerfect = 2 // one canvas unit = one target pixel
};

struct CanvasConfig {
    ScaleMode mode = ScaleMode::MatchHeight;
    f32 refWidth = rhi::kUICanvasWidth;
    f32 refHeight = rhi::kUICanvasHeight;
};

// Effective canvas size for a target under `config`.
glm::vec2 EffectiveCanvas(const CanvasConfig& config, glm::vec2 targetSize);

// An axis-aligned rectangle in canvas units (y-down, origin top-left).
struct Rect {
    f32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    glm::vec2 Min() const { return {x0, y0}; }
    glm::vec2 Size() const { return {x1 - x0, y1 - y0}; }
    bool Contains(glm::vec2 p) const {
        return p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1;
    }
};

// RectTransform math (Unity semantics). ComputeElementRect resolves an
// element inside its parent's rect; SolveElementFromRect back-solves
// offset/size so the element lands exactly on `desired` (editor dragging).
Rect ComputeElementRect(const UIElement& el, const Rect& parent);
void SolveElementFromRect(UIElement& el, const Rect& parent, const Rect& desired);

// One laid-out element. `canvas` is the effective canvas size of the tree the
// element belongs to (needed to map its rect to NDC/screen).
struct LayoutItem {
    entt::entity entity = entt::null;
    Rect rect;        // resolved, canvas units
    Rect parentRect;  // what the element laid out inside (editor dragging)
    glm::vec2 canvas{0.0f};
};

// Lays out every visible UI tree in draw order: legacy (canvas-less) elements
// first on the project canvas, then UICanvas trees by ascending sortOrder
// (depth-first, parents before children).
void LayoutUI(Scene& scene, glm::vec2 targetSize, const CanvasConfig& legacyConfig,
              std::vector<LayoutItem>& out);

// `pointerNorm` = pointer in normalized target coords (0..1; negative = none).
void UpdateInteraction(Scene& scene, const Input& input, glm::vec2 pointerNorm,
                       glm::vec2 targetSize, const CanvasConfig& config);

void BuildVertices(Scene& scene, Renderer& renderer,
                   const std::filesystem::path& assetsDir, glm::vec2 targetSize,
                   const CanvasConfig& config, std::vector<rhi::UIVertex>& out);

// Drops cached texture resolutions (call when assets change / project switches).
void ClearTextureCache(Scene* scene);

} // namespace ui
} // namespace hbe
