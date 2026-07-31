// Editor/UIEditor.cpp - the DEDICATED `.hbui` UI editor: its own 2D canvas.
//
// NOT the scene-view thing. This is a separate authoring surface, like Unity's UI
// Builder: the active document is laid out and rendered by the SHIPPED UI pass
// into its own render target and shown as an image, so what the author sees cannot
// drift from what ships. There is deliberately NO second UI renderer here - if a
// UI feature does not appear on this canvas, the fix belongs in
// ui::BuildVerticesImpl, not in this file.
//
// HOW THE IMAGE GETS THERE, and why it is NOT one frame stale:
//   ui::AcquireUITarget            -> one render target, keyed by SIZE (see budget)
//   ui::BuildDocumentVertices      -> the real layout + the real emission, one doc
//   Renderer::SetEditorUICanvas    -> a per-frame draw slot
//   Renderer::RenderScene          -> DrawUIToTexture, BEFORE the ImGui pass
// Engine::Run calls the editor's BuildUI hook BEFORE RenderScene, and inside
// RenderScene the UI-to-texture block runs before device_->RenderUI(). So a canvas
// built during this function is written and sampled in the same frame.
//
// ORDER INSIDE THIS FUNCTION, which is load-bearing: input is handled FIRST,
// against the PREVIOUS frame's layout, and the layout + vertices are rebuilt
// AFTER. That way a drag applied this frame is what the page picture and the
// selection outline show this frame - zero lag. Hit-testing a press against last
// frame's rects is not a compromise either: it is exactly the geometry the author
// was looking at when they pressed (and it is what the runtime's own interaction
// does with ctx.layout). uiEdLayoutCanvas_ guards the one case where last frame's
// rects are the wrong basis - the canvas itself changed - by skipping input for
// that single frame instead of mis-registering a click.
//
// THREE CONSTRAINTS THAT SHAPE THIS PANEL
//   1. UI RENDER TARGETS ARE CAPPED AT 8 PER PROCESS AND CANNOT BE FREED (both
//      backends: kMaxUITargets, and the RHI has no texture destroy). So the target
//      is sized to the DOCUMENT'S CANVAS and never to the ImGui window - zoom is a
//      pure display transform. Each distinct previewed canvas width costs one
//      permanent slot, which is why the budget is on screen.
//   2. Engine falls back to RAW EDITOR-WINDOW mouse coordinates for the in-game UI
//      pointer when nobody feeds one, and the only feeder is the Game view - which
//      early-returns when its tab is closed or behind another. That means dragging
//      in this panel could otherwise live-mutate the authored document (toggled /
//      value / scrollPos are written in place by UpdateInteraction). So the panel
//      pins the pointer OFF while it is open, unless the Game view is legitimately
//      driving it this frame.
//   3. OpenGL has no CreateUITarget and no ImGui texture ids, so the canvas
//      degrades to a frame + a message, exactly like the Viewport/Game panels do.
//
// WHAT THIS PANEL CAN DO
//   I1  the canvas: zoom, pan, fit, aspect preview, the screen picker, readouts.
//   I2  direct manipulation: click to select, drag to move, eight resize handles,
//       a draggable + preset-driven ANCHOR widget, grid snapping plus parent and
//       sibling edge/centre magnetism with a Ctrl override, and arrow-key nudge.
//   I3  a real editor rather than a viewer: the element PALETTE (drag onto the
//       canvas or click to add), the DOCUMENT TREE with reparent by drag, TABS
//       over every open document, clipboard / delete / duplicate / z-order, and
//       an INTERACT preview that runs the document through the real
//       ui::UpdateInteraction so states can be tested without entering Play.
//
// THE ONE RULE EVERY EDIT OBEYS: a gesture computes a RECT in the item's own
// canvas units and hands it to ui::SolveElementFromRect, which back-solves
// `offset` and `size` against the anchor region while leaving anchors and pivot
// alone. So the result is what the runtime's ComputeElementRect will produce -
// never a screen-space fudge - and a stretched element stays stretched.
//
// THE PALETTE IS NOT A SECOND CREATION PATH. It calls the SAME function the
// Hierarchy's Create > UI menu now calls - Editor::CreateUIElementInDocument -
// off the SAME recipe catalog, so the creation-time separation guard
// (`activeDoc_ != 0`) and the DocumentSet::Track ordering contract are enforced
// in exactly one place. Adding a palette that emplaced components itself would
// have re-opened the one hole this whole subsystem exists to close.
//
// WHAT IT DELIBERATELY DOES NOT DO
//   * No multi-select / marquee. `selected_` is one entity editor-wide (Inspector,
//     Hierarchy, gizmo, undo restore all key off it); a second selection model is
//     an editor-wide project, not a UI-editor-local hack.
//   * The canvas gestures write only `offset`, `size`, `anchorMin`, `anchorMax`
//     and `pivot`. Everything else about an element - action, text, colour, the
//     nine part textures - is the Inspector's; this panel opens it rather than
//     growing a second copy of it.
//   * No WorldText. It is not a `.hbui` key at all (UIDocument.h decision 4): 3D
//     signage is placed by a Transform in the LEVEL, and sweeping it in here would
//     yank it out of the scene it belongs to.
#include "Editor/Editor.h"

#include "Core/Input.h" // the Interact preview drives the real ui::UpdateInteraction
#include "Core/Log.h"
#include "Engine/Engine.h"
#include "Project/Project.h"
#include "Renderer/Renderer.h"
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h" // SaveSubtreeToString (the document-scoped fragment)
#include "UI/UISystem.h"
#include "UI/UIWorld.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace hbe {

namespace {

// Aspect-ratio previews. CanvasScaler behaviour depends on the OUTPUT aspect
// (MatchHeight derives the canvas width from it, PixelPerfect is the target
// outright), so an author has to be able to see other aspects - a menu that only
// ever gets checked at 16:9 is a menu that breaks on an ultrawide.
//
// COST: for MatchHeight / PixelPerfect each distinct aspect is a distinct canvas
// WIDTH, hence a distinct render target, hence one of the eight permanent slots.
// Kept short for that reason. (Stretch mode reuses one target for all of them:
// its canvas is always the reference size, and only the display box stretches.)
struct AspectPreset {
    const char* label;
    f32 aspect; // <= 0 = "the document's own reference aspect"
};
constexpr AspectPreset kAspects[] = {
    {"Document", 0.0f},
    {"16:9", 16.0f / 9.0f},
    {"16:10", 16.0f / 10.0f},
    {"4:3", 4.0f / 3.0f},
    {"21:9", 21.0f / 9.0f},
};
constexpr int kAspectCount = static_cast<int>(sizeof(kAspects) / sizeof(kAspects[0]));

// The PROJECT's canvas configuration - what Engine.cpp actually hands
// ui::BuildVertices as `uiConfig` every runtime frame. Duplicated from the
// identically-named helper in Editor.cpp (both are file-local by design; the
// value is three fields off BuildSettings and a shared header for it would be
// more coupling than it removes).
ui::CanvasConfig ProjectCanvasConfig() {
    ui::CanvasConfig config;
    if (Project::HasActive()) {
        const BuildSettings& build = Project::Active().Settings().build;
        config.mode = static_cast<ui::ScaleMode>(glm::clamp(build.uiScaleMode, 0u, 2u));
        config.refWidth = static_cast<f32>(glm::max(build.uiRefWidth, 64u));
        config.refHeight = static_cast<f32>(glm::max(build.uiRefHeight, 64u));
    }
    return config;
}

bool SameCanvasConfig(const ui::CanvasConfig& a, const ui::CanvasConfig& b) {
    return a.mode == b.mode && std::fabs(a.refWidth - b.refWidth) < 0.5f &&
           std::fabs(a.refHeight - b.refHeight) < 0.5f;
}

const char* ScaleModeName(ui::ScaleMode m) {
    switch (m) {
        case ui::ScaleMode::Stretch:      return "Stretch";
        case ui::ScaleMode::MatchHeight:  return "Match Height";
        case ui::ScaleMode::PixelPerfect: return "Pixel Perfect";
    }
    return "?";
}

// Zoom bounds in screen px per canvas unit. The low end has to reach a 4096-wide
// canvas inside a small docked panel; the high end is for eyeballing a 1px border.
constexpr f32 kZoomMin = 0.02f;
constexpr f32 kZoomMax = 8.0f;

// -- Gesture modes (Editor::uiEdDragMode_) ----------------------------------
constexpr int kModeNone = 0;
constexpr int kModeMove = 1;
constexpr int kModeResize0 = 2;  // + index into kHandles (8 of them) => 2..9
constexpr int kModeAnchor0 = 10; // + index into kAnchors  (4 of them) => 10..13

// The eight resize handles. `ex0/ey0/ex1/ey1` say which EDGES of the rect follow
// the cursor; `hx/hy` place the handle ON the rect (0 = min edge, 0.5 = centre,
// 1 = max edge). Corners are listed first so they win the press hit-test where
// they overlap an edge handle's grab box.
struct Handle {
    f32 hx, hy;
    bool ex0, ey0, ex1, ey1;
    ImGuiMouseCursor cursor;
};
constexpr Handle kHandles[8] = {
    {0.0f, 0.0f, true, true, false, false, ImGuiMouseCursor_ResizeNWSE},   // TL
    {1.0f, 0.0f, false, true, true, false, ImGuiMouseCursor_ResizeNESW},   // TR
    {1.0f, 1.0f, false, false, true, true, ImGuiMouseCursor_ResizeNWSE},   // BR
    {0.0f, 1.0f, true, false, false, true, ImGuiMouseCursor_ResizeNESW},   // BL
    {0.5f, 0.0f, false, true, false, false, ImGuiMouseCursor_ResizeNS},    // T
    {1.0f, 0.5f, false, false, true, false, ImGuiMouseCursor_ResizeEW},    // R
    {0.5f, 1.0f, false, false, false, true, ImGuiMouseCursor_ResizeNS},    // B
    {0.0f, 0.5f, true, false, false, false, ImGuiMouseCursor_ResizeEW},    // L
};
constexpr int kHandleCount = 8;

// The four anchor-region corners, as which COMPONENT of which anchor each one
// drives: 0 = anchorMin, 1 = anchorMax, per axis.
struct AnchorCorner {
    int xi, yi;
};
constexpr AnchorCorner kAnchors[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};

// Anchor presets. The 3x3 point grid is the common case (a corner-pinned or
// centred element); the three spreads are what makes a menu survive a resolution
// change, which is the single field authors get wrong most often.
struct AnchorPreset {
    const char* label;
    const char* tip;
    glm::vec2 mn, mx, pivot;
};
constexpr AnchorPreset kPointPresets[9] = {
    {"TL", "Pin to the parent's top-left", {0, 0}, {0, 0}, {0, 0}},
    {"T", "Pin to the top edge, centred", {0.5f, 0}, {0.5f, 0}, {0.5f, 0}},
    {"TR", "Pin to the top-right", {1, 0}, {1, 0}, {1, 0}},
    {"L", "Pin to the left edge, centred", {0, 0.5f}, {0, 0.5f}, {0, 0.5f}},
    {"C", "Centre on the parent", {0.5f, 0.5f}, {0.5f, 0.5f}, {0.5f, 0.5f}},
    {"R", "Pin to the right edge, centred", {1, 0.5f}, {1, 0.5f}, {1, 0.5f}},
    {"BL", "Pin to the bottom-left", {0, 1}, {0, 1}, {0, 1}},
    {"B", "Pin to the bottom edge, centred", {0.5f, 1}, {0.5f, 1}, {0.5f, 1}},
    {"BR", "Pin to the bottom-right", {1, 1}, {1, 1}, {1, 1}},
};
constexpr AnchorPreset kSpreadPresets[3] = {
    {"Stretch H", "Follow the parent's WIDTH; keep this height", {0, 0.5f}, {1, 0.5f},
     {0.5f, 0.5f}},
    {"Stretch V", "Follow the parent's HEIGHT; keep this width", {0.5f, 0}, {0.5f, 1},
     {0.5f, 0.5f}},
    {"Fill", "Follow the parent on BOTH axes", {0, 0}, {1, 1}, {0.5f, 0.5f}},
};

// Plain-English name for an anchor pair - the whole point of the widget is that
// the current anchoring is legible at a glance rather than four raw floats.
std::string AnchorLabel(glm::vec2 mn, glm::vec2 mx) {
    const auto eq = [](f32 a, f32 b) { return std::fabs(a - b) < 0.001f; };
    const bool spreadX = !eq(mn.x, mx.x), spreadY = !eq(mn.y, mx.y);
    const auto side = [&](f32 v, const char* lo, const char* mid, const char* hi) {
        if (eq(v, 0.0f)) return lo;
        if (eq(v, 1.0f)) return hi;
        if (eq(v, 0.5f)) return mid;
        return "custom";
    };
    if (spreadX && spreadY) {
        if (eq(mn.x, 0.0f) && eq(mn.y, 0.0f) && eq(mx.x, 1.0f) && eq(mx.y, 1.0f))
            return "Stretch both axes (fills the parent)";
        return "Stretch both axes (partial)";
    }
    if (spreadX) return std::string("Stretch horizontally, ") + side(mn.y, "top", "middle", "bottom");
    if (spreadY) return std::string("Stretch vertically, ") + side(mn.x, "left", "centre", "right");
    const char* v = side(mn.y, "Top", "Middle", "Bottom");
    const char* h = side(mn.x, "left", "centre", "right");
    return std::string(v) + "-" + h;
}

// The item for an entity in a layout list. Linear because a document is tens of
// elements, not thousands, and the list is in draw order (which the topmost-wins
// hit test depends on) so it cannot be an index map.
const ui::LayoutItem* FindItem(const std::vector<ui::LayoutItem>& layout, entt::entity e) {
    if (e == entt::null) return nullptr;
    for (const ui::LayoutItem& it : layout)
        if (it.entity == e) return &it;
    return nullptr;
}

// WHAT MAY BE DRAGGED, and why not. Refusing with a visible reason is the whole
// design here: the alternative is writing offset/size that the next layout
// silently overwrites, which reads to the author as a broken tool.
struct Editability {
    bool move = false;
    bool resize = false;
    bool anchors = false;
    const char* why = nullptr;     // shown on the selection badge when something is off
    const char* hint = nullptr;    // what to do instead
    entt::entity group = entt::null; // the UILayoutGroup to jump to, if any
    bool transformed = false;      // rotation/scale != identity: rect != on-screen quad
};

Editability Editable(const Scene& scene, entt::entity e) {
    Editability ed;
    const entt::registry& reg = scene.Registry();
    if (e == entt::null || !reg.valid(e)) return ed;
    const UIElement* el = reg.try_get<UIElement>(e);
    if (!el) return ed;

    // Fit-to-Parent: layout returns the parent rect outright and
    // SolveElementFromRect early-returns, so there is nothing to drag at all.
    if (el->fullscreen) {
        ed.why = "Fit to Parent: its rect IS the parent's";
        ed.hint = "Uncheck \"fullscreen\" in the Inspector to place it yourself.";
        return ed;
    }
    // A rotated / scaled element's LayoutItem rect is the PRE-transform rect; the
    // on-screen quad is that rect transformed about the pivot at emission time. So
    // the cursor and the rect disagree, and dragging would feel broken. The outline
    // is drawn as the real transformed quad instead, and handles are withheld.
    if (el->rotation != 0.0f || el->scale.x != 1.0f || el->scale.y != 1.0f) {
        ed.transformed = true;
        ed.why = "rotated / scaled: the drawn quad is not its layout rect";
        ed.hint = "Edit offset/size in the Inspector, or reset rotation and scale.";
        return ed;
    }

    // A PLAYING clip writes offset / scale / rotation / colour on this very
    // element every frame (ui::UpdateAnimations), so a drag would be overwritten
    // before the author let go. The engine now skips document members while the
    // authoring view is on, but a Manual/externally-started clip can still be
    // mid-flight, and Play mode drives every clip - so say so rather than offering
    // handles that do nothing.
    if (const UIAnimator* an = reg.try_get<UIAnimator>(e); an && an->playing) {
        ed.why = "driven by the clip it is playing";
        ed.hint = "Its offset / scale / rotation are written by the animation each "
                  "frame. Stop the clip (or leave Play) to author the rect.";
        return ed;
    }

    const ui::LayoutOwnership own = ui::LayoutGroupOwnership(scene, e);
    ed.anchors = true; // always legal: it is also how a child ESCAPES a layout group
    // THE TWO OWNERSHIPS ARE INDEPENDENT AND BOTH APPLY. A fitContent group nested
    // inside a Vertical group - the ordinary settings-menu shape, a column of
    // fit-to-content rows - is positionOwned by its parent AND selfFitsContent.
    // Returning on the first of those offered eight resize handles whose write
    // LayoutUIImpl discarded on the same frame from the children, which is exactly
    // the "drag that silently half-works" this predicate exists to prevent.
    if (own.positionOwned || own.selfFitsContent) {
        ed.group = own.group;
        // Vertical/Horizontal slots are built from the child's OWN natural size, so
        // a resize is honoured there (only the position keeps re-flowing). A Grid
        // slot is exactly cellSize, so it is not. A fitContent group rewrites its
        // own x1/y1 from its children, so its size is not its own either.
        ed.move = !own.positionOwned;
        ed.resize = !own.sizeOwned && !own.selfFitsContent;
        if (own.positionOwned && own.selfFitsContent) {
            ed.why = "placed by its parent's Layout Group, and fitContent sizes it "
                     "from its own children";
            ed.hint = "Neither edge is authorable here. Edit the parent group's "
                      "spacing / padding for the position, and this group's padding "
                      "(or turn fitContent off) for the size.";
        } else if (own.positionOwned) {
            ed.why = own.sizeOwned ? "placed AND sized by its parent's Grid Layout Group"
                                   : "placed by its parent's Layout Group";
            ed.hint = own.sizeOwned
                          ? "Edit the group's cellSize / spacing, or give this child an "
                            "anchor spread to opt out of the flow."
                          : "Resize works; the group keeps placing it. Edit the group's "
                            "spacing / padding, or give this child an anchor spread to opt out.";
        } else {
            ed.why = "Layout Group with fitContent: its right/bottom edge is computed";
            ed.hint = "Move works. Turn off fitContent, or resize via padding, to size it.";
        }
        return ed;
    }
    ed.move = true;
    ed.resize = true;
    return ed;
}

// Rect helpers on ui::Rect (which is a plain aggregate).
ui::Rect RectFromCenter(glm::vec2 c, glm::vec2 s) {
    return {c.x - s.x * 0.5f, c.y - s.y * 0.5f, c.x + s.x * 0.5f, c.y + s.y * 0.5f};
}

// One axis of snap magnetism: the smallest correction that brings ANY of `mine`
// onto ANY of `targets`, or zero when nothing is within reach.
struct Magnet {
    f32 delta = 0.0f;
    f32 guide = 0.0f;
    bool hit = false;
};
Magnet Magnetize(const f32* mine, int mineCount, const std::vector<f32>& targets,
                 f32 reach) {
    Magnet best;
    f32 bestAbs = reach;
    for (int i = 0; i < mineCount; ++i) {
        for (const f32 t : targets) {
            const f32 d = t - mine[i];
            if (std::fabs(d) < bestAbs) {
                bestAbs = std::fabs(d);
                best.delta = d;
                best.guide = t;
                best.hit = true;
            }
        }
    }
    return best;
}

} // namespace

