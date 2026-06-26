// UI/UISystem.cpp
#include "UI/UISystem.h"

#include "Assets/AssetLoader.h"
#include "Core/Input.h"
#include "Renderer/Renderer.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "UI/FontAtlas.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace hbe::ui {

namespace {

// Canvas-space -> NDC emission. All Push helpers go through this.
struct Emitter {
    std::vector<rhi::UIVertex>* out;
    glm::vec2 canvas;

    rhi::UIVertex Vertex(f32 x, f32 y, f32 u, f32 v, const glm::vec4& c, u32 tex) const {
        rhi::UIVertex vert;
        vert.x = x / canvas.x * 2.0f - 1.0f;
        vert.y = 1.0f - y / canvas.y * 2.0f; // canvas y-down -> NDC y-up
        vert.u = u;
        vert.v = v;
        vert.r = c.r; vert.g = c.g; vert.b = c.b; vert.a = c.a;
        vert.texIndex = tex;
        return vert;
    }

    void Quad(const Rect& r, const glm::vec4& color, u32 tex = 0, f32 u0 = 0,
              f32 v0 = 0, f32 u1 = 1, f32 v1 = 1) const {
        const rhi::UIVertex a = Vertex(r.x0, r.y0, u0, v0, color, tex);
        const rhi::UIVertex b = Vertex(r.x1, r.y0, u1, v0, color, tex);
        const rhi::UIVertex c = Vertex(r.x1, r.y1, u1, v1, color, tex);
        const rhi::UIVertex d = Vertex(r.x0, r.y1, u0, v1, color, tex);
        out->push_back(a); out->push_back(b); out->push_back(c);
        out->push_back(a); out->push_back(c); out->push_back(d);
    }

    // TTF text aligned within `rect` (h/v alignment). `sizePx` = glyph height.
    void Text(const std::string& text, const Rect& rect, UIElement::HAlign ha,
              UIElement::VAlign va, f32 sizePx, const glm::vec4& color,
              FontAtlas& font) const {
        if (!font.Ready() || text.empty()) return;
        static thread_local std::vector<GlyphQuad> quads;
        f32 w = 0, h = 0;
        font.Layout(text, sizePx, quads, w, h);
        // Inset left/right/top/bottom alignments a touch so glyphs aren't flush
        // against the element edge; centered alignment uses no inset.
        const f32 pad = 4.0f;
        f32 ox;
        switch (ha) {
            case UIElement::HAlign::Left:  ox = rect.x0 + pad; break;
            case UIElement::HAlign::Right: ox = rect.x1 - w - pad; break;
            default:                       ox = (rect.x0 + rect.x1) * 0.5f - w * 0.5f; break;
        }
        f32 oy;
        switch (va) {
            case UIElement::VAlign::Top:    oy = rect.y0 + pad; break;
            case UIElement::VAlign::Bottom: oy = rect.y1 - h - pad; break;
            default:                        oy = (rect.y0 + rect.y1) * 0.5f - h * 0.5f; break;
        }
        const glm::vec2 origin(ox, oy);
        for (const GlyphQuad& g : quads) {
            const rhi::UIVertex a = Vertex(origin.x + g.x0, origin.y + g.y0, g.u0, g.v0,
                                           color, font.TextureIndex());
            const rhi::UIVertex b = Vertex(origin.x + g.x1, origin.y + g.y0, g.u1, g.v0,
                                           color, font.TextureIndex());
            const rhi::UIVertex c = Vertex(origin.x + g.x1, origin.y + g.y1, g.u1, g.v1,
                                           color, font.TextureIndex());
            const rhi::UIVertex d = Vertex(origin.x + g.x0, origin.y + g.y1, g.u0, g.v1,
                                           color, font.TextureIndex());
            out->push_back(a); out->push_back(b); out->push_back(c);
            out->push_back(a); out->push_back(c); out->push_back(d);
        }
    }

