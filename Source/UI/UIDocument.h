// UI/UIDocument.h - the `.hbui` UI DOCUMENT asset.
//
// A `.hbui` is a DOCUMENT, not a scene. It parses into `ui::DocData`, which has
// no field for a mesh, a rigid body, a SceneLayer, a SceneSource, a guid or a
// stream tag - so a UI document literally cannot carry world content, and a
// scene cannot carry a document. The separation is a property of the TYPES, not
// a convention somebody has to remember.
//
// STATUS (P3 of docs/Design-TagStreaming.md): SHIPPED. The engine boots its
// splash and its resident menu layer from documents (ProjectSettings::
// bootDocument / uiDocument), the editor authors them, and the two Replace
// sweeps spare them. P2 built the format, the loader and the migrator.
//
// FILE SHAPE
// ----------
//   {
//     "version": 1,
//     "kind": "uidoc",
//     "canvas": { "scaleMode": 1, "refWidth": 1920, "refHeight": 1080 },
//     "ambientIntensity": 0.8395,
//     "exposure": 1.25,
//     "post": { ...rhi::PostSettings, PostToJson format... },
//     "entities": [
//       { "name": "MainMenu", "uiPanel": {...} },
//       { "name": "Title", "parent": 0, "ui": {...} }
//     ]
//   }
//
// Per-entity keys, and ONLY these: `name`, `parent` (index into `entities`),
// `transform`, `ui`, `uiCanvas`, `uiAnimator`, `uiPanel`, `uiLayoutGroup`,
// `uiCanvasGroup`. Every sub-object is byte-identical to what a `.hbscene`
// stores, because both go through UI/UIDocumentJson.h.
//
// FOUR DECISIONS THAT ARE NOT OBVIOUS, AND WHY
// --------------------------------------------
// 1. `post` IS MANDATORY, and `ambientIntensity`/`exposure` ride with it.
//    The UI layer loads ADDITIVELY, so it deliberately never applies a scene
//    environment - instead Engine captures the UI file's post block into
//    `uiScenePost_` and `ApplyMenuPost()` replays it whenever the menu is up.
//    Drop the block and every menu silently re-renders with the LEVEL's look.
//    In the reference project the two differ in 20 of 63 post keys (auto-
//    exposure and fog on in the level, off in the menu), so this is not
//    hypothetical. SaveDocument therefore always writes `post`.
//    SCOPE, precisely: `post` is the load-bearing field - Engine reads it and
//    ApplyMenuPost replays it. `ambientIntensity` and `exposure` are CARRIED
//    (parsed, captured, round-tripped byte-stably, and preserved from the source
//    scene by the migrator) but nothing applies them today: ApplyMenuPost
//    restores `Environment().post` only, and scene::SetupEnvironment stamps the
//    project's ambient over anything a UI load could have set. They are in the
//    format so a document is self-describing and so wiring them up later is not
//    a format break - do not read this as "the menu's ambient comes from here".
//    Author the block with the UI Document panel's "Capture the live look"
//    button; there is deliberately no separate post editor for documents.
//
// 2. `canvas` is the document's OWN CanvasConfig, and it is the fallback for
//    CANVAS-LESS roots only. Elements under a `uiCanvas` entity use that
//    component's fields, exactly as in a scene. This exists because the
//    reference project's menu has ZERO UICanvas entities - all four panels are
//    canvas-less roots laying out against the project-wide BuildSettings - so a
//    document that did not carry the config would not be self-describing. The
//    migrator seeds it from the project's BuildSettings and does NOT synthesise
//    a UICanvas entity: doing so would move those entities from the legacy walk
//    to the canvas walk, changing draw order and world-space routing.
//
// 3. A `.hbui` entity carries NO `guid`, ever. This is the same deliberate rule
//    `.hbprefab` follows (BuildSubtreeJson erases the key), and for the same
//    reason: a document is an instantiable TEMPLATE addressed by UIPanel::name
//    and UIElement::action, never by identity. Nothing keys persistent state off
//    a UI entity - `world::` admits only Interactable/TriggerVolume/Health, and
//    document entities are excluded from `.hbsave` snapshots - and a document
//    must remain openable more than once (two world-space pages showing the same
//    file) without two live entities claiming one guid. `ui::ConvertSceneToDocument`
//    therefore STRIPS `guid` from every entity it lifts. The source `.hbscene`
//    keeps its guids untouched; only the document copy is guid-free.
//
// 4. `worldText` is NOT a document key. WorldText is world-space 3D text placed
//    by a Transform and drawn through the particle pass - it is level signage,
//    not screen UI, and it only ever shared a code neighbourhood with the UI
//    blocks. Sweeping it into `.hbui` would yank 3D signage out of the level it
//    belongs to (and, once P3 spares documents from the Replace sweep, leave it
//    hanging over gameplay). It stays in `.hbscene`. CONSEQUENCE FOR P3: the
//    "UI component" set that the creation-time guard and the SaveScene refusal
//    police must be the six document components, NOT seven - otherwise a level
//    with a 3D sign becomes unsaveable.
#pragma once