// ============================================================================
// THE RECIPE CATALOG - the single source of truth for what can be authored.
//
// Enumerated from the ACTUAL document component set, not from memory:
// UIElement's ten Type values, plus the four other `.hbui` components that have
// a place on a 2D layout surface (UICanvas, UIPanel, UILayoutGroup,
// UICanvasGroup). Two deliberate absences:
//   * WorldText - NOT a document key (UIDocument.h decision 4). It is 3D signage
//     placed by a Transform in the level; putting it here would author it into
//     the wrong file.
//   * UIAnimator - a `.hbuianim` clip binding with no rect of its own, so it has
//     nothing to place. It stays on the Inspector's Add Component menu.
// "Progress Wheel" is the radial variant of ProgressBar rather than a type, and
// the three Layout Groups differ only by Kind, which is why this is a list of
// RECIPES: label -> components + the defaults that make the thing visible.
// ============================================================================
const Editor::UICreateDesc* Editor::UICreateCatalog(int& count) {
    static const UICreateDesc kCatalog[] = {
        {UICreate::Panel, "Panel", "UI Panel",
         "A coloured (optionally textured, optionally 9-sliced) rect. The usual "
         "background / grouping box.", false},
        {UICreate::Label, "Label", "UI Label", "Text only.", false},
        {UICreate::Button, "Button", "UI Button",
         "Interactive rect + caption. Its `action` string is what the game flow, "
         "the settings bindings and the schematics all read.", false},
        {UICreate::Image, "Image", "UI Image", "A textured rect; `color` tints it.",
         false},
        {UICreate::ProgressBar, "Progress Bar", "UI Progress Bar",
         "Background + fill; `fill` is 0..1.", false},
        {UICreate::ProgressWheel, "Progress Wheel", "UI Progress Wheel",
         "The same thing radial (`radial` = true) - a loading spinner or a cooldown.",
         false},
        {UICreate::Slider, "Slider", "UI Slider",
         "Draggable track + handle; `value` is 0..1.", false},
        {UICreate::Toggle, "Toggle", "UI Toggle", "On / off (`toggled`).", false},
        {UICreate::Selector, "Selector", "UI Selector",
         "One of N `options` (High / Med / Low).", false},
        {UICreate::ScrollView, "Scroll View", "UI Scroll View",
         "Clips and scrolls its children (wheel or autoScroll). Children are "
         "clipped to it, so drag them in from the tree.", false},
        {UICreate::TextInput, "Text Input", "UI Text Input",
         "Editable text box; `text` is the buffer.", false},
        {UICreate::Screen, "Screen", "Screen",
         "A NAMED SCREEN: a full-bleed Panel carrying UIPanel, which is what the "
         "UIManager pushes and pops (MainMenu / Settings / HUD / Pause). Name it in "
         "the Inspector - that name is the id the game flow uses.", true},
        {UICreate::VerticalGroup, "Vertical Group", "UI Vertical Group",
         "A Panel that lays its children out in a COLUMN (UILayoutGroup). Children "
         "with an anchor spread opt out and keep their own rect.", true},
        {UICreate::HorizontalGroup, "Horizontal Group", "UI Horizontal Group",
         "The same in a ROW.", true},
        {UICreate::GridGroup, "Grid Group", "UI Grid Group",
         "The same in a GRID of cellSize cells - the children's own sizes are "
         "ignored there.", true},
        {UICreate::FadeGroup, "Fade Group", "UI Fade Group",
         "A Panel carrying UICanvasGroup: one `opacity` fades the whole subtree, and "
         "`interactable` can make it inert without hiding it.", true},
        {UICreate::Canvas, "Canvas", "Canvas",
         "A UICanvas root: its OWN scale mode and reference size, its own sortOrder, "
         "and the only way to make a WORLD-SPACE page. A document does not need one "
         "- canvas-less roots lay out against the document's header canvas.", true},
    };
    static_assert(sizeof(kCatalog) / sizeof(kCatalog[0]) ==
                      static_cast<usize>(UICreate::Count),
                  "UICreateCatalog must describe every UICreate value");
    count = static_cast<int>(sizeof(kCatalog) / sizeof(kCatalog[0]));
    return kCatalog;
}

entt::entity Editor::UIParentForNew(Scene& scene, ui::DocHandle doc,
                                    entt::entity hint) const {
    const entt::registry& reg = scene.Registry();
    // The nearest UI entity IN THIS DOCUMENT at or above `from`. Bounded walk:
    // a corrupt Parent cycle must not hang the editor.
    const auto climb = [&](entt::entity from) -> entt::entity {
        int depth = 0;
        for (entt::entity cur = from; cur != entt::null && depth < 64; ++depth) {
            if (!reg.valid(cur)) return entt::null;
            const UIDocMember* m = reg.try_get<UIDocMember>(cur);
            if (m && m->doc == doc &&
                (reg.all_of<UICanvas>(cur) || reg.all_of<UIElement>(cur)))
                return cur;
            const Parent* p = reg.try_get<Parent>(cur);
            cur = (p && reg.valid(p->entity)) ? p->entity : entt::null;
        }
        return entt::null;
    };
    if (hint != entt::null && reg.valid(hint))
        if (const entt::entity h = climb(hint); h != entt::null) return h;
    if (const entt::entity s = climb(selected_); s != entt::null) return s;
    // The screen the canvas is currently SHOWING is the strongest remaining
    // signal: an author looking at MainMenu means the new button to go in it.
    for (const entt::entity e : reg.view<UIPanel>()) {
        const UIDocMember* m = reg.try_get<UIDocMember>(e);
        if (m && m->doc == doc && reg.all_of<EditorUIShow>(e)) return e;
    }
    // Document-scoped: an unscoped search grabs the FIRST canvas in the whole
    // registry, which with two documents open is very likely the other one's.
    for (const entt::entity e : reg.view<UICanvas>()) {
        const UIDocMember* m = reg.try_get<UIDocMember>(e);
        if (m && m->doc == doc) return e;
    }
    return entt::null; // a DOCUMENT ROOT - see the Editor.h note
}

// The RECIPE, with no Editor and no Engine in it: the parent is already resolved
// and the undo/selection/dirty bookkeeping is the caller's. Split out so
// --test-uieditor can exercise every recipe for real (a test that re-implemented
// them would only prove the test agrees with itself).
entt::entity Editor::UIBuildRecipe(Scene& scene, ui::DocumentSet& docs, ui::DocHandle doc,
                                   UICreate what, entt::entity parent) {
    if (doc == 0 || !docs.Get(doc)) return entt::null;
    auto& reg = scene.Registry();
    int count = 0;
    const UICreateDesc* cat = UICreateCatalog(count);
    const int idx = static_cast<int>(what);
    if (idx < 0 || idx >= count) return entt::null;
    const UICreateDesc& d = cat[idx];

    if (what == UICreate::Canvas) {
        const entt::entity e = scene.CreateEntity(d.entName);
        reg.emplace<UICanvas>(e);
        // Through Track, never a bare emplace: it also appends to the instance's
        // order list, which is the order CaptureDocument writes the file in AND
        // (because InstantiateDocument replays it) the document's z-order.
        docs.Track(scene, doc, e);
        return e;
    }

    const entt::entity e = scene.CreateEntity(d.entName);
    if (parent != entt::null && reg.valid(parent)) reg.emplace<Parent>(e, Parent{parent});
    docs.Track(scene, doc, e);

    UIElement el;
    switch (what) {
        case UICreate::Label:
            el.type = UIElement::Type::Label;
            el.text = "New Label";
            break;
        case UICreate::Button:
            el.type = UIElement::Type::Button;
            el.text = "Button";
            el.size = {320.0f, 90.0f};
            el.color = {0.86f, 0.27f, 0.33f, 1.0f};
            break;
        case UICreate::Panel:
            el.type = UIElement::Type::Panel;
            el.size = {500.0f, 300.0f};
            el.color = {0.10f, 0.10f, 0.14f, 0.85f};
            break;
        case UICreate::Image:
            el.type = UIElement::Type::Image;
            el.size = {256.0f, 256.0f};
            break;
        case UICreate::ProgressBar:
            el.type = UIElement::Type::ProgressBar;
            el.size = {420.0f, 36.0f};
            el.color = {0.12f, 0.12f, 0.16f, 0.9f};
            break;
        case UICreate::ProgressWheel:
            el.type = UIElement::Type::ProgressBar;
            el.radial = true;
            el.size = {220.0f, 220.0f};
            el.color = {0.12f, 0.12f, 0.16f, 0.9f};
            break;
        case UICreate::Slider:
            el.type = UIElement::Type::Slider;
            el.size = {420.0f, 40.0f};
            el.color = {0.12f, 0.12f, 0.16f, 0.9f};
            break;
        case UICreate::Toggle:
            el.type = UIElement::Type::Toggle;
            el.size = {160.0f, 56.0f};
            el.color = {0.12f, 0.12f, 0.16f, 0.9f};
            break;
        case UICreate::Selector:
            el.type = UIElement::Type::Selector;
            el.size = {420.0f, 56.0f};
            el.color = {0.12f, 0.12f, 0.16f, 0.9f};
            el.options = {"High", "Med", "Low"};
            break;
        case UICreate::ScrollView:
            el.type = UIElement::Type::ScrollView;
            el.size = {600.0f, 400.0f};
            el.color = {0.08f, 0.08f, 0.10f, 0.85f};
            break;
        case UICreate::TextInput:
            el.type = UIElement::Type::TextInput;
            el.placeholder = "Enter text...";
            el.size = {420.0f, 56.0f};
            el.color = {0.12f, 0.12f, 0.16f, 0.9f};
            break;
        case UICreate::Screen:
            // A screen must carry a UIElement as well as UIPanel: the layout walk
            // starts at UIElement roots, and the UIPanel::active gate is evaluated
            // ON the walked entity - a UIPanel with no UIElement is never walked,
            // so its children would lay out as roots and `active` would do nothing.
            el.type = UIElement::Type::Panel;
            el.fullscreen = true; // a screen covers its canvas
            el.color = {0.0f, 0.0f, 0.0f, 0.0f}; // invisible by default: a container
            break;
        case UICreate::VerticalGroup:
        case UICreate::HorizontalGroup:
        case UICreate::GridGroup:
            el.type = UIElement::Type::Panel;
            el.size = {360.0f, 400.0f};
            el.color = {0.0f, 0.0f, 0.0f, 0.0f}; // a container, not a visible box
            break;
        case UICreate::FadeGroup:
            el.type = UIElement::Type::Panel;
            el.size = {500.0f, 300.0f};
            el.color = {0.0f, 0.0f, 0.0f, 0.0f};
            break;
        default:
            el.type = UIElement::Type::Label;
            break;
    }
    reg.emplace<UIElement>(e, el);

    if (what == UICreate::Screen) {
        UIPanel p;
        p.name = "Screen"; // the UIManager id; renamed in the Inspector
        reg.emplace<UIPanel>(e, p);
        // Force it visible on the authoring canvas straight away (session-only
        // tag): a fresh screen is inactive, so without this the author would
        // create one and watch nothing appear.
        for (const entt::entity other : reg.view<UIPanel>()) {
            const UIDocMember* m = reg.try_get<UIDocMember>(other);
            if (m && m->doc == doc) reg.remove<EditorUIShow>(other);
        }
        reg.emplace_or_replace<EditorUIShow>(e);
    }
    if (what == UICreate::VerticalGroup || what == UICreate::HorizontalGroup ||
        what == UICreate::GridGroup) {
        UILayoutGroup lg;
        lg.kind = what == UICreate::VerticalGroup   ? UILayoutGroup::Kind::Vertical
                  : what == UICreate::HorizontalGroup ? UILayoutGroup::Kind::Horizontal
                                                      : UILayoutGroup::Kind::Grid;
        if (lg.kind == UILayoutGroup::Kind::Grid) lg.columns = 2;
        reg.emplace<UILayoutGroup>(e, lg);
    }
    if (what == UICreate::FadeGroup) reg.emplace<UICanvasGroup>(e);
    return e;
}

std::string Editor::UIEdDocKey(const ui::DocumentSet& docs) const {
    if (activeDoc_ == 0) return {};
    const ui::DocumentInstance* inst = docs.Get(activeDoc_);
    if (!inst) return {};
    // `rel` is stable across the close-and-reopen that RestoreSnapshot performs;
    // the DocHandle is not (OpenFromData mints a fresh one). A never-saved "New"
    // has no rel, and also cannot survive a reopen, so its handle is key enough.
    return inst->rel.empty() ? ("#" + std::to_string(activeDoc_)) : inst->rel;
}

entt::entity Editor::CreateUIElementInDocument(Engine& engine, UICreate what,
                                              entt::entity parentHint) {
    // THE CREATION-TIME SEPARATION GUARD, and the reason this function exists at
    // all: every creation path funnels through it, so there is exactly one place
    // that can let UI into a scene - and it cannot.
    if (activeDoc_ == 0) return entt::null;
    Scene& scene = engine.GetScene();
    const ui::DocHandle doc = activeDoc_;
    if (!engine.Documents().Get(doc)) return entt::null; // stale handle
    int catCount = 0;
    UICreateCatalog(catCount);
    if (static_cast<int>(what) < 0 || static_cast<int>(what) >= catCount)
        return entt::null; // validated BEFORE the undo, so a bad call costs no slot
    PushUndo(engine); // one entry per create (the Engine overload: documents too)
    // A SCREEN and a CANVAS are always document ROOTS. For a Canvas that is what the
    // component means. For a Screen it is a correctness rule, and it is not
    // theoretical: UIParentForNew's strongest signal is "the screen currently shown",
    // so a new Screen landed INSIDE that one and then took the EditorUIShow tag off
    // its own parent - whose subtree the layout walk promptly skipped, taking the new
    // screen with it and blanking the canvas. A UIPanel nested under another panel is
    // meaningless anyway: the UIManager pushes and pops TOP-LEVEL screens by name.
    const entt::entity parent =
        (what == UICreate::Canvas || what == UICreate::Screen)
            ? entt::null
            : UIParentForNew(scene, doc, parentHint);
    const entt::entity e =
        UIBuildRecipe(scene, engine.Documents(), doc, what, parent);
    if (e == entt::null) return entt::null;
    // UIBuildRecipe force-shows a new Screen (otherwise the author creates one and
    // watches nothing appear, since a fresh UIPanel is inactive). Remember it by
    // name so the choice survives the next undo.
    if (what == UICreate::Screen)
        if (const UIPanel* p = scene.Registry().try_get<UIPanel>(e))
            uiEdShownScreen_[UIEdDocKey(engine.Documents())] = p->name;
    selected_ = e;
    MarkDocumentDirty(engine, e);
    return e;
}

// --- Z-order ----------------------------------------------------------------

void Editor::UISwapOrder(Scene& scene, ui::DocumentSet& docs, entt::entity a,
                         entt::entity b) {
    auto& reg = scene.Registry();
    if (a == b || !reg.valid(a) || !reg.valid(b)) return;
    const UIDocMember* ma = reg.try_get<UIDocMember>(a);
    const UIDocMember* mb = reg.try_get<UIDocMember>(b);
    if (!ma || !mb || ma->doc == 0 || ma->doc != mb->doc) return;

    // The pools that decide draw order. Swapping POSITIONS of two entities inside
    // a pool permutes only those two slots, so every OTHER entity - world content
    // included - keeps its position and its relative order. That is why this is a
    // pair of swap_elements rather than a registry-wide sort.
    //
    // Both must be in the pool being swapped. If NEITHER pool governs both, the
    // pair's order is not pool-decided at all (two UICanvas roots sort by
    // sortOrder) and the whole operation is abandoned - swapping the SAVED order
    // alone would silently desynchronise it from the live one, which is precisely
    // the divergence that makes a z-order edit not survive a save.
    bool swapped = false;
    if (reg.all_of<UIElement>(a) && reg.all_of<UIElement>(b)) {
        reg.storage<UIElement>().swap_elements(a, b); // canvas-less root order
        swapped = true;
    }
    if (reg.all_of<Parent>(a) && reg.all_of<Parent>(b)) {
        reg.storage<Parent>().swap_elements(a, b); // sibling order under a parent
        swapped = true;
    }
    if (!swapped) return;

    // ...and the SAVED order, so the result survives a save + reopen. Both swaps
    // are the same permutation, which is what keeps "document order reproduces
    // pool order" true - the property InstantiateDocument relies on, and what
    // --test-uieditor round-trips.
    if (ui::DocumentInstance* inst = docs.Get(ma->doc)) {
        auto& list = inst->entities;
        const auto ia = std::find(list.begin(), list.end(), a);
        const auto ib = std::find(list.begin(), list.end(), b);
        if (ia != list.end() && ib != list.end()) std::iter_swap(ia, ib);
        inst->dirty = true;
    }
    // No EnTT construct/destroy signal fires for a pool permutation, so the
    // RUNTIME's cached parent->children map would keep the old order (the editor's
    // own pass rebuilds it every frame and would look right while the Game view
    // did not).
    scene.BumpUIVersion();
}

void Editor::UIReorder(Engine& engine, entt::entity e, int dir, bool run) {
    Scene& scene = engine.GetScene();
    auto& reg = scene.Registry();
    if (e == entt::null || !reg.valid(e) || dir == 0) return;

    // A CANVAS ROOT is the one thing whose order is authored rather than
    // positional: LayoutUIImpl sorts UICanvas trees by `sortOrder` (then entity),
    // so a pool swap would do nothing to it. Move the authored field instead.
    if (reg.all_of<UICanvas>(e) && !reg.all_of<UIElement>(e)) {
        const UIDocMember* m = reg.try_get<UIDocMember>(e);
        if (!m) return;
        UICanvas& c = reg.get<UICanvas>(e);
        int lo = c.sortOrder, hi = c.sortOrder;
        for (const entt::entity o : reg.view<UICanvas>()) {
            const UIDocMember* om = reg.try_get<UIDocMember>(o);
            if (!om || om->doc != m->doc || o == e) continue;
            lo = glm::min(lo, reg.get<UICanvas>(o).sortOrder);
            hi = glm::max(hi, reg.get<UICanvas>(o).sortOrder);
        }
        const int next = run ? (dir > 0 ? hi + 1 : lo - 1) : c.sortOrder + dir;
        if (next == c.sortOrder) return;
        PushUndo(engine);
        c.sortOrder = next;
        scene.BumpUIVersion();
        MarkDocumentDirty(engine, e);
        return;
    }

    // Siblings IN DRAW ORDER, straight out of the layout the canvas is showing.
    // Reading the order rather than deriving it from pool indices is what makes
    // this independent of which direction EnTT happens to iterate a storage.
    // Restricted to siblings the SAME pool governs (see UISwapOrder), so a canvas
    // root sitting next to an element root in the list cannot be swapped with it.
    const Parent* pp = reg.try_get<Parent>(e);
    const entt::entity parent = pp && reg.valid(pp->entity) ? pp->entity : entt::null;
    const bool wantElement = reg.all_of<UIElement>(e);
    std::vector<entt::entity> sib;
    int at = -1;
    for (const ui::LayoutItem& it : uiEdLayout_) {
        if (!reg.valid(it.entity)) continue;
        if (it.parentEntity != parent) continue;
        if (reg.all_of<UIElement>(it.entity) != wantElement) continue;
        if (it.entity == e) at = static_cast<int>(sib.size());
        sib.push_back(it.entity);
    }
    if (at < 0) return;
    const int n = static_cast<int>(sib.size());
    int steps = 0;
    for (int i = at; dir > 0 ? i < n - 1 : i > 0; i += dir) {
        if (steps == 0) PushUndo(engine); // one undo entry for the whole run
        UISwapOrder(scene, engine.Documents(), sib[static_cast<usize>(i)],
                    sib[static_cast<usize>(i + dir)]);
        std::swap(sib[static_cast<usize>(i)], sib[static_cast<usize>(i + dir)]);
        ++steps;
        if (!run) break;
    }
}

// --- Interact preview -------------------------------------------------------

void Editor::UIEditorBeginInteract(Engine& engine) {
    // Snapshot the four fields interaction WRITES IN PLACE. They are authored
    // initial state (a slider's starting value, which selector cell is chosen),
    // and the runtime overwrites them - so a preview that left them changed would
    // silently re-author the document just because somebody tested a click.
    uiEdPreview_.clear();
    if (activeDoc_ == 0) return;
    const auto& reg = engine.GetScene().Registry();
    for (const entt::entity e : reg.view<UIElement>()) {
        const UIDocMember* m = reg.try_get<UIDocMember>(e);
        if (!m || m->doc != activeDoc_) continue;
        const UIElement& el = reg.get<UIElement>(e);
        uiEdPreview_.push_back({e, el.value, el.toggled, el.selected, el.scrollPos});
    }
    uiEdInteractCtx_.layout.clear();
    uiEdInteractCtx_.interactives.clear();
}

void Editor::UIEditorEndInteract(Engine& engine) {
    auto& reg = engine.GetScene().Registry();
    for (const UIPreviewState& s : uiEdPreview_) {
        if (!reg.valid(s.e)) continue;
        UIElement* el = reg.try_get<UIElement>(s.e);
        if (!el) continue;
        el->value = s.value;
        el->toggled = s.toggled;
        el->selected = s.selected;
        el->scrollPos = s.scrollPos;
        el->hovered = false;
        el->clicked = false;
        el->changed = false;
        el->dragging = false;
    }
    uiEdPreview_.clear();
    uiEdInteractCtx_.layout.clear();
    uiEdInteractCtx_.interactives.clear();
}