    // Radial fill "wheel": an annular sweep from 12 o'clock, clockwise.
    void Wheel(glm::vec2 center, f32 outerR, f32 innerR, f32 fraction,
               const glm::vec4& color) const {
        fraction = glm::clamp(fraction, 0.0f, 1.0f);
        if (fraction <= 0.0f) return;
        const int kSegments = 64;
        const int steps = glm::max(1, static_cast<int>(kSegments * fraction));
        const f32 sweep = fraction * glm::two_pi<f32>();
        for (int i = 0; i < steps; ++i) {
            const f32 a0 = -glm::half_pi<f32>() + sweep * (static_cast<f32>(i) / steps);
            const f32 a1 = -glm::half_pi<f32>() + sweep * (static_cast<f32>(i + 1) / steps);
            const glm::vec2 d0(std::cos(a0), std::sin(a0));
            const glm::vec2 d1(std::cos(a1), std::sin(a1));
            const rhi::UIVertex i0 = Vertex(center.x + d0.x * innerR,
                                            center.y + d0.y * innerR, 0, 0, color, 0);
            const rhi::UIVertex o0 = Vertex(center.x + d0.x * outerR,
                                            center.y + d0.y * outerR, 0, 0, color, 0);
            const rhi::UIVertex i1 = Vertex(center.x + d1.x * innerR,
                                            center.y + d1.y * innerR, 0, 0, color, 0);
            const rhi::UIVertex o1 = Vertex(center.x + d1.x * outerR,
                                            center.y + d1.y * outerR, 0, 0, color, 0);
            out->push_back(i0); out->push_back(o0); out->push_back(o1);
            out->push_back(i0); out->push_back(o1); out->push_back(i1);
        }
    }
};

// Resolves an element's texture ref to a bindless index (cached per element;
// reset textureResolved to re-resolve after edits).
u32 ResolveTexture(UIElement& el, Renderer& renderer,
                   const std::filesystem::path& assetsDir) {
    if (el.textureResolved) return el.textureIndexCache;
    el.textureResolved = true;
    el.textureIndexCache = 0;
    if (!el.texture.empty() && !assetsDir.empty()) {
        const rhi::TextureHandle handle =
            assets::LoadTexture(renderer, assetsDir / el.texture);
        el.textureIndexCache = handle.index;
    }
    return el.textureIndexCache;
}

} // namespace

glm::vec2 EffectiveCanvas(const CanvasConfig& config, glm::vec2 target) {
    target = glm::max(target, glm::vec2(1.0f));
    switch (config.mode) {
        case ScaleMode::PixelPerfect:
            return target;
        case ScaleMode::MatchHeight:
            return {config.refHeight * (target.x / target.y), config.refHeight};
        case ScaleMode::Stretch:
        default:
            return {config.refWidth, config.refHeight};
    }
}

Rect ComputeElementRect(const UIElement& el, const Rect& parent) {
    if (el.fullscreen) return parent;

    const glm::vec2 pMin = parent.Min();
    const glm::vec2 pSize = parent.Size();
    const glm::vec2 regionMin = pMin + el.anchorMin * pSize;
    const glm::vec2 regionMax = pMin + el.anchorMax * pSize;
    const glm::vec2 regionSize = glm::max(regionMax - regionMin, glm::vec2(0.0f));

    // Unity RectTransform: size = anchor region + sizeDelta; the pivot point
    // sits at the anchor region's pivot plus the anchored offset.
    const glm::vec2 size = glm::max(regionSize + el.size, glm::vec2(0.0f));
    const glm::vec2 pivotPoint = regionMin + regionSize * el.pivot + el.offset;
    const glm::vec2 mn = pivotPoint - el.pivot * size;
    return {mn.x, mn.y, mn.x + size.x, mn.y + size.y};
}

void SolveElementFromRect(UIElement& el, const Rect& parent, const Rect& desired) {
    if (el.fullscreen) return; // nothing to solve; layout ignores the transform

    const glm::vec2 pMin = parent.Min();
    const glm::vec2 pSize = parent.Size();
    const glm::vec2 regionMin = pMin + el.anchorMin * pSize;
    const glm::vec2 regionMax = pMin + el.anchorMax * pSize;
    const glm::vec2 regionSize = glm::max(regionMax - regionMin, glm::vec2(0.0f));

    const glm::vec2 size = glm::max(desired.Size(), glm::vec2(0.0f));
    el.size = size - regionSize;
    const glm::vec2 pivotPoint = desired.Min() + el.pivot * size;
    el.offset = pivotPoint - (regionMin + regionSize * el.pivot);
}

