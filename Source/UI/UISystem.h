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
#include "UI/FontAtlas.h" // GlyphQuad (per-element glyph-layout cache)

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <filesystem>
#include <string>
#include <unordered_map>
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

// Global UI scale (P6): >1 enlarges the whole UI uniformly (accessibility / display
// density / Steam Deck), clamped to [0.25, 4]. Applied inside EffectiveCanvas, so it
// affects every canvas + layout path. The engine drives it from UserSettings; the
// editor authoring canvas and headless tests leave it at 1.0.
void SetUIScale(f32 scale);
f32 GetUIScale();

// P9.2 WidgetVTable (D3): route UI emit through the per-type registry instead of the
// legacy switch. OFF by default (the shipped runtime keeps the legacy path until the
// byte-identity gate signs off); --test-uivtable and --uivtable flip it. See
// docs/Design-UIWidgetRegistry.md.
void SetUIUseVTable(bool on);
bool GetUIUseVTable();

// P9.2 WidgetVTable dispatch shared with UIFocus (keyboard/gamepad). `WidgetIsFocusable`
// is the single source of truth for which types take focus; `WidgetNav` adjusts a value
// widget on left/right (dir 2/3) and returns true if it consumed the direction;
// `WidgetActivate` runs Enter/A activation and returns true when the widget wants a
// text-edit session (TextInput). Each honours GetUIUseVTable(). (Element-typed, not
// Type-typed: this header only forward-declares UIElement, not its nested enum.)
bool WidgetIsFocusable(const UIElement& el);
bool WidgetNav(UIElement& el, int dir);
bool WidgetActivate(UIElement& el);

// P9 Tab switching: apply any tab clicks THIS frame (an element with a `tabTarget` shows
// that content and hides the other targets in its `tabGroup`). Call once per frame after
// interaction/navigation. Composable - no new widget type. See UIElement::tabGroup.
void ProcessTabs(Scene& scene);

// The frozen-vertex parity gate: builds an in-code corpus of every widget type + a
// state cross-product, emits it with the legacy switch and again with the vtable, and
// asserts the two rhi::UIVertex streams are byte-identical (plus a self-consistency
// re-run). Runs in a real GPU session but needs NO project (own corpus). Returns true
// on PASS. Defined in WidgetVTableTest.cpp.
bool WidgetVTableSelfTest(Scene& scene, Renderer& renderer);

// P9.2 interact parity: runs ApplyPointerToElement over every interactive element in
// `scene` both ways (legacy switch vs vtable onPointer) and asserts identical flag
// mutations. Headless (no emit); called by WidgetVTableSelfTest. Defined in UISystem.cpp.
bool WidgetPointerParitySelfTest(Scene& scene, glm::vec2 target, const CanvasConfig& cfg);

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

// Mirrors UILayoutGroup::Kind. Duplicated rather than included because this
// header is below Scene/Components.h in the include order; the two are pinned
// together by a static_assert in UISystem.cpp.
enum class LayoutFlow : u8 { Vertical = 0, Horizontal = 1, Grid = 2 };

// WHO OWNS AN ELEMENT'S RECT. A UILayoutGroup positions its non-stretch direct
// children into forced slots, which OVERWRITE the rect the child's own
// RectTransform computed (see LayoutUIImpl's `forced` argument). So a direct
// manipulation tool that writes offset/size on such a child produces an edit the
// very next layout discards - and worse, a PARTIAL one, because a Vertical /
// Horizontal slot is built from the child's own natural SIZE while its POSITION
// is the group's cursor. The editor asks this before it offers a handle.
//
// The classification is deliberately the same test layout itself applies (the
// `stretch` escape class: fullscreen, or anchorMin != anchorMax on either axis),
// and --test-uisolve cross-checks this function against what LayoutUI actually
// did rather than against a copy of the rule.
struct LayoutOwnership {
    // The parent group places this element: its `offset` is ignored on layout,
    // so a MOVE is a no-op the author would watch snap back.
    bool positionOwned = false;
    // ...and dictates its size too (a Grid slot is exactly cellSize), so a
    // RESIZE is a no-op as well. False for Vertical/Horizontal, whose slots are
    // built from the child's own natural size - there a resize is honoured and
    // only the position keeps re-flowing.
    bool sizeOwned = false;
    entt::entity group = entt::null; // the owning UILayoutGroup entity
    LayoutFlow flow = LayoutFlow::Vertical;
    // Set when `e` ITSELF is a fitContent UILayoutGroup: layout rewrites its own
    // x1/y1 from its children after they lay out, so its far edges are not
    // authorable either (an independent condition from the three above).
    bool selfFitsContent = false;
};
LayoutOwnership LayoutGroupOwnership(const Scene& scene, entt::entity e);