void Editor::UIEditorInvalidate() {
    // Abandon the gesture AND drop the cached layout. After a snapshot restore every
    // entity id in it is dangling, and an in-flight drag would otherwise keep
    // writing to whatever id got recycled. Handles are not revalidated on purpose:
    // Editor.h's Snapshot comment is explicit that they cannot be.
    uiEdDragMode_ = kModeNone;
    uiEdDragLive_ = false;
    uiEdPanning_ = false;
    uiEdDragEntity_ = entt::null;
    uiEdLayout_.clear();
    uiEdLayoutCanvas_ = glm::vec2(0.0f);
    uiEdGuideX_.clear();
    uiEdGuideY_.clear();
    // Deferred structural edits name entities that may no longer exist.
    uiEdPendingDelete_ = entt::null;
    uiEdPendingReparentChild_ = entt::null;
    uiEdPendingReparentTo_ = entt::null;
    uiEdPendingUnparent_ = false;
    uiEdPendingCreate_ = -1;
    uiEdPendingCreateParent_ = entt::null;
    uiEdPendingCreatePlace_ = false;
    // The Interact snapshot is DROPPED, not replayed: after an undo/redo every
    // entity in it is dangling, and the restore has already put the document back
    // to a recorded state. Leaving Interact on with no snapshot would let the next
    // toggle-off write stale values, so the mode goes off with it.
    uiEdInteract_ = false;
    uiEdPreview_.clear();
    uiEdInteractCtx_.layout.clear();
    uiEdInteractCtx_.interactives.clear();
}

