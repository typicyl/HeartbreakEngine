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

// LayoutFlow mirrors UILayoutGroup::Kind (UISystem.h cannot see Components.h).
// Pinned here so adding a Kind without adding a LayoutFlow is a build error.
static_assert(static_cast<u8>(UILayoutGroup::Kind::Vertical) ==
                  static_cast<u8>(LayoutFlow::Vertical) &&
              static_cast<u8>(UILayoutGroup::Kind::Horizontal) ==
                  static_cast<u8>(LayoutFlow::Horizontal) &&
              static_cast<u8>(UILayoutGroup::Kind::Grid) ==
                  static_cast<u8>(LayoutFlow::Grid),
              "ui::LayoutFlow must mirror UILayoutGroup::Kind");

LayoutOwnership LayoutGroupOwnership(const Scene& scene, entt::entity e) {
    LayoutOwnership own;
    const entt::registry& reg = scene.Registry();
    if (e == entt::null || !reg.valid(e)) return own;
    const UIElement* el = reg.try_get<UIElement>(e);
    if (!el) return own;

    // A fitContent group rewrites its OWN far edges after its children lay out.
    if (const UILayoutGroup* self = reg.try_get<UILayoutGroup>(e); self && self->fitContent)
        own.selfFitsContent = true;

    const Parent* p = reg.try_get<Parent>(e);
    if (!p || !reg.valid(p->entity)) return own;
    const UILayoutGroup* lg = reg.try_get<UILayoutGroup>(p->entity);
    if (!lg) return own;
    // THE ESCAPE CLASS, verbatim from the layout walk: a stretch or fullscreen
    // child recurses un-positioned, so its RectTransform is honoured normally.
    if (el->fullscreen || el->anchorMin != el->anchorMax) return own;

    own.positionOwned = true;
    own.group = p->entity;
    own.flow = static_cast<LayoutFlow>(lg->kind);
    // Grid slots are exactly cellSize; Vertical/Horizontal slots take the child's
    // own natural size, so only its position is the group's.
    own.sizeOwned = lg->kind == UILayoutGroup::Kind::Grid;
    return own;
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

bool CanvasAncestryActive(const entt::registry& reg, entt::entity canvas,
                          bool editorForced) {
    if (!reg.valid(canvas)) return false;
    // Walk ANCESTORS only - the canvas's own `visible` flag is the caller's
    // business (layout, the pick and the surface pass each already test it).
    int depth = 0;
    const Parent* p0 = reg.try_get<Parent>(canvas);
    for (entt::entity cur = (p0 && reg.valid(p0->entity)) ? p0->entity : entt::null;
         cur != entt::null && depth < 64; ++depth) {
        if (const UIPanel* panel = reg.try_get<UIPanel>(cur);
            panel && !panel->active && !(editorForced && reg.all_of<EditorUIShow>(cur)))
            return false;
        if (const UIElement* el = reg.try_get<UIElement>(cur); el && !el->visible)
            return false;
        // A fully-faded group is an invisible click trap; matches the walk's own
        // `gOpacity <= 0.001f` rule. `interactable` alone is NOT checked here: it
        // must not stop the page from DRAWING, only from being pressed, and that
        // half is already folded per-element inside the walk.
        if (const UICanvasGroup* cg = reg.try_get<UICanvasGroup>(cur);
            cg && cg->opacity <= 0.001f)
            return false;
        const Parent* pp = reg.try_get<Parent>(cur);
        cur = (pp && reg.valid(pp->entity)) ? pp->entity : entt::null;
    }
    return true;
}

bool IsGlobalAction(const std::string& a) {
    if (a.rfind("setting:", 0) == 0) return true;
    static const char* kVerbs[] = {"play", "menu",  "restart", "settings",
                                   "back", "quit",  "caption"};
    for (const char* v : kVerbs)
        if (a == v) return true;
    return false;
}

// Shared walk over a prebuilt children map (the map is the expensive part; the
// walk itself is cheap and runs once per frame).
//
// `docFilter` (0 = every tree, the runtime) restricts the walk to the TREE ROOTS
// carrying UIDocMember{docFilter} - the dedicated `.hbui` editor's authoring
// build. Only the two ROOT-SELECTION loops consult it: a document is a set of
// whole subtrees, so filtering roots filters everything under them, and the
// recursive walk is untouched (i.e. byte-identical for docFilter == 0).
static void LayoutUIImpl(Scene& scene, glm::vec2 targetSize,
                         const CanvasConfig& legacyConfig,
                         const std::unordered_map<u32, std::vector<entt::entity>>& children,
                         std::vector<LayoutItem>& out, u32 docFilter = 0) {
    out.clear();
    auto& reg = scene.Registry();
    // EditorUIShow (a session-only tag) lets the editor look at an INACTIVE panel.
    // Gated on EditorView so neither the runtime nor a shipped build can be
    // affected by one, AND on UIAuthoringView so PLAY MODE isn't either: EditorView
    // stays true for the whole editor process (it drives EditorHidden culling), so
    // on its own it left the authored screen force-visible while playing, stacked
    // over whatever the game flow actually showed. See Scene::SetUIAuthoringView.
    const bool editorView = scene.EditorView() && scene.UIAuthoringView();

    // Root membership test for the editor's document-scoped build.
    const auto inDoc = [&](entt::entity e) {
        if (docFilter == 0) return true;
        const UIDocMember* m = reg.try_get<UIDocMember>(e);
        return m != nullptr && m->doc == docFilter;
    };

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
        // subtree: skip laying it or its descendants out at all. The editor may
        // force ONE such screen visible for authoring (EditorUIShow) - session-only
        // view state, never serialized, never honoured outside the editor.
        if (const UIPanel* panel = reg.try_get<UIPanel>(e);
            panel && !panel->active && !(editorView && reg.all_of<EditorUIShow>(e)))
            return;
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
            if (const Parent* p = reg.try_get<Parent>(e); p && reg.valid(p->entity))
                item.parentEntity = p->entity;
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
            if (!inDoc(e)) continue; // editor document-scoped build
            if (!underCanvas(e) && !hasElementAncestor(e)) {
                walk(walk, e, root, canvas, entt::null, Rect{}, false, 1.0f, true, nullptr);
            }
        }
    }

    // Canvas trees, ascending sortOrder.
    std::vector<entt::entity> canvases;
    for (const entt::entity e : reg.view<UICanvas>()) {
        if (!inDoc(e)) continue; // editor document-scoped build
        if (!reg.get<UICanvas>(e).visible) continue;
        // A canvas is walked FROM ITSELF, so the UIPanel::active / element-visible
        // gates inside the walk never see anything above it. Without this, a
        // world-space page authored under the `Settings` screen root kept laying
        // out (and therefore kept taking picks) after the screen was popped.
        if (!CanvasAncestryActive(reg, e, editorView)) continue;
        canvases.push_back(e);
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
        // (surfaceInv is pruned per-pick by stamp in ui::PickWorldPage - a blunt
        // clear here re-inverted every live page on any UI structure change.)
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
// `setHover` is false for the element that only receives the WHEEL (a ScrollView
// under a Button): the hover highlight still belongs to exactly one element.
static bool ApplyPointerToElement(UIElement& el, const LayoutItem& item,
                                  glm::vec2 pointer, bool pressed, bool down,
                                  f32 wheel, bool setHover = true) {
    // Disabled widgets and non-interactive UICanvasGroup subtrees ignore the
    // pointer entirely (no hover, no click). Return false so the caller doesn't
    // add them to the touched list.
    if (!el.enabled || !item.groupInteractive) {
        el.hovered = false;
        el.held = false;
        return false;
    }
    // Content clipped away by an ancestor ScrollView can't be hovered/clicked.
    if (item.hasClip && !item.clip.Contains(pointer)) return false;
    const bool over = item.rect.Contains(pointer);
    if (setHover) el.hovered = over;
    // PRESS state, for as long as the button is down over the element. This is what
    // makes a 3D button look pressed while the player holds Interact; `clicked` is
    // one frame and would flicker. Not gated on `setHover`: the wheel-only target
    // is not being pressed either way, and this keeps "held" and "hovered" the same
    // element. RELEASE needs no flag - it is this going false.
    if (setHover) el.held = over && down;
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

// ONE WINNER PER POINTER PER FRAME - the shared hit-test, used by both the legacy
// and the cached interaction paths.
//
// Before this, EVERY overlapping element under the pointer hovered and clicked:
// there was no topmost rule, no break, no consumed flag, so a Button drawn over a
// Toggle pressed both, and a menu overlay clicked straight through to whatever sat
// behind it. Three rules, in order:
//
//   1. A LATCHED DRAG CANNOT BE STOLEN. A Slider grabbed last frame keeps the
//      pointer while the button is held, even when the pointer leaves its track
//      and even if something else is now on top.
//   2. TOPMOST WINS, and SCREEN SPACE BEATS WORLD SPACE. LayoutUI emits in draw
//      order (canvas-less roots, then canvases by ascending sortOrder, depth-first
//      parents-before-children), so topmost = the LAST eligible item - hence the
//      reverse walk. A screen overlay is by definition in front of the 3D world,
//      so a screen winner suppresses every world page this frame.
//   3. THE WHEEL BUBBLES SEPARATELY. Scroll lists are made of buttons; if the
//      wheel followed the hover winner, a list would stop scrolling wherever a
//      Button happened to be under the cursor. The wheel goes to the topmost
//      ScrollView under the pointer, which may be an ancestor of the hover winner.
static void ApplyPointerPass(Scene& scene, const std::vector<LayoutItem>& layout,
                             glm::vec2 pointerNorm, const PointerState* pointers,
                             bool pressed, bool down, f32 wheel,
                             std::vector<entt::entity>* touched) {
    auto& reg = scene.Registry();
    const bool hasPointer = pointerNorm.x >= 0.0f && pointerNorm.y >= 0.0f;
    const bool editorView = scene.EditorView();
    // Reticle mode: world pages are pressed with the Interact action, not LMB.
    const bool worldOverride = pointers && pointers->worldButtonOverride;
    const bool worldPressed = worldOverride ? pointers->worldPressed : pressed;
    const bool worldDown = worldOverride ? pointers->worldDown : down;

    // Resolves an item to its pointer position; false = this item is not a
    // candidate at all (invisible, non-interactive, editor-hidden, or its canvas
    // has no pointer this frame).
    const auto resolve = [&](const LayoutItem& item, glm::vec2& pointer,
                             bool& world) -> bool {
        if (!reg.valid(item.entity) || !reg.all_of<UIElement>(item.entity)) return false;
        const UIElement& el = reg.get<UIElement>(item.entity);
        if (!el.visible || !IsInteractive(el.type)) return false;
        if (editorView && scene.IsEditorHidden(item.entity)) return false;
        world = item.canvasEntity != entt::null && reg.valid(item.canvasEntity) &&
                reg.all_of<UICanvas>(item.canvasEntity) &&
                reg.get<UICanvas>(item.canvasEntity).worldSpace;
        if (world) {
            if (!pointers) return false;
            const auto pit =
                pointers->worldCanvasPx.find(static_cast<u32>(item.canvasEntity));
            if (pit == pointers->worldCanvasPx.end()) return false;
            pointer = pit->second;
        } else {
            if (!hasPointer) return false;
            pointer = pointerNorm * item.canvas;
        }
        return true;
    };
    // Eligible AND actually under the pointer.
    const auto over = [&](const LayoutItem& item, glm::vec2 pointer) -> bool {
        const UIElement& el = reg.get<UIElement>(item.entity);
        if (!el.enabled || !item.groupInteractive) return false;
        if (item.hasClip && !item.clip.Contains(pointer)) return false;
        return item.rect.Contains(pointer);
    };

    const usize n = layout.size();
    const LayoutItem* winner = nullptr;   // hover / press / drag
    glm::vec2 winnerPointer{0.0f};
    bool winnerWorld = false;
    const LayoutItem* wheelItem = nullptr; // wheel only
    glm::vec2 wheelPointer{0.0f};
    bool wheelWorld = false;

    // 1. Latched drag.
    for (usize i = n; i-- > 0;) {
        const LayoutItem& item = layout[i];
        glm::vec2 p; bool w = false;
        if (!resolve(item, p, w)) continue;
        const UIElement& el = reg.get<UIElement>(item.entity);
        if (!el.dragging || !el.enabled || !item.groupInteractive) continue;
        if (!(w ? worldDown : down)) continue;
        winner = &item; winnerPointer = p; winnerWorld = w;
        break;
    }
    // 2. Topmost, screen space before world space.
    for (int space = 0; space < 2 && !winner; ++space) {
        for (usize i = n; i-- > 0;) {
            const LayoutItem& item = layout[i];
            glm::vec2 p; bool w = false;
            if (!resolve(item, p, w)) continue;
            if (w != (space == 1)) continue;
            if (!over(item, p)) continue;
            winner = &item; winnerPointer = p; winnerWorld = w;
            break;
        }
    }
    // 3. Wheel target: topmost ScrollView under the pointer, same space priority.
    if (wheel != 0.0f) {
        for (int space = 0; space < 2 && !wheelItem; ++space) {
            for (usize i = n; i-- > 0;) {
                const LayoutItem& item = layout[i];
                glm::vec2 p; bool w = false;
                if (!resolve(item, p, w)) continue;
                if (w != (space == 1)) continue;
                if (reg.get<UIElement>(item.entity).type != UIElement::Type::ScrollView)
                    continue;
                if (!over(item, p)) continue;
                wheelItem = &item; wheelPointer = p; wheelWorld = w;
                break;
            }
        }
    }

    const auto apply = [&](const LayoutItem& item, glm::vec2 pointer, bool world, bool pr,
                           bool dn, f32 wh, bool setHover) {
        UIElement& el = reg.get<UIElement>(item.entity);
        (void)world;
        if (ApplyPointerToElement(el, item, pointer, pr, dn, wh, setHover) && touched)
            touched->push_back(item.entity);
    };
    if (winner) {
        const bool sameWheel = wheelItem == winner;
        apply(*winner, winnerPointer, winnerWorld, winnerWorld ? worldPressed : pressed,
              winnerWorld ? worldDown : down, sameWheel ? wheel : 0.0f, true);
        if (wheelItem && !sameWheel)
            apply(*wheelItem, wheelPointer, wheelWorld, false, false, wheel, false);
    } else if (wheelItem) {
        apply(*wheelItem, wheelPointer, wheelWorld, false, false, wheel, true);
    }
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
    const bool anyDown =
        down || (pointers && pointers->worldButtonOverride && pointers->worldDown);

    // Clear per-frame hover/click/changed. `dragging` persists across frames while the
    // button is held, so it's only cleared on release (below), not here.
    for (const entt::entity e : reg.view<UIElement>()) {
        UIElement& el = reg.get<UIElement>(e);
        el.hovered = false;
        el.clicked = false;
        el.held = false;
        el.changed = false;
        if (!anyDown) el.dragging = false; // released -> end any slider drag
    }
    if (!hasPointer && !hasWorldPointer) return;

    static thread_local std::vector<LayoutItem> layout;
    LayoutUI(scene, targetSize, config, layout);
    ApplyPointerPass(scene, layout, pointerNorm, pointers, pressed, down, wheel, nullptr);
}

void UpdateInteraction(Scene& scene, const Input& input, glm::vec2 pointerNorm,
                       const PointerState* pointers, UIContext& ctx) {
    auto& reg = scene.Registry();
    const bool hasPointer = pointerNorm.x >= 0.0f && pointerNorm.y >= 0.0f;
    const bool hasWorldPointer = pointers && !pointers->worldCanvasPx.empty();
    const bool pressed = input.WasMousePressed(MouseButton::Left);
    const bool down = input.IsMouseDown(MouseButton::Left);
    const f32 wheel = input.MouseWheel();
    const bool anyDown =
        down || (pointers && pointers->worldButtonOverride && pointers->worldDown);

    // Clear per-frame flags ONLY on the elements touched last frame (instead of
    // a full registry scan). `dragging` persists while the button is held.
    for (const entt::entity e : ctx.interactives) {
        if (!reg.valid(e) || !reg.all_of<UIElement>(e)) continue;
        UIElement& el = reg.get<UIElement>(e);
        el.hovered = false;
        el.clicked = false;
        el.held = false;
        el.changed = false;
        if (!anyDown) el.dragging = false;
    }
    ctx.interactives.clear();
    if (!hasPointer && !hasWorldPointer) return;

    // Hit-test against LAST frame's layout (ctx.layout, filled by BuildVertices)
    // - the effective behavior of the old two-walk code, since animations run
    // after interaction. Empty on the very first frame (nothing to hit).
    ApplyPointerPass(scene, ctx.layout, pointerNorm, pointers, pressed, down, wheel,
                     &ctx.interactives);
}

static void BuildVerticesImpl(Scene& scene, Renderer& renderer,
                              const std::filesystem::path& assetsDir,
                              glm::vec2 targetSize, const CanvasConfig& config,
                              std::vector<rhi::UIVertex>& out,
                              std::vector<WorldUIBatch>* worldOut,
                              const std::vector<LayoutItem>& layout, UIContext* ctx,
                              bool allToOut = false) {
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
        // `allToOut` (the editor's document-scoped authoring build) bypasses the
        // routing entirely: ONE buffer, world-space canvases included, because the
        // authoring surface IS a single texture and a page the author is editing
        // must be visible on it.
        std::vector<rhi::UIVertex>* dest = &out;
        if (!allToOut && item.canvasEntity != entt::null) {
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
        // PRESSED = the click edge OR the whole hold. `clicked` alone made the
        // pressed skin visible for exactly one frame, which reads as a flicker on a
        // screen button and as nothing at all on a 3D button the player is holding.
        const bool pressedState = el.clicked || el.held;
        const auto stateFill = [&](const glm::vec4& base) -> glm::vec4 {
            glm::vec4 f = base;
            if (disabled)
                f = el.disabledColor.a > 0.0f ? el.disabledColor : base;
            else if (pressedState)
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
                else if (pressedState && !el.pressedTexture.empty()) stateRel = &el.pressedTexture;
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
                // A non-empty texture path that failed to resolve (tex==0) would draw
                // a solid white box; skip it. An empty path is an intentional colour rect.
                if (el.texture.empty() || tex != 0) quadOrSlice(rect, el.color, tex, el.texture);
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

void BuildDocumentVertices(Scene& scene, Renderer& renderer,
                           const std::filesystem::path& assetsDir,
                           glm::vec2 targetSize, const CanvasConfig& docConfig,
                           u32 doc, std::vector<rhi::UIVertex>& out,
                           std::vector<LayoutItem>& outLayout) {
    out.clear();
    outLayout.clear();
    if (doc == 0) return;
    auto& reg = scene.Registry();

    // LayoutUIImpl WRITES contentExtent/viewExtent on ScrollViews (the auto
    // content measurement the wheel clamp and auto-scroll read). This is an EXTRA
    // layout pass, at the AUTHORING canvas size, inside a frame whose runtime pass
    // already ran - so snapshot those two fields and put them back, or a document
    // with a ScrollView would have its runtime scroll limits follow the editor
    // panel's canvas for a frame. Nothing else in the layout or the emission
    // writes to a UIElement (verified), so this is the whole exposure.
    struct SavedScroll {
        entt::entity e;
        glm::vec2 content;
        glm::vec2 view;
    };
    static thread_local std::vector<SavedScroll> saved;
    saved.clear();
    for (const entt::entity e : reg.view<UIElement>()) {
        const UIElement& el = reg.get<UIElement>(e);
        if (el.type == UIElement::Type::ScrollView)
            saved.push_back({e, el.contentExtent, el.viewExtent});
    }

    // Uncached children map (the editor's own pass must not touch the runtime
    // UIContext, and one map rebuild for one document is nothing).
    static thread_local std::unordered_map<u32, std::vector<entt::entity>> children;
    BuildChildrenMap(reg, children);
    LayoutUIImpl(scene, targetSize, docConfig, children, outLayout, doc);
    BuildVerticesImpl(scene, renderer, assetsDir, targetSize, docConfig, out,
                      /*worldOut*/ nullptr, outLayout, /*ctx*/ nullptr,
                      /*allToOut*/ true);

    for (const SavedScroll& s : saved) {
        if (UIElement* el = reg.try_get<UIElement>(s.e)) {
            el->contentExtent = s.content;
            el->viewExtent = s.view;
        }
    }
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

// --- --test-uisolve: the direct-manipulation math gate ----------------------
//
// Everything the dedicated `.hbui` editor's drag loop does reduces to
// ComputeElementRect / SolveElementFromRect plus one classification
// (LayoutGroupOwnership). That is the whole pipeline minus the mouse, and it is
// genuinely headless - so it is tested, rather than eyeballed.
bool ManipulationSelfTest() {
    bool ok = true;
    const auto expect = [&ok](bool cond, const std::string& what) {
        if (!cond) {
            ok = false;
            HBE_ERROR("uisolve: FAILED - {}", what);
        }
    };
    const auto near2 = [](glm::vec2 a, glm::vec2 b, f32 eps = 0.002f) {
        return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps;
    };
    const auto nearRect = [&](const Rect& a, const Rect& b, f32 eps = 0.002f) {
        return near2({a.x0, a.y0}, {b.x0, b.y0}, eps) && near2({a.x1, a.y1}, {b.x1, b.y1}, eps);
    };

    // --- 1) SolveElementFromRect is the inverse of ComputeElementRect ---------
    // Deterministic LCG fuzz: anchors (including inverted and degenerate ones),
    // pivots, sizes, offsets, and parent rects that are not at the origin.
    {
        u32 s = 0x51ed270bu;
        const auto rnd = [&s]() {
            s = s * 1664525u + 1013904223u;
            return static_cast<f32>((s >> 8) & 0xffffu) / 65535.0f;
        };
        const f32 anchorPick[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
        int cases = 0;
        for (int i = 0; i < 4000; ++i) {
            UIElement el;
            el.anchorMin = {anchorPick[static_cast<int>(rnd() * 4.999f)],
                            anchorPick[static_cast<int>(rnd() * 4.999f)]};
            el.anchorMax = {anchorPick[static_cast<int>(rnd() * 4.999f)],
                            anchorPick[static_cast<int>(rnd() * 4.999f)]};
            el.pivot = {rnd(), rnd()};
            const Rect parent{rnd() * 400.0f - 200.0f, rnd() * 400.0f - 200.0f, 0.0f, 0.0f};
            const Rect p{parent.x0, parent.y0, parent.x0 + 100.0f + rnd() * 1800.0f,
                         parent.y0 + 100.0f + rnd() * 1000.0f};
            // A target rect the drag loop could plausibly ask for, including ones
            // outside the parent (dragging an element off the edge is legal).
            const f32 w = 8.0f + rnd() * 900.0f, h = 8.0f + rnd() * 500.0f;
            const f32 x = p.x0 + rnd() * 1400.0f - 300.0f;
            const f32 y = p.y0 + rnd() * 900.0f - 200.0f;
            const Rect desired{x, y, x + w, y + h};

            const glm::vec2 aMin = el.anchorMin, aMax = el.anchorMax, piv = el.pivot;
            SolveElementFromRect(el, p, desired);
            expect(el.anchorMin == aMin && el.anchorMax == aMax && el.pivot == piv,
                   "a solve changed the anchors or the pivot");
            // UNCONDITIONAL: the inverse holds for any `desired` with a
            // non-negative size, including an INVERTED anchor region (anchorMax <
            // anchorMin), because both sides clamp regionSize the same way. No
            // case is skipped - if that ever stops being true the drag loop has a
            // silent dead zone and this must fail rather than filter.
            expect(nearRect(ComputeElementRect(el, p), desired),
                   "solve -> compute did not reproduce the rect");
            ++cases;
        }
        expect(cases == 4000, "the fuzz did not run every case");
    }

    // --- 2) fullscreen is a no-op on BOTH sides ------------------------------
    {
        UIElement el;
        el.fullscreen = true;
        el.offset = {11.0f, 22.0f};
        el.size = {33.0f, 44.0f};
        const Rect p{10.0f, 20.0f, 1930.0f, 1100.0f};
        expect(nearRect(ComputeElementRect(el, p), p), "fullscreen rect is not the parent's");
        SolveElementFromRect(el, p, Rect{0.0f, 0.0f, 5.0f, 5.0f});
        expect(el.offset == glm::vec2(11.0f, 22.0f) && el.size == glm::vec2(33.0f, 44.0f),
               "solving a fullscreen element wrote to it");
    }

    // --- 3) RE-ANCHORING: write the anchors, re-solve the SAME rect ----------
    // This IS the anchor widget: presets and the draggable diamonds both work by
    // keeping the rect the author can see and re-deriving offset/size for the new
    // anchors. If it is not bit-stable the element visibly jumps on every preset
    // click, so the guarantee is pinned at full float precision, not an epsilon.
    {
        const Rect p{0.0f, 0.0f, 1920.0f, 1080.0f};
        const glm::vec2 presets[] = {{0, 0}, {0.5f, 0}, {1, 0}, {0, 0.5f}, {0.5f, 0.5f},
                                     {1, 0.5f}, {0, 1}, {0.5f, 1}, {1, 1}};
        UIElement el;
        el.anchorMin = el.anchorMax = {0.5f, 0.5f};
        el.pivot = {0.5f, 0.5f};
        el.size = {320.0f, 96.0f};
        el.offset = {-40.0f, 210.0f};
        const Rect start = ComputeElementRect(el, p);
        for (const glm::vec2 a : presets) {
            // point anchors
            const Rect keep = ComputeElementRect(el, p);
            el.anchorMin = el.anchorMax = a;
            el.pivot = a;
            SolveElementFromRect(el, p, keep);
            const Rect after = ComputeElementRect(el, p);
            expect(after.x0 == keep.x0 && after.y0 == keep.y0 && after.x1 == keep.x1 &&
                       after.y1 == keep.y1,
                   "a point-anchor preset moved the element");
        }
        // stretch presets: an anchor SPREAD must also preserve the rect
        const glm::vec2 spreadMin[] = {{0, 0}, {0, 0}, {0, 0.5f}};
        const glm::vec2 spreadMax[] = {{1, 1}, {1, 0}, {1, 0.5f}};
        for (int i = 0; i < 3; ++i) {
            const Rect keep = ComputeElementRect(el, p);
            el.anchorMin = spreadMin[i];
            el.anchorMax = spreadMax[i];
            el.pivot = (spreadMin[i] + spreadMax[i]) * 0.5f;
            SolveElementFromRect(el, p, keep);
            const Rect after = ComputeElementRect(el, p);
            expect(after.x0 == keep.x0 && after.y0 == keep.y0 && after.x1 == keep.x1 &&
                       after.y1 == keep.y1,
                   "a stretch-anchor preset moved the element");
        }
        // ...and after all that churn the rect is still the one we started with.
        expect(nearRect(ComputeElementRect(el, p), start),
               "the rect drifted across a chain of re-anchors");
        // A stretched element must RESPOND to a parent resize (that is the whole
        // point of stretching) while a point-anchored one must not.
        {
            UIElement str;
            str.anchorMin = {0.0f, 0.0f};
            str.anchorMax = {1.0f, 1.0f};
            str.pivot = {0.5f, 0.5f};
            SolveElementFromRect(str, p, Rect{100.0f, 100.0f, 1820.0f, 980.0f});
            const Rect wide = ComputeElementRect(str, Rect{0.0f, 0.0f, 2560.0f, 1080.0f});
            expect(std::fabs(wide.x1 - wide.x0 - 1720.0f - 640.0f) < 0.01f,
                   "a stretched element did not follow a wider parent");
            UIElement pt;
            pt.anchorMin = pt.anchorMax = {0.5f, 0.5f};
            pt.pivot = {0.5f, 0.5f};
            SolveElementFromRect(pt, p, Rect{100.0f, 100.0f, 420.0f, 200.0f});
            const Rect same = ComputeElementRect(pt, Rect{0.0f, 0.0f, 2560.0f, 1080.0f});
            expect(std::fabs(same.Size().x - 320.0f) < 0.01f &&
                       std::fabs(same.Size().y - 100.0f) < 0.01f,
                   "a point-anchored element resized with its parent");
        }
    }

    // --- 4) the snap helper is idempotent -----------------------------------
    // The editor snaps a candidate coordinate to a grid every frame of a drag; a
    // non-idempotent snap makes a held drag creep.
    {
        const auto snap = [](f32 v, f32 step) { return std::round(v / step) * step; };
        for (int i = -400; i <= 400; ++i) {
            const f32 v = static_cast<f32>(i) * 1.37f;
            for (const f32 step : {1.0f, 4.0f, 8.0f, 25.0f}) {
                const f32 a = snap(v, step);
                expect(snap(a, step) == a, "snap is not idempotent");
            }
        }
    }

    // --- 5) LayoutGroupOwnership vs what LayoutUI ACTUALLY did ---------------
    // The predicate exists to stop the editor writing a rect layout discards, so
    // it is cross-checked against the real layout walk instead of against a
    // second copy of the escape rule.
    {
        for (int k = 0; k < 3; ++k) {
            Scene scene;
            auto& reg = scene.Registry();
            const entt::entity root = scene.CreateEntity("root");
            UIElement& rel = reg.emplace<UIElement>(root);
            rel.type = UIElement::Type::Panel;
            rel.anchorMin = {0.0f, 0.0f};
            rel.anchorMax = {0.0f, 0.0f};
            rel.pivot = {0.0f, 0.0f};
            rel.size = {800.0f, 600.0f};
            UILayoutGroup& lg = reg.emplace<UILayoutGroup>(root);
            lg.kind = static_cast<UILayoutGroup::Kind>(k);
            lg.spacing = 6.0f;
            lg.columns = 2;
            lg.cellSize = {120.0f, 40.0f};

            // three managed children + one stretch-class escapee + one fullscreen
            entt::entity kids[5];
            for (int i = 0; i < 5; ++i) {
                kids[i] = scene.CreateEntity("kid");
                reg.emplace<Parent>(kids[i]).entity = root;
                UIElement& el = reg.emplace<UIElement>(kids[i]);
                el.type = UIElement::Type::Label;
                el.anchorMin = el.anchorMax = {0.0f, 0.0f};
                el.pivot = {0.0f, 0.0f};
                el.size = {90.0f + static_cast<f32>(i) * 7.0f, 30.0f};
                el.offset = {17.0f * static_cast<f32>(i + 1), 300.0f}; // ignored if managed
            }
            reg.get<UIElement>(kids[3]).anchorMax = {1.0f, 0.0f}; // stretch on x
            reg.get<UIElement>(kids[4]).fullscreen = true;

            std::vector<LayoutItem> layout;
            CanvasConfig cfg;
            cfg.mode = ScaleMode::Stretch;
            cfg.refWidth = 1920.0f;
            cfg.refHeight = 1080.0f;
            LayoutUI(scene, glm::vec2(1920.0f, 1080.0f), cfg, layout);

            const auto itemFor = [&](entt::entity e) -> const LayoutItem* {
                for (const LayoutItem& it : layout)
                    if (it.entity == e) return &it;
                return nullptr;
            };
            const LayoutItem* rit = itemFor(root);
            expect(rit != nullptr, "the layout-group root did not lay out");
            if (!rit) continue;

            for (int i = 0; i < 5; ++i) {
                const LayoutOwnership own = LayoutGroupOwnership(scene, kids[i]);
                const LayoutItem* it = itemFor(kids[i]);
                expect(it != nullptr, "a layout-group child did not lay out");
                if (!it) continue;
                // What the child's OWN RectTransform would have produced.
                const Rect self = ComputeElementRect(reg.get<UIElement>(kids[i]), it->parentRect);
                const bool overwritten = !nearRect(self, it->rect, 0.01f);
                if (i < 3) {
                    expect(own.positionOwned,
                           "a group-placed child was reported as authorable");
                    expect(overwritten, "layout did NOT overwrite a rect we refuse to edit");
                    expect(own.group == root, "the reported owning group is wrong");
                    expect(own.sizeOwned == (k == 2), "sizeOwned disagrees with Grid-ness");
                    if (k == 2) {
                        expect(near2(it->rect.Size(), lg.cellSize, 0.01f),
                               "a Grid slot is not cellSize");
                    } else {
                        expect(near2(it->rect.Size(), self.Size(), 0.01f),
                               "a Vertical/Horizontal slot did not keep the child's size");
                    }
                } else {
                    expect(!own.positionOwned,
                           "a stretch/fullscreen escapee was reported as group-owned");
                    expect(!overwritten,
                           "layout overwrote a rect the predicate calls authorable");
                }
            }
            // The group element itself: fitContent off here, so its own rect is a
            // pure ComputeElementRect result and authorable.
            expect(!LayoutGroupOwnership(scene, root).selfFitsContent,
                   "a non-fitContent group reported itself as content-fitted");
            expect(!LayoutGroupOwnership(scene, root).positionOwned,
                   "a root reported a position owner");
        }
        // fitContent: the group's own far edges ARE rewritten after its children.
        {
            Scene scene;
            auto& reg = scene.Registry();
            const entt::entity root = scene.CreateEntity("root");
            UIElement& rel = reg.emplace<UIElement>(root);
            rel.anchorMin = rel.anchorMax = {0.0f, 0.0f};
            rel.pivot = {0.0f, 0.0f};
            rel.size = {40.0f, 40.0f}; // deliberately too small for its children
            UILayoutGroup& lg = reg.emplace<UILayoutGroup>(root);
            lg.kind = UILayoutGroup::Kind::Vertical;
            lg.fitContent = true;
            for (int i = 0; i < 3; ++i) {
                const entt::entity kid = scene.CreateEntity("kid");
                reg.emplace<Parent>(kid).entity = root;
                UIElement& el = reg.emplace<UIElement>(kid);
                el.anchorMin = el.anchorMax = {0.0f, 0.0f};
                el.pivot = {0.0f, 0.0f};
                el.size = {200.0f, 50.0f};
            }
            std::vector<LayoutItem> layout;
            CanvasConfig cfg;
            cfg.mode = ScaleMode::Stretch;
            LayoutUI(scene, glm::vec2(1920.0f, 1080.0f), cfg, layout);
            const LayoutOwnership own = LayoutGroupOwnership(scene, root);
            expect(own.selfFitsContent, "a fitContent group did not report itself");
            bool grew = false;
            for (const LayoutItem& it : layout)
                if (it.entity == root) grew = it.rect.Size().y > 60.0f;
            expect(grew, "fitContent did not rewrite the group's own far edge");
        }
        // NESTED: a fitContent group that is ITSELF a managed child of another
        // group - the ordinary settings-menu shape (a column of fit-to-content
        // rows). Both ownerships apply at once, and the editor's Editable() used to
        // return on the first one and offer resize handles whose write layout threw
        // away on the same frame.
        {
            Scene scene;
            auto& reg = scene.Registry();
            const entt::entity outer = scene.CreateEntity("outer");
            {
                UIElement& el = reg.emplace<UIElement>(outer);
                el.anchorMin = el.anchorMax = {0.0f, 0.0f};
                el.pivot = {0.0f, 0.0f};
                el.size = {800.0f, 600.0f};
                UILayoutGroup& lg = reg.emplace<UILayoutGroup>(outer);
                lg.kind = UILayoutGroup::Kind::Vertical;
                lg.spacing = 4.0f;
            }
            const entt::entity row = scene.CreateEntity("row");
            reg.emplace<Parent>(row).entity = outer;
            {
                UIElement& el = reg.emplace<UIElement>(row);
                el.anchorMin = el.anchorMax = {0.0f, 0.0f};
                el.pivot = {0.0f, 0.0f};
                el.size = {40.0f, 40.0f}; // deliberately smaller than its children
                UILayoutGroup& lg = reg.emplace<UILayoutGroup>(row);
                lg.kind = UILayoutGroup::Kind::Horizontal;
                lg.fitContent = true;
            }
            for (int i = 0; i < 2; ++i) {
                const entt::entity cell = scene.CreateEntity("cell");
                reg.emplace<Parent>(cell).entity = row;
                UIElement& el = reg.emplace<UIElement>(cell);
                el.anchorMin = el.anchorMax = {0.0f, 0.0f};
                el.pivot = {0.0f, 0.0f};
                el.size = {150.0f, 30.0f};
            }
            const LayoutOwnership own = LayoutGroupOwnership(scene, row);
            expect(own.positionOwned && own.selfFitsContent,
                   "a fitContent group inside another group must report BOTH "
                   "ownerships - the editor combines them to decide what may be "
                   "dragged");
            expect(!own.sizeOwned, "a Horizontal parent does not own its child's size");

            std::vector<LayoutItem> layout;
            CanvasConfig cfg;
            cfg.mode = ScaleMode::Stretch;
            LayoutUI(scene, glm::vec2(1920.0f, 1080.0f), cfg, layout);
            const LayoutItem* rowIt = nullptr;
            for (const LayoutItem& it : layout)
                if (it.entity == row) rowIt = &it;
            expect(rowIt != nullptr, "the nested group did not lay out");
            // THE GATE: its authored 40x40 is honoured by NEITHER axis - the parent
            // group places it and fitContent sizes it - so an editor that offered a
            // resize would be writing into a field layout overwrites every frame.
            if (rowIt)
                expect(rowIt->rect.Size().x > 200.0f,
                       "fitContent did not rewrite a NESTED group's far edge (the "
                       "resize refusal depends on this)");
        }
        // No group at all: nothing is owned.
        {
            Scene scene;
            auto& reg = scene.Registry();
            const entt::entity a = scene.CreateEntity("a");
            reg.emplace<UIElement>(a);
            const entt::entity b = scene.CreateEntity("b");
            reg.emplace<Parent>(b).entity = a;
            reg.emplace<UIElement>(b);
            expect(!LayoutGroupOwnership(scene, b).positionOwned,
                   "a plain parent was reported as a layout group");
            expect(!LayoutGroupOwnership(scene, entt::null).positionOwned,
                   "entt::null was reported as owned");
        }
    }

    return ok;
}

} // namespace hbe::ui