// One laid-out element. `canvas` is the effective canvas size of the tree the
// element belongs to (needed to map its rect to NDC/screen).
struct LayoutItem {
    entt::entity entity = entt::null;
    Rect rect;        // resolved, canvas units
    Rect parentRect;  // what the element laid out inside (editor dragging)
    // The element's Parent (entt::null = a tree root). `parentRect` says WHAT it
    // laid out inside; this says WHO owns it - which the dedicated UI editor needs
    // to detect a UILayoutGroup-managed child (whose rect layout overwrites) and
    // to walk up the ancestry on Alt+click. Taken from the Parent component, i.e.
    // the same source BuildChildrenMap walks, so the two agree by construction.
    entt::entity parentEntity = entt::null;
    glm::vec2 canvas{0.0f};
    // The UICanvas entity this element laid out under (entt::null = legacy /
    // canvas-less). World-space canvases route their items to a texture batch
    // instead of the screen overlay.
    entt::entity canvasEntity = entt::null;
    // Clip rect inherited from ancestor ScrollViews (canvas units; the
    // intersection when nested). Emission converts it to the per-vertex NDC
    // clip; interaction rejects pointers outside it.
    Rect clip;
    bool hasClip = false;
    // Inherited UICanvasGroup opacity (product down the ancestry); 1 = opaque.
    f32 groupOpacity = 1.0f;
    // Inherited UICanvasGroup interactivity: false = this item ignores the
    // pointer (a faded-out / disabled subtree).
    bool groupInteractive = true;
};

// Per-frame UI statistics (verification: idle menus should sit near zero).
struct UIFrameStats {
    u32 elements = 0;    // layout items emitted this frame
    u32 verts = 0;       // overlay vertices built this frame
    u32 textLayouts = 0; // glyph re-layouts (cache misses) this frame
    u32 mapRebuilds = 0; // children-map rebuilds (structure changed)
    // CPU profiling scopes (P7). microseconds, this frame. shapeUs is a SUBSET of
    // emitUs (text shaping happens inside the emit walk); layoutUs + emitUs is the
    // whole CPU UI-build cost (interaction/animation are timed at the engine level).
    f32 layoutUs = 0.0f; // the layout walk (LayoutUI)
    f32 emitUs = 0.0f;   // vertex emission (BuildVerticesImpl), includes shapeUs
    f32 shapeUs = 0.0f;  // text shaping on cache misses
};

// Persistent UI processing context (one per scene lifetime, owned by the
// Engine). Caches the parent->children map against Scene::UIStructureVersion,
// per-element glyph layouts, the interaction touch-list, and world-surface
// inverse transforms - so a static menu does near-zero per-frame work.
// The layout built by BuildVertices is REUSED by next frame's interaction
// (interaction always effectively ran against 1-frame-old rects, since
// animations run after it).
struct UIContext {
    std::vector<LayoutItem> layout; // built once per frame (BuildVertices)
    std::unordered_map<u32, std::vector<entt::entity>> children; // persistent
    u64 seenStructureVersion = 0;

    // Per-element glyph-layout cache. Key mixes the entity and a per-element
    // text-slot counter (Selector emits one text per option); `hash` guards
    // against text/font/size changes.
    struct TextCacheEntry {
        u64 hash = 0;
        std::vector<GlyphQuad> quads;
        f32 w = 0.0f, h = 0.0f;
    };
    std::unordered_map<u64, TextCacheEntry> textCache;