void Editor::DrawUIEditorPanel(Engine& engine) {
    // LEAVING THE PREVIEW IS PART OF CLOSING. Interact writes the four AUTHORED
    // widget fields in place (value / toggled / selected / scrollPos) and only the
    // matching restore puts them back; Editor.h's contract already says "or the
    // panel closes". Without this, clicking the tab's X with Interact on stranded
    // the snapshot forever and the test-clicked values reached disk on the next
    // Ctrl+Shift+S with no `*` and no prompt.
    if (!panelOpen_[Panel_UIEditor]) {
        if (uiEdInteract_) {
            UIEditorEndInteract(engine);
            uiEdInteract_ = false;
        }
        return;
    }

    ui::DocumentSet& docs = engine.Documents();

    // CLAIM THE IN-GAME UI POINTER (constraint 2 above) before any early return:
    // the hazard exists whenever this panel is open, not only when it draws. Play
    // mode is excluded - there the runtime owns interaction and this panel is a
    // live view.
    if (!playMode_ && !gameViewPointerFed_) engine.SetUIPointer(-1.0f, -1.0f);

    const bool uiEdVisible = ImGui::Begin("UI Editor", &panelOpen_[Panel_UIEditor]);
    // CLAIM Ctrl+S. THE USER'S REPORTED BUG: this panel had no keyboard handling at
    // all except the arrow-nudge, so Ctrl+S over an open `.hbui` fell through to the
    // unguarded global handler and saved the SCENE - the one file the document's
    // entities are deliberately absent from. The document went unwritten and the
    // author had no way to tell.
    //
    // `activeDoc_` is already the single edit target, driven by the tab bar below and
    // read by the create guard, the Inspector and the hierarchy grouping alike, so
    // "which document" was never in question. Claimed above the project check so an
    // empty editor reports itself rather than silently writing the level.
    //
    // CLAIMED ABOVE THE `Begin() == false` EARLY RETURN, not below it. Clicking a
    // floating window's collapse arrow FOCUSES it and then collapses it, so the
    // window is still g.NavWindow while Begin returns false - a claim placed after
    // the early return would be skipped, the global route (score 1) would win, and
    // Ctrl+S would write the SCENE while the focused, titled window says "UI Editor".
    // Begin() pushes the focus scope unconditionally (imgui.cpp, "Add to focus scope
    // stack") and End() pops it, so claiming here is legal for a skipped window; a
    // docked-but-inactive tab is absent from NavFocusRoute and simply scores 0.
    ClaimSave(editor::SaveSurface::UIDocument);

    if (!uiEdVisible) {
        // Collapsed, OR the X was just clicked (Begin clears panelOpen_ and returns
        // false on that very frame, so the guard above has not seen it yet). Same
        // restore, same reason.
        if (!panelOpen_[Panel_UIEditor] && uiEdInteract_) {
            UIEditorEndInteract(engine);
            uiEdInteract_ = false;
        }
        ImGui::End();
        return;
    }

    if (!Project::HasActive()) {
        ImGui::TextDisabled("No project open.");
        ImGui::End();
        return;
    }

    Scene& scene = engine.GetScene();
    Renderer& renderer = engine.GetRenderer();
    Project& project = Project::Active();

    // -- Document TABS ---------------------------------------------------------
    // ui::DocumentSet already supports any number of simultaneously open
    // documents (and two instances of one file, deliberately - a document is a
    // template). One tab per open instance, the selected tab IS activeDoc_, so
    // switching the edit target is one click and the whole editor follows (the
    // create guard, the Inspector, the hierarchy grouping all read activeDoc_).
    //
    // New / Save / Save As / the canvas + menu-look block stay in the UI Document
    // panel, which owns that flow and its refusal paths. What lives here is the
    // part an author does constantly: pick the target, open another file, close
    // one.
    bool wantOpenPopup = false;
    {
        ui::DocHandle closeReq = 0;
        if (ImGui::BeginTabBar("##uiedtabs", ImGuiTabBarFlags_AutoSelectNewTabs |
                                                 ImGuiTabBarFlags_FittingPolicyScroll)) {
            for (const ui::DocumentInstance& inst : docs.All()) {
                // Engine-owned documents (the project's UI + boot slots) may be
                // EDITED but not closed from here: the Engine holds their handles
                // and the runtime flow expects them open. Same rule the UI Document
                // panel applies to its Close button.
                // ALL of them: the runtime holds one handle per SCREEN document.
                const bool engineOwned = engine.IsEngineDocument(inst.handle);
                std::string label =
                    inst.rel.empty()
                        ? std::string("(unsaved)")
                        : std::filesystem::path(inst.rel).filename().generic_string();
                if (inst.dirty) label += " *";
                label += "###uidoc" + std::to_string(inst.handle);
                bool open = true;
                const bool sel = ImGui::BeginTabItem(
                    label.c_str(), engineOwned ? nullptr : &open,
                    inst.handle == activeDoc_ && activeDoc_ != uiEdLastDoc_
                        ? ImGuiTabItemFlags_SetSelected
                        : ImGuiTabItemFlags_None);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s%s%s", inst.rel.empty() ? "(never saved)"
                                                                 : inst.rel.c_str(),
                                      inst.dirty ? "\nEdited - Ctrl+Shift+S saves every "
                                                   "open document."
                                                 : "",
                                      engineOwned ? "\nThis is one of the project's UI "
                                                    "slots, so it stays open."
                                                  : "");
                }
                if (sel) {
                    activeDoc_ = inst.handle;
                    ImGui::EndTabItem();
                }
                if (!open) closeReq = inst.handle;
            }
            if (docs.Empty()) {
                ImGui::TabItemButton("(nothing open)", ImGuiTabItemFlags_NoTooltip |
                                                           ImGuiTabItemFlags_Leading);
            }
            if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing |
                                              ImGuiTabItemFlags_NoTooltip))
                wantOpenPopup = true;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open another .hbui");
            ImGui::EndTabBar();
        }
        // Applied after the tab bar: Close destroys entities, and doing that inside
        // the bar would pull the rug from under the widget being drawn.
        //
        // DocumentSet::Close destroys every member entity and pushes NO undo entry,
        // so an unsaved document is simply gone. The tooltip above already tells the
        // author the file is edited; the X next to it must not then throw it away
        // without asking. A never-saved document counts as unsaved whatever its
        // dirty flag says - SaveAll skips path-less documents by design.
        if (closeReq != 0) {
            const ui::DocumentInstance* ci = docs.Get(closeReq);
            const bool unsaved = ci && (ci->dirty || ci->path.empty());
            if (unsaved) {
                uiEdCloseConfirm_ = closeReq;
            } else {
                if (uiEdInteract_) UIEditorEndInteract(engine); // restore before the entities go
                docs.Close(scene, closeReq);
                if (activeDoc_ == closeReq) {
                    activeDoc_ = docs.Empty() ? 0 : docs.All().front().handle;
                    selected_ = entt::null; // the selection may have been inside it
                }
                UIEditorInvalidate();
            }
        }
    }
    // -- "really close it?" ----------------------------------------------------
    if (uiEdCloseConfirm_ != 0) {
        const ui::DocumentInstance* ci = docs.Get(uiEdCloseConfirm_);
        if (!ci) {
            uiEdCloseConfirm_ = 0; // it went away by some other route
        } else {
            if (!ImGui::IsPopupOpen("Close UI document?"))
                ImGui::OpenPopup("Close UI document?");
            if (ImGui::BeginPopupModal("Close UI document?", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                const bool named = !ci->path.empty();
                ImGui::TextWrapped(
                    "'%s' has unsaved changes.%s",
                    named ? ci->rel.c_str() : "(unsaved)",
                    named ? "" : "\n\nIt has never been saved, so there is no file to go "
                                 "back to - closing discards it entirely.");
                ImGui::Spacing();
                const ui::DocHandle h = uiEdCloseConfirm_;
                const auto shut = [&]() {
                    if (uiEdInteract_) UIEditorEndInteract(engine);
                    docs.Close(scene, h);
                    if (activeDoc_ == h) {
                        activeDoc_ = docs.Empty() ? 0 : docs.All().front().handle;
                        selected_ = entt::null;
                    }
                    UIEditorInvalidate();
                    uiEdCloseConfirm_ = 0;
                    ImGui::CloseCurrentPopup();
                };
                ImGui::BeginDisabled(!named);
                if (ImGui::Button("Save and close", ImVec2(140.0f, 0.0f))) {
                    if (SaveUIDocument(engine, h)) shut();
                    else uiEdCloseConfirm_ = 0; // the refusal message says why
                }
                ImGui::EndDisabled();
                if (!named && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Never saved - use Save As in the UI Document panel "
                                      "first.");
                ImGui::SameLine();
                if (ImGui::Button("Discard", ImVec2(100.0f, 0.0f))) shut();
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f))) {
                    uiEdCloseConfirm_ = 0;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }
    }
    if (wantOpenPopup) ImGui::OpenPopup("##uiedopen");
    if (ImGui::BeginPopup("##uiedopen")) {
        if (!scenesScanned_) RefreshScenes();
        if (uiDocList_.empty()) ImGui::TextDisabled("No .hbui under Assets/.");
        for (const std::filesystem::path& p : uiDocList_) {
            const std::string rel = project.RelativeAssetPath(p);
            if (ImGui::Selectable(rel.c_str())) {
                const ui::DocHandle h = docs.Open(scene, &renderer, p, /*screenOwned*/ true);
                if (h != 0) activeDoc_ = h;
                else uiDocError_ = "Failed to open '" + rel + "'.";
            }
        }
        ImGui::Separator();
        if (ImGui::Selectable("Refresh")) RefreshScenes();
        if (ImGui::Selectable("New / Save / Save As...  (UI Document panel)")) {
            panelOpen_[Panel_UIDocument] = true;
            ImGui::SetWindowFocus("UI Document");
        }
        ImGui::EndPopup();
    }

    const ui::DocumentInstance* act = docs.Get(activeDoc_);
    if (!act) {
        // Leave the Interact preview cleanly even when there is nothing to draw:
        // its snapshot names entities by handle, and holding it while the edit
        // target goes away risks writing those values into recycled ids later.
        if (uiEdInteract_) {
            UIEditorEndInteract(engine);
            uiEdInteract_ = false;
        }
        ImGui::Separator();
        ImGui::TextWrapped(
            "Pick a .hbui above to author it here. This canvas shows the document "
            "exactly as the game renders it - it is the shipped UI pass drawing into "
            "an offscreen target, not a preview drawn a second way.");
        if (!uiDocError_.empty()) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", uiDocError_.c_str());
            ImGui::SameLine();
            if (ImGui::Button("Dismiss")) uiDocError_.clear();
        }
        ImGui::End();
        return;
    }

    // A change of edit target refits, so switching documents never lands you
    // zoomed into empty space - and invalidates the gesture + cached layout,
    // whose entities and frozen parent rect belong to the document it started in.
    // Consumed unconditionally: a restore that happened while this panel was not
    // even drawing must not leave the flag armed for the author's NEXT real
    // document switch.
    const bool preserveView = uiEdPreserveView_;
    uiEdPreserveView_ = false;
    if (activeDoc_ != uiEdLastDoc_) {
        // Put the PREVIOUS document's widget state back first - its entities are
        // still alive here, and UIEditorInvalidate deliberately drops the snapshot
        // rather than replaying it (after an undo it cannot be replayed at all).
        if (uiEdInteract_) UIEditorEndInteract(engine);
        uiEdLastDoc_ = activeDoc_;
        // ...unless the handle only changed because a snapshot restore reopened the
        // same files. The gesture and the cached layout still have to go (their
        // entity ids are dangling), which UIEditorInvalidate does - but the view is
        // the author's, not the document's.
        if (!preserveView) {
            uiEdFitPending_ = true;
            uiEdPan_ = glm::vec2(0.0f);
        }
        UIEditorInvalidate(); // also switches Interact off
    }

    // -- The canvas basis ------------------------------------------------------
    // The document's OWN header canvas: that is what makes a `.hbui`
    // self-describing, and it is the basis its canvas-less roots lay out against
    // (the reference project's menu has zero UICanvas entities). Deliberately not
    // the project config - that fallback exists for runtime-spawned parentless UI,
    // which is not what this panel edits.
    const ui::CanvasConfig cfg = act->header.canvas;
    const f32 refW = glm::max(cfg.refWidth, 64.0f);
    const f32 refH = glm::max(cfg.refHeight, 64.0f);
    uiEdAspect_ = glm::clamp(uiEdAspect_, 0, kAspectCount - 1);
    const f32 previewAspect =
        kAspects[uiEdAspect_].aspect > 0.0f ? kAspects[uiEdAspect_].aspect : refW / refH;
    // The output the game would be rendering at, in pixels, at the document's own
    // reference height. This is what EffectiveCanvas resolves the canvas against.
    const glm::vec2 target(refH * previewAspect, refH);
    const glm::vec2 canvas = glm::max(ui::EffectiveCanvas(cfg, target), glm::vec2(1.0f));

    // -- Screen (UIPanel) selector ---------------------------------------------
    // A `.hbui` holds several named SCREENS, and UIPanel::active is runtime state
    // that only UIManager ever sets - so a document opened in the editor lays out
    // NOTHING until one is forced visible. This picks which screen the canvas
    // shows, via the session-only EditorUIShow tag: it writes no authored field, so
    // it cannot be saved into the document and it cannot reach a shipped build.
    bool docHasScreens = false; // does the "Screen" combo below actually exist?
    {
        auto& reg = scene.Registry();
        std::vector<entt::entity> panels;
        std::vector<std::string> names;
        for (const entt::entity e : reg.view<UIPanel>()) {
            const UIDocMember* m = reg.try_get<UIDocMember>(e);
            if (!m || m->doc != activeDoc_) continue;
            const UIPanel& p = reg.get<UIPanel>(e);
            panels.push_back(e);
            names.push_back(p.name.empty() ? std::string("(unnamed panel)") : p.name);
        }
        docHasScreens = !panels.empty();
        if (!panels.empty()) {
            int shown = -1;
            for (usize i = 0; i < panels.size(); ++i)
                if (reg.all_of<EditorUIShow>(panels[i])) shown = static_cast<int>(i);
            // Re-apply the remembered screen when the tag is gone but the choice
            // stands: an undo, a document reopen or a play-snapshot restore recreates
            // these entities, and EditorUIShow is session-only by design (it must
            // never reach a file). Keying off the panel NAME is what makes the choice
            // outlive the entity.
            // PER DOCUMENT: with one screen per document, a single remembered name
            // could only ever re-apply to one open tab, so every other tab came
            // back blank after an undo/reopen/play-restore.
            const std::string docKey = UIEdDocKey(engine.Documents());
            const auto remembered = uiEdShownScreen_.find(docKey);
            if (shown < 0 && remembered != uiEdShownScreen_.end() &&
                !remembered->second.empty()) {
                for (usize i = 0; i < panels.size(); ++i) {
                    if (names[i] != remembered->second) continue;
                    reg.emplace_or_replace<EditorUIShow>(panels[i]);
                    shown = static_cast<int>(i);
                    break;
                }
            }
            std::string label = "(as the game left it)";
            if (shown >= 0) label = names[static_cast<usize>(shown)];
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::BeginCombo("Screen", label.c_str())) {
                if (ImGui::Selectable("(as the game left it)", shown < 0)) {
                    for (const entt::entity e : panels) reg.remove<EditorUIShow>(e);
                    uiEdShownScreen_.erase(docKey); // explicit: stop re-applying HERE
                }
                for (usize i = 0; i < panels.size(); ++i) {
                    const UIPanel& p = reg.get<UIPanel>(panels[i]);
                    const std::string item =
                        names[i] + (p.startVisible ? "   (start visible)" : "") +
                        (p.active ? "   [active]" : "");
                    if (ImGui::Selectable(item.c_str(), shown == static_cast<int>(i))) {
                        for (const entt::entity e : panels) reg.remove<EditorUIShow>(e);
                        reg.emplace_or_replace<EditorUIShow>(panels[i]);
                        uiEdShownScreen_[docKey] = names[i];
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Which named screen to author. UIPanel::active is RUNTIME state (the "
                    "game flow owns it), so the editor forces the choice with a "
                    "session-only tag instead - nothing here is written to the .hbui.");
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
        }
    }

    // -- Toolbar ---------------------------------------------------------------
    // INTERACT: run the document through the REAL ui::UpdateInteraction so an
    // author can hover, click, toggle, drag a slider and scroll a view without
    // entering Play (Play replaces the world, resets story flags and hands the
    // camera over - far too heavy for "does this button light up?"). While it is
    // on, authoring gestures are suspended: the same press cannot both select and
    // click a button.
    ImGui::BeginDisabled(playMode_);
    if (ImGui::Checkbox("Interact", &uiEdInteract_)) {
        if (uiEdInteract_) UIEditorBeginInteract(engine);
        else UIEditorEndInteract(engine);
        uiEdDragMode_ = kModeNone;
        uiEdDragLive_ = false;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(
            playMode_
                ? "While playing, the runtime already owns interaction."
                : "Test the screen: hover, click, toggle, drag sliders, scroll views -\n"
                  "through ui::UpdateInteraction, the same code the game runs.\n\n"
                  "Interaction WRITES the authored initial state (value / toggled /\n"
                  "selected / scroll), so those four are snapshotted now and restored\n"
                  "when you switch Interact off. Editing gestures are suspended.\n"
                  "The wheel scrolls instead of zooming; hold Ctrl to zoom.\n\n"
                  "Mouse only: keyboard / gamepad focus navigation and TextInput typing\n"
                  "need the runtime's navigation pass, which the editor suppresses so\n"
                  "ImGui typing cannot leak into the game UI. Press Play for those.");
    ImGui::SameLine();
    ImGui::Checkbox("Palette", &uiEdShowPalette_);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The element palette and this document's tree.");
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    {
        const char* items[kAspectCount];
        for (int i = 0; i < kAspectCount; ++i) items[i] = kAspects[i].label;
        ImGui::Combo("Aspect", &uiEdAspect_, items, kAspectCount);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Preview the document at another output aspect - "
                          "%s derives the canvas from it.\n"
                          "Each distinct previewed canvas width permanently costs one "
                          "of the 8 process-wide UI render targets (the RHI cannot free "
                          "them).",
                          ScaleModeName(cfg.mode));
    ImGui::SameLine();
    if (ImGui::Button("Fit")) uiEdFitPending_ = true;
    ImGui::SameLine();
    if (ImGui::Button("100%")) {
        uiEdZoom_ = 1.0f;
        uiEdPan_ = glm::vec2(0.0f);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("One canvas unit = one screen pixel.");
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Text("Reference %.0fx%.0f (%s)", refW, refH, ScaleModeName(cfg.mode));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The document's own canvas block (UI Document panel > Canvas): "
                          "the basis THIS PANEL lays the file out against, which is what "
                          "makes a .hbui self-describing.\n\n"
                          "It is NOT what ships for canvas-less roots. The runtime hands "
                          "ui::BuildVertices the PROJECT's canvas config (Project "
                          "Settings > Build), because that same fallback also carries "
                          "runtime-spawned UI - dialogue choices, the interact prompt - "
                          "which belongs to no document. Keep the two equal or this "
                          "canvas is not WYSIWYG.");
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Text("Canvas %.0fx%.0f", canvas.x, canvas.y);
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    // Shared with the viewport overlay on purpose: an author sets a grid once and
    // both surfaces obey it.
    ImGui::Checkbox("Snap", &uiSnap_);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Grid step, plus magnetism to the PARENT's edges/centre and "
                          "to SIBLING edges/centres.\nHold Ctrl during a drag to "
                          "suspend all of it.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0f);
    ImGui::DragFloat("##uiedsnapstep", &uiSnapStep_, 0.5f, 1.0f, 200.0f, "%.0f u");
    ImGui::SameLine();
    ImGui::Checkbox("Anchors", &uiEdShowAnchors_);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show the selection's anchor region on the canvas, with four "
                          "draggable corners. Anchoring is what decides whether this "
                          "element survives a change of resolution.");
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::TextDisabled("targets %u/8", ui::UITargetCacheCount());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("UI render targets minted this run (world-space canvases share "
                          "the same budget). Neither backend can free one; restart the "
                          "editor to reclaim them.");

    if (playMode_) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.3f, 1.0f), "PLAYING - live view");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("While playing, the runtime writes the widget state fields "
                              "(toggled / value / selected / scroll) on these very "
                              "entities. Stop to author.");
    }
    uiEdNote_.clear();

    // -- WYSIWYG GUARD: header canvas vs the project's -------------------------
    // This panel lays the document out against its own header block. The RUNTIME
    // does not: Engine hands ui::BuildVertices the PROJECT's config, deliberately
    // (that same fallback carries dialogue choices and the interact prompt, which
    // belong to no document). For a canvas-less root - which is every screen of the
    // reference project's menu - the two must therefore AGREE or this canvas is
    // lying about what ships. Nothing else notices: the migrator seeds them from
    // the same place, so they only drift when someone edits one of them.
    {
        const ui::CanvasConfig proj = ProjectCanvasConfig();
        if (!SameCanvasConfig(cfg, proj)) {
            int canvasLessRoots = 0;
            auto& reg = scene.Registry();
            for (const entt::entity e : reg.view<UIElement>()) {
                const UIDocMember* m = reg.try_get<UIDocMember>(e);
                if (!m || m->doc != activeDoc_) continue;
                // A root of the layout walk: no UIElement ancestor. Those are the
                // ones the project config governs at runtime.
                const Parent* p = reg.try_get<Parent>(e);
                if (p && reg.valid(p->entity)) continue;
                if (reg.all_of<UICanvas>(e)) continue;
                ++canvasLessRoots;
            }
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.35f, 1.0f));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextWrapped(
                "This document's canvas (%.0fx%.0f %s) DIFFERS from the project's "
                "(%.0fx%.0f %s). The runtime lays canvas-less roots out against the "
                "PROJECT's - this document has %d of them - so what you see here is "
                "not what ships.",
                refW, refH, ScaleModeName(cfg.mode), proj.refWidth, proj.refHeight,
                ScaleModeName(proj.mode), canvasLessRoots);
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            if (ImGui::Button("Match the project")) {
                if (ui::DocumentInstance* mut = docs.Get(activeDoc_)) {
                    PushUndo(engine);
                    mut->header.canvas = proj;
                    mut->dirty = true;
                    UIEditorInvalidate(); // the cached layout's basis just changed
                }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Copies Project Settings > Build's UI scale mode and "
                                  "reference size into this document's header. It does "
                                  "NOT move any element: the numbers change, so the "
                                  "layout re-resolves against the shipped basis.");
            ImGui::SameLine();
            if (ImGui::Button("Project Settings...")) panelOpen_[Panel_ProjectSettings] = true;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("...or change the PROJECT to match this document, if "
                                  "this file is the one that is right.");
        }
    }

    // -- This frame's render target -------------------------------------------
    // Sized to the CANVAS, never to the panel. Keyed by size so two 1920x1080
    // documents share one slot.
    const u32 rtW = static_cast<u32>(glm::clamp(canvas.x, 64.0f, 4096.0f));
    const u32 rtH = static_cast<u32>(glm::clamp(canvas.y, 64.0f, 4096.0f));
    const std::string key =
        "uiedit@" + std::to_string(rtW) + "x" + std::to_string(rtH);
    const rhi::TextureHandle rt = ui::AcquireUITarget(renderer, key, rtW, rtH);
    u64 texId = 0;
    if (rt.IsValid()) {
        texId = renderer.TextureUIId(rt);
        if (texId == 0)
            uiEdNote_ = "This backend cannot present a render target inside a panel, so "
                        "the canvas picture is unavailable. Layout is still correct.";
    } else {
        uiEdNote_ = "UI render target unavailable: either the backend has none "
                    "(--opengl) or the 8-target process budget is spent. Layout is "
                    "still correct; only the picture is missing.";
    }
    if (!uiEdNote_.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.3f, 1.0f), "%s", uiEdNote_.c_str());
        ImGui::PopTextWrapPos();
    }

    // -- Split: palette+tree | canvas | inspector strip ------------------------
    // Each side strip is dropped entirely once the panel gets narrow rather than
    // squeezing the canvas into uselessness (the DialogueEditor rule). The canvas
    // is the deliverable; the strips are how you reach what a rect cannot express.
    constexpr f32 kInspW = 306.0f;
    constexpr f32 kPalW = 206.0f;
    const f32 availW = ImGui::GetContentRegionAvail().x;
    const bool showStrip = availW > kInspW + 340.0f;
    const bool showPal = uiEdShowPalette_ && availW > kInspW + kPalW + 340.0f;
    f32 canvasW = 0.0f;
    if (showStrip) canvasW = availW - kInspW - 8.0f;
    if (showPal) canvasW -= kPalW + 8.0f;

    if (showPal) {
        DrawUIEditorPalette(engine, kPalW);
        ImGui::SameLine();
    }

    // -- The 2D canvas surface -------------------------------------------------
    // BeginChild + the WINDOW draw list (the schematic / cutscene idiom), not the
    // foreground list the viewport overlay uses: a foreground list forfeits ImGui
    // item input, which is why that overlay has to hand-roll everything.
    ImGui::BeginChild("##uieditcanvas", ImVec2(canvasW, 0.0f), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                          ImGuiWindowFlags_NoMove);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 sz = ImGui::GetContentRegionAvail();
    // ONE canvas-wide invisible button owning all three buttons (the cutscene
    // timeline idiom). Per-element buttons would be wrong here: UI elements
    // overlap by design and their z-order is layout order, not widget order.
    ImGui::InvisibleButton("##uiedsurface", ImVec2(glm::max(sz.x, 1.0f), glm::max(sz.y, 1.0f)),
                           ImGuiButtonFlags_MouseButtonLeft |
                               ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const bool focused = ImGui::IsWindowFocused();

    dl->AddRectFilled(p0, ImVec2(p0.x + sz.x, p0.y + sz.y), IM_COL32(24, 25, 29, 255));

    // The display box, in "canvas-height units". For MatchHeight and PixelPerfect
    // this IS the canvas. For Stretch it is the previewed OUTPUT box, and the
    // reference-sized canvas is stretched into it - which is exactly what Stretch
    // does on a real screen, so it belongs on a WYSIWYG surface.
    const glm::vec2 displayUnits(canvas.y * previewAspect, canvas.y);
    const f32 squeeze = displayUnits.x / canvas.x; // 1 unless Stretch at another aspect

    if (uiEdFitPending_ || uiEdZoom_ <= 0.0f) {
        uiEdFitPending_ = false;
        uiEdZoom_ = glm::clamp(glm::min(sz.x * 0.94f / displayUnits.x,
                                        sz.y * 0.94f / displayUnits.y),
                               kZoomMin, kZoomMax);
        uiEdPan_ = glm::vec2(0.0f);
    }

    const auto imgRect = [&](f32 zoom, glm::vec2 pan) {
        const glm::vec2 size = displayUnits * zoom;
        return glm::vec4(p0.x + (sz.x - size.x) * 0.5f + pan.x,
                         p0.y + (sz.y - size.y) * 0.5f + pan.y, size.x, size.y);
    };

    // Wheel zoom keeping the canvas point under the cursor fixed (the cutscene
    // timeline's zoom rule, in 2D). In Interact mode the wheel belongs to the
    // document's ScrollViews instead - that is the widget it is hardest to test any
    // other way - so zooming there is Ctrl+wheel.
    if (hovered && io.MouseWheel != 0.0f && (!uiEdInteract_ || io.KeyCtrl)) {
        const glm::vec4 rw = imgRect(uiEdZoom_, uiEdPan_);
        const glm::vec2 underCursor((io.MousePos.x - rw.x) / uiEdZoom_,
                                    (io.MousePos.y - rw.y) / uiEdZoom_);
        const f32 next =
            glm::clamp(uiEdZoom_ * std::pow(1.12f, io.MouseWheel), kZoomMin, kZoomMax);
        const glm::vec2 size = displayUnits * next;
        uiEdPan_ = glm::vec2(io.MousePos.x - underCursor.x * next - p0.x - (sz.x - size.x) * 0.5f,
                             io.MousePos.y - underCursor.y * next - p0.y - (sz.y - size.y) * 0.5f);
        uiEdZoom_ = next;
    }

    // ==== DIRECT MANIPULATION ================================================
    // Mapped per ITEM, not per document: a LayoutItem carries its own canvas size
    // (a UICanvas child may differ from the document header's), and the display box
    // represents that canvas.
    const glm::vec4 box = imgRect(uiEdZoom_, uiEdPan_);
    const auto toScreen = [&box](glm::vec2 c, glm::vec2 itemCanvas) {
        const glm::vec2 k = glm::max(itemCanvas, glm::vec2(1.0f));
        return ImVec2(box.x + c.x / k.x * box.z, box.y + c.y / k.y * box.w);
    };
    const auto toCanvas = [&box](ImVec2 s, glm::vec2 itemCanvas) {
        const glm::vec2 k = glm::max(itemCanvas, glm::vec2(1.0f));
        return glm::vec2((s.x - box.x) / glm::max(box.z, 1e-6f) * k.x,
                         (s.y - box.y) / glm::max(box.w, 1e-6f) * k.y);
    };

    auto& reg = scene.Registry();
    // Authoring is off while the runtime owns these very components, and while the
    // Interact preview owns the pointer (one press cannot both select an element
    // and click it).
    const bool canEdit = !playMode_ && !uiEdInteract_;

    // -- PALETTE DROP ----------------------------------------------------------
    // The canvas-wide InvisibleButton is the drop target (it is the last item, and
    // it is what owns the mouse here). Creation itself is DEFERRED to the end of
    // the panel: it adds entities, and the layout list this frame's outlines and
    // hit tests read is a snapshot taken before it.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pay = ImGui::AcceptDragDropPayload("UIED_NEW")) {
            if (pay->DataSize == static_cast<int>(sizeof(int)) && activeDoc_ != 0 &&
                !playMode_) {
                int idx = 0;
                std::memcpy(&idx, pay->Data, sizeof(int));
                // Parent it under whatever element is under the cursor (topmost
                // wins, as everywhere on this surface); null = the document root
                // chain in UIParentForNew decides.
                entt::entity under = entt::null;
                glm::vec2 dropCanvas = canvas;
                for (const ui::LayoutItem& it : uiEdLayout_) {
                    if (!reg.valid(it.entity)) continue;
                    if (it.hasClip && !it.clip.Contains(toCanvas(io.MousePos, it.canvas)))
                        continue;
                    if (it.rect.Contains(toCanvas(io.MousePos, it.canvas))) {
                        under = it.entity;
                        dropCanvas = it.canvas;
                    }
                }
                uiEdPendingCreate_ = idx;
                uiEdPendingCreateParent_ = under;
                uiEdPendingCreateAt_ = toCanvas(io.MousePos, dropCanvas);
                uiEdPendingCreatePlace_ = true;
            }
        }
        ImGui::EndDragDropTarget();
    }

    // -- INTERACT PREVIEW ------------------------------------------------------
    // The REAL runtime interaction, on the editor's OWN UIContext (never the
    // Engine's - this is an extra pass inside a frame whose runtime pass already
    // ran, and sharing the context would corrupt its touched-flag list and stats).
    // ctx.layout holds LAST frame's layout, which is exactly what the runtime path
    // hit-tests too, and the emission below then re-draws with the fresh flags.
    //
    // The Engine's own pass runs earlier in the frame with the pointer pinned off
    // (see the top of this function), so it clears nothing this pass set and fires
    // no game-flow verb - flowActive_ is false in the editor, and schematics only
    // tick while the simulation runs.
    if (uiEdInteract_ && !playMode_) {
        glm::vec2 ptr(-1.0f, -1.0f);
        if (hovered) {
            // Fraction of the DISPLAY BOX, which is the fraction of the canvas in
            // every scale mode (a Stretch canvas is stretched to fill the box).
            const glm::vec2 f((io.MousePos.x - box.x) / glm::max(box.z, 1e-6f),
                              (io.MousePos.y - box.y) / glm::max(box.w, 1e-6f));
            if (f.x >= 0.0f && f.x <= 1.0f && f.y >= 0.0f && f.y <= 1.0f) ptr = f;
        }
        ui::UpdateInteraction(scene, engine.GetInput(), ptr, /*pointers*/ nullptr,
                              uiEdInteractCtx_);
    }

    // Input is hit-tested against LAST frame's layout (see the header note). It is
    // only a valid basis when it was built at the same canvas - otherwise skip a
    // single frame rather than mis-register a click.
    const bool inputValid = !uiEdLayout_.empty() &&
                            std::fabs(uiEdLayoutCanvas_.x - canvas.x) < 0.5f &&
                            std::fabs(uiEdLayoutCanvas_.y - canvas.y) < 0.5f;
    // Ctrl suspends snapping mid-drag (the gizmo's convention).
    const bool snapOn = uiSnap_ && !io.KeyCtrl;
    const auto snap = [&](f32 v) {
        const f32 step = glm::max(uiSnapStep_, 0.01f);
        return snapOn ? std::round(v / step) * step : v;
    };
    constexpr f32 kHandlePx = 4.5f; // half-extent drawn
    constexpr f32 kGrabPx = 8.0f;   // half-extent that accepts a press
    constexpr f32 kMagnetPx = 9.0f; // magnetism reach, in SCREEN px (zoom-stable)

    const ui::LayoutItem* selItem = inputValid ? FindItem(uiEdLayout_, selected_) : nullptr;
    Editability selEd = Editable(scene, selected_);

    // -- press: handle, then anchor corner, then body, then empty --------------
    if (canEdit && inputValid && hovered && uiEdDragMode_ == kModeNone && !uiEdPanning_ &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        // Freeze the basis at press: the parent rect and the canvas size. Solving
        // against a rect that moves under the gesture is unstable, and for an anchor
        // drag the START anchors are what every frame re-derives from.
        const auto begin = [&](const ui::LayoutItem& it, int mode) {
            uiEdDragMode_ = mode;
            uiEdDragLive_ = false; // nothing is written until the threshold is crossed
            uiEdDragEntity_ = it.entity;
            uiEdPressScreen_ = glm::vec2(io.MousePos.x, io.MousePos.y);
            uiEdDragStartMouse_ = toCanvas(io.MousePos, it.canvas);
            uiEdDragStartCenter_ =
                glm::vec2((it.rect.x0 + it.rect.x1) * 0.5f, (it.rect.y0 + it.rect.y1) * 0.5f);
            uiEdDragStartSize_ = glm::vec2(it.rect.x1 - it.rect.x0, it.rect.y1 - it.rect.y0);
            uiEdDragParentRect_ =
                glm::vec4(it.parentRect.x0, it.parentRect.y0, it.parentRect.x1, it.parentRect.y1);
            uiEdDragCanvasSize_ = glm::max(it.canvas, glm::vec2(1.0f));
            if (const UIElement* el = reg.try_get<UIElement>(it.entity)) {
                uiEdDragStartAMin_ = el->anchorMin;
                uiEdDragStartAMax_ = el->anchorMax;
            }
        };

        bool started = false;
        // 1) Resize handles of the CURRENT selection win over everything: they sit
        //    on the element's edge, where a body hit-test would also succeed.
        if (selItem && selEd.resize) {
            for (int i = 0; i < kHandleCount && !started; ++i) {
                const glm::vec2 c(
                    selItem->rect.x0 + (selItem->rect.x1 - selItem->rect.x0) * kHandles[i].hx,
                    selItem->rect.y0 + (selItem->rect.y1 - selItem->rect.y0) * kHandles[i].hy);
                const ImVec2 s = toScreen(c, selItem->canvas);
                if (std::fabs(io.MousePos.x - s.x) <= kGrabPx &&
                    std::fabs(io.MousePos.y - s.y) <= kGrabPx) {
                    begin(*selItem, kModeResize0 + i);
                    started = true;
                }
            }
        }
        // 2) Anchor corners, which live OUT on the parent rect, so they only
        //    conflict with the body when the element already fills its parent.
        if (!started && selItem && selEd.anchors && uiEdShowAnchors_) {
            const UIElement* el = reg.try_get<UIElement>(selected_);
            if (el) {
                const glm::vec2 pmin = selItem->parentRect.Min();
                const glm::vec2 psz = selItem->parentRect.Size();
                for (int i = 0; i < 4 && !started; ++i) {
                    const glm::vec2 a(kAnchors[i].xi == 0 ? el->anchorMin.x : el->anchorMax.x,
                                      kAnchors[i].yi == 0 ? el->anchorMin.y : el->anchorMax.y);
                    const ImVec2 s = toScreen(pmin + a * psz, selItem->canvas);
                    if (std::fabs(io.MousePos.x - s.x) <= kGrabPx &&
                        std::fabs(io.MousePos.y - s.y) <= kGrabPx) {
                        begin(*selItem, kModeAnchor0 + i);
                        started = true;
                    }
                }
            }
        }
        // 3) The body: SELECT immediately, and arm a move in the same press so a
        //    click-drag both selects and moves (a layout tool's expected feel).
        if (!started) {
            // Topmost element under the cursor: LAST laid out wins, because layout
            // order IS draw order, so the last one is what the author can see.
            const ui::LayoutItem* hit = nullptr;
            for (const ui::LayoutItem& it : uiEdLayout_) {
                // Last frame's list can name an entity destroyed since (the
                // Hierarchy's Delete, a document close), so validity is checked
                // here rather than trusted.
                if (!reg.valid(it.entity)) continue;
                // groupInteractive is deliberately NOT consulted: a faded-out
                // subtree is exactly the thing you still need to select to author it.
                if (it.hasClip && !it.clip.Contains(toCanvas(io.MousePos, it.canvas)))
                    continue; // scrolled out of its ScrollView viewport
                if (it.rect.Contains(toCanvas(io.MousePos, it.canvas))) hit = &it;
            }
            if (hit) {
                // Alt+click selects the PARENT: the only way to reach a container
                // that its own children completely cover. (Chosen over "repeat click
                // walks up" because that would make the second click of a
                // double-click change the selection under the author.)
                entt::entity pick = hit->entity;
                if (io.KeyAlt && hit->parentEntity != entt::null &&
                    reg.valid(hit->parentEntity) && FindItem(uiEdLayout_, hit->parentEntity))
                    pick = hit->parentEntity;
                selected_ = pick;
                const ui::LayoutItem* it = FindItem(uiEdLayout_, pick);
                if (it && Editable(scene, pick).move) begin(*it, kModeMove);
                started = true;
            }
        }
        // 4) Empty canvas: deselect and pan.
        if (!started) {
            if (selected_ != entt::null && reg.valid(selected_) &&
                reg.all_of<UIElement>(selected_))
                selected_ = entt::null;
            uiEdPanning_ = true;
        }
    } else if (playMode_ && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        uiEdPanning_ = true; // while playing the canvas is a view: left-drag pans
    }
    // Note there is deliberately NO left-drag pan in Interact mode: a left drag is
    // how a slider is scrubbed, which is the whole point of the preview. Middle-drag
    // still pans.

    // -- the gesture ----------------------------------------------------------
    // THRESHOLD ARMING + THE SINGLE UNDO ENTRY. The press classified a gesture but
    // wrote nothing; the first movement past ImGui's drag threshold promotes it,
    // and that promotion is the one and only PushUndo of the whole drag. So a plain
    // click costs no undo slot and a completed drag is exactly one entry.
    if (uiEdDragMode_ != kModeNone && ImGui::IsMouseDown(ImGuiMouseButton_Left) && canEdit &&
        reg.valid(uiEdDragEntity_) && reg.all_of<UIElement>(uiEdDragEntity_)) {
        if (!uiEdDragLive_) {
            const glm::vec2 d(io.MousePos.x - uiEdPressScreen_.x,
                              io.MousePos.y - uiEdPressScreen_.y);
            const f32 thresh = glm::max(io.MouseDragThreshold, 1.0f);
            if (d.x * d.x + d.y * d.y > thresh * thresh) {
                PushUndo(engine); // Engine overload: captures the open DOCUMENTS
                uiEdDragLive_ = true;
            }
        }
        if (uiEdDragLive_) {
            UIElement& el = reg.get<UIElement>(uiEdDragEntity_);
            const glm::vec2 icanvas = uiEdDragCanvasSize_;
            const ui::Rect parent{uiEdDragParentRect_.x, uiEdDragParentRect_.y,
                                  uiEdDragParentRect_.z, uiEdDragParentRect_.w};
            const glm::vec2 pmin = parent.Min();
            const glm::vec2 psz = glm::max(parent.Size(), glm::vec2(1e-3f));
            const glm::vec2 mouse = toCanvas(io.MousePos, icanvas);
            const glm::vec2 delta = mouse - uiEdDragStartMouse_;
            const ui::Rect startRect = RectFromCenter(uiEdDragStartCenter_, uiEdDragStartSize_);
            uiEdGuideX_.clear();
            uiEdGuideY_.clear();

            if (uiEdDragMode_ >= kModeAnchor0) {
                // ANCHOR DRAG. The trick that makes this safe (and makes the preset
                // buttons safe too): write the anchors, then re-solve the SAME rect.
                // SolveElementFromRect preserves anchors, so its inverse - "keep the
                // rect, change the anchors" - re-derives offset/size with no jump.
                const AnchorCorner ac = kAnchors[uiEdDragMode_ - kModeAnchor0];
                glm::vec2 f = (mouse - pmin) / psz;
                f = glm::clamp(f, glm::vec2(0.0f), glm::vec2(1.0f));
                if (snapOn) { // the quarters are what an author actually wants
                    const f32 stops[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
                    const f32 reachX = kMagnetPx / glm::max(box.z, 1.0f) * icanvas.x / psz.x;
                    const f32 reachY = kMagnetPx / glm::max(box.w, 1.0f) * icanvas.y / psz.y;
                    for (const f32 s : stops) {
                        if (std::fabs(f.x - s) < reachX) f.x = s;
                        if (std::fabs(f.y - s) < reachY) f.y = s;
                    }
                }
                glm::vec2 amin = uiEdDragStartAMin_, amax = uiEdDragStartAMax_;
                if (ac.xi == 0) amin.x = glm::min(f.x, amax.x); else amax.x = glm::max(f.x, amin.x);
                if (ac.yi == 0) amin.y = glm::min(f.y, amax.y); else amax.y = glm::max(f.y, amin.y);
                el.anchorMin = amin;
                el.anchorMax = amax;
                ui::SolveElementFromRect(el, parent, startRect); // the element must NOT move
                MarkDocumentDirty(engine, uiEdDragEntity_);
            } else {
                // MOVE / RESIZE. Both produce a desired RECT; snapping operates on
                // the rect's own edges and centres so it reads identically either way.
                //
                // Snap targets, in the dragged item's canvas units: the parent's
                // edges + centre, and every SIBLING's edges + centres. Gathered from
                // THIS frame's registry via last frame's layout - siblings do not
                // move during the drag, so they are stable references.
                std::vector<f32> tx, ty;
                if (snapOn) {
                    tx = {parent.x0, (parent.x0 + parent.x1) * 0.5f, parent.x1};
                    ty = {parent.y0, (parent.y0 + parent.y1) * 0.5f, parent.y1};
                    const Parent* pp = reg.try_get<Parent>(uiEdDragEntity_);
                    const entt::entity parentEnt = pp ? pp->entity : entt::null;
                    for (const ui::LayoutItem& it : uiEdLayout_) {
                        if (it.entity == uiEdDragEntity_) continue;
                        if (it.parentEntity != parentEnt) continue;
                        tx.push_back(it.rect.x0);
                        tx.push_back((it.rect.x0 + it.rect.x1) * 0.5f);
                        tx.push_back(it.rect.x1);
                        ty.push_back(it.rect.y0);
                        ty.push_back((it.rect.y0 + it.rect.y1) * 0.5f);
                        ty.push_back(it.rect.y1);
                    }
                }
                // Magnetism reach converted from screen px so it feels the same at
                // every zoom (a fixed canvas-unit reach is unusable when zoomed out).
                const f32 reachX = kMagnetPx / glm::max(box.z, 1.0f) * icanvas.x;
                const f32 reachY = kMagnetPx / glm::max(box.w, 1.0f) * icanvas.y;

                ui::Rect d = startRect;
                if (uiEdDragMode_ == kModeMove) {
                    glm::vec2 c = uiEdDragStartCenter_ + delta;
                    c = {snap(c.x), snap(c.y)};
                    d = RectFromCenter(c, uiEdDragStartSize_);
                    if (snapOn) {
                        const f32 mx[3] = {d.x0, (d.x0 + d.x1) * 0.5f, d.x1};
                        const Magnet gx = Magnetize(mx, 3, tx, reachX);
                        if (gx.hit) {
                            d.x0 += gx.delta;
                            d.x1 += gx.delta;
                            uiEdGuideX_.push_back(gx.guide);
                        }
                        const f32 my[3] = {d.y0, (d.y0 + d.y1) * 0.5f, d.y1};
                        const Magnet gy = Magnetize(my, 3, ty, reachY);
                        if (gy.hit) {
                            d.y0 += gy.delta;
                            d.y1 += gy.delta;
                            uiEdGuideY_.push_back(gy.guide);
                        }
                    }
                } else {
                    const Handle& h = kHandles[uiEdDragMode_ - kModeResize0];
                    if (h.ex0) d.x0 = snap(startRect.x0 + delta.x);
                    if (h.ex1) d.x1 = snap(startRect.x1 + delta.x);
                    if (h.ey0) d.y0 = snap(startRect.y0 + delta.y);
                    if (h.ey1) d.y1 = snap(startRect.y1 + delta.y);
                    if (snapOn) { // only the MOVING edges snap
                        if (h.ex0 || h.ex1) {
                            const f32 mx[1] = {h.ex0 ? d.x0 : d.x1};
                            const Magnet g = Magnetize(mx, 1, tx, reachX);
                            if (g.hit) {
                                if (h.ex0) d.x0 = g.guide; else d.x1 = g.guide;
                                uiEdGuideX_.push_back(g.guide);
                            }
                        }
                        if (h.ey0 || h.ey1) {
                            const f32 my[1] = {h.ey0 ? d.y0 : d.y1};
                            const Magnet g = Magnetize(my, 1, ty, reachY);
                            if (g.hit) {
                                if (h.ey0) d.y0 = g.guide; else d.y1 = g.guide;
                                uiEdGuideY_.push_back(g.guide);
                            }
                        }
                    }
                    // Minimum extent, enforced by pushing back the MOVING edge so the
                    // pinned one really stays pinned - and ONLY on the axis this
                    // handle actually moves. `d` starts life as startRect, so an
                    // unconditional clamp INFLATED the other axis: grabbing R on a
                    // 420x2 separator silently made it 8 units tall.
                    constexpr f32 kMinExtent = 8.0f;
                    if ((h.ex0 || h.ex1) && d.x1 - d.x0 < kMinExtent) {
                        if (h.ex0) d.x0 = d.x1 - kMinExtent; else d.x1 = d.x0 + kMinExtent;
                    }
                    if ((h.ey0 || h.ey1) && d.y1 - d.y0 < kMinExtent) {
                        if (h.ey0) d.y0 = d.y1 - kMinExtent; else d.y1 = d.y0 + kMinExtent;
                    }
                }
                // Back-solve offset/size against the anchor region: anchors and pivot
                // are preserved, so a centre-anchored element stays centre-anchored
                // and a stretched one stays stretched.
                //
                // ...then PUT BACK whatever the refusal covered. SolveElementFromRect
                // writes offset AND size unconditionally, and the rect it is handed
                // is the LAID-OUT one, so a permitted gesture would overwrite the
                // refused field with a layout-derived value: a pure move of a
                // fitContent group baked its grown size into `size`, and a resize of
                // a group-managed child overwrote the authored `offset` with the
                // group's cursor position (destroying where it goes when it escapes).
                // Editability of the DRAGGED entity, freshly: `selEd` above was
                // computed before this frame's press classified the gesture.
                const Editability dragEd = Editable(scene, uiEdDragEntity_);
                const glm::vec2 keepOffset = el.offset;
                const glm::vec2 keepSize = el.size;
                ui::SolveElementFromRect(el, parent, d);
                if (!dragEd.move) el.offset = keepOffset;
                if (!dragEd.resize) el.size = keepSize;
                MarkDocumentDirty(engine, uiEdDragEntity_);
            }
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        uiEdDragMode_ = kModeNone;
        uiEdDragLive_ = false;
        uiEdPanning_ = false;
        uiEdDragEntity_ = entt::null;
        uiEdGuideX_.clear();
        uiEdGuideY_.clear();
    }

    // The press above may have changed the selection, and every use below this
    // point (nudge, outlines, handles, the cursor) has to reflect the NEW one -
    // otherwise the frame you click on something shows the previous element's
    // handles.
    selEd = Editable(scene, selected_);
    selItem = inputValid ? FindItem(uiEdLayout_, selected_) : nullptr;

    // -- arrow-key nudge -------------------------------------------------------
    // One undo entry per discrete key press (no auto-repeat coalescing): holding a
    // key would otherwise fill the 64-entry stack with 1-unit steps.
    if (canEdit && focused && !io.WantTextInput && uiEdDragMode_ == kModeNone && selItem &&
        selEd.move) {
        const struct { ImGuiKey key; glm::vec2 dir; } kNudges[4] = {
            {ImGuiKey_LeftArrow, {-1.0f, 0.0f}},
            {ImGuiKey_RightArrow, {1.0f, 0.0f}},
            {ImGuiKey_UpArrow, {0.0f, -1.0f}},
            {ImGuiKey_DownArrow, {0.0f, 1.0f}},
        };
        // ACCUMULATED, then applied once. Both axes can be pressed on the same
        // frame, and solving per key from the SAME cached rect made the second
        // write overwrite the first (a diagonal nudge moved on one axis only) while
        // charging two undo entries for it.
        const f32 step = io.KeyShift ? glm::max(uiSnapStep_, 1.0f) : 1.0f;
        glm::vec2 nudge(0.0f);
        for (const auto& n : kNudges)
            if (ImGui::IsKeyPressed(n.key, /*repeat*/ false)) nudge += n.dir * step;
        if (nudge != glm::vec2(0.0f)) {
            PushUndo(engine);
            UIElement& el = reg.get<UIElement>(selected_);
            const ui::Rect d{selItem->rect.x0 + nudge.x, selItem->rect.y0 + nudge.y,
                             selItem->rect.x1 + nudge.x, selItem->rect.y1 + nudge.y};
            // A nudge is a pure MOVE: keep the authored size (the laid-out rect can
            // be a fitContent group's grown one).
            const glm::vec2 keepSize = el.size;
            ui::SolveElementFromRect(el, selItem->parentRect, d);
            el.size = keepSize;
            MarkDocumentDirty(engine, selected_);
        }
    }

    // Middle-drag always pans. Left-drag pans only when this gesture STARTED on
    // empty canvas - otherwise a move-drag would pan the view under itself.
    if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
                   (uiEdPanning_ && ImGui::IsMouseDragging(ImGuiMouseButton_Left)))) {
        uiEdPan_ += glm::vec2(io.MouseDelta.x, io.MouseDelta.y);
    }
    // Keep a slice of the canvas reachable however far it was flung.
    {
        const glm::vec2 size = displayUnits * uiEdZoom_;
        const glm::vec2 limit = (size + glm::vec2(sz.x, sz.y)) * 0.5f - glm::vec2(48.0f);
        uiEdPan_ = glm::clamp(uiEdPan_, -glm::max(limit, glm::vec2(0.0f)),
                              glm::max(limit, glm::vec2(0.0f)));
    }

    // -- NOW build the canvas -------------------------------------------------
    // AFTER input, so a drag applied above is what this frame's picture and
    // outlines show. The vertex vector is a member: the renderer borrows the
    // pointer until RenderScene consumes it later this frame. An empty document
    // still submits - DrawUIToTexture clears, so the page reads transparent
    // instead of showing the previously selected document.
    ui::BuildDocumentVertices(scene, renderer, project.AssetsDir(),
                              glm::vec2(static_cast<f32>(rtW), static_cast<f32>(rtH)), cfg,
                              activeDoc_, uiEdVerts_, uiEdLayout_);
    uiEdLayoutCanvas_ = canvas;
    if (rt.IsValid()) {
        renderer.SetEditorUICanvas(rt, uiEdVerts_.empty() ? nullptr : uiEdVerts_.data(),
                                   static_cast<u32>(uiEdVerts_.size()));
    }
    // Hand THIS frame's layout to the Interact context for NEXT frame's hit test.
    // Same one-frame-old basis the runtime uses (ui::UpdateInteraction's own comment
    // says so), and it means the preview hit-tests the rects the author is looking
    // at rather than a layout built from a canvas that has since changed.
    if (uiEdInteract_) uiEdInteractCtx_.layout = uiEdLayout_;

    // The pan may have moved since the press math, so re-derive the box for drawing.
    const glm::vec4 r = imgRect(uiEdZoom_, uiEdPan_);
    const ImVec2 imgMin(r.x, r.y);
    const ImVec2 imgMax(r.x + r.z, r.y + r.w);
    const auto draw = [&r](glm::vec2 c, glm::vec2 itemCanvas) {
        const glm::vec2 k = glm::max(itemCanvas, glm::vec2(1.0f));
        return ImVec2(r.x + c.x / k.x * r.z, r.y + c.y / k.y * r.w);
    };

    dl->PushClipRect(p0, ImVec2(p0.x + sz.x, p0.y + sz.y), true);

    // Checkerboard UNDER the canvas: DrawUIToTexture clears to transparent black
    // and ImGui alpha-blends, so without a backdrop an unauthored region would be
    // indistinguishable from a black panel.
    //
    // Tiled over the VISIBLE intersection only, not over the whole page. Zoomed to
    // 8 px per canvas unit a 1920x1080 document is 15360x8640 screen px, which
    // naively is ~330k quads a frame; clamping to what is on screen bounds it at
    // (panel area / cell^2) regardless of zoom. Anchored to the page corner so the
    // pattern travels with the canvas while panning (Photoshop behaviour).
    {
        constexpr f32 kCell = 14.0f;
        const f32 vx0 = glm::max(imgMin.x, p0.x), vy0 = glm::max(imgMin.y, p0.y);
        const f32 vx1 = glm::min(imgMax.x, p0.x + sz.x), vy1 = glm::min(imgMax.y, p0.y + sz.y);
        if (vx1 > vx0 && vy1 > vy0) {
            dl->AddRectFilled(ImVec2(vx0, vy0), ImVec2(vx1, vy1), IM_COL32(58, 58, 64, 255));
            const int cx0 = static_cast<int>(std::floor((vx0 - imgMin.x) / kCell));
            const int cy0 = static_cast<int>(std::floor((vy0 - imgMin.y) / kCell));
            const int cx1 = static_cast<int>(std::ceil((vx1 - imgMin.x) / kCell));
            const int cy1 = static_cast<int>(std::ceil((vy1 - imgMin.y) / kCell));
            for (int y = cy0; y < cy1; ++y) {
                for (int x = cx0; x < cx1; ++x) {
                    if (((x + y) & 1) == 0) continue;
                    const ImVec2 c0(glm::max(imgMin.x + static_cast<f32>(x) * kCell, vx0),
                                    glm::max(imgMin.y + static_cast<f32>(y) * kCell, vy0));
                    const ImVec2 c1(glm::min(imgMin.x + static_cast<f32>(x + 1) * kCell, vx1),
                                    glm::min(imgMin.y + static_cast<f32>(y + 1) * kCell, vy1));
                    if (c1.x > c0.x && c1.y > c0.y)
                        dl->AddRectFilled(c0, c1, IM_COL32(72, 72, 79, 255));
                }
            }
        }
    }

    // The page itself, drawn through the DRAW LIST rather than ImGui::Image: an
    // Image would be a later overlapping item and would steal the hover/drag the
    // canvas-wide InvisibleButton above owns.
    if (texId != 0) {
        dl->AddImage(static_cast<ImTextureID>(texId), imgMin, imgMax);
    } else {
        const char* msg = rt.IsValid()
                              ? "(UI canvas not available on this backend)"
                              : "(no UI render target - see the note above)";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2((imgMin.x + imgMax.x - ts.x) * 0.5f,
                           (imgMin.y + imgMax.y - ts.y) * 0.5f),
                    IM_COL32(210, 210, 215, 200), msg);
    }

    // Canvas frame + centre guides. The frame is the document's canvas edge, i.e.
    // the screen edge at this aspect - the single most useful reference line on a
    // UI authoring surface.
    dl->AddRect(imgMin, imgMax, IM_COL32(255, 255, 255, 130), 0.0f, 0, 1.0f);
    dl->AddLine(ImVec2((imgMin.x + imgMax.x) * 0.5f, imgMin.y),
                ImVec2((imgMin.x + imgMax.x) * 0.5f, imgMax.y),
                IM_COL32(255, 255, 255, 26));
    dl->AddLine(ImVec2(imgMin.x, (imgMin.y + imgMax.y) * 0.5f),
                ImVec2(imgMax.x, (imgMin.y + imgMax.y) * 0.5f),
                IM_COL32(255, 255, 255, 26));

    // -- Element outlines, the selection, handles, the anchor widget ----------
    // Drawn AFTER the page so they sit on top of it, and inside the same clip rect
    // so a panned-away element's outline doesn't spill over the toolbar. From the
    // FRESH layout, i.e. including this frame's drag.
    const ui::LayoutItem* sel = FindItem(uiEdLayout_, selected_);
    {
        for (const ui::LayoutItem& it : uiEdLayout_) {
            if (it.entity == selected_) continue;
            // Faint box for every element, so a transparent Panel or an empty Label
            // is discoverable at all - on a WYSIWYG surface an invisible element is
            // otherwise unselectable by definition. groupOpacity is deliberately
            // ignored: these outlines are the editor's, not the UI pass's, so a
            // faded-out subtree stays navigable.
            dl->AddRect(draw({it.rect.x0, it.rect.y0}, it.canvas),
                        draw({it.rect.x1, it.rect.y1}, it.canvas),
                        IM_COL32(255, 255, 255, 38));
        }
        if (sel) {
            const UIElement* el = reg.try_get<UIElement>(selected_);
            // Orange = the rect is this element's to edit; grey = something else
            // owns it (fullscreen, a layout group, or a 2D transform).
            const bool free = selEd.move || selEd.resize;
            const ImU32 col = free ? IM_COL32(255, 160, 40, 235) : IM_COL32(170, 175, 185, 225);
            if (selEd.transformed && el) {
                // Draw the REAL on-screen quad: rect scaled+rotated about the pivot
                // point, exactly as the emitter does it. Otherwise the tool would
                // outline a box that is not where the element visibly is.
                const glm::vec2 pv = sel->rect.Min() + el->pivot * sel->rect.Size();
                const f32 rad = glm::radians(el->rotation);
                const f32 cs = std::cos(rad), sn = std::sin(rad);
                const glm::vec2 corners[4] = {{sel->rect.x0, sel->rect.y0},
                                              {sel->rect.x1, sel->rect.y0},
                                              {sel->rect.x1, sel->rect.y1},
                                              {sel->rect.x0, sel->rect.y1}};
                ImVec2 q[4];
                for (int i = 0; i < 4; ++i) {
                    const f32 dx = (corners[i].x - pv.x) * el->scale.x;
                    const f32 dy = (corners[i].y - pv.y) * el->scale.y;
                    q[i] = draw({pv.x + dx * cs - dy * sn, pv.y + dx * sn + dy * cs}, sel->canvas);
                }
                dl->AddPolyline(q, 4, col, ImDrawFlags_Closed, 2.0f);
                // ...and the untransformed layout rect faintly, since that is what
                // offset/size in the Inspector actually control.
                dl->AddRect(draw({sel->rect.x0, sel->rect.y0}, sel->canvas),
                            draw({sel->rect.x1, sel->rect.y1}, sel->canvas),
                            IM_COL32(170, 175, 185, 90), 0.0f, 0, 1.0f);
            } else {
                const ImVec2 s0 = draw({sel->rect.x0, sel->rect.y0}, sel->canvas);
                const ImVec2 s1 = draw({sel->rect.x1, sel->rect.y1}, sel->canvas);
                dl->AddRect(s0, s1, col, 0.0f, 0, 2.0f);
                // Handles are only drawn when they would actually work: offering a
                // grab point that silently does nothing (while playing, or on a
                // Grid-sized child) is worse than showing none.
                if (selEd.resize && canEdit) {
                    for (int i = 0; i < kHandleCount; ++i) {
                        const ImVec2 c(s0.x + (s1.x - s0.x) * kHandles[i].hx,
                                       s0.y + (s1.y - s0.y) * kHandles[i].hy);
                        dl->AddRectFilled(ImVec2(c.x - kHandlePx, c.y - kHandlePx),
                                          ImVec2(c.x + kHandlePx, c.y + kHandlePx), col);
                        dl->AddRect(ImVec2(c.x - kHandlePx, c.y - kHandlePx),
                                    ImVec2(c.x + kHandlePx, c.y + kHandlePx),
                                    IM_COL32(20, 20, 24, 220));
                    }
                }
            }
            // THE ANCHOR WIDGET. Anchoring is invisible in a rendered menu and it is
            // what decides whether the menu survives another resolution, so it gets a
            // permanent on-canvas representation: the anchor REGION inside the parent
            // (dashed), four draggable corners, and - when the anchors are spread -
            // ties from the element's edges to the region, which is the picture that
            // explains what "stretch" means.
            if (el && selEd.anchors && uiEdShowAnchors_ && canEdit) {
                const glm::vec2 pmin = sel->parentRect.Min();
                const glm::vec2 psz = sel->parentRect.Size();
                const glm::vec2 rmin = pmin + el->anchorMin * psz;
                const glm::vec2 rmax = pmin + el->anchorMax * psz;
                const ImVec2 a0 = draw(rmin, sel->canvas);
                const ImVec2 a1 = draw(rmax, sel->canvas);
                constexpr ImU32 kAnchorCol = IM_COL32(120, 210, 255, 230);
                // Dashed region box (hand-drawn dashes: ImGui has no dash style).
                const auto dash = [&](ImVec2 f, ImVec2 t) {
                    const f32 len = std::sqrt((t.x - f.x) * (t.x - f.x) + (t.y - f.y) * (t.y - f.y));
                    const int segs = glm::clamp(static_cast<int>(len / 8.0f), 1, 256);
                    for (int i = 0; i < segs; i += 2) {
                        const f32 u0 = static_cast<f32>(i) / static_cast<f32>(segs);
                        const f32 u1 = glm::min(static_cast<f32>(i + 1) / static_cast<f32>(segs), 1.0f);
                        dl->AddLine(ImVec2(f.x + (t.x - f.x) * u0, f.y + (t.y - f.y) * u0),
                                    ImVec2(f.x + (t.x - f.x) * u1, f.y + (t.y - f.y) * u1),
                                    IM_COL32(120, 210, 255, 150));
                    }
                };
                if (std::fabs(a1.x - a0.x) > 1.0f || std::fabs(a1.y - a0.y) > 1.0f) {
                    dash(ImVec2(a0.x, a0.y), ImVec2(a1.x, a0.y));
                    dash(ImVec2(a1.x, a0.y), ImVec2(a1.x, a1.y));
                    dash(ImVec2(a1.x, a1.y), ImVec2(a0.x, a1.y));
                    dash(ImVec2(a0.x, a1.y), ImVec2(a0.x, a0.y));
                }
                for (int i = 0; i < 4; ++i) {
                    const ImVec2 c(kAnchors[i].xi == 0 ? a0.x : a1.x,
                                   kAnchors[i].yi == 0 ? a0.y : a1.y);
                    const ImVec2 pts[4] = {ImVec2(c.x, c.y - 6.0f), ImVec2(c.x + 6.0f, c.y),
                                           ImVec2(c.x, c.y + 6.0f), ImVec2(c.x - 6.0f, c.y)};
                    dl->AddConvexPolyFilled(pts, 4, kAnchorCol);
                    dl->AddPolyline(pts, 4, IM_COL32(15, 30, 45, 230), ImDrawFlags_Closed, 1.0f);
                }
            }
        }
        // Snap guides the live drag locked onto, full-page so the alignment is
        // obvious. Drawn last so they sit over the outlines they explain.
        if (uiEdDragLive_) {
            const glm::vec2 gc = uiEdDragCanvasSize_;
            for (const f32 x : uiEdGuideX_) {
                const ImVec2 a = draw({x, 0.0f}, gc), b = draw({x, gc.y}, gc);
                dl->AddLine(a, b, IM_COL32(255, 90, 200, 190));
            }
            for (const f32 y : uiEdGuideY_) {
                const ImVec2 a = draw({0.0f, y}, gc), b = draw({gc.x, y}, gc);
                dl->AddLine(a, b, IM_COL32(255, 90, 200, 190));
            }
        }
    }

    // Cursor feedback: the resize arrows over a handle, and NotAllowed over the
    // body of something whose position is not its own to change.
    if (hovered && canEdit && sel) {
        if (uiEdDragMode_ >= kModeResize0 && uiEdDragMode_ < kModeAnchor0) {
            ImGui::SetMouseCursor(kHandles[uiEdDragMode_ - kModeResize0].cursor);
        } else if (uiEdDragMode_ == kModeNone) {
            bool onHandle = false;
            if (selEd.resize) {
                const ImVec2 s0 = draw({sel->rect.x0, sel->rect.y0}, sel->canvas);
                const ImVec2 s1 = draw({sel->rect.x1, sel->rect.y1}, sel->canvas);
                for (int i = 0; i < kHandleCount && !onHandle; ++i) {
                    const ImVec2 c(s0.x + (s1.x - s0.x) * kHandles[i].hx,
                                   s0.y + (s1.y - s0.y) * kHandles[i].hy);
                    if (std::fabs(io.MousePos.x - c.x) <= kGrabPx &&
                        std::fabs(io.MousePos.y - c.y) <= kGrabPx) {
                        ImGui::SetMouseCursor(kHandles[i].cursor);
                        onHandle = true;
                    }
                }
            }
            if (!onHandle && !selEd.move &&
                sel->rect.Contains(toCanvas(io.MousePos, sel->canvas)))
                ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed);
        }
    }

    // Nothing laid out is almost always "every screen in this document is
    // inactive", which is the normal as-loaded state of a multi-screen `.hbui` -
    // say so instead of showing a convincingly empty canvas.
    if (uiEdLayout_.empty()) {
        // ...but only point at the Screen combo when there IS one: it is drawn only
        // for documents that contain UIPanel entities, so a brand-new document was
        // being told to use a control that is not on screen.
        const char* msg =
            docHasScreens ? "Nothing to lay out - pick a screen in \"Screen\" above."
                          : "Empty document - drag something out of the palette.";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        const ImVec2 at((imgMin.x + imgMax.x - ts.x) * 0.5f,
                        (imgMin.y + imgMax.y - ts.y) * 0.5f);
        dl->AddRectFilled(ImVec2(at.x - 8.0f, at.y - 5.0f),
                          ImVec2(at.x + ts.x + 8.0f, at.y + ts.y + 5.0f),
                          IM_COL32(0, 0, 0, 170), 4.0f);
        dl->AddText(at, IM_COL32(235, 225, 180, 255), msg);
    }

    dl->PopClipRect();

    // -- Readouts, bottom-left of the canvas ----------------------------------
    {
        char line[256];
        std::snprintf(line, sizeof(line), "%.0f%%  |  %.0f x %.0f canvas units  |  %u elements",
                      uiEdZoom_ * 100.0f, canvas.x, canvas.y,
                      static_cast<u32>(uiEdLayout_.size()));
        const ImVec2 ts = ImGui::CalcTextSize(line);
        const ImVec2 at(p0.x + 8.0f, p0.y + sz.y - ts.y - 8.0f);
        dl->AddRectFilled(ImVec2(at.x - 5.0f, at.y - 3.0f),
                          ImVec2(at.x + ts.x + 5.0f, at.y + ts.y + 3.0f),
                          IM_COL32(0, 0, 0, 150), 3.0f);
        dl->AddText(at, IM_COL32(225, 225, 230, 255), line);
    }
    // Selection readout, top-left: name, rect in canvas units, and - when the rect
    // is not fully draggable - WHY, because a handle-less selection is otherwise
    // just a tool that appears broken.
    if (sel) {
        const Name* n = reg.try_get<Name>(selected_);
        char line[400];
        std::snprintf(line, sizeof(line), "%s   %.0f, %.0f   %.0f x %.0f%s%s",
                      (n && !n->value.empty()) ? n->value.c_str() : "(unnamed)",
                      sel->rect.x0, sel->rect.y0, sel->rect.x1 - sel->rect.x0,
                      sel->rect.y1 - sel->rect.y0, selEd.why ? "   - " : "",
                      selEd.why ? selEd.why : "");
        const ImVec2 ts = ImGui::CalcTextSize(line);
        const ImVec2 at(p0.x + 8.0f, p0.y + 8.0f);
        dl->AddRectFilled(ImVec2(at.x - 5.0f, at.y - 3.0f),
                          ImVec2(at.x + ts.x + 5.0f, at.y + ts.y + 3.0f),
                          IM_COL32(0, 0, 0, 165), 3.0f);
        dl->AddText(at, selEd.why ? IM_COL32(190, 195, 205, 255) : IM_COL32(255, 190, 110, 255),
                    line);
    } else if (canEdit && !uiEdLayout_.empty()) {
        const char* msg = "Click to select and drag   -   8 handles resize   -   blue "
                          "diamonds re-anchor   -   Alt+click selects the parent   -   "
                          "arrows nudge   -   Ctrl suspends snapping";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        const ImVec2 at(p0.x + 8.0f, p0.y + 8.0f);
        dl->AddRectFilled(ImVec2(at.x - 5.0f, at.y - 3.0f),
                          ImVec2(at.x + ts.x + 5.0f, at.y + ts.y + 3.0f),
                          IM_COL32(0, 0, 0, 130), 3.0f);
        dl->AddText(at, IM_COL32(170, 175, 185, 255), msg);
    }
    if (std::fabs(squeeze - 1.0f) > 0.001f) {
        char line[192];
        std::snprintf(line, sizeof(line),
                      "Stretch: the %.0f-wide canvas is squeezed to this %.2f:1 output",
                      canvas.x, previewAspect);
        const ImVec2 ts = ImGui::CalcTextSize(line);
        const ImVec2 at(p0.x + 8.0f, p0.y + sz.y - ts.y * 2.0f - 14.0f);
        dl->AddRectFilled(ImVec2(at.x - 5.0f, at.y - 3.0f),
                          ImVec2(at.x + ts.x + 5.0f, at.y + ts.y + 3.0f),
                          IM_COL32(0, 0, 0, 150), 3.0f);
        dl->AddText(at, IM_COL32(240, 200, 120, 255), line);
    }
    if (hovered) {
        // Cursor position in canvas units - the coordinate space every authored
        // offset/size is in.
        const glm::vec2 c((io.MousePos.x - r.x) / glm::max(uiEdZoom_ * squeeze, 1e-6f),
                          (io.MousePos.y - r.y) / glm::max(uiEdZoom_, 1e-6f));
        char line[96];
        std::snprintf(line, sizeof(line), "%.0f, %.0f", c.x, c.y);
        const ImVec2 ts = ImGui::CalcTextSize(line);
        const ImVec2 at(p0.x + sz.x - ts.x - 12.0f, p0.y + sz.y - ts.y - 8.0f);
        dl->AddRectFilled(ImVec2(at.x - 5.0f, at.y - 3.0f),
                          ImVec2(at.x + ts.x + 5.0f, at.y + ts.y + 3.0f),
                          IM_COL32(0, 0, 0, 150), 3.0f);
        dl->AddText(at, IM_COL32(190, 205, 235, 255), line);
    }

    // Right-click on the canvas: the same tool set the strip shows, plus Add, so
    // the two cannot drift apart. Opened on the canvas-wide InvisibleButton, which
    // is what owns the mouse here.
    if (canEdit) {
        if (ImGui::BeginPopupContextItem("##uiedctx")) {
            const Name* n = (selected_ != entt::null && reg.valid(selected_))
                                ? reg.try_get<Name>(selected_)
                                : nullptr;
            ImGui::TextDisabled("%s", n && !n->value.empty() ? n->value.c_str()
                                                             : "(nothing selected)");
            ImGui::Separator();
            if (ImGui::BeginMenu("Add")) {
                int catCount = 0;
                const UICreateDesc* cat = UICreateCatalog(catCount);
                for (int i = 0; i < catCount; ++i) {
                    if (i > 0 && cat[i].container != cat[i - 1].container)
                        ImGui::Separator();
                    if (ImGui::MenuItem(cat[i].label)) {
                        uiEdPendingCreate_ = i;
                        uiEdPendingCreateParent_ = selected_;
                        uiEdPendingCreatePlace_ = false;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", cat[i].tip);
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            DrawUIEditorTools(engine, /*compact*/ false);
            ImGui::EndPopup();
        }
    }

    ImGui::EndChild();

    if (showStrip) {
        ImGui::SameLine();
        DrawUIEditorInspector(engine, kInspW);
    }

    // -- Deferred structural edits --------------------------------------------
    // Applied here, at the very end, because everything above walks SNAPSHOTS -
    // the layout list and the tree's row list - and creating, destroying or
    // reparenting mid-walk leaves stale entities behind the cursor. The next
    // frame's build picks the result up.
    if (uiEdPendingCreate_ >= 0) {
        int catCount = 0;
        const UICreateDesc* cat = UICreateCatalog(catCount);
        const int idx = uiEdPendingCreate_;
        const entt::entity hint = uiEdPendingCreateParent_;
        const bool place = uiEdPendingCreatePlace_;
        const glm::vec2 at = uiEdPendingCreateAt_;
        uiEdPendingCreate_ = -1;
        uiEdPendingCreateParent_ = entt::null;
        uiEdPendingCreatePlace_ = false;
        if (idx < catCount) {
            const entt::entity made =
                CreateUIElementInDocument(engine, cat[idx].what, hint);
            // A DROP lands where the cursor was. The rect is solved against the new
            // element's own parent rect, which needs a layout - so lay the document
            // out once more (uncached, the editor's own map) and place it from that.
            if (made != entt::null && place && reg.all_of<UIElement>(made)) {
                std::vector<ui::LayoutItem> fresh;
                std::vector<rhi::UIVertex> ignore;
                ui::BuildDocumentVertices(
                    scene, renderer, project.AssetsDir(),
                    glm::vec2(static_cast<f32>(rtW), static_cast<f32>(rtH)), cfg,
                    activeDoc_, ignore, fresh);
                if (const ui::LayoutItem* it = FindItem(fresh, made)) {
                    UIElement& el = reg.get<UIElement>(made);
                    if (!el.fullscreen) {
                        const glm::vec2 half = it->rect.Size() * 0.5f;
                        ui::SolveElementFromRect(
                            el, it->parentRect,
                            ui::Rect{at.x - half.x, at.y - half.y, at.x + half.x,
                                     at.y + half.y});
                    }
                }
                // The vertex pointer the renderer is holding belonged to uiEdVerts_,
                // which the throwaway build above did NOT touch - but the layout the
                // outlines were drawn from is now stale, so drop it and let the next
                // frame rebuild rather than hit-test against it.
                uiEdLayoutCanvas_ = glm::vec2(0.0f);
            }
        }
    }
    if (uiEdPendingReparentChild_ != entt::null || uiEdPendingUnparent_) {
        const entt::entity child = uiEdPendingReparentChild_;
        const entt::entity to = uiEdPendingUnparent_ ? entt::null : uiEdPendingReparentTo_;
        uiEdPendingReparentChild_ = entt::null;
        uiEdPendingReparentTo_ = entt::null;
        uiEdPendingUnparent_ = false;
        if (reg.valid(child)) {
            // Reparent pushes its own undo and enforces the cross-document refusal
            // (an UNPARENT is exempt on purpose: a document element becoming a
            // document ROOT stays in the same document - the reference project's
            // menu is four canvas-less roots).
            Reparent(scene, child, to);
            MarkDocumentDirty(engine, child);
            uiEdLayoutCanvas_ = glm::vec2(0.0f);
        }
    }
    if (uiEdPendingDelete_ != entt::null) {
        const entt::entity victim = uiEdPendingDelete_;
        uiEdPendingDelete_ = entt::null;
        if (reg.valid(victim)) {
            // The Interact snapshot names entities by handle, and this is about to
            // destroy some of them: restore now, while they are still alive, rather
            // than write into recycled ids later.
            const bool wasInteract = uiEdInteract_;
            if (wasInteract) UIEditorEndInteract(engine);
            MarkDocumentDirty(engine, victim);
            PushUndo(engine);
            DestroyRecursive(scene, victim); // clears selected_ if it was the victim
            // Drop the gesture + the layout BASIS (not the whole panel state): the
            // cached list still names the destroyed subtree, and every consumer
            // guards with reg.valid(), but a press must not hit-test against it.
            uiEdDragMode_ = kModeNone;
            uiEdDragLive_ = false;
            uiEdDragEntity_ = entt::null;
            uiEdLayoutCanvas_ = glm::vec2(0.0f);
            if (wasInteract) UIEditorBeginInteract(engine); // re-snapshot what is left
        }
    }
    ImGui::End();
}

// The right-hand strip. Everything here writes through the SAME path the canvas
// gestures use - a desired rect into ui::SolveElementFromRect - so the numeric
// fields and the drag cannot disagree about what a rect means.
void Editor::DrawUIEditorInspector(Engine& engine, f32 width) {
    ImGui::BeginChild("##uiedinsp", ImVec2(width, 0.0f), ImGuiChildFlags_Borders);
    Scene& scene = engine.GetScene();
    auto& reg = scene.Registry();

    const ui::LayoutItem* sel = FindItem(uiEdLayout_, selected_);
    UIElement* el = (selected_ != entt::null && reg.valid(selected_))
                        ? reg.try_get<UIElement>(selected_)
                        : nullptr;
    if (!el) {
        // A CANVAS is a container with NO rect of its own: it carries UICanvas and
        // no UIElement, so LayoutUIImpl emits no LayoutItem for it and this strip
        // has nothing to edit. Creating one from the palette selects it, so a bare
        // "Nothing selected" here directly contradicts what the author just did.
        if (selected_ != entt::null && reg.valid(selected_) &&
            reg.all_of<UICanvas>(selected_)) {
            ImGui::TextDisabled("A Canvas has no rect of its own.");
            ImGui::Spacing();
            ImGui::TextWrapped("It is a container: its scale mode and reference size "
                               "decide the coordinate space its children lay out in, "
                               "and both live in the Inspector panel. Select one of its "
                               "children to place it, or drop something out of the "
                               "palette onto it.");
            ImGui::EndChild();
            return;
        }
        ImGui::TextDisabled("Nothing selected.");
        ImGui::Spacing();
        ImGui::TextWrapped("Click an element on the canvas. Everything this strip edits - "
                           "the rect, the anchors, the pivot - is the RectTransform the "
                           "runtime lays the element out with; text, colour and the "
                           "button action live in the Inspector panel.");
        ImGui::EndChild();
        return;
    }
    if (!sel) {
        // Selected, but absent from THIS document's layout. Distinguishing the two
        // matters: the fix is different (pick its screen / switch document vs. click
        // something), and a bare "nothing selected" here would contradict the
        // Hierarchy's highlight.
        ImGui::TextDisabled("Selected, but not laid out on this canvas.");
        ImGui::Spacing();
        ImGui::TextWrapped("It belongs to another document, or its screen is not the one "
                           "shown, or it (or an ancestor) is hidden. Pick its screen in "
                           "\"Screen\", or switch the Document above.");
        ImGui::EndChild();
        return;
    }
    const bool canEdit = !playMode_;
    const Editability ed = Editable(scene, selected_);

    const Name* n = reg.try_get<Name>(selected_);
    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.43f, 1.0f), "%s",
                       (n && !n->value.empty()) ? n->value.c_str() : "(unnamed)");
    if (playMode_) {
        ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.3f, 1.0f), "Stop to edit.");
    }
    ImGui::Separator();

    // -- Rect ------------------------------------------------------------------
    // The rect the runtime resolves, in canvas units - not the raw offset/size,
    // because those mean different things depending on the anchors and that is
    // precisely the confusion this panel exists to remove.
    ImGui::TextDisabled("Rect (canvas units)");
    glm::vec2 pos(sel->rect.x0, sel->rect.y0);
    glm::vec2 size(sel->rect.x1 - sel->rect.x0, sel->rect.y1 - sel->rect.y0);
    ImGui::BeginDisabled(!canEdit || !(ed.move || ed.resize));
    bool rectEdited = false;
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::DragFloat2("##uiedpos", &pos.x, 1.0f, 0.0f, 0.0f, "%.0f")) rectEdited = true;
    if (ImGui::IsItemActivated()) PushUndo(engine); // one entry per field edit session
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Top-left corner, in canvas units.");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::DragFloat2("##uiedsize", &size.x, 1.0f, 1.0f, 8192.0f, "%.0f")) rectEdited = true;
    if (ImGui::IsItemActivated()) PushUndo(engine);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Width x height, in canvas units.");
    ImGui::EndDisabled();
    if (rectEdited && canEdit) {
        // Respect the same refusals the canvas does: a layout-group child's
        // position is not its own, so keep its laid-out origin.
        if (!ed.move) pos = glm::vec2(sel->rect.x0, sel->rect.y0);
        if (!ed.resize) size = glm::vec2(sel->rect.x1 - sel->rect.x0, sel->rect.y1 - sel->rect.y0);
        size = glm::max(size, glm::vec2(1.0f));
        // Pinning the DISPLAYED value is not enough: SolveElementFromRect writes
        // offset AND size unconditionally, from the LAID-OUT rect. Put the refused
        // field back, exactly as the canvas gesture does.
        const glm::vec2 keepOffset = el->offset;
        const glm::vec2 keepSize = el->size;
        ui::SolveElementFromRect(*el, sel->parentRect,
                                 ui::Rect{pos.x, pos.y, pos.x + size.x, pos.y + size.y});
        if (!ed.move) el->offset = keepOffset;
        if (!ed.resize) el->size = keepSize;
        MarkDocumentDirty(engine, selected_);
    }

    // -- Anchors ---------------------------------------------------------------
    ImGui::Spacing();
    ImGui::TextDisabled("Anchors");
    ImGui::TextWrapped("%s", AnchorLabel(el->anchorMin, el->anchorMax).c_str());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("anchorMin %.2f, %.2f   anchorMax %.2f, %.2f\n"
                          "Anchors are FRACTIONS of the parent rect. Equal min/max pins "
                          "the element to a point; a spread makes it follow the parent's "
                          "size on that axis.",
                          el->anchorMin.x, el->anchorMin.y, el->anchorMax.x, el->anchorMax.y);

    // Re-anchoring KEEPS THE RECT: write the anchors, then re-solve the rect the
    // author can already see. Otherwise every preset click teleports the element,
    // which is what makes anchors feel unusable.
    const auto reAnchor = [&](glm::vec2 mn, glm::vec2 mx, glm::vec2 pivot) {
        PushUndo(engine);
        const ui::Rect keep = sel->rect;
        el->anchorMin = mn;
        el->anchorMax = mx;
        el->pivot = pivot;
        ui::SolveElementFromRect(*el, sel->parentRect, keep);
        MarkDocumentDirty(engine, selected_);
    };
    ImGui::BeginDisabled(!canEdit || !ed.anchors);
    const f32 cell = 30.0f;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const AnchorPreset& p = kPointPresets[row * 3 + col];
            if (col > 0) ImGui::SameLine();
            const bool on = el->anchorMin == p.mn && el->anchorMax == p.mx;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.55f, 0.85f, 1.0f));
            if (ImGui::Button(p.label, ImVec2(cell, cell))) reAnchor(p.mn, p.mx, p.pivot);
            if (on) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s (keeps the current rect)", p.tip);
        }
    }
    for (int i = 0; i < 3; ++i) {
        if (i > 0) ImGui::SameLine();
        const AnchorPreset& p = kSpreadPresets[i];
        const bool on = el->anchorMin == p.mn && el->anchorMax == p.mx;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.55f, 0.85f, 1.0f));
        if (ImGui::Button(p.label, ImVec2(cell * 3.0f + 16.0f, 0.0f))) reAnchor(p.mn, p.mx, p.pivot);
        if (on) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s (keeps the current rect)", p.tip);
    }
    ImGui::EndDisabled();
    if (el->anchorMin != el->anchorMax) {
        // With the anchors spread, `size` is a DELTA against the anchor region, not a
        // size - so say what the numbers mean rather than letting the author guess.
        ImGui::TextDisabled("size is a delta here: %+.0f, %+.0f", el->size.x, el->size.y);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Stretching: the element's size follows the anchor region "
                              "plus this delta (Unity sizeDelta). The Rect fields above "
                              "still show real canvas units.");
    }

    // -- Pivot -----------------------------------------------------------------
    ImGui::Spacing();
    ImGui::TextDisabled("Pivot");
    ImGui::BeginDisabled(!canEdit || !ed.anchors);
    glm::vec2 pivot = el->pivot;
    ImGui::SetNextItemWidth(-1.0f);
    const bool pivotEdited = ImGui::DragFloat2("##uiedpivot", &pivot.x, 0.01f, 0.0f, 1.0f, "%.2f");
    // Captured BEFORE the write below: on the frame a drag both activates and
    // changes the value, a snapshot taken after the mutation would record the
    // post-edit state and the undo would be a no-op.
    if (ImGui::IsItemActivated()) PushUndo(engine);
    if (pivotEdited) {
        // Keep the rect here too: a pivot is a reference point, and moving the
        // element as a side effect of changing it is never what was meant.
        const ui::Rect keep = sel->rect;
        // A pivot NEVER changes an element's size (ComputeElementRect derives size
        // from the anchor region + `size` alone), but SolveElementFromRect writes
        // both fields - and `keep` is the laid-out rect, which for a fitContent
        // group is its GROWN one. Restoring the size keeps that invariant true.
        const glm::vec2 keepSize = el->size;
        el->pivot = glm::clamp(pivot, glm::vec2(0.0f), glm::vec2(1.0f));
        ui::SolveElementFromRect(*el, sel->parentRect, keep);
        el->size = keepSize;
        MarkDocumentDirty(engine, selected_);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The element's own reference point (0,0 = top-left, 1,1 = "
                          "bottom-right). Rotation and scale happen about it. Changing "
                          "it keeps the rect where it is.");

    // -- Why a gesture was refused, and what to do instead --------------------
    if (ed.why) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.45f, 1.0f), "%s", ed.why);
        if (ed.hint) ImGui::TextWrapped("%s", ed.hint);
        if (ed.group != entt::null && reg.valid(ed.group)) {
            const Name* gn = reg.try_get<Name>(ed.group);
            const std::string label =
                std::string("Select the group: ") +
                ((gn && !gn->value.empty()) ? gn->value : std::string("(unnamed)"));
            if (ImGui::Button(label.c_str(), ImVec2(-1.0f, 0.0f))) selected_ = ed.group;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("A Layout Group's spacing / padding / cellSize is where "
                                  "its children's positions actually come from - edit it "
                                  "in the Inspector panel.");
        }
    }

    // -- Structure + tools -----------------------------------------------------
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("Draw order");
    DrawUIEditorTools(engine, /*compact*/ true);
    ImGui::Spacing();
    if (ImGui::Button("Inspector panel...", ImVec2(-1.0f, 0.0f))) {
        panelOpen_[Panel_Inspector] = true;
        ImGui::SetWindowFocus("Inspector");
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Text, colour, textures, the button `action`, the widget "
                          "options, the Layout Group fields - everything a rect cannot "
                          "express lives there, for the same selection.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("Ctrl suspends snapping. Shift+arrow nudges by the grid step.");
    ImGui::EndChild();
}