void LayoutUI(Scene& scene, glm::vec2 targetSize, const CanvasConfig& legacyConfig,
              std::vector<LayoutItem>& out) {
    out.clear();
    auto& reg = scene.Registry();

    // Parent -> children map over everything that can appear in a UI tree.
    std::unordered_map<u32, std::vector<entt::entity>> children;
    for (const entt::entity e : reg.view<Parent>()) {
        const Parent& p = reg.get<Parent>(e);
        if (reg.valid(p.entity)) {
            children[static_cast<u32>(p.entity)].push_back(e);
        }
    }

    // Walks one UI subtree, laying out parents before children (depth-first).
    const auto walk = [&](auto&& self, entt::entity e, const Rect& parentRect,
                          glm::vec2 canvas) -> void {
        Rect rectForChildren = parentRect;
        if (UIElement* el = reg.try_get<UIElement>(e)) {
            LayoutItem item;
            item.entity = e;
            item.parentRect = parentRect;
            item.rect = ComputeElementRect(*el, parentRect);
            item.canvas = canvas;
            rectForChildren = item.rect;
            out.push_back(item);
        }
        if (auto it = children.find(static_cast<u32>(e)); it != children.end()) {
            for (const entt::entity child : it->second) {
                self(self, child, rectForChildren, canvas);
            }
        }
    };

    // True when `e` sits under (or on) an entity carrying a UICanvas.
    const auto underCanvas = [&](entt::entity e) {
        int depth = 0;
        for (entt::entity cur = e; cur != entt::null && depth < 64; ++depth) {
            if (reg.all_of<UICanvas>(cur)) return true;
            const Parent* p = reg.try_get<Parent>(cur);
            cur = (p && reg.valid(p->entity)) ? p->entity : entt::null;
        }
        return false;
    };

    // True when any strict ancestor of `e` carries a UIElement (i.e. `e` is
    // laid out by a walk that started higher up).
    const auto hasElementAncestor = [&](entt::entity e) {
        int depth = 0;
        const Parent* p = reg.try_get<Parent>(e);
        for (entt::entity cur = (p && reg.valid(p->entity)) ? p->entity : entt::null;
             cur != entt::null && depth < 64; ++depth) {
            if (reg.all_of<UIElement>(cur)) return true;
            const Parent* pp = reg.try_get<Parent>(cur);
            cur = (pp && reg.valid(pp->entity)) ? pp->entity : entt::null;
        }
        return false;
    };

    // Legacy canvas-less elements first (drawn under every canvas). Walk from
    // tree roots only (elements with no UI ancestor) so nothing lays out twice.
    {
        const glm::vec2 canvas = EffectiveCanvas(legacyConfig, targetSize);
        const Rect root{0.0f, 0.0f, canvas.x, canvas.y};
        for (const entt::entity e : reg.view<UIElement>()) {
            if (!underCanvas(e) && !hasElementAncestor(e)) {
                walk(walk, e, root, canvas);
            }
        }
    }

    // Canvas trees, ascending sortOrder.
    std::vector<entt::entity> canvases;
    for (const entt::entity e : reg.view<UICanvas>()) {
        if (reg.get<UICanvas>(e).visible) canvases.push_back(e);
    }
    std::sort(canvases.begin(), canvases.end(), [&](entt::entity a, entt::entity b) {
        const int sa = reg.get<UICanvas>(a).sortOrder;
        const int sb = reg.get<UICanvas>(b).sortOrder;
        if (sa != sb) return sa < sb;
        return a < b;
    });
    for (const entt::entity canvasEntity : canvases) {
        const UICanvas& c = reg.get<UICanvas>(canvasEntity);
        CanvasConfig config;
        config.mode = static_cast<ScaleMode>(glm::clamp(c.scaleMode, 0u, 2u));
        config.refWidth = glm::max(c.refWidth, 64.0f);
        config.refHeight = glm::max(c.refHeight, 64.0f);
        const glm::vec2 canvas = EffectiveCanvas(config, targetSize);
        const Rect root{0.0f, 0.0f, canvas.x, canvas.y};
        walk(walk, canvasEntity, root, canvas);
    }
}

void UpdateInteraction(Scene& scene, const Input& input, glm::vec2 pointerNorm,
                       glm::vec2 targetSize, const CanvasConfig& config) {
    auto& reg = scene.Registry();
    const bool hasPointer = pointerNorm.x >= 0.0f && pointerNorm.y >= 0.0f;
    const bool pressed = input.WasMousePressed(MouseButton::Left);

    // Clear, then mark hit buttons from the layout (later items draw on top,
    // but buttons don't occlude each other here - all hits report hover).
    for (const entt::entity e : reg.view<UIElement>()) {
        UIElement& el = reg.get<UIElement>(e);
        el.hovered = false;
        el.clicked = false;
    }
    if (!hasPointer) return;

    static thread_local std::vector<LayoutItem> layout;
    LayoutUI(scene, targetSize, config, layout);
    const bool editorView = scene.EditorView();
    for (const LayoutItem& item : layout) {
        UIElement& el = reg.get<UIElement>(item.entity);
        if (!el.visible || el.type != UIElement::Type::Button) continue;
        if (editorView && scene.IsEditorHidden(item.entity)) continue; // editor-hidden
        const glm::vec2 pointer = pointerNorm * item.canvas;
        el.hovered = item.rect.Contains(pointer);
        el.clicked = el.hovered && pressed;
    }
}