#include "Core/Types.h"
#include "Scene/Components.h" // ONE-WAY: DocEntity holds UI components by value.
#include "UI/UISystem.h"      // CanvasConfig

#include <entt/entt.hpp> // entt::entity (a live document owns its entities)

#include <filesystem>
#include <string>
#include <vector>

namespace hbe {
class Scene;
class Renderer;
}

namespace hbe::ui {

// Bumped only for a BREAKING shape change; readers use .value() defaults for
// additive keys, exactly like the scene reader.
inline constexpr int kDocVersion = 1;
// Header discriminator. Deliberately "uidoc" and not "ui": SceneKind stringified
// to "ui" until it was deleted, and a discriminator that collides with a live
// enum value during a migration window is a trap for nothing.
inline constexpr const char* kDocKind = "uidoc";

// Handle to an OPEN document instance (P3's DocumentSet mints these). 0 = none.
// Components.h stores this as a bare u32 in UIDocMember rather than including
// this header - see B10.
using DocHandle = u32;

// One entity in a document. Compare EntityData's ~65 fields: there is no field
// here that could hold world content.
struct DocEntity {
    std::string name;
    int parent = -1; // index into DocData::entities; -1 = root
    // Screen-space UI ignores Transform (layout is anchors + offsets), but a
    // world-space UICanvas is MOUNTED by its Transform. Preserved whenever the
    // source has one - dropping it is lossy and buys nothing.
    bool hasTransform = false;
    Transform transform;
    bool hasElement = false;
    UIElement element;
    bool hasCanvas = false;
    UICanvas canvas;
    bool hasAnimator = false;
    UIAnimator animator;
    bool hasPanel = false;
    UIPanel panel;
    bool hasLayout = false;
    UILayoutGroup layout;
    bool hasGroup = false;
    UICanvasGroup group;
};

struct DocData {
    std::vector<DocEntity> entities;
    CanvasConfig canvas;    // fallback for canvas-less roots (see decision 2)
    rhi::PostSettings post; // MANDATORY on disk (see decision 1)
    f32 ambientIntensity = 1.0f;
    f32 exposure = 1.0f;
};

// The six component keys a document admits. `worldText` is deliberately absent
// (decision 4). Shared with the migrator and, in P3, with the paste guard and
// the SaveScene refusal so those lists cannot drift from the format.
const std::vector<std::string>& DocumentComponentKeys();

// --- Load / Save --------------------------------------------------------------
// Reads a `.hbui` THROUGH THE VFS, so it works out of a mounted .uap in a
// shipped build. Never std::ifstream: `.hbuianim` shipped broken for exactly
// that reason (UI/UIAnimation.cpp:43-46 records it) and `.hbworld` died of it.
bool LoadDocument(const std::filesystem::path& path, DocData& out);

// Editor-only. std::ofstream is the sanctioned asymmetry (read via VFS, write
// via the filesystem) - the runtime never saves documents.
bool SaveDocument(const DocData& doc, const std::filesystem::path& path);

// In-memory forms. These exist for the editor's undo/redo and play-revert
// snapshots (B12): a document's entities are excluded from the scene snapshot
// by design, so the undo stack has to carry the documents ALONGSIDE the scene
// string or a UI edit is captured by nothing and restored by nothing.
std::string SaveDocumentToString(const DocData& doc);
bool LoadDocumentFromString(const std::string& text, DocData& out);

// --- Live documents -----------------------------------------------------------
// An OPEN document: the parsed file plus the entities it created. The handle is
// stamped into every one of those entities as UIDocMember{handle, screenOwned}.
struct DocumentInstance {
    DocHandle handle = 0;
    std::filesystem::path path; // absolute; empty for a never-saved "New" document
    std::string rel;            // path relative to Assets/ (display + .hbproj slot)
    // Spared by BOTH Replace sweeps (scene::Instantiate and Engine::FlowMainMenu).
    // True for every document the Engine or the Editor owns - a document is an
    // ASSET that outlives whatever world happens to be loaded. It is duplicated
    // into UIDocMember because those two sweeps have no access to a DocumentSet.
    bool screenOwned = true;
    // The header as loaded (canvas config + the mandatory post block). Entities
    // are NOT mirrored here: the live registry is the truth once a document is
    // open, and CaptureDocument rebuilds the entity list from it on save.
    DocData header;
    // Live entities, document order. Entries may go stale if something outside
    // the DocumentSet destroys them; every consumer re-validates.
    std::vector<entt::entity> entities;
    bool legacy = false; // adopted from a `.hbscene` by the compatibility branch
    bool dirty = false;  // edited since load/save (editor only)
};

// Creates a document's entities in `scene`, stamping UIDocMember{doc,screenOwned}
// on each. Emplaces EXACTLY: Name, Parent, Transform, UIElement, UICanvas,
// UIAnimator, UIPanel, UILayoutGroup, UICanvasGroup, UIDocMember - there is no
// code path here that reaches a MeshInstance, a SceneLayer, a SceneSource or a
// guid, which is the separation guarantee stated as a property of the function.
//
// `renderer` may be null when `preload` is false: the structural half then runs
// with no GPU at all (fonts + UI textures are what need a device), which is what
// makes the invariants testable headlessly.
std::vector<entt::entity> InstantiateDocument(Scene& scene, Renderer* renderer,
                                              const DocData& doc, DocHandle handle,
                                              bool screenOwned, bool preload = true);

// Rebuilds a DocData from the LIVE entities carrying UIDocMember{doc}. Parent
// links are remapped to document indices; a parent outside the document becomes
// a root. This is how the editor saves: entities created/deleted/reparented
// since the open are picked up because the registry, not a cached list, is read.
//
// `order` (pass `&DocumentInstance::entities`) is the DOCUMENT ORDER, and it is
// not optional in spirit - only in signature, so the self-tests can call this
// with none. Entity order IS z-order and hit order for canvas-less roots
// (UISystem walks `view<UIElement>` pool order), so it has to survive a
// save/reopen. Sorting members by their raw entt handle does NOT do that:
// entt 3.13 packs a 12-bit VERSION into the high bits of the 32-bit handle
// (entity_mask 0xFFFFF), so one delete-then-create inside an open document
// yields a recycled handle numerically ABOVE every fresh one and silently
// permutes the file relative to what the editor was showing. Members present in
// `order` keep that order; anything created since (or all of them, when `order`
// is null) follows, sorted by entity INDEX with the version bits masked off.
void CaptureDocument(const Scene& scene, DocHandle doc, const DocData& header,
                     DocData& out,
                     const std::vector<entt::entity>* order = nullptr);

// Destroys every live entity carrying UIDocMember{doc} (not just the ones the
// open recorded - the editor creates more) and forgets the instance.
class DocumentSet {
public:
    // Loads `path` and instantiates it. Returns 0 on failure. Re-opening the
    // same path opens a SECOND independent instance on purpose: a document is a
    // template, and two world-space pages may show one file.
    DocHandle Open(Scene& scene, Renderer* renderer, const std::filesystem::path& path,
                   bool screenOwned = true, bool preload = true);
    // Same, from data already in hand (undo restore, "New", the self-tests).
    DocHandle OpenFromData(Scene& scene, Renderer* renderer, const DocData& doc,
                           const std::filesystem::path& path, bool screenOwned = true,
                           bool preload = true);
    // LEGACY COMPATIBILITY: adopt entities that a `.hbscene` load already created,
    // so a half-migrated project (a `.hbproj` still pointing at a UI `.hbscene`)
    // behaves identically to a migrated one for UIManager::Bind and the sweeps.
    DocHandle AdoptLegacy(Scene& scene, const std::vector<entt::entity>& entities,
                          const std::filesystem::path& path, const DocData& header,
                          bool screenOwned = true);
    void Close(Scene& scene, DocHandle doc);
    void CloseAll(Scene& scene);