    // Elements whose interaction flags were touched last frame (flag clearing
    // iterates this instead of every UIElement in the registry).
    std::vector<entt::entity> interactives;

    // World-surface transform cache: inverse recomputed only when the world
    // matrix actually changes (ui::PickWorldPage). `stamp` is the pick call that
    // last touched the entry - entries for pages that went away are dropped by
    // stamp rather than by wiping the whole map.
    struct SurfaceXf {
        glm::mat4 world{1.0f};
        glm::mat4 inv{1.0f};
        u64 stamp = 0;
    };
    std::unordered_map<u32, SurfaceXf> surfaceInv;
    u64 surfaceInvStamp = 0;

    // Keyboard/gamepad focus + text editing (driven by ui::UpdateNavigation in
    // UIFocus.cpp; BuildVertices draws the focus ring and the caret from here).
    entt::entity focused = entt::null; // element carrying the focus ring
    bool focusVisible = false;         // ring shown after key/pad nav; mouse hides it
    entt::entity editing = entt::null; // TextInput with an active edit session
    std::string preEditText;           // Escape restores this snapshot
    int caretPos = 0;                  // caret index into the edit buffer
    f32 caretBlink = 0.0f;             // seconds since edit start (blink phase)
    f32 navRepeat = 0.0f;              // held-direction auto-repeat countdown
    int navHeldDir = -1;               // 0=up 1=down 2=left 3=right (-1 = none)

    // Tooltip (P9): the pointer pass records the topmost element under the cursor that
    // carries a non-empty `tooltip` into `tooltipCandidate` (independent of the
    // interaction gate, so a DISABLED widget can still explain itself). UpdateNavigation
    // (which has dt) advances `tooltipTime` while the candidate holds steady and resets
    // it when the candidate changes; once it passes the element's `tooltipDelay`,
    // BuildVertices emits the popup below `tooltipHover`.
    entt::entity tooltipCandidate = entt::null; // under the cursor THIS frame
    entt::entity tooltipHover = entt::null;      // what the timer is counting on
    f32 tooltipTime = 0.0f;

    // P9.4 data-binding: the model globalRev applied by the last ui::ResolveBindings, so an
    // unchanged model fast-skips the per-element bind walk.
    u64 lastBindGlobalRev = 0;

    UIFrameStats stats;
};

// Is this UICanvas reachable through its ANCESTORS? A canvas tree is walked from
// the canvas itself, so the UIPanel::active gate and the UIElement::visible gate
// that guard a normal subtree never ran on anything ABOVE it: a world-space page
// authored under the `Settings` screen root kept laying out, kept rendering and
// kept taking picks after the screen was popped. Every consumer of a canvas -
// layout, the world-page pick, the world-surface pass - asks this first.
// `editorForced` is `scene.EditorView() && scene.UIAuthoringView()`, which lets a
// single EditorUIShow-tagged panel override its own inactive state for authoring.
bool CanvasAncestryActive(const entt::registry& reg, entt::entity canvas,
                          bool editorForced);

// Actions the ENGINE resolves GLOBALLY by name over the whole registry - the ones
// a screen split can break by duplicating. ONE definition: the boot audit, the
// --test-uiscreens gate and the migrator's collision report all call this, so
// adding a verb cannot silently weaken one of the three.
bool IsGlobalAction(const std::string& action);

// Lays out every visible UI tree in draw order: legacy (canvas-less) elements
// first on the project canvas, then UICanvas trees by ascending sortOrder
// (depth-first, parents before children).
void LayoutUI(Scene& scene, glm::vec2 targetSize, const CanvasConfig& legacyConfig,
              std::vector<LayoutItem>& out);
// Cached variant: rebuilds the children map only when the scene's UI structure
// version changed; fills ctx.layout.
void LayoutUI(Scene& scene, glm::vec2 targetSize, const CanvasConfig& legacyConfig,
              UIContext& ctx);