void BuildVertices(Scene& scene, Renderer& renderer,
                   const std::filesystem::path& assetsDir, glm::vec2 targetSize,
                   const CanvasConfig& config, std::vector<rhi::UIVertex>& out) {
    out.clear();
    SharedFont().Initialize(renderer); // lazy one-time bake

    static thread_local std::vector<LayoutItem> layout;
    LayoutUI(scene, targetSize, config, layout);

    auto& reg = scene.Registry();
    const bool editorView = scene.EditorView();
    for (const LayoutItem& item : layout) {
        UIElement& el = reg.get<UIElement>(item.entity);
        if (!el.visible) continue;
        if (editorView && scene.IsEditorHidden(item.entity)) continue; // editor-hidden
        const Emitter emit{&out, item.canvas};
        const Rect& rect = item.rect;
        const glm::vec2 center = (rect.Min() + glm::vec2(rect.x1, rect.y1)) * 0.5f;
        FontAtlas& font = ResolveFont(renderer, assetsDir, el.font);
        // Runtime-resolved caption (token substitution) wins over the template.
        const std::string& disp = el.runtimeText.empty() ? el.text : el.runtimeText;

        switch (el.type) {
            case UIElement::Type::Panel: {
                emit.Quad(rect, el.color, ResolveTexture(el, renderer, assetsDir));
                if (!disp.empty()) {
                    emit.Text(disp, rect, el.hAlign, el.vAlign, el.textSize,
                              {1.0f, 1.0f, 1.0f, el.color.a}, font);
                }
                break;
            }
            case UIElement::Type::Label:
                emit.Text(disp, rect, el.hAlign, el.vAlign, el.textSize, el.color, font);
                break;
            case UIElement::Type::Button: {
                glm::vec4 fill = el.color;
                if (el.clicked) fill *= glm::vec4(0.72f, 0.72f, 0.72f, 1.0f);
                else if (el.hovered) fill *= glm::vec4(1.22f, 1.22f, 1.22f, 1.0f);
                fill = glm::clamp(fill, glm::vec4(0.0f), glm::vec4(1.0f));
                emit.Quad(rect, fill, ResolveTexture(el, renderer, assetsDir));
                const f32 luma = 0.299f * fill.r + 0.587f * fill.g + 0.114f * fill.b;
                const glm::vec4 textColor =
                    luma > 0.55f ? glm::vec4(0.05f, 0.05f, 0.06f, 1.0f)
                                 : glm::vec4(0.96f, 0.96f, 0.98f, 1.0f);
                emit.Text(disp, rect, el.hAlign, el.vAlign, el.textSize, textColor, font);
                break;
            }
            case UIElement::Type::Image: {
                const u32 tex = ResolveTexture(el, renderer, assetsDir);
                emit.Quad(rect, el.color, tex);
                break;
            }
            case UIElement::Type::ProgressBar: {
                const f32 fraction = glm::clamp(el.fill, 0.0f, 1.0f);
                if (el.radial) {
                    // "Wheel": ring background + swept fill.
                    const glm::vec2 size = rect.Size();
                    const f32 outerR = glm::min(size.x, size.y) * 0.5f;
                    const f32 innerR = outerR * 0.62f;
                    emit.Wheel(center, outerR, innerR, 1.0f, el.color);
                    emit.Wheel(center, outerR, innerR, fraction, el.fillColor);
                } else {
                    emit.Quad(rect, el.color);
                    Rect fillRect = rect;
                    fillRect.x1 = rect.x0 + (rect.x1 - rect.x0) * fraction;
                    emit.Quad(fillRect, el.fillColor);
                }
                if (!disp.empty()) {
                    emit.Text(disp, rect, el.hAlign, el.vAlign, el.textSize,
                              {1.0f, 1.0f, 1.0f, 1.0f}, font);
                }
                break;
            }
        }
    }
}

void ClearTextureCache(Scene* scene) {
    if (!scene) return;
    for (const entt::entity e : scene->Registry().view<UIElement>()) {
        UIElement& el = scene->Registry().get<UIElement>(e);
        el.textureResolved = false;
        el.textureIndexCache = 0;
    }
}

} // namespace hbe::ui