// ============================================================================
// THE TOOL ROW - clipboard, delete, duplicate, z-order.
//
// Drawn in BOTH the inspector strip and the canvas's right-click menu off this
// one function, so the two can never offer different things.
//
// The clipboard deliberately reuses the editor-wide Copy / Cut / Paste /
// Duplicate rather than growing a UI-specific one. That path is the FIXED one:
// BuildSubtreeJson's document skip is scoped to the requested root's OWN
// document, PasteSubtree refuses a UI fragment when no document is the edit
// target and re-Tracks the whole pasted subtree into the active one, and
// CopySelection remembers `clipboardFromDoc_` so a bare grouping node with no UI
// component of its own still goes back into a document. A second fragment
// builder here is exactly how the "Cut deleted content it had not copied" bug
// (an empty-but-not-empty fragment string) would come back.
// ============================================================================
void Editor::DrawUIEditorTools(Engine& engine, bool compact) {
    Scene& scene = engine.GetScene();
    auto& reg = scene.Registry();
    const bool live = !playMode_ && !uiEdInteract_;
    const bool hasSel = selected_ != entt::null && reg.valid(selected_) &&
                        reg.all_of<UIDocMember>(selected_) &&
                        reg.get<UIDocMember>(selected_).doc == activeDoc_ && activeDoc_ != 0;
    // Four buttons per row when the strip is driving: subtract the three gaps
    // ImGui will insert, or the last one wraps onto its own line.
    const f32 gap = ImGui::GetStyle().ItemSpacing.x;
    const f32 w = compact
                      ? glm::max((ImGui::GetContentRegionAvail().x - gap * 3.0f) * 0.25f,
                                 24.0f)
                      : 0.0f;
    const ImVec2 bs(w, 0.0f);

    ImGui::BeginDisabled(!live || !hasSel);
    if (ImGui::Button("Front", bs)) UIReorder(engine, selected_, +1, /*run*/ true);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Draw LAST among its siblings, i.e. on top of them.\n"
                          "Draw order is sibling order, and it is saved as the "
                          "document's entity order.");
    ImGui::SameLine();
    if (ImGui::Button("Raise", bs)) UIReorder(engine, selected_, +1, /*run*/ false);
    ImGui::SameLine();
    if (ImGui::Button("Lower", bs)) UIReorder(engine, selected_, -1, /*run*/ false);
    ImGui::SameLine();
    if (ImGui::Button("Back", bs)) UIReorder(engine, selected_, -1, /*run*/ true);
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!live || !hasSel);
    if (ImGui::Button("Copy", bs)) CopySelection(scene);
    ImGui::SameLine();
    if (ImGui::Button("Cut", bs)) {
        // Copy FIRST, while the subtree is still alive; the destroy is deferred to
        // the end of the panel like every other structural edit.
        CopySelection(scene);
        uiEdPendingDelete_ = selected_;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Copies the subtree, then deletes it. Ctrl+V pastes it back "
                          "into whichever document is the edit target.");
    ImGui::SameLine();
    if (ImGui::Button("Dupe", bs)) DuplicateSelection(engine);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Duplicate in place - the clone lands exactly on top of the "
                          "original, in the same document. Drag it off.");
    ImGui::SameLine();
    if (ImGui::Button("Del", bs)) uiEdPendingDelete_ = selected_;
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!live || clipboard_.empty() || activeDoc_ == 0);
    if (ImGui::Button("Paste", ImVec2(compact ? -1.0f : 0.0f, 0.0f)))
        PasteClipboard(engine);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Pastes into the ACTIVE document. A UI fragment with no "
                          "document open is refused, not quietly turned into scene "
                          "content.");
    if (compact)
        ImGui::TextDisabled("Ctrl+C / X / V / D work anywhere in the editor.");
}

