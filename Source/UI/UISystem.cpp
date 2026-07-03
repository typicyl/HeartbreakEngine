// UI/UISystem.cpp
#include "UI/UISystem.h"

#include "Assets/AssetLoader.h"
#include "Core/Input.h"
#include "Core/Log.h"
#include "Renderer/Renderer.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "UI/FontAtlas.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace hbe::ui {

namespace {

// Canvas-space -> NDC emission. All Push helpers go through this.
struct Emitter {
    std::vector<rhi::UIVertex>* out;
    glm::vec2 canvas;
    // Optional per-element 2D transform (UI animation): scale + rotation about a
    // pivot point in canvas space, applied to every emitted vertex before the NDC
    // conversion. Off by default so plain rects are untouched (bit-identical output).
    glm::vec2 xfPivot{0.0f};
    glm::vec2 xfScale{1.0f};
    f32 xfCos = 1.0f;
    f32 xfSin = 0.0f;
    bool xfOn = false;
    // Optional per-element glyph-layout cache (the BuildVertices ctx path).
    // entityKey identifies the element; textSlot disambiguates multiple Text()
    // calls per element (Selector emits one per option; order is deterministic).
    UIContext* cache = nullptr;
    u64 entityKey = 0;
    mutable u32 textSlot = 0;
    // NDC clip rect stamped into every vertex (ScrollView content). The default
    // sentinel (-2..2) covers all of NDC = the shader's clip test never fires.
    glm::vec2 clipMin{-2.0f, -2.0f};
    glm::vec2 clipMax{2.0f, 2.0f};
    // Multiplies every emitted vertex color: carries the disabled-widget dim and
    // the inherited UICanvasGroup opacity. Default white = bit-identical output.
    glm::vec4 tint{1.0f};
    // Word-wrap captions to the rect's inner width (Text()).
    bool wrapText = false;

    rhi::UIVertex Vertex(f32 x, f32 y, f32 u, f32 v, const glm::vec4& c, u32 tex) const {
        if (xfOn) {
            const f32 dx = (x - xfPivot.x) * xfScale.x;
            const f32 dy = (y - xfPivot.y) * xfScale.y;
            x = xfPivot.x + dx * xfCos - dy * xfSin;
            y = xfPivot.y + dx * xfSin + dy * xfCos;
        }
        rhi::UIVertex vert;
        vert.x = x / canvas.x * 2.0f - 1.0f;
        vert.y = 1.0f - y / canvas.y * 2.0f; // canvas y-down -> NDC y-up
        vert.u = u;
        vert.v = v;
        vert.r = c.r * tint.r; vert.g = c.g * tint.g;
        vert.b = c.b * tint.b; vert.a = c.a * tint.a;
        vert.texIndex = tex;
        vert.clipX0 = clipMin.x; vert.clipY0 = clipMin.y;
        vert.clipX1 = clipMax.x; vert.clipY1 = clipMax.y;
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

    // 9-slice: `border` = L,T,R,B in SOURCE pixels; texW/texH = the source size.
    // Corners keep their native pixel size, edges/center stretch. Borders that
    // would overlap are scaled down to fit the rect (never negative).
    void NineSlice(const Rect& r, const glm::vec4& color, u32 tex, f32 texW,
                   f32 texH, const glm::vec4& border) const {
        const f32 rw = r.x1 - r.x0, rh = r.y1 - r.y0;
        f32 L = glm::max(border.x, 0.0f), T = glm::max(border.y, 0.0f);
        f32 R = glm::max(border.z, 0.0f), B = glm::max(border.w, 0.0f);
        // Source-fraction borders, clamped so a border wider than the texture
        // can't produce uL+uR > 1 (which would flip/overlap the center UVs).
        f32 uL = texW > 0.0f ? glm::clamp(border.x / texW, 0.0f, 1.0f) : 0.0f;
        f32 uR = texW > 0.0f ? glm::clamp(border.z / texW, 0.0f, 1.0f) : 0.0f;
        f32 vT = texH > 0.0f ? glm::clamp(border.y / texH, 0.0f, 1.0f) : 0.0f;
        f32 vB = texH > 0.0f ? glm::clamp(border.w / texH, 0.0f, 1.0f) : 0.0f;
        if (uL + uR > 1.0f) { const f32 k = 1.0f / (uL + uR); uL *= k; uR *= k; }
        if (vT + vB > 1.0f) { const f32 k = 1.0f / (vT + vB); vT *= k; vB *= k; }
        // Shrink dest borders to fit a small rect, applying the SAME factor to
        // the source split so corners stay proportional (no UV drift).
        if (L + R > rw && L + R > 0.0f) {
            const f32 k = rw / (L + R);
            L *= k; R *= k; uL *= k; uR *= k;
        }
        if (T + B > rh && T + B > 0.0f) {
            const f32 k = rh / (T + B);
            T *= k; B *= k; vT *= k; vB *= k;
        }
        const f32 xs[4] = {r.x0, r.x0 + L, r.x1 - R, r.x1};
        const f32 ys[4] = {r.y0, r.y0 + T, r.y1 - B, r.y1};
        const f32 us[4] = {0.0f, uL, 1.0f - uR, 1.0f};
        const f32 vs[4] = {0.0f, vT, 1.0f - vB, 1.0f};
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col) {
                const Rect cell{xs[col], ys[row], xs[col + 1], ys[row + 1]};
                Quad(cell, color, tex, us[col], vs[row], us[col + 1], vs[row + 1]);
            }
    }