// Per-frame pointer(s): the screen pointer plus, for world-space canvases, the
// ray-picked pointer in each canvas's pixel space (absent = the ray missed that
// page; its widgets simply aren't hovered). Filled by ui::ComputeWorldPointers.
struct PointerState {
    // (There is no `screenNorm` here. It existed as a write-only field that three
    // call sites filled and nothing ever read - and the Engine filled it with the
    // WORLD pointer under a name that says screen, so the first reader would have
    // got the reticle instead of the cursor. The screen pointer is already an
    // explicit argument to UpdateInteraction; a second, silently-wrong copy of it
    // is worse than none.)
    // At most ONE entry: the nearest un-occluded page under the ray. (It stays a
    // map so every consumer keeps its "look up my canvas" shape.)
    std::unordered_map<u32, glm::vec2> worldCanvasPx; // canvas entity -> canvas px
    // RETICLE MODE. While the cursor is locked (first-person gameplay) the pointer
    // is screen centre and the press verb is the "Interact" ACTION, not the left
    // mouse button - LMB is fire. When `worldButtonOverride` is set, world-page
    // widgets use worldPressed/worldDown instead of the mouse; screen canvases are
    // unaffected (they get no pointer at all in that mode).
    bool worldButtonOverride = false;
    bool worldPressed = false;
    bool worldDown = false;
};

// `pointerNorm` = pointer in normalized target coords (0..1; negative = none).
// `pointers` adds world-canvas pointers (null = screen canvases only; world
// widgets stay inert - the old behavior, compile-enforced).
void UpdateInteraction(Scene& scene, const Input& input, glm::vec2 pointerNorm,
                       glm::vec2 targetSize, const CanvasConfig& config,
                       const PointerState* pointers = nullptr);
// Cached variant: hit-tests against ctx.layout (LAST frame's layout - the
// effective behavior today, since animations run after interaction) and clears
// flags via ctx.interactives instead of a full registry scan.
void UpdateInteraction(Scene& scene, const Input& input, glm::vec2 pointerNorm,
                       const PointerState* pointers, UIContext& ctx);

// True when the pointer is currently over (or holding) an interactive widget -
// i.e. this click belongs to the UI, not to the world.
//
// Call AFTER UpdateInteraction, which is what fills `ctx.interactives` for the
// frame. Cheap: walks only the elements the hit-test actually touched, not the
// registry. Exists because gameplay's "player fires on left-mouse" branch had no
// way to ask, so every dialogue-choice / pause / HUD click also discharged the
// player's weapon.
bool PointerOverInteractive(Scene& scene, const UIContext& ctx);

// One world-space canvas's triangles for this frame, rendered into its texture
// (the lit page quad in the scene samples it).
struct WorldUIBatch {
    entt::entity canvas = entt::null;
    rhi::TextureHandle target;
    std::vector<rhi::UIVertex> verts;
};

// Emits the overlay triangles into `out`. When `worldOut` is given, visible
// world-space canvases route their triangles into per-canvas batches there
// instead (same layout + emit math; only the destination differs). Callers
// that pass nullptr (boot splash) simply skip world canvases.
void BuildVertices(Scene& scene, Renderer& renderer,
                   const std::filesystem::path& assetsDir, glm::vec2 targetSize,
                   const CanvasConfig& config, std::vector<rhi::UIVertex>& out,
                   std::vector<WorldUIBatch>* worldOut = nullptr);
// Cached variant: the one layout walk per frame (children map cached against
// the structure version) + the per-element glyph cache. `ctx.layout` is left
// populated for next frame's UpdateInteraction.
void BuildVertices(Scene& scene, Renderer& renderer,
                   const std::filesystem::path& assetsDir, glm::vec2 targetSize,
                   const CanvasConfig& config, std::vector<rhi::UIVertex>& out,
                   std::vector<WorldUIBatch>* worldOut, UIContext& ctx);