// ============================================================================
// THE LEFT STRIP - the element palette above this document's tree.
// ============================================================================
void Editor::DrawUIEditorPalette(Engine& engine, f32 width) {
    ImGui::BeginChild("##uiedpal", ImVec2(width, 0.0f), ImGuiChildFlags_Borders);

    const bool live = !playMode_ && !uiEdInteract_ && activeDoc_ != 0;
    ImGui::TextDisabled("Palette");
    if (!live) {
        ImGui::TextWrapped("%s", activeDoc_ == 0
                                     ? "No document is the edit target."
                                     : (playMode_ ? "Stop to author."
                                                  : "Switch Interact off to author."));
    }
    ImGui::BeginDisabled(!live);
    int count = 0;
    const UICreateDesc* cat = UICreateCatalog(count);
    bool sawContainer = false;
    for (int i = 0; i < count; ++i) {
        if (cat[i].container && !sawContainer) {
            sawContainer = true;
            ImGui::Spacing();
            ImGui::TextDisabled("Containers");
        }
        ImGui::PushID(i);
        // Selectable, not Button: it is a natural drag source and reads as a list.
        if (ImGui::Selectable(cat[i].label, false, ImGuiSelectableFlags_None,
                             ImVec2(0.0f, 0.0f))) {
            // Click = add without a drop point: parented by the fallback chain and
            // left at the recipe's default rect (centred by its default anchors).
            uiEdPendingCreate_ = i;
            uiEdPendingCreateParent_ = selected_;
            uiEdPendingCreatePlace_ = false;
        }
        if (live && ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            const int payload = i;
            ImGui::SetDragDropPayload("UIED_NEW", &payload, sizeof(int));
            ImGui::Text("+ %s", cat[i].label);
            ImGui::TextDisabled("drop it on the canvas");
            ImGui::EndDragDropSource();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(320.0f);
            ImGui::TextUnformatted(cat[i].tip);
            ImGui::TextDisabled("Drag onto the canvas to place it there, or click to add "
                                "it to the selection.");
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    ImGui::Spacing();
    ImGui::TextDisabled("No World Text here: 3D signage is not a\n.hbui key at all - it "
                        "belongs to the level.");

    ImGui::Spacing();
    ImGui::Separator();
    DrawUIEditorTree(engine);
    ImGui::EndChild();
}

// The open document's tree. Ordered the way it DRAWS (first drawn at the top,
// front-most at the bottom - Unity's convention), so the tree and the Raise /
// Lower buttons tell the same story.
void Editor::DrawUIEditorTree(Engine& engine) {
    Scene& scene = engine.GetScene();
    auto& reg = scene.Registry();
    ImGui::TextDisabled("Document");
    if (activeDoc_ == 0) {
        ImGui::TextDisabled("(nothing open)");
        return;
    }
    const bool live = !playMode_ && !uiEdInteract_;

    // Position in the layout = draw order. Anything absent from it is hidden,
    // clipped away, or on an inactive screen; it still has to be reachable (that
    // is how an author un-hides it), so it sorts after its laid-out siblings and
    // is drawn dim.
    std::unordered_map<u32, int> drawAt;
    for (usize i = 0; i < uiEdLayout_.size(); ++i)
        drawAt[static_cast<u32>(uiEdLayout_[i].entity)] = static_cast<int>(i);

    std::vector<entt::entity> members;
    for (const entt::entity e : reg.view<UIDocMember>()) {
        if (reg.get<UIDocMember>(e).doc != activeDoc_) continue;
        members.push_back(e);
    }
    const auto isMember = [&](entt::entity e) {
        if (e == entt::null || !reg.valid(e)) return false;
        const UIDocMember* m = reg.try_get<UIDocMember>(e);
        return m && m->doc == activeDoc_;
    };
    const auto order = [&](entt::entity a, entt::entity b) {
        const auto ia = drawAt.find(static_cast<u32>(a));
        const auto ib = drawAt.find(static_cast<u32>(b));
        const int ka = ia == drawAt.end() ? 1 << 28 : ia->second;
        const int kb = ib == drawAt.end() ? 1 << 28 : ib->second;
        if (ka != kb) return ka < kb;
        return static_cast<u32>(a) < static_cast<u32>(b);
    };
    std::unordered_map<u32, std::vector<entt::entity>> kids;
    std::vector<entt::entity> roots;
    for (const entt::entity e : members) {
        const Parent* p = reg.try_get<Parent>(e);
        if (p && isMember(p->entity)) kids[static_cast<u32>(p->entity)].push_back(e);
        else roots.push_back(e);
    }
    std::sort(roots.begin(), roots.end(), order);
    for (auto& [k, v] : kids) std::sort(v.begin(), v.end(), order);

    // A drop ZONE above the tree that unparents. Reparent allows an unparent
    // across the board (a document element becoming a document ROOT stays in the
    // same document), and it is the only way to pull something back out of a
    // ScrollView or a Layout Group by dragging.
    // (a plain Selectable, not a disabled one: ImGui skips disabled items as drop
    // targets, which is the only thing this row is for)
    ImGui::Selectable("- document root: drop here to unparent -", false);
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pay = ImGui::AcceptDragDropPayload("UIED_ENT")) {
            entt::entity dropped = entt::null;
            if (pay->DataSize == static_cast<int>(sizeof(entt::entity)))
                std::memcpy(&dropped, pay->Data, sizeof(entt::entity));
            if (live && reg.valid(dropped)) {
                uiEdPendingReparentChild_ = dropped;
                uiEdPendingUnparent_ = true;
            }
        }
        if (const ImGuiPayload* pay = ImGui::AcceptDragDropPayload("UIED_NEW")) {
            int idx = 0;
            if (pay->DataSize == static_cast<int>(sizeof(int)))
                std::memcpy(&idx, pay->Data, sizeof(int));
            if (live) {
                uiEdPendingCreate_ = idx;
                uiEdPendingCreateParent_ = entt::null;
                uiEdPendingCreatePlace_ = false;
            }
        }
        ImGui::EndDragDropTarget();
    }

    // One row. Recursive lambda: the tree is tens of nodes, and a member function
    // would need the four maps above threaded through it.
    const auto row = [&](auto&& self, entt::entity e, int depth) -> void {
        (void)depth;
        if (!reg.valid(e)) return;
        const auto kit = kids.find(static_cast<u32>(e));
        const bool hasKids = kit != kids.end() && !kit->second.empty();
        const Name* n = reg.try_get<Name>(e);
        const UIElement* el = reg.try_get<UIElement>(e);
        const UIPanel* panel = reg.try_get<UIPanel>(e);
        const bool laidOut = drawAt.count(static_cast<u32>(e)) != 0;

        // A one-letter kind tag: on a narrow strip the type matters more than any
        // decoration, and the name alone ("UI Label") often does not say it.
        const char* kind = "?";
        if (reg.all_of<UICanvas>(e)) kind = "CV";
        else if (panel) kind = "SC";
        else if (el) {
            switch (el->type) {
                case UIElement::Type::Panel: kind = reg.all_of<UILayoutGroup>(e) ? "LG"
                                                    : reg.all_of<UICanvasGroup>(e) ? "CG"
                                                                                   : "Pn"; break;
                case UIElement::Type::Label: kind = "Tx"; break;
                case UIElement::Type::Button: kind = "Bt"; break;
                case UIElement::Type::Image: kind = "Im"; break;
                case UIElement::Type::ProgressBar: kind = el->radial ? "Wh" : "Pr"; break;
                case UIElement::Type::Slider: kind = "Sl"; break;
                case UIElement::Type::Toggle: kind = "Tg"; break;
                case UIElement::Type::Selector: kind = "Se"; break;
                case UIElement::Type::ScrollView: kind = "Sv"; break;
                case UIElement::Type::TextInput: kind = "In"; break;
            }
        }
        char label[192];
        std::snprintf(label, sizeof(label), "%s  %s", kind,
                      (n && !n->value.empty()) ? n->value.c_str() : "(unnamed)");

        ImGui::PushID(static_cast<int>(static_cast<u32>(e)));
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                   ImGuiTreeNodeFlags_SpanAvailWidth |
                                   ImGuiTreeNodeFlags_DefaultOpen;
        if (!hasKids) flags |= ImGuiTreeNodeFlags_Leaf;
        if (e == selected_) flags |= ImGuiTreeNodeFlags_Selected;
        if (!laidOut) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.56f, 0.60f, 1.0f));
        const bool open = ImGui::TreeNodeEx(label, flags);
        if (!laidOut) ImGui::PopStyleColor();
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) selected_ = e;
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(340.0f);
            if (panel)
                ImGui::Text("Screen \"%s\"%s", panel->name.c_str(),
                            panel->startVisible ? "  (start visible)" : "");
            if (const ui::LayoutItem* it = FindItem(uiEdLayout_, e))
                ImGui::Text("rect  %.0f, %.0f   %.0f x %.0f", it->rect.x0, it->rect.y0,
                            it->rect.x1 - it->rect.x0, it->rect.y1 - it->rect.y0);
            else
                ImGui::TextUnformatted("Not laid out: hidden, clipped away, or on a "
                                       "screen that is not the one being shown.");
            if (el && !el->action.empty()) ImGui::Text("action  \"%s\"", el->action.c_str());
            ImGui::TextDisabled("Drag onto another row to re-parent, or onto "
                                "\"- document root -\" to unparent.");
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        // Drag source / drop target: reparenting by drag, through the SAME
        // Editor::Reparent the Hierarchy uses - so the cross-document refusal and
        // the transform rebase are enforced in one place.
        if (live && ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            ImGui::SetDragDropPayload("UIED_ENT", &e, sizeof(entt::entity));
            ImGui::Text("%s", label);
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pay = ImGui::AcceptDragDropPayload("UIED_ENT")) {
                entt::entity dropped = entt::null;
                if (pay->DataSize == static_cast<int>(sizeof(entt::entity)))
                    std::memcpy(&dropped, pay->Data, sizeof(entt::entity));
                if (live && reg.valid(dropped) && dropped != e) {
                    uiEdPendingReparentChild_ = dropped;
                    uiEdPendingReparentTo_ = e;
                    uiEdPendingUnparent_ = false;
                }
            }
            if (const ImGuiPayload* pay = ImGui::AcceptDragDropPayload("UIED_NEW")) {
                int idx = 0;
                if (pay->DataSize == static_cast<int>(sizeof(int)))
                    std::memcpy(&idx, pay->Data, sizeof(int));
                if (live) {
                    uiEdPendingCreate_ = idx;
                    uiEdPendingCreateParent_ = e;
                    uiEdPendingCreatePlace_ = false;
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::BeginPopupContextItem("##uiedrow")) {
            selected_ = e;
            ImGui::TextDisabled("%s", label);
            ImGui::Separator();
            if (panel && ImGui::MenuItem("Show this screen on the canvas")) {
                for (const entt::entity other : reg.view<UIPanel>()) {
                    const UIDocMember* m = reg.try_get<UIDocMember>(other);
                    if (m && m->doc == activeDoc_) reg.remove<EditorUIShow>(other);
                }
                reg.emplace_or_replace<EditorUIShow>(e);
                uiEdShownScreen_[UIEdDocKey(engine.Documents())] = panel->name;
            }
            if (el && ImGui::MenuItem(el->visible ? "Hide (visible = false)"
                                                  : "Show (visible = true)",
                                      nullptr, false, live)) {
                PushUndo(engine);
                reg.get<UIElement>(e).visible = !el->visible;
                MarkDocumentDirty(engine, e);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Unparent", nullptr, false, live && reg.all_of<Parent>(e))) {
                uiEdPendingReparentChild_ = e;
                uiEdPendingUnparent_ = true;
            }
            ImGui::Separator();
            DrawUIEditorTools(engine, /*compact*/ false);
            ImGui::EndPopup();
        }
        if (open) {
            if (hasKids)
                for (const entt::entity c : kit->second) self(self, c, depth + 1);
            ImGui::TreePop();
        }
        ImGui::PopID();
    };

    if (roots.empty()) {
        ImGui::TextDisabled("Empty document - drag something out of the palette.");
    } else {
        for (const entt::entity e : roots) row(row, e, 0);
    }
}