    // TTF text aligned within `rect` (h/v alignment). `sizePx` = glyph height.
    // When wrapText is set, the text word-wraps to the rect's inner width.
    void Text(const std::string& text, const Rect& rect, UIElement::HAlign ha,
              UIElement::VAlign va, f32 sizePx, const glm::vec4& color,
              FontAtlas& font) const {
        if (!font.Ready() || text.empty()) return;
        // Wrap width = the rect's inner width (minus the alignment pad); 0 when
        // not wrapping, which makes LayoutWrapped behave like Layout.
        const f32 pad = 4.0f;
        const f32 wrapW = wrapText ? glm::max((rect.x1 - rect.x0) - pad * 2.0f, 8.0f)
                                   : 0.0f;
        // Glyph layout via the per-element cache when available (the stbtt
        // shaping is the dominant per-frame text cost on static menus); the
        // uncached path (boot splash / editor) shapes into a scratch as before.
        const std::vector<GlyphQuad>* quadsPtr = nullptr;
        f32 w = 0, h = 0;
        if (cache) {
            const u64 slotKey = (entityKey << 4) | (textSlot++ & 15u);
            u64 hash = 1469598103934665603ull; // FNV-1a over text + font + size + wrap
            for (const char ch : text)
                hash = (hash ^ static_cast<u8>(ch)) * 1099511628211ull;
            hash = (hash ^ font.TextureIndex()) * 1099511628211ull;
            u32 sizeBits;
            std::memcpy(&sizeBits, &sizePx, sizeof(sizeBits));
            hash = (hash ^ sizeBits) * 1099511628211ull;
            u32 wrapBits;
            std::memcpy(&wrapBits, &wrapW, sizeof(wrapBits));
            hash = (hash ^ wrapBits) * 1099511628211ull;
            if (hash == 0) hash = 1; // 0 = "empty entry" sentinel
            UIContext::TextCacheEntry& entry = cache->textCache[slotKey];
            if (entry.hash != hash) {
                entry.hash = hash;
                font.LayoutWrapped(text, sizePx, wrapW, entry.quads, entry.w, entry.h);
                ++cache->stats.textLayouts;
            }
            quadsPtr = &entry.quads;
            w = entry.w;
            h = entry.h;
        } else {
            static thread_local std::vector<GlyphQuad> scratch;
            font.LayoutWrapped(text, sizePx, wrapW, scratch, w, h);
            quadsPtr = &scratch;
        }
        const std::vector<GlyphQuad>& quads = *quadsPtr;
        // Inset left/right/top/bottom alignments a touch so glyphs aren't flush
        // against the element edge; centered alignment uses no inset. (`pad`
        // computed above for the wrap width.)
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

// A resolved UI texture: bindless index + source pixel size (the size is
// filled lazily, only for textures that 9-slice actually needs).
struct UITex {
    u32 index = 0;
    u32 w = 0; // 0 = not yet peeked
    u32 h = 0;
};

// Path -> {index,w,h} cache shared by the preload pass and ResolveTexture.
// assets::LoadTexture has NO cache of its own, so without this every element
// referencing the same file uploads a duplicate texture. Keyed by the
// Assets-relative path string. Cleared with ClearTextureCache (project switch /
// texture re-import).
std::unordered_map<std::string, UITex>& UITexCache() {
    static std::unordered_map<std::string, UITex> s_cache;
    return s_cache;
}

// Loads (or cache-hits) a UI texture by its Assets-relative path.
u32 LoadUITexture(Renderer& renderer, const std::filesystem::path& assetsDir,
                  const std::string& rel) {
    if (rel.empty() || assetsDir.empty()) return 0;
    auto& cache = UITexCache();
    if (auto it = cache.find(rel); it != cache.end()) return it->second.index;
    const rhi::TextureHandle handle = assets::LoadTexture(renderer, assetsDir / rel);
    cache[rel] = {handle.index, 0, 0};
    return handle.index;
}

// Like LoadUITexture but also resolves the source width/height (peeked once,
// then cached) - needed for 9-slice UV borders. On peek failure sets 1x1 so it
// isn't retried every frame (9-slice then degrades to a plain stretch).
UITex UITexInfo(Renderer& renderer, const std::filesystem::path& assetsDir,
                const std::string& rel) {
    if (rel.empty() || assetsDir.empty()) return {};
    LoadUITexture(renderer, assetsDir, rel); // ensure the entry exists
    UITex& e = UITexCache()[rel];
    if (e.index != 0 && e.w == 0) {
        u32 w = 0, h = 0;
        if (uaf::PeekTextureSize(assetsDir / rel, w, h) && w > 0 && h > 0) {
            e.w = w;
            e.h = h;
        } else {
            e.w = e.h = 1;
        }
    }
    return e;
}

// Resolves an element's texture ref to a bindless index (cached per element;
// reset textureResolved to re-resolve after edits).
u32 ResolveTexture(UIElement& el, Renderer& renderer,
                   const std::filesystem::path& assetsDir) {
    if (el.textureResolved) return el.textureIndexCache;
    el.textureResolved = true;
    el.textureIndexCache = LoadUITexture(renderer, assetsDir, el.texture);
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

namespace {
// Rebuilds a parent -> children map over everything that can appear in a UI tree.
void BuildChildrenMap(entt::registry& reg,
                      std::unordered_map<u32, std::vector<entt::entity>>& children) {
    children.clear();
    for (const entt::entity e : reg.view<Parent>()) {
        const Parent& p = reg.get<Parent>(e);
        if (reg.valid(p.entity)) {
            children[static_cast<u32>(p.entity)].push_back(e);
        }
    }
}

// Intersection of two clip rects (may be degenerate when disjoint - the clip
// test then simply rejects everything, which is the right outcome).
Rect IntersectRects(const Rect& a, const Rect& b) {
    return {glm::max(a.x0, b.x0), glm::max(a.y0, b.y0), glm::min(a.x1, b.x1),
            glm::min(a.y1, b.y1)};
}
} // namespace

// Shared walk over a prebuilt children map (the map is the expensive part; the
// walk itself is cheap and runs once per frame).
static void LayoutUIImpl(Scene& scene, glm::vec2 targetSize,
                         const CanvasConfig& legacyConfig,
                         const std::unordered_map<u32, std::vector<entt::entity>>& children,
                         std::vector<LayoutItem>& out) {
    out.clear();
    auto& reg = scene.Registry();

    // Walks one UI subtree, laying out parents before children (depth-first).
    // `canvasEntity` tags each item with the UICanvas it lays out under
    // (entt::null = legacy) so world-space canvases can route elsewhere.
    // `clip`/`hasClip` = the clip rect inherited from ancestor ScrollViews.
    // `gOpacity`/`gInteractive` = inherited UICanvasGroup state; `forced` (when
    // non-null) overrides this node's rect (a UILayoutGroup positioning a child).
    const auto walk = [&](auto&& self, entt::entity e, const Rect& parentRect,
                          glm::vec2 canvas, entt::entity canvasEntity,
                          const Rect& clip, bool hasClip, f32 gOpacity,
                          bool gInteractive, const Rect* forced) -> void {
        // An inactive panel (a named screen the UIManager toggles) hides its whole
        // subtree: skip laying it or its descendants out at all.
        if (const UIPanel* panel = reg.try_get<UIPanel>(e); panel && !panel->active) return;
        // A hidden element hides its ENTIRE subtree, not just its own quad: skip the
        // layout here so descendants are neither drawn nor hit-tested (both consume
        // this one walk). Mirrors the UIPanel.active gate above.
        if (const UIElement* el = reg.try_get<UIElement>(e); el && !el->visible) return;
        // Fold this node's UICanvasGroup into the inherited opacity/interactivity
        // (affects the node itself and every descendant, Unity-style).
        if (const UICanvasGroup* cg = reg.try_get<UICanvasGroup>(e)) {
            gOpacity *= glm::clamp(cg->opacity, 0.0f, 1.0f);
            gInteractive = gInteractive && cg->interactable;
        }
        // A fully-faded subtree can't be interacted with (else it's an invisible
        // click/focus trap over whatever is behind it).
        if (gOpacity <= 0.001f) gInteractive = false;
        Rect rectForChildren = parentRect;
        Rect childClip = clip;
        bool childHasClip = hasClip;
        UIElement* scroll = nullptr; // non-null when `e` is a ScrollView
        usize selfIdx = static_cast<usize>(-1);
        if (UIElement* el = reg.try_get<UIElement>(e)) {
            LayoutItem item;
            item.entity = e;
            item.parentRect = parentRect;
            item.rect = forced ? *forced : ComputeElementRect(*el, parentRect);
            item.canvas = canvas;
            item.canvasEntity = canvasEntity;
            item.clip = clip;
            item.hasClip = hasClip;
            item.groupOpacity = gOpacity;
            item.groupInteractive = gInteractive;
            rectForChildren = item.rect;
            if (el->type == UIElement::Type::ScrollView) {
                // Children lay out inside the CONTENT rect (the view shifted by
                // -scrollPos, sized to the content) and the view rect clips the
                // whole subtree. Auto content size (contentSize 0) uses LAST
                // frame's measured extent - stable by the second frame; an
                // explicit contentSize wins outright.
                const glm::vec2 view = item.rect.Size();
                el->viewExtent = view;
                const glm::vec2 content = glm::max(
                    glm::vec2(el->contentSize.x > 0.0f ? el->contentSize.x
                                                       : el->contentExtent.x,
                              el->contentSize.y > 0.0f ? el->contentSize.y
                                                       : el->contentExtent.y),
                    view);
                rectForChildren = {item.rect.x0 - el->scrollPos.x,
                                   item.rect.y0 - el->scrollPos.y,
                                   item.rect.x0 - el->scrollPos.x + content.x,
                                   item.rect.y0 - el->scrollPos.y + content.y};
                childClip = hasClip ? IntersectRects(clip, item.rect) : item.rect;
                childHasClip = true;
                scroll = el;
            }
            selfIdx = out.size();
            out.push_back(item);
        }
        const usize mark = out.size(); // this node's descendants start here

        // A UILayoutGroup positions its DIRECT children sequentially (row /
        // column / grid) instead of by their own RectTransform. Non-UIElement
        // children (rare) recurse un-positioned.
        const UILayoutGroup* lg = reg.try_get<UILayoutGroup>(e);
        if (auto it = children.find(static_cast<u32>(e)); it != children.end()) {
            if (lg) {
                const glm::vec4 pad = lg->padding; // L,T,R,B
                const int cols = glm::max(lg->columns, 1);
                glm::vec2 cursor{rectForChildren.x0 + pad.x, rectForChildren.y0 + pad.y};
                int gridCol = 0;
                for (const entt::entity child : it->second) {
                    const UIElement* cel = reg.try_get<UIElement>(child);
                    // Non-UIElement, and STRETCH/fullscreen children (anchorMin
                    // != anchorMax, or Fit-to-Parent), recurse un-positioned -
                    // the documented rule is that layout groups only place fixed-
                    // size children; a stretch child would otherwise claim the
                    // whole group and shove its siblings off-region.
                    const bool stretch =
                        cel && (cel->fullscreen || cel->anchorMin != cel->anchorMax);
                    if (!cel || stretch) {
                        self(self, child, rectForChildren, canvas, canvasEntity,
                             childClip, childHasClip, gOpacity, gInteractive, nullptr);
                        continue;
                    }
                    const glm::vec2 nsz = ComputeElementRect(*cel, rectForChildren).Size();
                    Rect slot;
                    bool grid = false;
                    if (lg->kind == UILayoutGroup::Kind::Grid) {
                        grid = true;
                        slot = {cursor.x, cursor.y, cursor.x + lg->cellSize.x,
                                cursor.y + lg->cellSize.y};
                        if (++gridCol >= cols) {
                            gridCol = 0;
                            cursor.x = rectForChildren.x0 + pad.x;
                            cursor.y += lg->cellSize.y + lg->spacing;
                        } else {
                            cursor.x += lg->cellSize.x + lg->spacing;
                        }
                    } else { // Vertical / Horizontal
                        slot = {cursor.x, cursor.y, cursor.x + nsz.x, cursor.y + nsz.y};
                    }
                    // Advance the flow axis by the child's FINAL size after the
                    // recursion - a fitContent child grows its own rect, so the
                    // pre-grow natural size would overlap the next sibling.
                    const usize childIdx = out.size();
                    self(self, child, rectForChildren, canvas, canvasEntity, childClip,
                         childHasClip, gOpacity, gInteractive, &slot);
                    if (!grid && childIdx < out.size()) {
                        const glm::vec2 gsz = out[childIdx].rect.Size();
                        if (lg->kind == UILayoutGroup::Kind::Horizontal)
                            cursor.x += gsz.x + lg->spacing;
                        else
                            cursor.y += gsz.y + lg->spacing;
                    }
                }
            } else {
                for (const entt::entity child : it->second) {
                    self(self, child, rectForChildren, canvas, canvasEntity, childClip,
                         childHasClip, gOpacity, gInteractive, nullptr);
                }
            }
        }

        // ContentSizeFitter: grow the group element's rect to wrap its children.
        if (lg && lg->fitContent && selfIdx != static_cast<usize>(-1)) {
            glm::vec2 far(rectForChildren.x0, rectForChildren.y0);
            for (usize i = mark; i < out.size(); ++i) {
                far.x = glm::max(far.x, out[i].rect.x1);
                far.y = glm::max(far.y, out[i].rect.y1);
            }
            out[selfIdx].rect.x1 = far.x + lg->padding.z;
            out[selfIdx].rect.y1 = far.y + lg->padding.w;
        }

        if (scroll) {
            // Measure the laid-out subtree against the content origin so wheel
            // clamping / auto-scroll know the real extent (auto contentSize).
            glm::vec2 extent(0.0f);
            for (usize i = mark; i < out.size(); ++i) {
                extent.x = glm::max(extent.x, out[i].rect.x1 - rectForChildren.x0);
                extent.y = glm::max(extent.y, out[i].rect.y1 - rectForChildren.y0);
            }
            scroll->contentExtent = {
                scroll->contentSize.x > 0.0f
                    ? scroll->contentSize.x
                    : glm::max(extent.x, scroll->viewExtent.x),
                scroll->contentSize.y > 0.0f
                    ? scroll->contentSize.y
                    : glm::max(extent.y, scroll->viewExtent.y)};
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
                walk(walk, e, root, canvas, entt::null, Rect{}, false, 1.0f, true, nullptr);
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
        glm::vec2 canvas;
        if (c.worldSpace) {
            // World canvases are exactly ref-sized: their "screen" is the render
            // target the lit page quad samples (scaleMode is meaningless there).
            canvas = {glm::max(c.refWidth, 64.0f), glm::max(c.refHeight, 64.0f)};
        } else {
            CanvasConfig config;
            config.mode = static_cast<ScaleMode>(glm::clamp(c.scaleMode, 0u, 2u));
            config.refWidth = glm::max(c.refWidth, 64.0f);
            config.refHeight = glm::max(c.refHeight, 64.0f);
            canvas = EffectiveCanvas(config, targetSize);
        }
        const Rect root{0.0f, 0.0f, canvas.x, canvas.y};
        walk(walk, canvasEntity, root, canvas, canvasEntity, Rect{}, false, 1.0f, true,
             nullptr);
    }
}

void LayoutUI(Scene& scene, glm::vec2 targetSize, const CanvasConfig& legacyConfig,
              std::vector<LayoutItem>& out) {
    // Uncached path (boot splash, editor drag): rebuilds the map every call.
    static thread_local std::unordered_map<u32, std::vector<entt::entity>> children;
    BuildChildrenMap(scene.Registry(), children);
    LayoutUIImpl(scene, targetSize, legacyConfig, children, out);
}

void LayoutUI(Scene& scene, glm::vec2 targetSize, const CanvasConfig& legacyConfig,
              UIContext& ctx) {
    if (ctx.seenStructureVersion != scene.UIStructureVersion()) {
        ctx.seenStructureVersion = scene.UIStructureVersion();
        BuildChildrenMap(scene.Registry(), ctx.children);
        ++ctx.stats.mapRebuilds;
        // Entity-keyed caches can hold dead entries after structural churn; the
        // per-entry hash makes them self-correcting, so only prune when bloated.
        if (ctx.textCache.size() > 4 * (ctx.layout.size() + 16)) ctx.textCache.clear();
        if (ctx.surfaceInv.size() > 64) ctx.surfaceInv.clear();
    }
    LayoutUIImpl(scene, targetSize, legacyConfig, ctx.children, ctx.layout);
}

// Widget types that respond to the pointer (hover/click/drag/wheel).
static bool IsInteractive(UIElement::Type t) {
    return t == UIElement::Type::Button || t == UIElement::Type::Slider ||
           t == UIElement::Type::Toggle || t == UIElement::Type::Selector ||
           t == UIElement::Type::ScrollView || t == UIElement::Type::TextInput;
}

// One laid-out interactive element vs the resolved pointer (shared by the
// legacy and cached interaction paths). Returns true when any flag was touched.
static bool ApplyPointerToElement(UIElement& el, const LayoutItem& item,
                                  glm::vec2 pointer, bool pressed, bool down,
                                  f32 wheel) {
    // Disabled widgets and non-interactive UICanvasGroup subtrees ignore the
    // pointer entirely (no hover, no click). Return false so the caller doesn't
    // add them to the touched list.
    if (!el.enabled || !item.groupInteractive) {
        el.hovered = false;
        return false;
    }
    // Content clipped away by an ancestor ScrollView can't be hovered/clicked.
    if (item.hasClip && !item.clip.Contains(pointer)) return false;
    const bool over = item.rect.Contains(pointer);
    el.hovered = over;
    bool touched = over;
    switch (el.type) {
        case UIElement::Type::Button:
            el.clicked = over && pressed;
            touched |= el.clicked;
            break;
        case UIElement::Type::Toggle:
            if (over && pressed) {
                el.toggled = !el.toggled;
                el.clicked = true;
                el.changed = true;
                touched = true;
            }
            break;
        case UIElement::Type::Selector:
            if (over && pressed && !el.options.empty()) {
                // Click the option cell under the pointer (fallback: advance).
                const int n = static_cast<int>(el.options.size());
                const f32 w = (item.rect.x1 - item.rect.x0) / static_cast<f32>(n);
                int cell = w > 0.0f ? static_cast<int>((pointer.x - item.rect.x0) / w) : 0;
                cell = glm::clamp(cell, 0, n - 1);
                el.selected = cell;
                el.clicked = true;
                el.changed = true;
                touched = true;
            }
            break;
        case UIElement::Type::Slider: {
            if (over && pressed) el.dragging = true;
            if (el.dragging && down) {
                const f32 span = glm::max(item.rect.x1 - item.rect.x0, 1e-3f);
                const f32 nv = glm::clamp((pointer.x - item.rect.x0) / span, 0.0f, 1.0f);
                if (nv != el.value) el.changed = true;
                el.value = nv;
                touched = true;
            }
            break;
        }
        case UIElement::Type::TextInput:
            // Click reports like a Button; ui::UpdateNavigation turns it into
            // an edit session (focus, caret, typed characters).
            el.clicked = over && pressed;
            touched |= el.clicked;
            break;
        case UIElement::Type::ScrollView: {
            // Mouse wheel scrolls the hovered view (+1 notch = away from the
            // user = scroll toward the top). Auto-scrolling views stay inert
            // (a credits roll shouldn't fight the wheel).
            if (over && wheel != 0.0f && el.autoScroll == 0.0f) {
                const glm::vec2 maxScroll =
                    glm::max(el.contentExtent - el.viewExtent, glm::vec2(0.0f));
                glm::vec2 next = el.scrollPos;
                if (el.scrollVertical) next.y -= wheel * el.scrollSpeed;
                else if (el.scrollHorizontal) next.x -= wheel * el.scrollSpeed;
                next = glm::clamp(next, glm::vec2(0.0f), maxScroll);
                if (next != el.scrollPos) {
                    el.scrollPos = next;
                    el.changed = true;
                }
                touched = true;
            }
            break;
        }
        default:
            break;
    }
    return touched;
}

void UpdateInteraction(Scene& scene, const Input& input, glm::vec2 pointerNorm,
                       glm::vec2 targetSize, const CanvasConfig& config,
                       const PointerState* pointers) {
    auto& reg = scene.Registry();
    const bool hasPointer = pointerNorm.x >= 0.0f && pointerNorm.y >= 0.0f;
    const bool hasWorldPointer = pointers && !pointers->worldCanvasPx.empty();
    const bool pressed = input.WasMousePressed(MouseButton::Left);
    const bool down = input.IsMouseDown(MouseButton::Left);
    const f32 wheel = input.MouseWheel();

    // Clear per-frame hover/click/changed. `dragging` persists across frames while the
    // button is held, so it's only cleared on release (below), not here.
    for (const entt::entity e : reg.view<UIElement>()) {
        UIElement& el = reg.get<UIElement>(e);
        el.hovered = false;
        el.clicked = false;
        el.changed = false;
        if (!down) el.dragging = false; // mouse released -> end any slider drag
    }
    if (!hasPointer && !hasWorldPointer) return;

    static thread_local std::vector<LayoutItem> layout;
    LayoutUI(scene, targetSize, config, layout);
    const bool editorView = scene.EditorView();
    for (const LayoutItem& item : layout) {
        UIElement& el = reg.get<UIElement>(item.entity);
        if (!el.visible || !IsInteractive(el.type)) continue;
        if (editorView && scene.IsEditorHidden(item.entity)) continue; // editor-hidden
        // Resolve the pointer for THIS element's canvas: a world-space canvas
        // uses its ray-picked canvas-pixel pointer (absent = ray missed the page
        // -> not hovered); everything else maps the screen pointer as before.
        glm::vec2 pointer;
        if (item.canvasEntity != entt::null && reg.all_of<UICanvas>(item.canvasEntity) &&
            reg.get<UICanvas>(item.canvasEntity).worldSpace) {
            if (!pointers) continue;
            const auto pit = pointers->worldCanvasPx.find(static_cast<u32>(item.canvasEntity));
            if (pit == pointers->worldCanvasPx.end()) continue;
            pointer = pit->second;
        } else {
            if (!hasPointer) continue;
            pointer = pointerNorm * item.canvas;
        }
        ApplyPointerToElement(el, item, pointer, pressed, down, wheel);
    }
}

void UpdateInteraction(Scene& scene, const Input& input, glm::vec2 pointerNorm,
                       const PointerState* pointers, UIContext& ctx) {
    auto& reg = scene.Registry();
    const bool hasPointer = pointerNorm.x >= 0.0f && pointerNorm.y >= 0.0f;
    const bool hasWorldPointer = pointers && !pointers->worldCanvasPx.empty();
    const bool pressed = input.WasMousePressed(MouseButton::Left);
    const bool down = input.IsMouseDown(MouseButton::Left);
    const f32 wheel = input.MouseWheel();

    // Clear per-frame flags ONLY on the elements touched last frame (instead of
    // a full registry scan). `dragging` persists while the button is held.
    for (const entt::entity e : ctx.interactives) {
        if (!reg.valid(e) || !reg.all_of<UIElement>(e)) continue;
        UIElement& el = reg.get<UIElement>(e);
        el.hovered = false;
        el.clicked = false;
        el.changed = false;
        if (!down) el.dragging = false;
    }
    ctx.interactives.clear();
    if (!hasPointer && !hasWorldPointer) return;

    // Hit-test against LAST frame's layout (ctx.layout, filled by BuildVertices)
    // - the effective behavior of the old two-walk code, since animations run
    // after interaction. Empty on the very first frame (nothing to hit).
    const bool editorView = scene.EditorView();
    for (const LayoutItem& item : ctx.layout) {
        if (!reg.valid(item.entity) || !reg.all_of<UIElement>(item.entity)) continue;
        UIElement& el = reg.get<UIElement>(item.entity);
        if (!el.visible || !IsInteractive(el.type)) continue;
        if (editorView && scene.IsEditorHidden(item.entity)) continue;
        glm::vec2 pointer;
        if (item.canvasEntity != entt::null && reg.valid(item.canvasEntity) &&
            reg.all_of<UICanvas>(item.canvasEntity) &&
            reg.get<UICanvas>(item.canvasEntity).worldSpace) {
            if (!pointers) continue;
            const auto pit =
                pointers->worldCanvasPx.find(static_cast<u32>(item.canvasEntity));
            if (pit == pointers->worldCanvasPx.end()) continue;
            pointer = pit->second;
        } else {
            if (!hasPointer) continue;
            pointer = pointerNorm * item.canvas;
        }
        if (ApplyPointerToElement(el, item, pointer, pressed, down, wheel))
            ctx.interactives.push_back(item.entity);
    }
}

static void BuildVerticesImpl(Scene& scene, Renderer& renderer,
                              const std::filesystem::path& assetsDir,
                              glm::vec2 targetSize, const CanvasConfig& config,
                              std::vector<rhi::UIVertex>& out,
                              std::vector<WorldUIBatch>* worldOut,
                              const std::vector<LayoutItem>& layout, UIContext* ctx) {
    out.clear();
    SharedFont().Initialize(renderer); // lazy one-time bake (preload usually beat us)

    auto& reg = scene.Registry();

    // World-space canvases get their own per-canvas batch (rendered to texture);
    // everything else emits into the screen overlay `out` exactly as before.
    std::unordered_map<u32, usize> worldBatch; // canvas entity -> worldOut index
    if (worldOut) {
        worldOut->clear();
        for (const entt::entity e : reg.view<UICanvas>()) {
            const UICanvas& c = reg.get<UICanvas>(e);
            if (!c.worldSpace || !c.visible || !c.rtTexture.IsValid()) continue;
            worldBatch[static_cast<u32>(e)] = worldOut->size();
            WorldUIBatch batch;
            batch.canvas = e;
            batch.target = c.rtTexture;
            worldOut->push_back(std::move(batch));
        }
    }

    const bool editorView = scene.EditorView();
    for (const LayoutItem& item : layout) {
        UIElement& el = reg.get<UIElement>(item.entity);
        if (!el.visible) continue;
        if (editorView && scene.IsEditorHidden(item.entity)) continue; // editor-hidden
        // Route: world-canvas items -> that canvas's texture batch; no worldOut
        // (boot splash) or no RT yet -> skip them (never the screen overlay).
        std::vector<rhi::UIVertex>* dest = &out;
        if (item.canvasEntity != entt::null) {
            if (const UICanvas* c = reg.try_get<UICanvas>(item.canvasEntity);
                c && c->worldSpace) {
                const auto bit = worldBatch.find(static_cast<u32>(item.canvasEntity));
                if (bit == worldBatch.end()) continue;
                dest = &(*worldOut)[bit->second].verts;
            }
        }
        const Rect& rect = item.rect;
        // Per-element 2D transform (scale + rotation about the pivot). Skipped entirely
        // at identity so untransformed elements emit exactly as before.
        const bool hasXf =
            el.rotation != 0.0f || el.scale.x != 1.0f || el.scale.y != 1.0f;
        glm::vec2 xfPivot(0.0f), xfScale(1.0f);
        f32 xfCos = 1.0f, xfSin = 0.0f;
        if (hasXf) {
            xfPivot = rect.Min() + el.pivot * rect.Size(); // pivot point, canvas space
            xfScale = el.scale;
            const f32 rad = glm::radians(el.rotation);
            xfCos = std::cos(rad);
            xfSin = std::sin(rad);
        }
        // Ancestor-ScrollView clip -> the per-vertex NDC clip rect (same y-flip
        // as Emitter::Vertex; canvas y0/top maps to the NDC max).
        glm::vec2 clipMinN(-2.0f), clipMaxN(2.0f);
        if (item.hasClip) {
            clipMinN = {item.clip.x0 / item.canvas.x * 2.0f - 1.0f,
                        1.0f - item.clip.y1 / item.canvas.y * 2.0f};
            clipMaxN = {item.clip.x1 / item.canvas.x * 2.0f - 1.0f,
                        1.0f - item.clip.y0 / item.canvas.y * 2.0f};
        }
        // Uniform tint: a disabled widget dims (unless it carries an explicit
        // disabledColor, which recolors it instead), times the inherited
        // UICanvasGroup opacity (multiplied into the alpha channel).
        glm::vec4 groupTint(1.0f);
        // Auto-dim only when the widget carries NEITHER an explicit disabled
        // color NOR a purpose-made disabled sprite (either would already read as
        // disabled; dimming on top would double-darken it).
        if (!el.enabled && el.disabledColor.a <= 0.0f && el.disabledTexture.empty())
            groupTint = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);
        groupTint.a *= item.groupOpacity;
        const Emitter emit{dest,   item.canvas, xfPivot, xfScale,
                           xfCos,  xfSin,       hasXf,   ctx,
                           static_cast<u64>(static_cast<u32>(item.entity)), 0,
                           clipMinN, clipMaxN, groupTint, el.wrap};
        const glm::vec2 center = (rect.Min() + glm::vec2(rect.x1, rect.y1)) * 0.5f;
        FontAtlas& font = ResolveFont(renderer, assetsDir, el.font);
        // Runtime-resolved caption (token substitution) wins over the template.
        const std::string& disp = el.runtimeText.empty() ? el.text : el.runtimeText;

        // Skinning helpers (U5). partTex resolves a widget-part texture (path-
        // cached; 0 = none). stateFill/stateTex pick the per-state color/texture
        // for interactive widgets: an alpha-0 state color means "use the legacy
        // automatic multiply", so unskinned widgets are pixel-identical.
        const bool disabled = !el.enabled;
        const auto partTex = [&](const std::string& rel) -> u32 {
            return rel.empty() ? 0u : LoadUITexture(renderer, assetsDir, rel);
        };
        const auto stateFill = [&](const glm::vec4& base) -> glm::vec4 {
            glm::vec4 f = base;
            if (disabled)
                f = el.disabledColor.a > 0.0f ? el.disabledColor : base;
            else if (el.clicked)
                f = el.pressedColor.a > 0.0f ? el.pressedColor
                                             : base * glm::vec4(0.72f, 0.72f, 0.72f, 1.0f);
            else if (el.hovered)
                f = el.hoverColor.a > 0.0f ? el.hoverColor
                                           : base * glm::vec4(1.22f, 1.22f, 1.22f, 1.0f);
            return glm::clamp(f, glm::vec4(0.0f), glm::vec4(1.0f));
        };
        // Draws a background quad, or a 9-slice when `slice` is set and `rel`
        // (the texture actually being drawn) has a resolvable source size.
        const bool sliced =
            (el.slice.x + el.slice.y + el.slice.z + el.slice.w) > 0.0f;
        const auto quadOrSlice = [&](const Rect& r, const glm::vec4& col, u32 tex,
                                     const std::string& rel) {
            if (sliced && tex != 0 && !rel.empty()) {
                const UITex info = UITexInfo(renderer, assetsDir, rel);
                emit.NineSlice(r, col, tex, static_cast<f32>(info.w),
                               static_cast<f32>(info.h), el.slice);
            } else {
                emit.Quad(r, col, tex);
            }
        };

        switch (el.type) {
            case UIElement::Type::Panel: {
                quadOrSlice(rect, el.color, ResolveTexture(el, renderer, assetsDir),
                            el.texture);
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
                const glm::vec4 fill = stateFill(el.color);
                // Swap to the state sprite when present AND it actually loaded
                // (index 0 = load failed -> keep the base skin instead of
                // flashing an untextured quad). tex + rel stay consistent so the
                // 9-slice size-peek matches the drawn texture.
                u32 tex = ResolveTexture(el, renderer, assetsDir);
                std::string activeRel = el.texture;
                const std::string* stateRel = nullptr;
                if (disabled && !el.disabledTexture.empty()) stateRel = &el.disabledTexture;
                else if (el.clicked && !el.pressedTexture.empty()) stateRel = &el.pressedTexture;
                else if (el.hovered && !el.hoverTexture.empty()) stateRel = &el.hoverTexture;
                if (stateRel) {
                    const u32 t = partTex(*stateRel);
                    if (t) { tex = t; activeRel = *stateRel; }
                }
                quadOrSlice(rect, fill, tex, activeRel);
                const f32 luma = 0.299f * fill.r + 0.587f * fill.g + 0.114f * fill.b;
                const glm::vec4 textColor =
                    luma > 0.55f ? glm::vec4(0.05f, 0.05f, 0.06f, 1.0f)
                                 : glm::vec4(0.96f, 0.96f, 0.98f, 1.0f);
                emit.Text(disp, rect, el.hAlign, el.vAlign, el.textSize, textColor, font);
                break;
            }
            case UIElement::Type::Image: {
                const u32 tex = ResolveTexture(el, renderer, assetsDir);
                quadOrSlice(rect, el.color, tex, el.texture);
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
                    const u32 baseTex = ResolveTexture(el, renderer, assetsDir);
                    const u32 fillTex = partTex(el.fillTexture);
                    emit.Quad(rect, baseTex ? glm::vec4(1.0f) : el.color, baseTex);
                    Rect fillRect = rect;
                    fillRect.x1 = rect.x0 + (rect.x1 - rect.x0) * fraction;
                    emit.Quad(fillRect, fillTex ? glm::vec4(1.0f) : el.fillColor, fillTex);
                }
                if (!disp.empty()) {
                    emit.Text(disp, rect, el.hAlign, el.vAlign, el.textSize,
                              {1.0f, 1.0f, 1.0f, 1.0f}, font);
                }
                break;
            }
            case UIElement::Type::Slider: {
                const f32 v = glm::clamp(el.value, 0.0f, 1.0f);
                const glm::vec2 sz = rect.Size();
                const f32 cy = (rect.y0 + rect.y1) * 0.5f;
                const u32 trackTex = partTex(el.trackTexture);
                const u32 fillTex = partTex(el.fillTexture);
                const u32 handleTex = partTex(el.handleTexture);
                // A track texture spans the full rect height (typical 9-slice
                // bar art); the flat groove keeps its thin centered look.
                Rect track;
                if (trackTex) {
                    track = rect;
                } else {
                    const f32 trackH = glm::min(sz.y * 0.35f, 12.0f);
                    track = {rect.x0, cy - trackH * 0.5f, rect.x1, cy + trackH * 0.5f};
                }
                emit.Quad(track, trackTex ? glm::vec4(1.0f) : el.color, trackTex);
                Rect fillR = track;
                fillR.x1 = track.x0 + (track.x1 - track.x0) * v;
                emit.Quad(fillR, fillTex ? glm::vec4(1.0f) : el.fillColor, fillTex);
                const f32 hx = rect.x0 + (rect.x1 - rect.x0) * v; // handle center
                const f32 hr = el.handleSize > 0.0f ? el.handleSize * 0.5f
                                                    : glm::max(sz.y * 0.5f, 8.0f);
                emit.Quad({hx - hr, cy - hr, hx + hr, cy + hr},
                          handleTex ? glm::vec4(1.0f) : el.fillColor, handleTex);
                break;
            }
            case UIElement::Type::Toggle: {
                const std::string& skin = el.toggled ? el.onTexture : el.offTexture;
                const u32 tex = partTex(skin);
                const glm::vec4 bg =
                    tex ? glm::vec4(1.0f) : (el.toggled ? el.fillColor : el.color);
                emit.Quad(rect, bg, tex);
                if (!tex) { // textured toggles carry their own On/Off art
                    emit.Text(el.toggled ? "On" : "Off", rect, el.hAlign, el.vAlign,
                              el.textSize, {1.0f, 1.0f, 1.0f, 1.0f}, font);
                }
                break;
            }
            case UIElement::Type::Selector: {
                emit.Quad(rect, el.color); // background
                const int n = static_cast<int>(el.options.size());
                if (n > 0) {
                    const f32 w = (rect.x1 - rect.x0) / static_cast<f32>(n);
                    const int sel = glm::clamp(el.selected, 0, n - 1);
                    const u32 cellTex = partTex(el.cellTexture);
                    for (int i = 0; i < n; ++i) {
                        const Rect opt{rect.x0 + w * i, rect.y0, rect.x0 + w * (i + 1), rect.y1};
                        if (i == sel) // highlight the active choice
                            emit.Quad(opt, cellTex ? glm::vec4(1.0f) : el.fillColor, cellTex);
                        emit.Text(el.options[i], opt, UIElement::HAlign::Center,
                                  UIElement::VAlign::Center, el.textSize,
                                  {1.0f, 1.0f, 1.0f, 1.0f}, font);
                    }
                }
                break;
            }
            case UIElement::Type::TextInput: {
                const bool editing = ctx && ctx->editing == item.entity;
                glm::vec4 bg = el.color;
                u32 bgTex = ResolveTexture(el, renderer, assetsDir);
                if (disabled) {
                    if (el.disabledColor.a > 0.0f) bg = el.disabledColor;
                    if (!el.disabledTexture.empty()) bgTex = partTex(el.disabledTexture);
                } else if (editing || el.hovered) {
                    bg = glm::clamp(bg * glm::vec4(1.22f, 1.22f, 1.22f, 1.0f),
                                    glm::vec4(0.0f), glm::vec4(1.0f));
                }
                const std::string& bgRel =
                    (disabled && !el.disabledTexture.empty()) ? el.disabledTexture
                                                              : el.texture;
                quadOrSlice(rect, bg, bgTex, bgRel);
                // Text + caret clip to the box itself (tightening any ancestor
                // clip) so long content never spills; the text shifts left to
                // keep the caret visible.
                Emitter clipped = emit;
                clipped.wrapText = false; // TextInput is single-line: caret +
                                          // horizontal-scroll math assume one line
                {
                    const glm::vec2 mn(rect.x0 / item.canvas.x * 2.0f - 1.0f,
                                       1.0f - rect.y1 / item.canvas.y * 2.0f);
                    const glm::vec2 mx(rect.x1 / item.canvas.x * 2.0f - 1.0f,
                                       1.0f - rect.y0 / item.canvas.y * 2.0f);
                    clipped.clipMin = glm::max(emit.clipMin, mn);
                    clipped.clipMax = glm::min(emit.clipMax, mx);
                }
                const f32 luma = 0.299f * bg.r + 0.587f * bg.g + 0.114f * bg.b;
                glm::vec4 textColor = luma > 0.55f
                                          ? glm::vec4(0.05f, 0.05f, 0.06f, 1.0f)
                                          : glm::vec4(0.96f, 0.96f, 0.98f, 1.0f);
                if (el.text.empty() && !editing) {
                    if (!el.placeholder.empty()) {
                        textColor.a = 0.45f; // grayed hint
                        clipped.Text(el.placeholder, rect, UIElement::HAlign::Left,
                                     UIElement::VAlign::Center, el.textSize,
                                     textColor, font);
                    }
                    break;
                }
                // Caret x = width of the text before the caret; measured
                // directly (edit sessions are transient - no cache pressure).
                f32 caretW = 0.0f;
                if (editing) {
                    int n = 0;
                    usize caretByte = el.text.size();
                    for (usize i = 0; i < el.text.size(); ++i) {
                        if ((static_cast<u8>(el.text[i]) & 0xC0) != 0x80) {
                            if (n == ctx->caretPos) { caretByte = i; break; }
                            ++n;
                        }
                    }
                    static thread_local std::vector<GlyphQuad> measure;
                    f32 mh = 0.0f;
                    font.Layout(el.text.substr(0, caretByte), el.textSize, measure,
                                caretW, mh);
                }
                const f32 inset = 4.0f; // matches Emitter::Text's alignment pad
                const f32 avail = glm::max((rect.x1 - rect.x0) - inset * 2.0f, 8.0f);
                const f32 shift = (editing && caretW > avail) ? avail - caretW : 0.0f;
                Rect textRect = rect;
                textRect.x0 += shift; // left origin slides; the clip hides spill
                clipped.Text(el.text, textRect, UIElement::HAlign::Left,
                             UIElement::VAlign::Center, el.textSize, textColor, font);
                if (editing && glm::fract(ctx->caretBlink * 1.6f) < 0.65f) {
                    const f32 cx = textRect.x0 + inset + caretW;
                    const f32 cy = (rect.y0 + rect.y1) * 0.5f;
                    const f32 half = el.textSize * 0.55f;
                    clipped.Quad({cx, cy - half, cx + 2.0f, cy + half}, textColor);
                }
                break;
            }
            case UIElement::Type::ScrollView: {
                // Optional background (children clip to this rect and draw after
                // it - parents emit before children in the walk).
                if (el.color.a > 0.0f)
                    emit.Quad(rect, el.color, ResolveTexture(el, renderer, assetsDir));
                // Thin scrollbar on the overflowing axis; hidden while
                // auto-scrolling (a credits roll shouldn't show chrome).
                if (el.autoScroll == 0.0f) {
                    const glm::vec2 view = rect.Size();
                    if (el.scrollVertical && el.contentExtent.y > view.y + 0.5f) {
                        const f32 thumbH = glm::max(
                            view.y * view.y / el.contentExtent.y, 24.0f);
                        const f32 maxScroll = el.contentExtent.y - view.y;
                        const f32 t = glm::clamp(el.scrollPos.y / maxScroll, 0.0f, 1.0f);
                        const f32 ty = rect.y0 + (view.y - thumbH) * t;
                        emit.Quad({rect.x1 - 6.0f, rect.y0, rect.x1 - 2.0f, rect.y1},
                                  {0.0f, 0.0f, 0.0f, 0.25f});
                        emit.Quad({rect.x1 - 6.0f, ty, rect.x1 - 2.0f, ty + thumbH},
                                  el.fillColor);
                    } else if (el.scrollHorizontal &&
                               el.contentExtent.x > view.x + 0.5f) {
                        const f32 thumbW = glm::max(
                            view.x * view.x / el.contentExtent.x, 24.0f);
                        const f32 maxScroll = el.contentExtent.x - view.x;
                        const f32 t = glm::clamp(el.scrollPos.x / maxScroll, 0.0f, 1.0f);
                        const f32 tx = rect.x0 + (view.x - thumbW) * t;
                        emit.Quad({rect.x0, rect.y1 - 6.0f, rect.x1, rect.y1 - 2.0f},
                                  {0.0f, 0.0f, 0.0f, 0.25f});
                        emit.Quad({tx, rect.y1 - 6.0f, tx + thumbW, rect.y1 - 2.0f},
                                  el.fillColor);
                    }
                }
                break;
            }
        }

        // Keyboard/gamepad focus ring: a thin outline around the focused
        // widget. Hidden while the mouse drives focus (focusVisible) - the
        // cursor itself is the indicator then, like Unity/UMG.
        if (ctx && ctx->focusVisible && ctx->focused == item.entity) {
            const glm::vec4 ring =
                el.fillColor.a > 0.05f
                    ? glm::vec4(el.fillColor.r, el.fillColor.g, el.fillColor.b, 1.0f)
                    : glm::vec4(1.0f);
            const f32 t = 3.0f;
            emit.Quad({rect.x0 - t, rect.y0 - t, rect.x1 + t, rect.y0}, ring);
            emit.Quad({rect.x0 - t, rect.y1, rect.x1 + t, rect.y1 + t}, ring);
            emit.Quad({rect.x0 - t, rect.y0, rect.x0, rect.y1}, ring);
            emit.Quad({rect.x1, rect.y0, rect.x1 + t, rect.y1}, ring);
        }
    }
}

void BuildVertices(Scene& scene, Renderer& renderer,
                   const std::filesystem::path& assetsDir, glm::vec2 targetSize,
                   const CanvasConfig& config, std::vector<rhi::UIVertex>& out,
                   std::vector<WorldUIBatch>* worldOut) {
    // Uncached path (boot splash / editor helpers): full layout each call.
    static thread_local std::vector<LayoutItem> layout;
    LayoutUI(scene, targetSize, config, layout);
    BuildVerticesImpl(scene, renderer, assetsDir, targetSize, config, out, worldOut,
                      layout, nullptr);
}

void BuildVertices(Scene& scene, Renderer& renderer,
                   const std::filesystem::path& assetsDir, glm::vec2 targetSize,
                   const CanvasConfig& config, std::vector<rhi::UIVertex>& out,
                   std::vector<WorldUIBatch>* worldOut, UIContext& ctx) {
    ctx.stats = {}; // fresh per-frame counters (LayoutUI + the emit fill them)
    LayoutUI(scene, targetSize, config, ctx); // cached children map
    BuildVerticesImpl(scene, renderer, assetsDir, targetSize, config, out, worldOut,
                      ctx.layout, &ctx);
    ctx.stats.elements = static_cast<u32>(ctx.layout.size());
    ctx.stats.verts = static_cast<u32>(out.size());
}

void ClearTextureCache(Scene* scene) {
    UITexCache().clear(); // path->index cache (project switch / re-import)
    if (!scene) return;
    for (const entt::entity e : scene->Registry().view<UIElement>()) {
        UIElement& el = scene->Registry().get<UIElement>(e);
        el.textureResolved = false;
        el.textureIndexCache = 0;
    }
}

void PreloadUIAssets(Scene& scene, Renderer& renderer,
                     const std::filesystem::path& assetsDir) {
    // Eagerly bake fonts + load every referenced UI texture at SCENE LOAD so the
    // first visible frame has everything (no blank-text frame, no white-quad
    // flash, no disk-I/O hitch inside the frame loop).
    const auto t0 = std::chrono::high_resolution_clock::now();
    u32 textures = 0, fonts = 0;

    SharedFont().Initialize(renderer);
    ++fonts;

    auto& reg = scene.Registry();
    for (const entt::entity e : reg.view<UIElement>()) {
        UIElement& el = reg.get<UIElement>(e);
        if (!el.font.empty()) {
            ResolveFont(renderer, assetsDir, el.font); // bakes + caches per asset
            ++fonts;
        }
        if (!el.texture.empty()) {
            el.textureIndexCache = LoadUITexture(renderer, assetsDir, el.texture);
            el.textureResolved = true;
            ++textures;
        }
        for (const std::string& f : el.frames) { // sprite-animation frames
            if (!f.empty()) {
                LoadUITexture(renderer, assetsDir, f);
                ++textures;
            }
        }
        // Widget-part skins (U5): warm them too so a skinned widget's first
        // visible frame is complete (no white-quad flash).
        for (const std::string* t :
             {&el.trackTexture, &el.fillTexture, &el.handleTexture, &el.onTexture,
              &el.offTexture, &el.hoverTexture, &el.pressedTexture,
              &el.disabledTexture, &el.cellTexture}) {
            if (!t->empty()) {
                LoadUITexture(renderer, assetsDir, *t);
                ++textures;
            }
        }
    }
    for (const entt::entity e : reg.view<WorldText>()) {
        const WorldText& wt = reg.get<WorldText>(e);
        if (!wt.font.empty()) {
            ResolveFont(renderer, assetsDir, wt.font);
            ++fonts;
        }
    }

    const auto t1 = std::chrono::high_resolution_clock::now();
    const f64 ms = std::chrono::duration<f64, std::milli>(t1 - t0).count();
    if (textures > 0 || fonts > 1 || ms > 1.0) {
        HBE_INFO("UI preload: {} texture ref(s), {} font(s) in {:.1f} ms.", textures,
                 fonts, ms);
    }
}

} // namespace hbe::ui