// EDITOR AUTHORING BUILD: lays out and emits exactly ONE open `.hbui` document's
// entities into a SINGLE vertex list at `targetSize`, and fills `outLayout` with
// the same LayoutItem set (the editor hit-tests and draws handles from it).
//
// Two differences from the runtime build, and both are why this entry has to
// exist rather than the editor reusing the world-space path:
//   1. `doc` filters the layout to that document's TREE ROOTS (a document is a
//      set of whole subtrees, so filtering roots filters everything below them).
//   2. Routing by UICanvas::worldSpace is BYPASSED - every item lands in `out`.
//      The reference project's menu has ZERO UICanvas entities (all four panels
//      are canvas-less roots, see UIDocument.h decision 2), so the world-space
//      batch path can never draw it; and a document that DOES contain a
//      world-space canvas must still show up on the authoring surface.
// Everything else - layout, emission, skinning, text, clips, transforms - is the
// SHIPPED code. That is the point: the authoring canvas cannot drift from the
// game because there is no second renderer to drift.
//
// `docConfig` is the document's own header canvas (the basis for its canvas-less
// roots). Deliberately does NOT take a UIContext: it must not perturb the
// runtime's per-frame caches or stats, since it runs as an EXTRA pass inside the
// same frame.
void BuildDocumentVertices(Scene& scene, Renderer& renderer,
                           const std::filesystem::path& assetsDir,
                           glm::vec2 targetSize, const CanvasConfig& docConfig,
                           u32 doc, std::vector<rhi::UIVertex>& out,
                           std::vector<LayoutItem>& outLayout);

// --test-uisolve: the DIRECT-MANIPULATION math gate for the dedicated `.hbui`
// editor. Pure CPU, no GPU, no window, no files. Proves that
//   * SolveElementFromRect is the exact inverse of ComputeElementRect over a
//     fuzz of anchors / pivots / sizes / offsets / parent rects,
//   * a solve NEVER touches anchors or the pivot (so a stretched element stays
//     stretched and a centre-anchored one stays centred),
//   * `fullscreen` is a documented no-op on both sides,
//   * re-anchoring by "write the anchors, then re-solve the SAME rect" leaves
//     the resolved rect bit-stable - the trick the anchor widget is built on,
//   * the snap helper is idempotent,
//   * LayoutGroupOwnership agrees with what LayoutUI ACTUALLY did (managed <=>
//     the laid-out rect is the group's forced slot and not the element's own
//     ComputeElementRect), including the stretch-class escapee and Grid.
bool ManipulationSelfTest();

// Drops cached texture resolutions (call when assets change / project switches).
void ClearTextureCache(Scene* scene);

// Eagerly bakes fonts + loads every texture the scene's UI references (elements,
// sprite frames, world text). Called at scene Instantiate so the FIRST visible
// frame has everything - no blank-text frame, no white-quad flash, no disk-I/O
// hitch inside the frame loop.
void PreloadUIAssets(Scene& scene, Renderer& renderer,
                     const std::filesystem::path& assetsDir);

// P5 (D2): find the first UIElement carrying the authored `id` (empty id never
// matches). Ids are unique within a document by convention; this returns the first
// match in entity-pool order across the whole scene, entt::null if none. The stable
// handle future data-binding / localization / animation-targeting reference elements
// by, instead of the non-unique name/action.
entt::entity FindElementById(Scene& scene, const std::string& id);

// P8 modal scope: the topmost SHOWN modal UIPanel (entt::null = none). Only a modal
// with a laid-out descendant in `layout` qualifies (so an active-but-unshown modal
// never locks the UI); among several, the topmost in draw order wins. While one is
// active, pointer hit-testing AND focus navigation are trapped to its subtree.
entt::entity ActiveModalPanel(Scene& scene, const std::vector<LayoutItem>& layout);
// True if `e` IS `ancestor` or a descendant (Parent-chain walk, depth-capped). An
// entt::null `ancestor` returns true (no modal = everything eligible).
bool IsDescendantOf(const entt::registry& reg, entt::entity e, entt::entity ancestor);

// P5 dirty-flag FOUNDATION. MarkElementDirty flags an element's layout/style dirty
// (call after mutating its geometry or style); MarkAllDirty flags every UI element.
// Today the full layout/emit walk still runs and these are advisory - P11's
// incremental layout is the CONSUMER that will skip clean elements. Kept as a small,
// stable API now so producers can start marking correctly ahead of that.
void MarkElementDirty(Scene& scene, entt::entity e, bool layout = true,
                      bool style = false);
void MarkAllDirty(Scene& scene);

} // namespace ui
} // namespace hbe