    // Stamps UIDocMember{doc,screenOwned} on `e` AND appends it to the
    // instance's `entities` list, which is what CaptureDocument orders by. Every
    // site that grows an open document (the editor's UI create menu, a paste, a
    // duplicate) must go through here rather than emplacing the tag by hand: an
    // entity that carries the tag but is missing from the list falls into the
    // by-index tail of the capture order, i.e. it lands wherever entt's free
    // list happened to put it instead of at the end of the document.
    void Track(Scene& scene, DocHandle doc, entt::entity e);

    DocumentInstance* Get(DocHandle doc);
    const DocumentInstance* Get(DocHandle doc) const;
    const DocData* Header(DocHandle doc) const;
    // Open instances, in open order.
    const std::vector<DocumentInstance>& All() const { return docs_; }
    std::vector<DocumentInstance>& All() { return docs_; }
    bool Empty() const { return docs_.empty(); }

private:
    std::vector<DocumentInstance> docs_;
    DocHandle next_ = 1; // 0 is reserved for "no document"
};

// --- Migration: .hbscene -> .hbui ---------------------------------------------
// PURE JSON. No registry, no Renderer, no GPU, no Project. Never modifies or
// deletes `src`: it writes a NEW document and leaves the original in place, so
// a half-migrated project is always recoverable by hand.
struct ConvertReport {
    u32 sceneEntities = 0; // entities in the source file
    u32 uiEntities = 0;    // entities lifted into the document
    u32 worldEntities = 0; // entities left behind in the scene
    u32 droppedKeys = 0;   // non-document keys removed from lifted entities
    std::vector<std::string> droppedKeyNames; // sorted, unique
    // Entities whose parent stayed BEHIND in the scene, so the lift made them
    // roots (salvage::PartitionEntitiesRemappingParents' cross-partition rule).
    // Counted and NAMED because it is the one lossy thing the conversion does and
    // `droppedKeys` cannot see it - `parent` is a whitelisted document key, so a
    // severed mount drops no key at all. The concrete case is a world-space
    // UICanvas parented under a door: the page lifts, the door does not, and the
    // page stops following the door with nothing in the log to say so.
    u32 severedParents = 0;
    std::vector<std::string> severedParentNames; // in document order, capped
    bool mixed = false;       // BOTH partitions non-empty
    bool convertible = false; // has at least one UI entity
    bool wrote = false;       // a file was actually written
};

// `canvas` seeds the document header (the migrator passes the project's
// BuildSettings-derived config). With `dryRun` nothing is written. Without
// `force` an existing `dstDoc` is refused rather than overwritten.
bool ConvertSceneToDocument(const std::filesystem::path& src,
                            const std::filesystem::path& dstDoc,
                            const CanvasConfig& canvas, ConvertReport& report,
                            bool dryRun, bool force);

struct UIMigrationStats {
    u32 files = 0;       // .hbscene files scanned
    u32 convertible = 0; // files containing at least one UI entity
    u32 mixed = 0;       // files containing BOTH UI and world entities
    u32 uiEntities = 0;  // total UI entities across convertible files
    u32 written = 0;     // .hbui files written (0 in a dry run)
    u32 skipped = 0;     // refused because the destination already exists
    u32 droppedKeys = 0;
    u32 severedParents = 0; // UI entities whose parent stayed in the scene
    u32 collisions = 0;     // two source scenes competing for one `UI/<stem>.hbui`
    u32 failed = 0;
};

// Scans `assetsDir` recursively for `.hbscene` files with UI content and writes
// `<assetsDir>/UI/<stem>.hbui` for each. SOURCE SCENES ARE NEVER TOUCHED - not
// rewritten, not deleted. Retiring them is the operator's call; the report says
// which ones are candidates.
//
// The destination is FLAT (`UI/<stem>.hbui`) because the reference project's
// `.hbproj` slots already point there, so two convertible scenes with the same
// stem in different folders (`Scenes/Menu.hbscene`, `Levels/Menu.hbscene`) would
// compete for one file - and with `force` the second would overwrite the first
// while the summary happily reported "2 written". A stem collision is therefore
// REFUSED (counted in `collisions`, named in the log) even under `force`; rename
// one of the sources or convert it by hand.
UIMigrationStats MigrateSceneUI(const std::filesystem::path& assetsDir,
                                const CanvasConfig& canvas, bool dryRun, bool force);

// Repoints a `.hbproj`'s two UI slots at the generated documents:
//   "uiScene": "Scenes/UIScene.hbscene"  ->  "uiDocument": "UI/UIScene.hbui"
//   "studioLoadingScene": ...            ->  "bootDocument": "UI/StudioOpen.hbui"
// A SURGICAL JSON edit, not a Project::Save() round trip, for the same reason
// MigrateSceneGuids is surgical: a load/save through the settings struct would
// silently drop any key the current parser does not know about. Only the four
// keys above are touched; everything else in the file is preserved verbatim.
// Refuses to repoint a slot whose destination document does not exist.
// Returns the number of slots changed (0 = nothing to do), -1 on failure.
int RepointProjectDocuments(const std::filesystem::path& hbproj,
                            const std::filesystem::path& assetsDir, bool dryRun);

// --- Migration: one all-in-one `.hbui` -> ONE DOCUMENT PER SCREEN -------------
// The `.hbui` format arrived holding EVERY screen in one file (four UIPanel roots
// in the reference project). A screen is the unit people author, review, diff and
// own, so each one becomes its own document; all of them are opened at boot and
// stay resident, which is what keeps a screen change a bool write.
//
// PURE JSON, like ConvertSceneToDocument: no registry, no Renderer, no GPU, no
// Project. IT NEVER MODIFIES OR DELETES THE SOURCE. The source document stays on
// disk, still loadable, still the fallback if the split is not wanted - retiring
// it is the operator's decision.
//
// Splits on ROOT UIPanel entities (`uiPanel` present, `parent` == -1). Each output
// carries that root plus its transitive descendants, with parent indices renumbered
// by salvage::PartitionEntitiesRemappingParents, and the SOURCE HEADER VERBATIM
// (canvas / post / ambientIntensity / exposure) so layout and look are bit-identical
// to what the combined document produced.
struct ScreenSplitReport {
    struct Screen {
        std::string panel;    // UIPanel::name
        std::string rel;      // path relative to Assets (the .hbproj entry)
        std::string file;     // absolute destination
        u32 entities = 0;     // root + descendants
        bool startVisible = false;
        bool wrote = false;
        bool existed = false; // destination already present (refused without force)
    };
    std::vector<Screen> screens;
    u32 sourceEntities = 0;
    // Entities under NO root panel. They exist in the combined document and would
    // be silently LOST by a panel-wise split, so they are counted and named and the
    // split REFUSES unless they are zero (or --force). Zero in the reference project.
    u32 orphans = 0;
    std::vector<std::string> orphanNames;
    // Panel names that appear twice among the roots: they would produce one file
    // and the second would overwrite the first. Refused.
    std::vector<std::string> duplicatePanels;
    // `UIElement::action` values the ENGINE resolves globally (setting:* and the
    // flow verbs + "caption") that end up in more than one output screen. Every
    // consumer addresses them by string over the whole registry, so a duplicate
    // means both fire. Reported, never fatal - it is authored content.
    std::vector<std::string> actionCollisions;
    bool ok = false;
};

// `outDir` is where the per-screen documents are written (typically
// `<assets>/UI`). `relPrefix` is prepended to each `rel` (typically "UI/").
// With `dryRun` nothing is written. Without `force` an existing destination, an
// orphan entity or a duplicate panel name refuses the whole split rather than
// writing a partial set.
bool SplitDocumentByPanel(const std::filesystem::path& srcDoc,
                          const std::filesystem::path& outDir,
                          const std::string& relPrefix, ScreenSplitReport& report,
                          bool dryRun, bool force);

// Points a `.hbproj`'s screen list at `rels`, in order. SURGICAL, for the same
// reason RepointProjectDocuments is: a load/save through ProjectSettings would
// silently drop any key the current parser does not know about. Writes
// `uiDocuments` (the list) and `uiDocument` (the legacy mirror == rels[0]); every
// other key is preserved verbatim. Returns true on success.
bool RepointProjectScreens(const std::filesystem::path& hbproj,
                           const std::vector<std::string>& rels, bool dryRun);

// --- Self-test (--test-uidoc) ---------------------------------------------------
// Headless, no GPU, no window. THE GATE for P2: carries a verbatim FROZEN copy of
// the pre-extraction per-component writer blocks and diffs them, byte for byte,
// against the extracted ones - over defaults, a deterministic fuzz, and (when
// scene paths are supplied) every UI component authored in real content, end to
// end through SaveScene. Also proves the document round-trip is lossless and
// that the converter's partition/remap/strip rules hold.
bool DocumentSelfTest(const std::vector<std::filesystem::path>& scenes);

// --- Self-test (--test-uidoc-invariants) ---------------------------------------
// Opens `path` into `scene` and asserts the P3 structural contract: every created
// entity carries UIDocMember with the right handle and screenOwned flag; NONE
// carries MeshInstance / RigidBody / SceneLayer / SceneSource / Persistent /
// WorldText; a Replace sweep spares the whole set; BuildSceneJson writes zero of
// them and BuildSubtreeJson never carries the membership tag out; and Close
// destroys all of them. Needs a Renderer only when `preload` is true
// (ui::PreloadUIAssets bakes fonts + uploads textures).
//
// `Guid` is deliberately NOT in that list and cannot be: Scene::CreateEntity is
// the one mint site and stamps a Guid on EVERY entity, document members
// included. The real invariant is that no guid is ever WRITTEN (decision 3) -
// documents are excluded from every snapshot and `guid` is not a document key -
// and that is what the round-trip half of this test and --test-uidoc pin.
bool DocumentInvariantsSelfTest(Scene& scene, Renderer* renderer,
                                const std::filesystem::path& path, bool preload);

// --- Self-test (--test-uicanvas) ------------------------------------------------
// THE ANTI-DRIFT GATE for the dedicated `.hbui` editor's authoring canvas. The
// whole design rests on one claim - that the canvas is the SHIPPED UI pass, not a
// second renderer - and this is the only thing that can hold that claim over time.
//
// Opens `path` and asserts, mechanically:
//   1. VERTEX PARITY. ui::BuildDocumentVertices and the RUNTIME ui::BuildVertices,
//      over the same scene at the same target size and canvas config, emit
//      BYTE-IDENTICAL rhi::UIVertex streams. Skipped (and reported) only when the
//      document contains a world-space UICanvas, whose items the runtime routes
//      into a texture batch instead of the screen list.
//   2. LAYOUT PARITY. The document-filtered layout matches an unfiltered LayoutUI
//      item for item - i.e. the additive `docFilter` parameter is inert at 0 and
//      exact at N.
//   3. DOCUMENT SCOPING. A SECOND open document's entities appear in neither the
//      first document's vertices nor its layout, and vice versa.
//   4. The authoring render target is real: AcquireUITarget returns a valid handle
//      and Renderer::TextureUIId can hand it to ImGui (asserted on D3D12/Vulkan;
//      reported, not required, elsewhere - OpenGL implements neither).
//
// GPU SESSION, not headless (same shape as --test-uidoc-invariants): the emission
// bakes fonts and uploads UI textures, and check 4 needs a device.
//
// NOT COVERED, and no test can cover it here: whether the picture LOOKS right
// (gamma, orientation, alpha). That is a two-window visual check.
bool DocumentCanvasSelfTest(Scene& scene, Renderer& renderer,
                            const std::filesystem::path& path);

} // namespace hbe::ui