// ============================================================================
// --test-uieditor - the AUTHORING contract, headless.
//
// Everything a mouse gesture in this panel ends up calling, minus the mouse. The
// two things worth naming, because no other test can reach them:
//
//   * Z-ORDER SURVIVES A SAVE. Draw order is the order the layout walk visits
//     siblings, i.e. entt POOL order, and no file records that directly - a
//     document reproduces it because InstantiateDocument creates in document
//     order. So the ONLY honest test of a reorder is a round trip: reorder,
//     capture, save to a string, load it back into a fresh scene, lay it out, and
//     compare the order. That also makes the test independent of which direction
//     EnTT happens to iterate a storage, which is exactly the assumption a
//     hand-derived check would bake in and then silently invert on an upgrade.
//   * CUT IS NOT DESTRUCTIVE. The B11 bug made a document-rooted subtree copy
//     serialize to `{"version":1,"entities":[]}` - a NON-EMPTY string, so every
//     `frag.empty()` guard passed and Cut deleted content it had not copied. The
//     UI editor's Cut is that same path, so it is pinned here again from the
//     panel's side: the fragment must contain the elements.
// ============================================================================
bool Editor::UIEditorSelfTest() {
    bool ok = true;
    const auto expect = [&ok](bool cond, const char* what) {
        if (!cond) {
            HBE_ERROR("UIEditorSelfTest FAIL: {}", what);
            ok = false;
        }
    };

    // A document laid out at a fixed canvas. `preload=false` + a null Renderer is
    // what makes this headless: fonts and textures are the only things that need a
    // device, and layout order does not depend on either.
    ui::CanvasConfig cfg;
    cfg.mode = ui::ScaleMode::Stretch;
    cfg.refWidth = 1920.0f;
    cfg.refHeight = 1080.0f;
    const glm::vec2 target(1920.0f, 1080.0f);

    // --- (a) every palette recipe creates DOCUMENT content, tracked in order ---
    {
        Scene scene;
        Editor ed;
        ui::DocumentSet docs;
        ui::DocData fresh;
        fresh.canvas = cfg;
        const ui::DocHandle doc =
            docs.OpenFromData(scene, nullptr, fresh, "Test.hbui", true, /*preload*/ false);
        expect(doc != 0, "(a) an empty document must open");
        ed.activeDoc_ = doc;

        // The REAL creation path minus the Engine wrapper: UIParentForNew resolves
        // the parent exactly as the panel does, UIBuildRecipe builds the entity
        // exactly as the panel does. Only PushUndo / selection / the dirty flag are
        // left out, and those are ImGui-side bookkeeping.
        int count = 0;
        const UICreateDesc* cat = UICreateCatalog(count);
        expect(count == static_cast<int>(UICreate::Count),
               "(a) the catalog must describe every recipe");
        auto& reg = scene.Registry();
        std::vector<entt::entity> made;
        for (int i = 0; i < count; ++i) {
            const entt::entity parent =
                cat[i].what == UICreate::Canvas
                    ? entt::null
                    : ed.UIParentForNew(scene, doc, entt::null);
            const entt::entity e =
                UIBuildRecipe(scene, docs, doc, cat[i].what, parent);
            expect(e != entt::null, "(a) every recipe must produce an entity");
            if (e == entt::null) continue;
            made.push_back(e);
            // Each recipe's promised components, so a future edit to the catalog
            // cannot quietly stop attaching one.
            switch (cat[i].what) {
                case UICreate::Canvas:
                    expect(reg.all_of<UICanvas>(e) && !reg.all_of<UIElement>(e),
                           "(a) Canvas must be a UICanvas root and nothing else");
                    break;
                case UICreate::Screen:
                    // Both, and that is load-bearing: the layout walk starts at
                    // UIElement roots and evaluates UIPanel::active ON the walked
                    // entity, so a UIPanel with no UIElement is never walked and
                    // `active` would do nothing at all.
                    expect(reg.all_of<UIElement>(e) && reg.all_of<UIPanel>(e),
                           "(a) Screen must carry BOTH UIElement and UIPanel");
                    expect(reg.get<UIElement>(e).fullscreen,
                           "(a) a Screen must cover its canvas");
                    break;
                case UICreate::VerticalGroup:
                case UICreate::HorizontalGroup:
                case UICreate::GridGroup:
                    expect(reg.all_of<UIElement>(e) && reg.all_of<UILayoutGroup>(e),
                           "(a) a Layout Group recipe must attach UILayoutGroup");
                    break;
                case UICreate::FadeGroup:
                    expect(reg.all_of<UIElement>(e) && reg.all_of<UICanvasGroup>(e),
                           "(a) Fade Group must attach UICanvasGroup");
                    break;
                case UICreate::ProgressWheel:
                    expect(reg.all_of<UIElement>(e) && reg.get<UIElement>(e).radial,
                           "(a) Progress Wheel must be the radial variant");
                    break;
                default:
                    expect(reg.all_of<UIElement>(e),
                           "(a) an element recipe must attach UIElement");
                    break;
            }
        }
        expect(reg.view<UILayoutGroup>().size() == 3,
               "(a) exactly the three Layout Group recipes attach one");
        const ui::DocumentInstance* inst = docs.Get(doc);
        expect(inst != nullptr, "(a) the instance must exist");
        for (const entt::entity e : made) {
            const UIDocMember* m = reg.try_get<UIDocMember>(e);
            expect(m != nullptr && m->doc == doc,
                   "(a) every created entity must carry UIDocMember for this document");
            expect(inst && std::find(inst->entities.begin(), inst->entities.end(), e) !=
                               inst->entities.end(),
                   "(a) every created entity must appear in the document ORDER LIST "
                   "(the DocumentSet::Track contract)");
        }
        // ...and none of it is world content, so the document save cannot refuse it.
        std::string why;
        expect(AuditDocumentForWorldContent(scene, doc, why),
               "(a) palette content must pass the document audit");
        // A scene saved alongside must contain none of it.
        const std::string sceneJson = scene::SaveSceneToString(scene);
        expect(sceneJson.find("\"ui\"") == std::string::npos &&
                   sceneJson.find("\"uiCanvas\"") == std::string::npos,
               "(a) document content leaked into the scene string");
    }

    // --- (b) no active document => no creation ---------------------------------
    {
        Scene scene;
        Editor ed;
        ed.activeDoc_ = 0;
        // The guard is the FIRST line of CreateUIElementInDocument and needs no
        // Engine to observe: with no document there is nothing to parent to either.
        expect(ed.UIParentForNew(scene, 0, entt::null) == entt::null,
               "(b) with no document there is no parent to create under");
        expect(scene.Registry().view<UIElement>().size() == 0,
               "(b) the guard must not have created anything");
    }

    // --- (c) Z-ORDER: a sibling swap survives capture -> save -> reload ---------
    {
        Scene scene;
        ui::DocumentSet docs;
        ui::DocData fresh;
        fresh.canvas = cfg;
        const ui::DocHandle doc =
            docs.OpenFromData(scene, nullptr, fresh, "Z.hbui", true, /*preload*/ false);
        auto& reg = scene.Registry();
        // A screen with three overlapping children, so sibling order is the only
        // thing that decides which one is on top.
        const entt::entity screen = scene.CreateEntity("Screen");
        docs.Track(scene, doc, screen);
        {
            UIElement el;
            el.fullscreen = true;
            reg.emplace<UIElement>(screen, el);
            UIPanel p;
            p.name = "S";
            p.active = true; // no EditorUIShow needed: this is not the editor view
            reg.emplace<UIPanel>(screen, p);
        }
        std::vector<entt::entity> kids;
        for (int i = 0; i < 3; ++i) {
            const entt::entity e = scene.CreateEntity(std::string("K") + std::to_string(i));
            reg.emplace<Parent>(e, Parent{screen});
            docs.Track(scene, doc, e);
            UIElement el;
            // Distinct widths, so the round trip also proves each COMPONENT
            // travelled with its own entity through the pool swap - a swap that
            // moved packed entities without their components would reorder the
            // sizes instead of the draw order, and a names-only check would miss it.
            el.size = {400.0f + 10.0f * static_cast<f32>(i), 200.0f};
            reg.emplace<UIElement>(e, el);
            kids.push_back(e);
        }
        const auto widthOf = [&](Scene& s, const char* name) -> f32 {
            const entt::entity e = s.FindByName(name);
            if (e == entt::null || !s.Registry().all_of<UIElement>(e)) return -1.0f;
            return s.Registry().get<UIElement>(e).size.x;
        };
        expect(widthOf(scene, "K0") == 400.0f && widthOf(scene, "K1") == 410.0f &&
                   widthOf(scene, "K2") == 420.0f,
               "(c) the fixture's widths must start out matched to their names");
        // Draw order of the three, straight out of the layout.
        const auto drawOrderNames = [&](Scene& s, ui::DocHandle d) {
            std::vector<ui::LayoutItem> layout;
            ui::LayoutUI(s, target, cfg, layout);
            std::vector<std::string> names;
            for (const ui::LayoutItem& it : layout) {
                const UIDocMember* m = s.Registry().try_get<UIDocMember>(it.entity);
                if (!m || m->doc != d) continue;
                const Name* n = s.Registry().try_get<Name>(it.entity);
                if (n && n->value.size() == 2 && n->value[0] == 'K') names.push_back(n->value);
            }
            return names;
        };
        const std::vector<std::string> before = drawOrderNames(scene, doc);
        expect(before.size() == 3, "(c) all three children must lay out");

        // The REAL reorder primitive the Raise / Lower buttons call.
        const entt::entity a = kids[0], b = kids[1];
        UISwapOrder(scene, docs, a, b);
        expect(widthOf(scene, "K0") == 400.0f && widthOf(scene, "K1") == 410.0f &&
                   widthOf(scene, "K2") == 420.0f,
               "(c) a pool swap must carry each component WITH its own entity");
        const std::vector<std::string> after = drawOrderNames(scene, doc);
        expect(after.size() == 3 && after != before,
               "(c) a pool swap must change the DRAW order of two siblings");

        // ...and now the part only a round trip can prove.
        ui::DocData captured;
        ui::CaptureDocument(scene, doc, docs.Get(doc)->header, captured,
                            &docs.Get(doc)->entities);
        const std::string text = ui::SaveDocumentToString(captured);
        expect(!text.empty(), "(c) the document must serialize");
        ui::DocData reloaded;
        expect(ui::LoadDocumentFromString(text, reloaded),
               "(c) the serialized document must parse back");
        Scene fresh2;
        ui::DocumentSet docs2;
        const ui::DocHandle doc2 =
            docs2.OpenFromData(fresh2, nullptr, reloaded, "Z.hbui", true, /*preload*/ false);
        expect(doc2 != 0, "(c) the reloaded document must open");
        // UIPanel::active is runtime state and is never serialized, so re-activate
        // the screen the way the UIManager would before comparing.
        for (const entt::entity e : fresh2.Registry().view<UIPanel>())
            fresh2.Registry().get<UIPanel>(e).active = true;
        fresh2.BumpUIVersion();
        expect(widthOf(fresh2, "K0") == 400.0f && widthOf(fresh2, "K1") == 410.0f &&
                   widthOf(fresh2, "K2") == 420.0f,
               "(c) the reload must keep each element's own authored size");
        const std::vector<std::string> roundTripped = drawOrderNames(fresh2, doc2);
        expect(roundTripped == after,
               "(c) THE GATE: draw order after save+reload must match what the editor "
               "was showing (document entity order reproduces pool order)");

        // ...and the SAME gate through the UNDO/PLAY capture, which is a different
        // call site. Editor::CaptureSnapshot omitted the order argument, so every
        // z-order edit was silently reverted by the next undo (and by Play -> Stop)
        // even though Save had always been right. A pool permutation does not move
        // entity indices, so the index fallback cannot stand in for the order list.
        {
            // The REAL function CaptureSnapshot calls, not a lookalike.
            const std::string snapText =
                Editor::CaptureDocumentSnapshotJson(scene, *docs.Get(doc));
            ui::DocData back;
            expect(ui::LoadDocumentFromString(snapText, back),
                   "(c) the snapshot string must parse back");
            Scene fresh3;
            ui::DocumentSet docs3;
            const ui::DocHandle doc3 =
                docs3.OpenFromData(fresh3, nullptr, back, "Z.hbui", true, /*preload*/ false);
            expect(doc3 != 0, "(c) the snapshot must reopen");
            for (const entt::entity e : fresh3.Registry().view<UIPanel>())
                fresh3.Registry().get<UIPanel>(e).active = true;
            fresh3.BumpUIVersion();
            expect(drawOrderNames(fresh3, doc3) == after,
                   "(c) THE UNDO GATE: a z-order edit must survive the snapshot "
                   "capture/restore round trip, not just the save");
        }
    }

    // --- (d) reparent rules ----------------------------------------------------
    {
        Scene scene;
        Editor ed;
        ui::DocumentSet docs;
        ui::DocData fresh;
        fresh.canvas = cfg;
        const ui::DocHandle d1 =
            docs.OpenFromData(scene, nullptr, fresh, "A.hbui", true, /*preload*/ false);
        const ui::DocHandle d2 =
            docs.OpenFromData(scene, nullptr, fresh, "B.hbui", true, /*preload*/ false);
        expect(d1 != 0 && d2 != 0 && d1 != d2,
               "(d) two independent instances must open");
        auto& reg = scene.Registry();
        const auto mk = [&](ui::DocHandle d, const char* name) {
            const entt::entity e = scene.CreateEntity(name);
            docs.Track(scene, d, e);
            reg.emplace<UIElement>(e);
            return e;
        };
        const entt::entity a1 = mk(d1, "A1"), a2 = mk(d1, "A2"), b1 = mk(d2, "B1");
        const entt::entity world = scene.CreateEntity("Crate");
        reg.emplace<Transform>(world);

        ed.Reparent(scene, a2, a1); // inside one document: allowed
        expect(reg.all_of<Parent>(a2) && reg.get<Parent>(a2).entity == a1,
               "(d) a reparent inside one document must be applied");
        ed.Reparent(scene, a2, entt::null); // UNPARENT: always allowed
        expect(!reg.all_of<Parent>(a2), "(d) an unparent must be applied");
        expect(reg.all_of<UIDocMember>(a2) && reg.get<UIDocMember>(a2).doc == d1,
               "(d) an unparent must keep document membership");
        ed.uiDocError_.clear();
        ed.Reparent(scene, b1, a1); // ACROSS documents: refused
        expect(!reg.all_of<Parent>(b1),
               "(d) a cross-DOCUMENT reparent must be refused, not applied");
        expect(!ed.uiDocError_.empty(), "(d) the refusal must surface a message");
        ed.uiDocError_.clear();
        ed.Reparent(scene, a1, world); // document -> scene: refused
        expect(!reg.all_of<Parent>(a1),
               "(d) parenting document content under world content must be refused");
        expect(!ed.uiDocError_.empty(), "(d) that refusal must surface a message too");
    }

    // --- (e) Cut copies before it deletes --------------------------------------
    {
        Scene scene;
        Editor ed;
        ui::DocumentSet docs;
        ui::DocData fresh;
        fresh.canvas = cfg;
        const ui::DocHandle doc =
            docs.OpenFromData(scene, nullptr, fresh, "C.hbui", true, /*preload*/ false);
        auto& reg = scene.Registry();
        const entt::entity root = scene.CreateEntity("Group");
        docs.Track(scene, doc, root);
        reg.emplace<UIElement>(root);
        const entt::entity kid = scene.CreateEntity("Play");
        reg.emplace<Parent>(kid, Parent{root});
        docs.Track(scene, doc, kid);
        {
            UIElement el;
            el.type = UIElement::Type::Button;
            el.action = "play"; // the verb string that must survive verbatim
            reg.emplace<UIElement>(kid, el);
        }
        ed.selected_ = root;
        ed.CopySelection(scene);
        expect(!ed.clipboard_.empty(), "(e) Cut/Copy produced an empty fragment");
        scene::SceneData frag;
        expect(scene::ParseSceneString(ed.clipboard_, frag) && frag.entities.size() == 2,
               "(e) THE REGRESSION: a document-rooted subtree copy must carry both "
               "entities, not an empty-but-non-empty string");
        expect(ed.clipboard_.find("\"play\"") != std::string::npos,
               "(e) the button `action` must survive the copy verbatim");
        expect(ed.clipboardFromDoc_,
               "(e) the copy must be remembered as coming FROM a document, so a paste "
               "goes back into one");
        // ...and the delete half really removes the subtree.
        ed.DestroyRecursive(scene, root);
        expect(!reg.valid(root) && !reg.valid(kid),
               "(e) the delete half must remove the whole subtree");
    }

    if (ok) HBE_INFO("UIEditorSelfTest PASS: palette creation, the document guard, "
                     "z-order round trip, reparent rules and the Cut fragment all hold.");
    return ok;
}

} // namespace hbe
