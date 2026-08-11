// Editor/Editor.h - Dear ImGui editor: hierarchy, inspector, gizmo, stats.
#pragma once

#include "Assets/AssetRefs.h" // pack dependency closure (BuildSettings::onlyReferenced)
#include "Assets/AudioEvent.h"
#include "Assets/CutsceneAsset.h"
#include "Assets/DialogueAsset.h"
#include "Dialogue/DialogueGraph.h" // branching-dialogue graph editor
#include "Assets/CharacterAsset.h"
#include "Assets/MaterialAsset.h"
#include "Assets/SeamWeld.h"
#include "Assets/UAF.h"
#include "Editor/MovieRender.h"
#include "Editor/CollabSession.h" // peer-to-peer sessions + this scene's history
#include "Editor/SaveDispatch.h" // Ctrl+S: which focused surface owns the chord
#include "Editor/TimelineSnap.h" // the shared per-frame grid for every timeline
#include "Core/Types.h"
#include "Navigation/GridNav.h"
#include "Renderer/CameraController.h"
#include "RHI/RHI.h"
#include "Assets/MusicGraph.h"
#include "Scene/Components.h"
#include "Scene/PaintSystem.h"
#include "Scene/SceneStreamer.h"
#include "Scene/StreamPolicy.h" // stream::Evaluate (the editor's streaming SIMULATION)
#include "Scene/TagShard.h" // save-time spatial shard bake (BakeReport / TagStat)
#include "Scene/TagStreaming.h" // stream::Streamer - the editor's own LIVE zone streamer
#include "UI/UIDocument.h" // `.hbui` documents: the UI Document panel authors these
#include "Volume/VolumeSimConfig.h" // Volume Baker panel edits a VolumeSimConfig
#include "Volume/IVolumeSimulation.h" // in-scene live-preview sim instance (unique_ptr member)

#include <entt/entt.hpp>
#include <memory>
#include <glm/glm.hpp>

#include <atomic>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace hbe {

class Scene;
class Renderer;
class Input;
class Engine;
class Project;

class Editor {
public:
    // Builds the editor UI for this frame. Call between Renderer::BeginUI and
    // Renderer::RenderScene (i.e. inside an active ImGui frame).
    void BuildUI(Engine& engine);

    // Hub mode (HeartbreakHub.exe): only the Project Manager is shown, and
    // opening/creating a project launches the full editor and quits.
    void SetHubMode(bool hub) { hubMode_ = hub; }

    // Art mode (HeartbreakArtEditor.exe): a painting-focused layout for 2D
    // artists - opens the Art Editor + viewport with paint mode already on and
    // hides the engineering panels. It is still the full editor underneath, so
    // scene/paint saving and everything else work identically.
    void SetArtMode(bool art) { artMode_ = art; }

    // Installs the editor's visual theme (custom dark style + system font).
    // Call once right after Renderer::InitUI, before the first frame builds
    // the font atlas.
    static void ApplyTheme();

    // Persists the window/dock layout to `iniPath` (Dear ImGui .ini). Call once
    // right after Renderer::InitUI, before the first frame (ImGui loads the file
    // on the first NewFrame). When a layout file already exists it is restored
    // and the built-in default DockBuilder arrangement is skipped.
    static void EnableLayoutPersistence(const char* iniPath);

    // Packs the active project's assets into <root>/<name>_N.uap (honoring the
    // project's BuildSettings: compression / referenced-only) and verifies a
    // readback. Static: also used by the --pack CLI flag before the UI exists.
    static bool BuildAssetPack(std::string& outMessage);
    // Assembles <root>/Build from the project's BuildSettings: runtime exe
    // (newest config found) + shaders + .hbproj + packs (+ loose Assets only
    // when packAssets is off).
    static bool BuildShipping(std::string& outMessage);
    // True when a Release runtime exists next to the editor - i.e. BuildShipping
    // will ship the FAST build. False means the ship falls back to a slower config
    // (Debug costs ~23 ms CPU/frame here), so the interactive Build paths confirm
    // first instead of producing an unshippable build that reports success.
    static bool HasReleaseRuntime();
    // The TRANSITIVE closure of every asset reachable from the active project's
    // roots (its scenes / UI documents / prefabs, plus the non-scene roots in
    // ProjectSettings: startupScene, bootDocument, every uiDocuments[] entry,
    // musicGraph and the input-icon glyphs). `included` is the pack filter used
    // when BuildSettings::onlyReferenced is on; the result also carries the
    // exclusion list and every unresolvable reference, so a cook can prove what
    // it left out instead of assuming. See Assets/AssetRefs.h.
    static assets::ClosureResult CollectReferencedAssets();

    // Gives a NEWLY CREATED asset its permanent pack slot and writes it into the
    // file (Assets/SlotIds.h). Call it right after any editor path that produces
    // a new asset - the number is a property of the asset from birth, and the
    // cooker reads it back rather than inventing one. Idempotent and cheap to get
    // wrong in the safe direction: an asset that already carries an id is left
    // exactly as it is. No project open, or not a packable file, does nothing.
    static void StampNewAsset(const std::filesystem::path& file);

    // Re-stamps every packable asset under Assets/ written at or after `mark` that
    // has NO embedded id. Call after any SAVE, not just after a creation.
    //
    // Necessary because the JSON savers build their document from scratch
    // (`json j;` ... `dump(2)`) and therefore DROP the top-level "packSlot" the
    // stamper wrote - so an asset that has been saved once is back on tier 2 until
    // something restores it. Restoring it is free and exact: StampNewAssets prefers
    // the id the manifest remembers for that same key, which IS the id the file had.
    // Doing it per-operation rather than per-file also covers a save's by-products
    // (the `.hbpaint` canvases a scene save writes, every additively streamed scene).
    static void StampSavedAssets(std::filesystem::file_time_type mark);

    // Carries slot memory across a rename or a move that has already happened on
    // disk (Assets/SlotIds.h). Handles files and whole folders.
    static void RekeyMovedAsset(const std::filesystem::path& from,
                                const std::filesystem::path& to);

    // --test-uiseparation: proves the SEPARATION GUARANTEE's save-time half.
    //
    // Creation-time gating (the UI create menu / Add-Component entries disabled
    // unless a document is the edit target) is the primary enforcement, but it is
    // ImGui state and cannot be asserted headlessly. The half that CAN be proven
    // is the backstop: SaveSceneToDisk must REFUSE, with a message, and write
    // NOTHING, when a non-document entity carries one of the six document
    // components - never silently drop it, which is what the cancelled U5 would
    // have done and which quietly deletes an author's UI on Ctrl+S.
    //
    // Also pins the deliberate exemptions: document content, generated world-UI
    // page quads, transient runtime UI, and WorldText (level signage - policing
    // it would make a level with a 3D sign unsaveable).
    //
    // Headless, no GPU/window, no project. Same contract as --test-seamweld.
    static bool SeparationSelfTest();

    // --test-uieditor: the AUTHORING contract of the dedicated `.hbui` editor's
    // palette, hierarchy and tools (phase I3). Everything here is what a mouse
    // gesture in that panel ends up calling, minus the mouse:
    //   1. THE PALETTE IS THE CREATE MENU. Every catalog recipe, created through
    //      Editor::CreateUIElementInDocument, joins the document via
    //      DocumentSet::Track - so it carries UIDocMember{doc} AND appears in the
    //      instance's order list (the list CaptureDocument writes the file in).
    //      An entity with the tag but not the list lands wherever entt's free list
    //      put its handle instead of at the end of the document.
    //   2. NO DOCUMENT, NO CREATION. With activeDoc_ == 0 the call creates nothing
    //      and returns null - the creation-time half of the separation guarantee,
    //      which the palette must not be a second hole in.
    //   3. NOTHING THE PALETTE MAKES IS WORLD CONTENT: every recipe passes
    //      AuditDocumentForWorldContent, and a save of the scene alongside it
    //      writes none of it.
    //   4. Z-ORDER SURVIVES A SAVE. Raise/lower swaps two siblings in the entt
    //      pool AND in the document order list; capture -> save -> reload must
    //      reproduce the SAME draw order. This is the gate on "what you see is
    //      what ships": draw order comes from pool iteration order, which no file
    //      records directly, so only a round-trip can prove it.
    //   5. REPARENT RULES: inside one document it nests; UNPARENT is allowed and
    //      keeps membership; ACROSS documents (and across the document/scene
    //      boundary) it is REFUSED, not "fixed".
    //   6. CUT IS NOT DESTRUCTIVE. A subtree copy rooted inside a document is a
    //      real fragment with the elements in it (the B11 bug produced a non-empty
    //      STRING holding zero entities, so every frag.empty() guard passed and
    //      Cut deleted content it had not copied).
    //
    // Headless, no GPU/window, no project - documents open with preload=false and
    // a null Renderer. Same contract as --test-uiseparation.
    static bool UIEditorSelfTest();

    // --test-pasteparent: THE PASTE-PARENTING CONTRACT (see AttachPastedRoot).
    //
    //   1. A copied CHILD pastes as a SIBLING of the source, under the same
    //      parent, LAST in that sibling group. This is the reported bug: the
    //      fragment cannot carry the root's parent (it is outside the subtree),
    //      so it has to be captured at COPY time.
    //   2. A copied ROOT still pastes as a root.
    //   3. Ctrl+D captures the SELECTION's parent, not the clipboard's.
    //   4. A `.hbprefab` drop is a ROOT - it has no source parent, and placeAt
    //      already positions it.
    //   5. Prefab REVERT restores the INSTANCE's own parent (plus its guid and
    //      its sibling order - a revert visibly changes nothing).
    //   6. Every way the captured parent can go bad is handled and never
    //      emplaced: destroyed since the copy, replaced world (undo/redo
    //      recycles handles), aliasing an entity the paste itself just created,
    //      or living in a different `.hbui` document.
    //   7. Clones still mint FRESH guids - parenting does not touch identity.
    //
    // Headless, no GPU/window/project. Same contract as --test-pasteorder.
    static bool PasteParentSelfTest();

    // --test-scenesave <scene.hbscene> [--project <proj>]: THE SCENE-SAVE
    // COMPLETENESS CONTRACT - "a .hbscene contains every entity of the active world,
    // or the save does not happen".
    //
    // Works on a COPY of `sceneFile` in the temp directory and re-checks the
    // original's bytes and mtime at the end; it never writes to the file it is
    // pointed at. An empty path FAILS rather than falling through.
    //
    // Proves, in order: no entity and no component key present in the file on disk
    // is missing from the file a load+save produces (compared against the FILE, so a
    // bug that drops the same thing twice cannot pass); the bytes are identical after
    // 0 and after 600 edit-mode ticks (a save is a function of the scene, not of how
    // long the editor had it open - the UIAnimator-bake class); save -> load -> save
    // is a fixed point; the subtree writer excludes what the file writer excludes;
    // and that a save is REFUSED, with the target file untouched, when shards are
    // despawned, when the world was replaced behind the editor's back, when the world
    // is empty over a populated file, and while playing.
    //
    // Headless: no GPU, no window, no ImGui context.
    static bool SceneSaveSelfTest(const std::filesystem::path& sceneFile);

    // `--test-editorzones`: LIVE EDITOR ZONES, end to end through the real Editor.
    //
    // Proves the non-destructive bind (a stream::Streamer bound to the editor's world
    // spawns and despawns without a single authored entity being touched, and without
    // BindWorld / DestroyWorld ever running); that a MANUAL override forces residency
    // in BOTH directions and composes with associations and with alwaysLoaded; that a
    // SAVE attempted mid-stream behaves exactly per the decision - refused with the
    // file untouched while a shard is genuinely missing, allowed once the save path
    // has settled the world, and still refused for a RUNTIME bind; that the Play/Stop
    // snapshot round-trips with streamed content present; and that undo after a stream
    // event does not undo the stream.
    //
    // Headless: no GPU, no window, no ImGui context, its own scratch level.
    static bool EditorZoneSelfTest();

private:
    void DrawViewport(Engine& engine);
    void DrawHierarchy(Engine& engine);
    // One tree row (recurses into children). Uses childrenByParent_.
    void DrawEntityNode(Scene& scene, Renderer& renderer, entt::entity e);
    // Makes `child` a child of `newParent` (entt::null = scene root), keeping
    // the child's world transform. Refuses cycles. Parenting under an entity
    // also moves the child's subtree into that parent's scene.
    void Reparent(Scene& scene, entt::entity child, entt::entity newParent);
    // Moves an entity (and its descendants) into a scene: tags the whole subtree
    // with SceneSource{scenePath}, or untags it (the active scene) when empty.
    // Used by drag-to-scene-group in the hierarchy. Caller pushes undo.
    void MoveToScene(Scene& scene, entt::entity root, const std::string& scenePath);
    // Destroys an entity together with all of its descendants.
    void DestroyRecursive(Scene& scene, entt::entity e);
    void DrawInspector(Scene& scene, Renderer& renderer);
    // Creates an entity: prim 0 = empty, 1 = cube, 2 = sphere.
    entt::entity CreateEntityPrim(Scene& scene, Renderer& renderer, int prim);
    // Creates a mesh entity from a named procedural primitive (see
    // mesh::kPrimitiveNames): MeshInstance + MeshRef "prim:<name>" + AABB + a
    // matching collider. Returns the new entity.
    entt::entity CreateMeshEntity(Scene& scene, Renderer& renderer, const char* prim);
    void EnsurePrimitives(Renderer& renderer);
    void DrawStats(Engine& engine);
    // HDR post-process stack controls (toggles + tuning for bloom/SSAO/TAA/DoF/
    // motion blur/SSR/exposure/grade), bound to the scene environment.
    void DrawPostProcess(Engine& engine);
    // Post-stack widgets, split so the LOOK is volume/scene-based and the
    // quality (AA + GTAO) is project-global:
    //   Look: exposure, bloom, SSGI, SSR, fog, DoF, motion blur, grade. `exposure`
    //         may be null (post volumes carry no manual exposure).
    //   Quality: GTAO, TAA, FXAA - edited once in Project Settings.
    void DrawPostLookControls(rhi::PostSettings& p, f32* exposure);
    bool DrawPostQualityControls(rhi::PostSettings& p); // returns true if edited
    // Keyframe timeline for the selected entity's AnimationTrack: scrubbable
    // playhead, key diamonds, capture/delete keys, play controls.
    void DrawTimeline(Engine& engine);
    void DrawAssetBrowser(Engine& engine);
    // Imports OS files dropped on the window into the current browser folder,
    // drained once per frame from the window's drop queue (editor-only).
    void ConsumeDroppedFiles(Engine& engine);
    void DrawGizmo(Engine& engine);
    // Spline-point editing for a selected CameraSpline: a translate gizmo on the
    // active control point, click-to-select points, and Tab to extend the end.
    void EditCameraSpline(Engine& engine, CameraSpline& sp);
    // Outlines the selected entity's world AABB in the viewport (pick feedback).
    void DrawSelectionOutline(Scene& scene, Renderer& renderer);
    // Billboard icons for non-mesh entities (lights / camera / zone / audio /
    // spline / empty): vector glyphs drawn at each entity's projected position so
    // they're visible and clickable in the viewport (they have no mesh to pick).
    void DrawEntityIcons(Scene& scene, Renderer& renderer);
    // Picture-in-picture render of what the selected camera sees (reuses the
    // renderer's editor-preview target), shown in the viewport corner.
    void DrawCameraPreview(Engine& engine);
    // Ray-picks the entity under the mouse in the viewport and selects it.
    void PickEntity(Scene& scene, Renderer& renderer);
    // Entity whose world AABB is nearest under normalized viewport coords
    // (mx, my in 0..1); entt::null when nothing is hit.
    entt::entity EntityUnderPixel(Scene& scene, Renderer& renderer, f32 mx, f32 my);
    // Spawns a mesh .uaf as entities; returns the root (or single) entity.
    entt::entity SpawnMeshAsset(Scene& scene, Renderer& renderer,
                                const std::filesystem::path& uaf,
                                bool frameCamera = true);
    // World position under a viewport pixel: nearest AABB hit, else a point a
    // few units along the pick ray (used to place drag-dropped assets).
    glm::vec3 DropPointInWorld(Scene& scene, Renderer& renderer, f32 mx, f32 my);
    // Loads a .hbscene into the live world and resets editor selection state.
    void LoadSceneInEditor(Engine& engine, const std::filesystem::path& path);
    void RefreshAssets();

    struct AssetItem {
        std::filesystem::path path;
        std::string label;    // filename stem shown under the tile
        std::string typeName; // "Folder" / "Mesh" / "Texture" / "Audio" / ...
        bool isFolder = false;
        bool isMesh = false;
        bool isAudio = false;
        bool isAudioEvent = false;
        bool isTexture = false;
        bool isScene = false;
        bool isMaterial = false;
        bool isSchematic = false;
        bool isPrefab = false;
        bool isFont = false;
        bool isDialogue = false;
        bool isCutscene = false;
        bool isMusic = false;      // .hbmusic adaptive-music graph
        bool isCharacter = false;  // .hbchar modular-character rig
        bool isUIAnim = false;     // .hbuianim UI animation clip
        bool isUIDoc = false;      // .hbui UI document (screen/world UI tree)
        bool isVolumeSim = false;  // .hbvolsim volume authoring config (opens the Volume Baker)
        bool isVolumeCache = false;// .hbvol baked volume frames (drag onto a Volume component)
        u64  thumbId = 0;       // ImGui texture id (textures only; 0 = icon)
        bool thumbTried = false;
    };
    std::vector<AssetItem> assets_;
    bool assetsScanned_ = false;
    f32  assetTileSize_ = 96.0f; // icon tile edge, user-adjustable
    // Folder navigation: the directory the grid currently shows (inside the
    // project's Assets/), and a deferred navigation target (applied after the
    // tile loop so the asset list isn't mutated mid-iteration).
    std::filesystem::path currentDir_;
    std::filesystem::path navTarget_;
    bool assetsDirty_ = false; // a tile interaction moved/changed files
    // Pending asset rename (set from the tile context menu; a modal in
    // DrawAssetBrowser applies it). Keeps the extension; folders rename whole.
    std::filesystem::path renameAsset_;
    char renameAssetBuf_[128] = {};
    // Pending asset DELETE (a modal in DrawAssetBrowser confirms it). Delete used
    // to be a one-click unrecoverable `remove()` whose error code was consulted
    // only to decide whether to refresh the grid - so a failed delete (locked
    // file, non-empty folder) was completely silent, and deleting something the
    // game still references produced a broken build with no diagnostic. The
    // reference set is computed ONCE when the modal opens, not per frame.
    std::filesystem::path deleteAsset_;
    bool deleteAssetIsFolder_ = false;
    bool deleteAssetRefsKnown_ = false; // false = closure could not be proven total
    std::vector<std::string> deleteAssetRefs_; // referenced keys under the target
    std::string deleteAssetError_;            // surfaced remove() failure
    // Pending "create asset" name prompt (a modal in DrawAssetBrowser confirms
    // it). kind: 1 = material, 2 = script, 3 = audio event.
    int pendingCreateKind_ = 0;
    std::filesystem::path pendingCreateDir_;
    char newAssetNameBuf_[128] = {};

    // One tile of the asset grid (icon/thumbnail + label + interactions).
    void DrawAssetTile(Engine& engine, AssetItem& item);
    // Loads + downsamples a texture asset into a GPU thumbnail (cached by path).
    void LoadThumbnail(Renderer& renderer, AssetItem& item);
    std::unordered_map<std::string, u64> thumbCache_; // path -> ImGui id

    // Cached primitive meshes for entity creation (uploaded lazily).
    rhi::MeshHandle cubeMesh_;
    rhi::MeshHandle sphereMesh_;
    // Loaded textures by .uaf name, so shared textures upload once.
    std::unordered_map<std::string, rhi::TextureHandle> textureCache_;
    // Flythrough camera: hold right mouse to look, WASD/QE to move (Shift = fast).
    void UpdateFreecam(Renderer& renderer, const Input& input, f32 dt);
    // Captures the live camera into the freecam state (prevents a snap when
    // handing control from auto-orbit to the freecam).
    void SyncFreecam(Renderer& renderer);
    // "F" hotkey: frame the editor camera on the selected entity's world bounds.
    void FrameSelected(Engine& engine);

    entt::entity selected_ = entt::null;
    int  selectedKey_ = -1;    // timeline keyframe selection
    // Plain Timeline (AnimationTrack) view state: zoom (px/s, 0 = refit to width),
    // scroll (seconds), the key-drag gesture, and a one-key clipboard.
    f32  timelineZoom_ = 0.0f;
    f32  timelineScroll_ = 0.0f;
    bool timelineDragArmed_ = false; // pressed a key, not yet past the drag threshold
    bool timelineDragKey_ = false;   // actively dragging the selected key's time
    f32  timelineDragOffset_ = 0.0f; // grab offset in seconds (key.time - cursorTime)
    std::vector<AnimationTrack::Key> timelineClipboard_;
    // Which entity selectedKey_ indexes into. selectedKey_ is a bare index, so it
    // must be cleared when the inspected entity changes or it points at a stranger's key.
    entt::entity timelineKeyOwner_ = entt::null;
    int  gizmoMode_ = 0;       // 0 = translate, 1 = rotate, 2 = scale
    // Grid snapping for the transform gizmo: round translation to gizmoSnapStep_
    // metres, rotation to gizmoSnapAngle_ degrees, scale to gizmoSnapScale_.
    // Timeline frame-grid snapping. Defaults ON, unlike gizmoSnap_: a timeline whose
    // keys land on arbitrary floats is the broken state, not the useful one. Ctrl
    // SUSPENDS it for a gesture (see the grid setup in DrawCutsceneTimeline).
    bool timelineSnap_ = true;
    bool gizmoSnap_ = false;
    f32  gizmoSnapStep_ = 1.0f;
    f32  gizmoSnapAngle_ = 15.0f;
    f32  gizmoSnapScale_ = 0.25f;
    bool showDemo_ = false;
    entt::entity pendingDelete_ = entt::null; // deferred to after the tree walk

    // Camera-spline point editing: when a CameraSpline entity is selected the
    // gizmo moves one CONTROL POINT (splinePoint_) instead of the entity, clicking
    // a point in the viewport selects it, and Tab extends the active end.
    int  splinePoint_ = -1;                    // active control point (-1 = none)
    entt::entity splineEntity_ = entt::null;    // spline splinePoint_ indexes into
    bool splineConsumedClick_ = false;          // a point click suppressed picking

    // Viewport billboard icons for non-mesh entities (lights/camera/...).
    bool showIcons_ = true;            // draw the icons (toggle in Stats)
    bool iconConsumedClick_ = false;   // an icon click suppressed entity picking

    // Picture-in-picture preview of the selected camera's view.
    bool showCameraPreview_ = true;             // toggle in Stats
    bool previewSubmitted_ = false;             // preview target claimed this frame
    std::vector<rhi::DrawItem> cameraPreviewItems_; // scene draw items for the PiP
    // Appends (or, atStart, prepends) a control point extrapolated from the end
    // segment and selects it. One undo step.
    void ExtendSpline(Scene& scene, CameraSpline& sp, bool atStart);

    // Parent -> children lookup, rebuilt each frame the hierarchy draws.
    std::unordered_map<u32, std::vector<entt::entity>> childrenByParent_;

    // Viewport panel rect (screen space) + per-frame image input state.
    f32  vpX_ = 0, vpY_ = 0, vpW_ = 1280, vpH_ = 720;
    // When false, the Scene viewport FILLS its panel at any aspect (per monitor); when
    // true it letterboxes to the game's target aspect (WYSIWYG). The dedicated "Game"
    // view stays locked regardless, so it remains the true export preview. Default off:
    // the user wants the scene preview to use the whole panel.
    bool lockViewportAspect_ = false;
    bool vpHovered_ = false; // mouse over the rendered image item
    bool vpClicked_ = false; // LMB pressed on the image this frame
    bool vpVisible_ = false; // the Viewport image was actually drawn this frame
    bool layoutBuilt_ = false;

    // -- Editor snapshot (undo/redo AND play-mode revert) -----------------------
    // A snapshot is the scene string PLUS every open `.hbui` document. Document
    // entities are deliberately excluded from BuildSceneJson (they are asset
    // content with their own file), so a scene-only snapshot captures nothing of
    // a UI edit and restores nothing of it. Putting documents back into
    // BuildSceneJson is NOT the fix: that reintroduces the duplication the skip
    // exists to prevent.
    // NOTE: no handle. A restore CLOSES every open document and OPENS the
    // captured ones fresh, so the restored handles are new. Nothing outside the
    // editor holds a handle across a restore (the runtime never restores), and
    // `active` is what has to survive, not the number.
    struct DocSnapshot {
        std::filesystem::path path;
        bool screenOwned = true;
        bool active = false; // was this the edit target?
        // Carried because OpenFromData always produces a CLEAN instance: without
        // this, one Ctrl+Z over three unsaved edits cleared the tab's `*` while the
        // document still differed from disk by two of them. Save is not gated on
        // the flag, so this is the unsaved INDICATOR, not the save itself.
        bool dirty = false;
        std::string json;    // ui::SaveDocumentToString
    };
    struct Snapshot {
        std::string scene;
        std::vector<DocSnapshot> docs;
    };
    Snapshot CaptureSnapshot(Engine& engine) const;
    // The capture CaptureSnapshot performs for ONE open document. Factored out and
    // engine-free so --test-uieditor drives the REAL call rather than a lookalike:
    // this site once omitted CaptureDocument's `order` argument while the Save site
    // passed it, and the difference silently reverted every z-order edit on the next
    // undo (a pool permutation does not move entity indices, so the index fallback
    // cannot stand in for the order list).
    static std::string CaptureDocumentSnapshotJson(Scene& scene,
                                                   const ui::DocumentInstance& inst);

    // -- Game view / play mode -------------------------------------------------
    // Unity-style Game tab: Play snapshots the scene and starts the simulation
    // (physics + scripts); Stop restores the snapshot; Pause freezes both.
    void DrawGameView(Engine& engine);
    void EnterPlayMode(Engine& engine);
    void StopPlayMode(Engine& engine);
    bool playMode_ = false;
    bool playPaused_ = false;
    Snapshot playSnapshot_;         // scene + open documents at Play, restored on Stop
    bool playSnapshotValid_ = false;
    std::string gameStateSnapshot_; // game:: story state (flags/objectives) at Play, restored on Stop
    bool focusGameView_ = false; // focus the Game tab next frame
    bool focusViewport_ = false; // focus the Viewport tab next frame
    // True when DrawGameView fed a LIVE in-game UI pointer this frame (the cursor
    // is over the game image). Reset at the top of BuildUI. The dedicated UI editor
    // reads it: that panel otherwise pins the pointer OFF so its own mouse cannot
    // live-activate the document it is showing (Engine falls back to raw editor-
    // window coords when nobody feeds one), and it must not do that while the Game
    // view is legitimately driving the UI. BuildUI draws the Game view first.
    bool gameViewPointerFed_ = false;

    // -- UI editing (Scene viewport, while not playing) ----------------------------
    // Click-select, drag to move, corner handles to resize, with grid +
    // canvas-center/edge snapping. Drawn over the viewport image (the UI
    // overlay renders into the same target). Returns true when it consumed
    // this frame's click (suppresses 3D entity picking).
    bool DrawUIEditOverlay(Engine& engine, const glm::vec2& imgPos,
                           const glm::vec2& imgSize);
    bool uiSnap_ = true;
    f32  uiSnapStep_ = 10.0f;  // canvas units
    int  uiDragMode_ = 0;      // 0 none, 1 move, 2 TL, 3 TR, 4 BR, 5 BL
    glm::vec2 uiDragStartMouse_{0.0f};  // canvas coords at drag start
    glm::vec2 uiDragStartCenter_{0.0f};
    glm::vec2 uiDragStartSize_{0.0f};
    glm::vec4 uiDragParentRect_{0.0f};  // x0,y0,x1,y1 (the item's parent rect)
    glm::vec2 uiDragCanvasSize_{1.0f};  // the dragged item's canvas size

    // Freecam (movement/look logic shared with the runtime).
    FlyCameraController freecam_;
    bool freecamActive_ = false;

    // -- Project manager -------------------------------------------------------
    // Modal for creating/opening projects; auto-opens while no project is
    // active and is reachable from the Project menu afterwards.
    void DrawProjectManager();
    // The launcher panel: which engine build this is, whether a newer one is published,
    // and the controls to fetch it. Hub mode only.
    void DrawHubLauncher();
    bool OpenProject(const std::filesystem::path& hbproj);
    bool CreateProject(const std::filesystem::path& directory, const std::string& name);
    void OnProjectChanged(); // resets browser state, seeds starter content
    void LoadRecentProjects();
    void SaveRecentProjects() const;
    void AddRecentProject(const std::filesystem::path& hbproj);

    // -- Drag & drop -----------------------------------------------------------
    // Every asset tile drags as "HBE_ASSET_PATH"; this dispatcher applies a
    // dropped asset to an entity: materials paint, textures auto-create a
    // material (or set a UI element's image), scripts attach, audio becomes an
    // AudioSource, fonts restyle UI text, meshes spawn as children. Returns
    // true when the asset type was consumed.
    bool ApplyAssetDropToEntity(Engine& engine, entt::entity target,
                                const std::filesystem::path& asset);
    // Finds-or-creates the .hbmat generated from a texture: albedo = the
    // texture, with sibling Normal / Metal-Rough / AO / Emissive maps matched
    // by naming convention in the same folder. Returns the material's path.
    std::filesystem::path MaterialFromTexture(const std::filesystem::path& texture);
    // Drop target over the LAST ImGui item accepting a texture/font/material
    // asset; invokes `apply` with the dropped path when types match.
    template <typename Fn>
    void AssetDropTarget(const char* extension, uaf::AssetType uafType, Fn&& apply);

    // -- Asset viewer --------------------------------------------------------------
    // Previews/edits the asset selected in the browser (click a tile).
    void DrawAssetViewer(Engine& engine);
    void SelectAsset(const std::filesystem::path& path); // resets viewer caches
    // Applies a .hbmat to an entity's MeshInstance (+ MaterialRef component).
    bool ApplyMaterialToEntity(Engine& engine, entt::entity e,
                               const std::filesystem::path& hbmat);
    // Creates Assets/<dir>/NewMaterial[N].hbmat and returns its path.
    std::filesystem::path CreateMaterialAsset(const std::filesystem::path& dir,
                                              const std::string& name = {});
    // Recursively lists assets under Assets/ matching `extension` (relative
    // generic paths, sorted), optionally filtered to a `.uaf` payload type.
    // Used by the texture / font pickers.
    std::vector<std::string> ListAssetsByExt(
        const char* extension,
        uaf::AssetType typeFilter = uaf::AssetType::Unknown) const;
    // Unified asset reference picker: a button showing `current` that opens a
    // popup with a search bar and a scrollable list of project assets matching
    // `extension` (+ optional `.uaf` payload type). Returns true and fills `out`
    // when the user picks one; `noneLabel` (nullptr to hide) offers a clear entry.
    // Used by every component field that references an asset.
    bool AssetPicker(const char* label, const std::string& current, const char* extension,
                     uaf::AssetType uafType, std::string& out,
                     const char* noneLabel = "(none)");
    char assetPickerSearch_[128] = {}; // shared filter text (one popup open at a time)
    // Case-insensitive test of `rel` against the shared assetPickerSearch_ (empty
    // filter matches all). Used by AssetPicker + the "open asset" list popups.
    bool AssetSearchMatch(const std::string& rel) const;

    // A searchable dropdown over an arbitrary string list (button preview + a
    // filter-box popup), modelled on AssetPicker and sharing its search buffer.
    // Returns true and writes `out` when a choice is made; `noneLabel` (nullptr to
    // hide) offers a clear-to-empty entry. For fields that pick an existing NAME
    // (a scene tag, a camera entity) rather than free text.
    bool SearchableStringCombo(const char* label, const std::string& current,
                               const std::vector<std::string>& options, std::string& out,
                               const char* tooltip = nullptr,
                               const char* noneLabel = "(none)");

    // STABLE Euler-degrees editor for a quaternion. Editing Euler naively re-derives all
    // three axes from the quaternion every frame, but glm::eulerAngles is multivalued, so
    // dragging one axis makes the others flip and the object oscillates. This caches the
    // Euler between frames and only re-seeds it from `rot` when the edited FIELD changes
    // (`id`, e.g. a stable pointer to the field) or when `rot` is changed by something
    // else (the gizmo, undo, a new selection). Returns true on the frames it edits `rot`.
    bool RotationEulerEditor(const char* label, glm::quat& rot, const void* id);

    // Material shading PRESET combo (Standard / Skin / Cloth / Eye / Hair). Detects the
    // current preset from the shading-family flag bits and, on selection, sets that flag
    // plus sensible metallic/roughness (and SSS tint/radius for Skin) - leaving baseColor
    // and texture slots untouched. `onApply` (optional) fires right BEFORE the mutation,
    // so an inspector caller can PushUndo. Returns true when it changed something.
    bool DrawMaterialPresetCombo(u32& flags, f32& metallic, f32& roughness,
                                 glm::vec3& subsurfaceColor, f32& subsurfaceRadius,
                                 const std::function<void()>& onApply = {});
    const void* rotEulerId_ = nullptr;  // which field the cached Euler belongs to
    glm::vec3 rotEuler_{0.0f};           // cached Euler DEGREES being edited
    glm::quat rotEulerQuat_{1.0f, 0.0f, 0.0f, 0.0f}; // the quat those Euler produced

    std::filesystem::path viewedAsset_;
    std::string viewedTypeName_;
    bool viewerDirty_ = false;          // rebuild cached preview/info
    u64  viewerPreviewId_ = 0;          // large preview image (0 = none)
    std::vector<std::string> viewerInfo_;
    int  viewedAudioKind_ = 0;          // edited audio tag (uaf::AudioKind) in the viewer
    char viewedAudioCaption_[256] = {}; // edited voiceline caption
    char viewedAudioSpeaker_[96] = {};  // edited voiceline speaker/character name
    std::unordered_map<std::string, u64> previewCache_; // path -> large preview
    MaterialAsset editedMat_;           // working copy of a viewed .hbmat
    bool editedMatValid_ = false;
    bool editedMatDirty_ = false;       // unsaved edits (preview refresh + save hint)

    // -- 3D mesh preview (Unreal-style mesh editor in the Asset Viewer) -----------
    // The viewed mesh renders into the RHI's preview scene (orbit with LMB,
    // zoom with the wheel); submesh material slots edit + save back to the .uaf.
    void EnsureMeshPreview(Engine& engine); // (re)load when the target changes
    std::filesystem::path previewPath_;     // currently loaded preview asset
    Model previewModel_;                    // CPU copy (slots + saving)
    std::vector<rhi::MeshHandle> previewMeshes_;   // one per submesh
    std::vector<MeshInstance> previewInstances_;   // materialized materials
    std::vector<rhi::DrawItem> previewDraw_;       // rebuilt per frame
    glm::vec3 previewCenter_{0.0f};
    f32 previewRadius_ = 1.0f;
    f32 previewYaw_ = 0.8f, previewPitch_ = 0.35f, previewZoom_ = 2.4f;
    bool previewMeshDirty_ = false;         // unsaved material-slot edits

    // -- Face select: give PART of a mesh its own material ------------------------
    // A submesh IS a MeshData with one Material, so "these faces get a different
    // material" is a mesh operation: split the chosen triangles into a new submesh.
    // The split APPENDS rather than reorders, because scenes reference submeshes
    // positionally as "uaf:<path>#<index>" and renumbering would silently repoint
    // every entity in every scene at different geometry.
    void DrawFaceSelectTools(Engine& engine);
    // glm rather than ImVec2 so this header stays free of imgui.h, which it does not
    // otherwise include.
    void PickPreviewFace(Engine& engine, glm::vec2 mouse, glm::vec2 imgMin, glm::vec2 imgSize,
                         bool add, bool remove);
    void RebuildPreviewGpu(Engine& engine);   // re-upload after the model changed
    void RefreshFaceHighlight(Engine& engine);
    bool faceSelectMode_ = false;
    usize faceSelectSubmesh_ = 0;             // selection belongs to ONE submesh
    std::vector<u32> faceSelection_;          // triangle indices, sorted, unique
    int faceSelectTool_ = 1;                  // 0 single, 1 linked, 2 similar facing
    f32 faceSelectAngle_ = 30.0f;             // degrees, for linked / similar
    // The highlight is ONE mesh, allocated once at full submesh capacity and then
    // updated in place - the RHI has no DestroyMesh, so creating one per selection
    // change would leak a handle on every click.
    glm::mat4 previewViewProj_{1.0f};         // the matrix actually submitted last frame
    glm::vec3 previewEye_{0.0f};
    rhi::MeshHandle faceHighlightMesh_;
    u32 faceHighlightCapacity_ = 0;
    u32 faceHighlightTris_ = 0;

    // -- Audio (mixer + event assets) --------------------------------------------
    // FMOD-style: the "Audio Mixer" panel edits the project's bus tree live;
    // .hbevent assets edit in the Asset Viewer and post through the mixer.
    void DrawAudioMixer(Engine& engine);

    // Adaptive music authoring ("Music" panel): edit a .hbmusic graph (states +
    // layers + parameters), set it as the project's music, and PREVIEW live - play a
    // state, drag a parameter and hear the layers fade. Backed by musicEdit_.
    void DrawMusicEditor(Engine& engine);
    MusicGraph musicEdit_;            // working copy of the project's music graph
    std::string musicEditPath_;       // .hbmusic path relative to Assets
    bool musicLoaded_ = false;        // musicEdit_ synced from the project this session
    int  musicStateSel_ = 0;          // selected state row
    bool musicPreviewing_ = false;    // a state is auditioning in the editor
    // DAW timeline (arrangement view of the selected state's layers over a bar grid).
    f32  musicZoom_ = 120.0f;         // pixels per second on the timeline
    f32  musicScroll_ = 0.0f;         // seconds at the timeline's left edge
    f32  musicEditTime_ = 0.0f;       // playhead (seconds; ticks while previewing)
    int  musicLayerSel_ = -1;         // selected layer lane in the timeline (-1 = none)
    // Creates Assets/<dir>/NewAudioEvent[N].hbevent and returns its path.
    std::filesystem::path CreateAudioEventAsset(const std::filesystem::path& dir,
                                                const std::string& name = {});
    // Creates <dir>/<name>.hbdialogue (a fresh branching graph = Start -> Line). dir
    // empty = Assets/Dialogue/.
    std::filesystem::path CreateDialogueAsset(const std::filesystem::path& dir = {},
                                              const std::string& name = {});
    bool mixerSynced_ = false;          // buses pushed to AudioSystem this project
    AudioEvent editedEvent_;            // working copy of a viewed .hbevent
    bool editedEventValid_ = false;
    bool editedEventDirty_ = false;
    DialogueAsset editedDialogue_;      // working copy of a viewed .hbdialogue
    bool editedDialogueValid_ = false;
    bool editedDialogueDirty_ = false;
    CutsceneAsset editedCutscene_;      // working copy of a viewed .hbcutscene
    bool editedCutsceneValid_ = false;
    bool editedCutsceneDirty_ = false;
    f32 cutsceneEditTime_ = 0.0f;       // timeline playhead (seconds)
    std::filesystem::path CreateCutsceneAsset(const std::filesystem::path& dir,
                                              const std::string& name = {});

    // -- Cutscene timeline panel ----------------------------------------------------
    // A dockable, artist-facing timeline: a horizontal time ruler, one lane per
    // track (camera, each animation entity, dialogue), keyframes drawn as
    // draggable diamonds, a scrubbable playhead, a selected-key inspector, and a
    // live viewport preview (evaluates cutscene::Evaluate into the scene/camera).
    std::filesystem::path editedCutscenePath_;   // file backing editedCutscene_
    bool cutsceneFocus_ = false;                 // request panel focus next frame
    cam::CinematicState csRig_;                  // preview-only rig state (handheld/breathing noise)
    f32 csZoom_ = 90.0f;                          // pixels per second
    f32 csScroll_ = 0.0f;                         // seconds at the lane's left edge
    // Selected item: kind 0=camera key, 1=transform key, 2=clip marker,
    // 3=dialogue marker; track = anim-track index (kinds 1/2); index = slot.
    int csSelKind_ = -1, csSelTrack_ = -1, csSelIndex_ = -1;
    // Which key the cursor is over, carried one frame (the hit test runs while drawing, so
    // the highlight lands on the next frame - imperceptible, and it means a user can SEE
    // what they are about to grab instead of guessing).
    int csHoverKind_ = -1, csHoverTrack_ = -1, csHoverIndex_ = -1;
    // Grab offset: the key's time MINUS the cursor's time at the moment of the press.
    // Without it a drag snaps the key to the cursor, so grabbing a key 10 px off-centre
    // teleported it by 10 px worth of time before the mouse had moved at all.
    f32 csDragGrabDt_ = 0.0f;
    bool csDragArmed_ = false;                   // pressed on a key, not yet past threshold
    bool csDragKey_ = false;                     // a key drag is in progress
    bool csDragPlayhead_ = false;                // playhead scrub in progress
    // Live preview: previewing owns the viewport; playing advances the playhead.
    bool csPreview_ = false;
    bool csPlaying_ = false;
    f32 csPrevTime_ = 0.0f;                      // last eval time (marker firing)
    std::string csPreviewSnapshot_;              // scene JSON to restore on stop
    void OpenCutscene(Engine& engine, const std::filesystem::path& path);
    void DrawCutsceneTimeline(Engine& engine);
    void CutscenePreviewBegin(Engine& engine);
    void CutscenePreviewEnd(Engine& engine);   // stop + restore the authored scene
    // Drop a live preview WITHOUT restoring: for paths that replace the whole
    // scene from another source (scene/level load, project switch, undo/redo),
    // where the snapshot is about to become stale and the scene is discarded.
    void CutscenePreviewAbandon() {
        csPreview_ = false; csPlaying_ = false; csPreviewSnapshot_.clear();
    }
    u32  lastPostedVoice_ = 0;          // editor Play test voice (stoppable)

    // -- Scene manager --------------------------------------------------------------
    void DrawSceneManager(Engine& engine);
    void RefreshScenes();
    std::vector<std::filesystem::path> sceneList_; // all .hbscene under Assets/
    std::vector<std::filesystem::path> uiDocList_; // all .hbui under Assets/
    bool scenesScanned_ = false;

    // -- UI documents (.hbui) ---------------------------------------------------
    // A `.hbui` is an ASSET, not scene content: its entities carry UIDocMember and
    // are excluded from every .hbscene / .hbprefab / .hbsave write, and both
    // Replace sweeps spare them. The editor opens them into the LIVE scene (so the
    // viewport lays them out and the gizmo/inspector work unchanged) through the
    // Engine's DocumentSet.
    //
    // activeDoc_ is the EDIT TARGET, and it is the whole separation guarantee:
    // the UI create menu and the UI Add-Component entries are disabled unless it
    // is set, so a UI component cannot be authored onto a world entity in the
    // first place. SaveSceneToDisk refuses (never silently drops) if one exists
    // anyway - a hand-edited file, or a scene authored before this landed.
    void DrawUIDocumentPanel(Engine& engine);
    ui::DocHandle activeDoc_ = 0;

    // -- Dedicated `.hbui` UI editor (Source/Editor/UIEditor.cpp) ----------------
    // A DEDICATED 2D authoring surface, not gizmos in the 3D viewport: the active
    // document is rendered by the REAL UI pass into its own render target and shown
    // as an ImGui image, so what the author sees cannot drift from what ships (see
    // ui::BuildDocumentVertices). Its own translation unit so Editor.cpp does not
    // grow further (it already needs /bigobj), following DialogueEditor.cpp.
    //
    // I1 = the canvas (zoom, pan, aspect preview, screen picker).
    // I2 = direct manipulation: select, move, 8-handle resize, the anchor widget,
    //      parent/sibling snapping - see uiEdDragMode_.
    // I3 = a real editor rather than a viewer: the element PALETTE (drag out or
    //      click, dropped where the cursor is), the DOCUMENT TREE with reparent by
    //      drag, TABS over every open document, clipboard + delete + z-order, and
    //      an INTERACT preview that runs the document through the real
    //      ui::UpdateInteraction without entering Play.
    void DrawUIEditorPanel(Engine& engine);
    // The right-hand strip: the numeric rect, the anchor preset grid, the pivot,
    // and the layout-group explanation. Same translation unit.
    void DrawUIEditorInspector(Engine& engine, f32 width);
    // The left strip: the element palette (a drag source per recipe) above the
    // open document's tree (select / reparent by drag / per-row tools).
    void DrawUIEditorPalette(Engine& engine, f32 width);
    void DrawUIEditorTree(Engine& engine);
    // The shared clipboard / delete / duplicate / z-order row, drawn in both the
    // left strip and the canvas's right-click menu so the two cannot diverge.
    void DrawUIEditorTools(Engine& engine, bool compact);
    // Abandons any in-flight gesture and drops the cached layout. Called on a
    // change of edit target and from RestoreSnapshot - after an undo every
    // DocHandle and every entt::entity is invalid (see the Snapshot comment
    // above), so cached per-element state must be dropped, never revalidated.
    void UIEditorInvalidate();
    f32  uiEdZoom_ = 0.0f;             // px per canvas unit; <= 0 = fit on next draw
    glm::vec2 uiEdPan_{0.0f};          // canvas image offset inside the view, px
    bool uiEdFitPending_ = true;       // refit at the next draw (open / doc change / F)
    int  uiEdAspect_ = 0;              // index into the aspect-preview presets (0 = document)
    ui::DocHandle uiEdLastDoc_ = 0;    // to detect an edit-target change and refit
    // Set by RestoreSnapshot: the documents were reopened, so every handle is new
    // even though the FILES are the same. The panel still compares handles (its tab
    // bar re-selects from that), but consumes this to keep the zoom and pan instead
    // of refitting - an undo must not throw away where the author was looking.
    bool uiEdPreserveView_ = false;
    // The authoring canvas's vertex buffer + layout for THIS frame. Members, not
    // locals: Renderer::SetEditorUICanvas borrows the vertex pointer until
    // RenderScene consumes it later in the same frame.
    std::vector<rhi::UIVertex> uiEdVerts_;
    std::vector<ui::LayoutItem> uiEdLayout_;
    std::string uiEdNote_; // budget / backend message shown in the panel
    // The canvas size uiEdLayout_ was built at. Input is hit-tested against the
    // PREVIOUS frame's layout (which is what the author was looking at when they
    // pressed, and lets this frame's rebuild show the drag with zero lag), so a
    // canvas that changed under it - a new aspect preview, a new document header -
    // must invalidate the mapping for one frame rather than mis-register a click.
    glm::vec2 uiEdLayoutCanvas_{0.0f};

    // -- Direct manipulation on the authoring canvas (phase I2) -------------------
    // Click to select; drag the body to move; drag one of eight handles to resize;
    // drag an anchor diamond to re-anchor. Every gesture edits a RECT in the item's
    // OWN canvas units and back-solves it into the RectTransform through
    // ui::SolveElementFromRect, so what the runtime computes is what the author
    // dragged - no screen-space fudge - and anchors + pivot survive a drag (a
    // stretched element stays stretched).
    //
    // Deliberately SEPARATE state from the viewport overlay's uiDrag*_: the two
    // surfaces can both be visible, they map the mouse through different
    // transforms, and one shared drag-in-progress would let a click in one finish a
    // gesture started in the other. `selected_` IS shared, on purpose - the
    // Hierarchy and the Inspector are how you edit everything a rect cannot express.
    //
    // GESTURE LIFECYCLE, and why it is armed rather than immediate: a press
    // SELECTS and classifies a candidate gesture but writes nothing. The first
    // mouse movement past ImGui's drag threshold promotes it - and THAT is where
    // the single PushUndo fires. So a plain click costs no undo slot (there are
    // only kMaxUndoSteps of them), a click-drag both selects and moves in one
    // motion, and a completed drag is exactly one undo entry.
    int  uiEdDragMode_ = 0;    // 0 none; see kUIEd* in UIEditor.cpp
    bool uiEdDragLive_ = false; // classified AND past the threshold: now writing
    bool uiEdPanning_ = false;  // this left-drag began on empty canvas, so it pans
    entt::entity uiEdDragEntity_ = entt::null; // who the gesture started on
    glm::vec2 uiEdPressScreen_{0.0f};          // screen px at press (threshold test)
    glm::vec2 uiEdDragStartMouse_{0.0f};       // canvas units at press
    glm::vec2 uiEdDragStartCenter_{0.0f};
    glm::vec2 uiEdDragStartSize_{0.0f};
    glm::vec4 uiEdDragParentRect_{0.0f}; // x0,y0,x1,y1 - what the item laid out in
    glm::vec2 uiEdDragCanvasSize_{1.0f}; // the dragged item's own canvas size
    // Anchors as they were at press. An anchor drag re-derives from these every
    // frame instead of accumulating, so it is idempotent and cannot creep.
    glm::vec2 uiEdDragStartAMin_{0.0f};
    glm::vec2 uiEdDragStartAMax_{0.0f};
    // Snap lines the last applied drag frame actually locked onto, in the dragged
    // item's canvas units. Drawn so the author can see WHY an edge stopped there.
    std::vector<f32> uiEdGuideX_, uiEdGuideY_;
    bool uiEdShowAnchors_ = true; // draw + allow dragging the anchor widget

    // -- Palette / tree / tools (phase I3) ---------------------------------------
public:
    // ONE authorable thing the palette can make. Deliberately a RECIPE, not a
    // component name: "Vertical Layout Group" is a Panel element plus a
    // UILayoutGroup plus the defaults that make it visible, and an author thinks
    // in those terms. Enumerated from the ACTUAL document component set
    // (UIElement's ten types + UICanvas / UIPanel / UILayoutGroup /
    // UICanvasGroup); WorldText is deliberately absent - it is not a `.hbui` key
    // at all (UIDocument.h decision 4), it is level signage placed by a Transform.
    // UIAnimator is absent too: it is a clip binding with no rect, so it belongs
    // to the Inspector's Add Component, not to a 2D layout palette.
    enum class UICreate : u8 {
        Panel = 0,
        Label,
        Button,
        Image,
        ProgressBar,
        ProgressWheel,
        Slider,
        Toggle,
        Selector,
        ScrollView,
        TextInput,
        Screen,          // + UIPanel: a NAMED SCREEN the UIManager can push/pop
        VerticalGroup,   // + UILayoutGroup{Vertical}
        HorizontalGroup, // + UILayoutGroup{Horizontal}
        GridGroup,       // + UILayoutGroup{Grid}
        FadeGroup,       // + UICanvasGroup: inherited opacity / interactivity
        Canvas,          // a UICanvas root (its own scale mode; world-space pages)
        Count
    };
    struct UICreateDesc {
        UICreate what;
        const char* label;    // palette button + menu item
        const char* entName;  // the Name component the new entity gets
        const char* tip;      // why an author would reach for it
        bool container;       // drawn in the palette's "containers" group
    };
    // The single source of truth for both the palette and the Hierarchy's
    // Create > UI menu, so the two cannot offer different sets or different
    // defaults. Defined in UIEditor.cpp next to the creation function.
    static const UICreateDesc* UICreateCatalog(int& count);

    // THE ONE CREATION PATH for `.hbui` content. Both the palette and the
    // Hierarchy menu call this; nothing else may create UI. Returns entt::null
    // (creating nothing, pushing no undo) when there is no active document - the
    // creation-time half of the separation guarantee. Pushes exactly one undo
    // entry, joins the document through DocumentSet::Track (tag AND order list),
    // selects the result and marks the document dirty.
    //
    // `parentHint` is where the author put it: the element under a palette drop,
    // or a tree row. See UIParentForNew for the fallback chain.
    entt::entity CreateUIElementInDocument(Engine& engine, UICreate what,
                                          entt::entity parentHint = entt::null);
    // The RECIPE half, with no Editor and no Engine in it: `parent` is already
    // resolved and the undo / selection / dirty bookkeeping is the caller's. Split
    // out so --test-uieditor can exercise every recipe for real - a test that
    // re-implemented the defaults would only prove it agrees with itself.
    static entt::entity UIBuildRecipe(Scene& scene, ui::DocumentSet& docs,
                                      ui::DocHandle doc, UICreate what,
                                      entt::entity parent);

private:
    // Resolves the parent a newly created element should get, in order: the hint's
    // nearest UI ancestor in this document, the selection's, the screen the canvas
    // is currently showing, the document's first UICanvas, else entt::null = a
    // DOCUMENT ROOT.
    //
    // That last step is a deliberate change from the old create menu, which minted
    // a "Canvas" entity when the document had none. The reference project's menu
    // has ZERO UICanvas entities - its screens are canvas-less roots laying out
    // against the document header - so auto-creating one injected a stray root
    // into exactly the documents that are built the other way, moving entities from
    // the canvas-less walk to the canvas walk (and thus changing draw order and
    // world-space routing). A root is what those documents are made of.
    // Not static: it consults `selected_` (the editor-wide selection) as one of
    // its fallbacks.
    entt::entity UIParentForNew(Scene& scene, ui::DocHandle doc,
                                entt::entity hint) const;

    // Z-ORDER. Draw order is the order the layout walk visits siblings, which is
    // entt POOL order (canvas-less roots come from view<UIElement>, children from
    // view<Parent>), and no file records it directly - a document's ENTITY ORDER
    // reproduces it because InstantiateDocument creates in that order. So a
    // reorder has to move the entity in BOTH: swap_elements in the governing pool
    // (immediately visible) and the same swap in DocumentInstance::entities (what
    // CaptureDocument writes). Doing both keeps the two in step, which is the only
    // reason the result survives a save; --test-uieditor round-trips it.
    //
    // `dir` > 0 = toward the FRONT (later in the layout), < 0 = toward the back.
    // `run` = go as far as it can (Bring to Front / Send to Back).
    // The neighbour is taken from uiEdLayout_, i.e. from what is actually drawn,
    // so this does not depend on which direction entt happens to iterate a pool.
    void UIReorder(Engine& engine, entt::entity e, int dir, bool run);
    // Swaps `a` and `b` in every pool that governs their order plus the document
    // order list. Both must be members of the same document and the same kind of
    // sibling (both roots, or both children of one parent); when NO pool governs
    // both it does nothing at all rather than desynchronise the saved order from
    // the live one. Engine-free so the self-test drives the real thing.
    static void UISwapOrder(Scene& scene, ui::DocumentSet& docs, entt::entity a,
                            entt::entity b);

    // INTERACT PREVIEW: run the document through the REAL ui::UpdateInteraction
    // (hover, click, toggle, slider drag, selector cells, scroll wheel) against
    // the authoring canvas, without entering Play. Its own UIContext, never the
    // Engine's: this is an extra pass inside a frame whose runtime pass already
    // ran, and sharing the context would corrupt the runtime's touched-flag list
    // and its stats.
    bool uiEdInteract_ = false;
    ui::UIContext uiEdInteractCtx_;
    // Interaction WRITES authored fields in place - value / toggled / selected /
    // scrollPos are the widget's serialized initial state, and the runtime
    // overwrites them. Testing states must not silently re-author the document, so
    // the four are snapshotted when Interact goes on and restored when it goes
    // off (or the document changes, or the panel closes, or an undo lands).
    struct UIPreviewState {
        entt::entity e = entt::null;
        f32 value = 0.0f;
        bool toggled = false;
        int selected = 0;
        glm::vec2 scrollPos{0.0f};
    };
    std::vector<UIPreviewState> uiEdPreview_;
    // A tab X was clicked on a document with unsaved work: the close waits for the
    // Save / Discard / Cancel modal. DocumentSet::Close destroys every member entity
    // and pushes no undo entry, so this is the only thing standing between the X and
    // a lost layout. 0 = nothing pending.
    ui::DocHandle uiEdCloseConfirm_ = 0;
    void UIEditorBeginInteract(Engine& engine);
    void UIEditorEndInteract(Engine& engine);

    bool uiEdShowPalette_ = true; // left strip (palette + document tree)
    // The screen being authored, by UIPanel NAME rather than by entity. EditorUIShow
    // is a session-only tag on a live entity, so every undo, every document reopen
    // and every play-snapshot restore destroys it and the canvas went blank until
    // the author re-picked from the combo. The NAME is the document's own stable id
    // (it is what the UIManager pushes), so re-applying from it survives all three.
    // Cleared when the author explicitly picks "(as the game left it)".
    //
    // PER DOCUMENT, keyed on DocumentInstance::rel. It was ONE name for the whole
    // editor, which worked while there was one open document holding N panels.
    // After the screen split there are N open documents holding ONE panel each, so
    // a single name can match in at most one of them: after any undo (RestoreSnapshot
    // closes and REOPENS every document) three of the four tabs came back with a
    // blank canvas. `rel` is the right key because DocHandle is NOT stable across
    // that reopen - OpenFromData mints a new one.
    std::unordered_map<std::string, std::string> uiEdShownScreen_;
    // Key for the map above: the active document's `rel`, falling back to its
    // handle for a never-saved "New" (which no reopen can outlive anyway).
    std::string UIEdDocKey(const ui::DocumentSet& docs) const;
    // Deferred structural edits: applied after the tree/canvas walks, because the
    // rows and the layout list are snapshots and destroying mid-walk would leave
    // stale entities behind them.
    entt::entity uiEdPendingDelete_ = entt::null;
    entt::entity uiEdPendingReparentChild_ = entt::null;
    entt::entity uiEdPendingReparentTo_ = entt::null;
    bool uiEdPendingUnparent_ = false; // distinguishes "to entt::null" from "none"
    int uiEdPendingCreate_ = -1;       // UICreate index queued by a palette drop
    entt::entity uiEdPendingCreateParent_ = entt::null;
    glm::vec2 uiEdPendingCreateAt_{0.0f}; // canvas units of the drop point
    bool uiEdPendingCreatePlace_ = false; // place it at uiEdPendingCreateAt_

    // -- Streaming tags -------------------------------------------------------
    // The project's tag list (ProjectSettings::tags) is the streaming CONFIG;
    // the Inspector's Tag combo is the per-object assignment. Editing the list
    // re-seeds the process-wide table (tags::SeedFromProject) because a TagId is
    // an index into it, and deleting a row goes through tags::RemoveTag so live
    // entities are remapped rather than silently repointed at the next tag.
    void DrawTagsPanel(Engine& engine);
    // The live-zone switchboard inside it (bind/unbind, follow-the-camera, the
    // forgotten-override banner).
    void DrawLiveZoneControls(Engine& engine);
    // Called from every site that mutates TagDef::associates. Re-resolves the bound
    // streamer's association graph AND the panel's prediction, so an edit takes effect
    // where the author is looking instead of at the next save.
    void OnAssociationsEdited();
    char tagNewName_[64] = {};   // "New Tag..." / Add-tag name buffer
    bool tagNewPopupOpen_ = false; // request to open the Inspector's naming modal
    // Creates `name` in the project's tag list (no-op when it already exists),
    // re-seeds the table and SAVES the `.hbproj` - the tag list is project data,
    // so an unsaved new tag would be lost on the next open. Returns its id.
    TagId CreateProjectTag(const std::string& name);
    // Deferred "Save As" / "New" naming, applied outside the ImGui draw.
    char uiDocNameBuf_[128] = {};
    ui::DocHandle uiDocSaveAs_ = 0;
    bool wantUIDocNew_ = false;
    std::string uiDocError_; // shown in the panel + the save-refusal modal
    // True when `e` is inside an open document (i.e. it is UI ASSET content).
    static bool IsDocumentEntity(const Scene& scene, entt::entity e);
    // "New scene" teardown: destroys the world but leaves every open document's
    // entities alive. Replaces the bare `Registry().clear()` the two New handlers
    // used to call, which orphaned open documents into 0-entity saves.
    static void ClearWorldSparingDocuments(Scene& scene);
    // Refuses with a message when a non-document entity carries one of the six
    // document components. This is the SaveScene half of the guarantee.
    static bool AuditSceneForLooseUI(const Scene& scene, std::string& why);
    // The MIRROR of that audit, on the document side: refuses when a document
    // member carries a component CaptureDocument cannot write (a MeshInstance, a
    // collider, a 3D sign, a Health...). Without it those components exist in no
    // file at all - BuildSceneJson skips the entity and the capture whitelist
    // drops the component - so they vanish on the next save+reopen, which is
    // exactly the silent-drop behaviour the cancelled U5 was rejected for.
    static bool AuditDocumentForWorldContent(const Scene& scene, ui::DocHandle doc,
                                             std::string& why);
    // Saves an open document back to its file (Save As when `as` is non-empty).
    // Flags the document that OWNS `e` (via UIDocMember) as edited, so the UI
    // Document panel shows its `*`. A no-op for a world entity or an untracked one.
    //
    // Both drag surfaces mutate UIElement components in place, which no save path
    // can observe, so the `*` used to never appear for a layout edit. SaveAll no
    // longer trusts this flag (it saves every open document regardless) - this is
    // the author-facing signal, not a correctness gate.
    void MarkDocumentDirty(Engine& engine, entt::entity e);
    bool SaveUIDocument(Engine& engine, ui::DocHandle doc,
                        const std::filesystem::path& as = {});
    std::filesystem::path renameScene_;  // pending rename target
    char renameBuf_[128] = {};

    // -- Schematic (visual scripting) editor -----------------------------------------
    // Node-graph editor for the project's .hbschem assets.
    void DrawSchematicEditor(Engine& engine);

    void DrawSchematicCanvas();        // the node canvas (links/nodes/interaction)
    void OpenSchematic(const std::filesystem::path& path);
    // Returns whether a file was WRITTEN. A failed write must not be reported as a
    // save (same reason SaveCurrent returns bool).
    bool SaveSchematic();
    // Creates <dir>/<name>.hbschem (a fresh graph seeded with On Update). `dir`
    // empty = Assets/Schematics/.
    std::filesystem::path CreateSchematicAsset(const std::filesystem::path& dir = {},
                                               const std::string& name = {});

    // --- Schematic (visual scripting) editor state ---
    std::filesystem::path editedSchematic_;   // open .hbschem (empty = none)
    schematic::Graph schematicGraph_;         // working copy
    bool schematicDirty_ = false;
    bool schematicFocus_ = false;             // focus panel when a graph opens
    glm::vec2 schemPan_{0.0f, 0.0f};          // canvas pan offset
    glm::vec2 schemAddPos_{0.0f, 0.0f};       // canvas-space pos for "Add Node"
    u32 schemSelected_ = 0;                   // selected node id (0 = none)
    u32 schemDragNode_ = 0;                   // wire-drag source node id
    u32 schemDragPin_ = 0;                    // wire-drag source pin index
    bool schemDragFromOutput_ = false;        // dragging from an output pin
    bool schemDragging_ = false;

    // --- Dialogue graph editor (its own window; same look as the schematic one) ---
    void DrawDialogueEditor(Engine& engine); // toolbar + canvas + node inspector
    void DrawDialogueCanvas(float width);    // node canvas (links/nodes/interaction)
    void OpenDialogue(const std::filesystem::path& path);
    bool SaveDialogue();                     // writes dlgGraph_ back to editedDialoguePath_
    std::filesystem::path editedDialoguePath_; // open .hbdialogue graph (empty = none)
    dlg::Graph dlgGraph_;                      // working copy
    bool dlgDirty_ = false;
    bool dlgFocus_ = false;                    // focus the panel when a graph opens
    glm::vec2 dlgPan_{0.0f, 0.0f};
    glm::vec2 dlgAddPos_{0.0f, 0.0f};
    u32 dlgSelected_ = 0;
    u32 dlgDragNode_ = 0;
    u32 dlgDragPin_ = 0;
    bool dlgDragFromOutput_ = false;
    bool dlgDragging_ = false;

    // -- Project settings (environment / skybox / lighting) -----------------------
    // Edits Project::Settings().environment; "Rebuild Sky" regenerates the
    // procedural sky + IBL so changes are visible immediately.
    void DrawProjectSettings(Engine& engine);

    // Saves the whole project: the active scene (Save As if never saved) plus the
    // project settings (.hbproj: environment, build, audio, startup scene), every
    // open `.hbui` document, and every file-backed asset editor that has something
    // open. Ctrl+Shift+S. Focus is irrelevant to "save everything", so it is the one
    // chord no panel may claim.
    void SaveAll(Engine& engine);

    // -- Ctrl+S DISPATCH -------------------------------------------------------
    // Ctrl+S saves the surface that owns the FOCUSED window, and exactly that.
    //
    // MECHANISM, and why it is imgui INPUT ROUTING and not IsWindowFocused(). The
    // global keyboard block in BuildUI runs before the dockspace and before every
    // Draw*Panel, where the "current window" is imgui's implicit fallback window -
    // ImGui::IsWindowFocused() is meaningless there, so a central dispatcher could
    // only ever read LAST frame's focus. Worse, a hand-rolled focus gate cannot
    // ARBITRATE: ImGui::IsKeyPressed() is frame state and is not consumed by
    // reading it, so the two panel-local Ctrl+S handlers that used to exist fired
    // IN ADDITION to the global one (schematic/dialogue: graph AND scene on one
    // keypress; a fresh project even popped Save-Scene-As over the graph editor).
    //
    // ImGui::Shortcut() grants a chord to exactly ONE owner per frame, and
    // RouteFocused outscores RouteGlobal, so:
    //   * every panel that owns a savable surface calls ClaimSave() right after its
    //     Begin(), UNCONDITIONALLY (even with nothing open - see below);
    //   * the global Ctrl+S route is the scene fallback and only fires when no panel
    //     claimed;
    //   * Ctrl+Shift+S registers ONE global route and is unclaimable. Shortcut()
    //     matches modifiers exactly, so a Ctrl+S route never fires on Ctrl+Shift+S -
    //     which is the other half of the old double-fire, for free.
    // Forgetting to claim in a new panel degrades to today's behaviour (the scene
    // saves): a missing feature, never a wrong write.
    //
    // The claim is unconditional so that a focused-but-empty surface reports
    // "nothing open to save" instead of silently writing the LEVEL - the author
    // believing they saved a graph is the exact bug class this replaces.
    //
    // KNOWN, ACCEPTED: imgui settles route arbitration with a one-frame delay (a
    // route registered this frame is scored against last frame's table). Focus is
    // gained by a click, which is itself a prior frame, so a human cannot reach it.
    // A per-DOCUMENT undo stack for the asset editors.
    //
    // The scene's PushUndo cannot serve these, and not merely because it is coarse:
    // CaptureSnapshot records the registry plus every open `.hbui`, and the four asset
    // documents (dlg::Graph, schematic::Graph, CutsceneAsset, MusicGraph,
    // CharacterAsset) live in plain Editor members that are in NEITHER. So Ctrl+Z in
    // the Dialogue Editor could never undo the edit just made, and always reverted an
    // unrelated scene edit instead - a no-op that also destroyed other work.
    //
    // Every one of those documents is a plain copyable value, so a snapshot is just a
    // copy: no serializer, no GPU touch, no allocation beyond the vectors themselves.
    template <class T>
    struct AssetHistory {
        std::vector<T> undo, redo;
        // Call with the document's CURRENT state BEFORE applying an edit - the same
        // ordering the inspector's undoOnActivate idiom uses (snapshot on
        // IsItemActivated, i.e. when the widget is grabbed, not after it changes).
        void Push(const T& before) {
            undo.push_back(before);
            if (undo.size() > 64) undo.erase(undo.begin());
            redo.clear(); // a new edit invalidates the redo branch
        }
        bool Undo(T& live) {
            if (undo.empty()) return false;
            redo.push_back(live);
            live = std::move(undo.back());
            undo.pop_back();
            return true;
        }
        bool Redo(T& live) {
            if (redo.empty()) return false;
            undo.push_back(live);
            live = std::move(redo.back());
            redo.pop_back();
            return true;
        }
        // Opening a DIFFERENT document must drop the history, or Ctrl+Z would paste
        // the previous file's contents over the current one.
        void Clear() {
            undo.clear();
            redo.clear();
        }
    };
    AssetHistory<dlg::Graph> dlgHistory_;
    // Cutscene undo. Snapshots the WHOLE asset: a cutscene is a few hundred keys at most,
    // so a full copy per edit is far cheaper than tracking per-field deltas, and it cannot
    // desynchronise the way an incremental log can.
    AssetHistory<CutsceneAsset> csHistory_;
    // Set while a drag is in flight so the snapshot is taken ONCE, at the grab, rather
    // than every frame the mouse moves - otherwise one drag fills the whole 64-entry
    // history with intermediate positions and Ctrl+Z walks back a pixel at a time.
    // The asset as it was at the START of this frame, held so an edit can be undone
    // without every one of the ~29 mutation sites in the panel having to remember to
    // snapshot. Wiring them individually is precisely how one gets missed - and a missing
    // snapshot is invisible until someone needs the undo.
    CutsceneAsset csFrameSnapshot_;
    bool csSnapshotValid_ = false;
    bool csEditedThisFrame_ = false;

    bool ClaimSave(editor::SaveSurface surface);
    // Registers EVERY focus-routed chord this panel owns - Ctrl+S plus the six edit
    // chords - in one call. Panels call THIS instead of ClaimSave, in the same
    // position (immediately after Begin(), ABOVE every early return) and for the same
    // reason: Begin() pushes the focus scope unconditionally, so a collapsed-but-
    // focused window must still claim or the global fallback writes/edits the scene
    // while the focused, titled window says "Dialogue Editor".
    void ClaimFocus(editor::SaveSurface surface);
    void ProcessEditRequest(Engine& engine);
    // Per-verb, because two chords can be granted to two different owners in the same
    // frame (an active InputText owns Ctrl+Z at route score 300 while the panel owns
    // Ctrl+D at 199). One byte each; a single shared claim would mis-attribute.
    editor::SaveSurface editVerbSurface_[static_cast<usize>(editor::EditVerb::Count)]{};
    u8 editVerbs_ = 0; // bitmask of verbs claimed this frame
    // The three facts DecideEdit needs that SurfaceHasContent does not cover.
    bool SurfaceHasSelection(editor::SaveSurface s) const;
    bool SurfaceClipboardEmpty(editor::SaveSurface s) const;
    bool SurfaceHistoryEmpty(editor::SaveSurface s, editor::EditVerb v) const;
    // Ctrl+S fallback + Ctrl+Shift+S. Called from the global keyboard block.
    void RegisterSaveShortcuts(Engine& engine);
    // End of frame, after every panel has had its chance to claim: turn the claim
    // into a SaveContext, ask editor::DecideSave, execute the one action it returns.
    void ProcessSaveRequest(Engine& engine);
    // Which of the Asset Viewer's three sub-editors is live right now.
    editor::SaveSurface AssetViewerSurface() const;
    // Does `s` actually have something open. Resolved at EXECUTION time, not at
    // claim time, so a panel only ever has to answer "I am focused".
    bool SurfaceHasContent(Engine& engine, editor::SaveSurface s) const;
    // The one author-facing save message. Errors/refusals are red and do not fade.
    void SetSaveStatus(std::string msg, bool error);
    void DrawSaveToast();
    // SaveCurrent + the status line, in one place. Every scene-save button and the
    // Ctrl+S dispatcher go through it, so a REFUSAL can no longer be discarded at
    // three of the five call sites (Scene menu / Scenes panel / Art Editor) the way
    // it silently was. Returns whether a file was written.
    bool SaveSceneWithStatus(Scene& scene);

    editor::SaveSurface saveClaim_ = editor::SaveSurface::None; // who won the chord
    bool saveRequested_ = false;                                // ...this frame
    editor::SaveSurface pendingSaveSurface_ = editor::SaveSurface::None;
    bool pendingSave_ = false; // deferred across a text field's commit frame
    // Deliberately NOT buildResult_: a save must not clobber build output, and the
    // build result renders only in Stats / Build Settings, which an author editing
    // UI is not looking at.
    std::string saveStatus_;
    f64 saveStatusTime_ = -1.0; // ImGui::GetTime() when it was set (<0 = never)
    bool saveStatusError_ = false;
    // Bumped by every SetSaveStatus. A WRAPPER'S GENERIC MESSAGE MUST NOT OVERWRITE
    // A SPECIFIC ONE: SaveSceneToDisk refuses for five different reasons and four of
    // them set a precise, actionable line (which shards are missing, that a preview
    // is posing the scene, that the world is empty), while only one - loose UI in a
    // non-document entity - deliberately sets none. SaveSceneWithStatus and SaveAll
    // used to react to `false` by stamping "See the UI Document panel" over all of
    // them, so the author read the wrong cause on the only surface they were looking
    // at. Snapshot this before the call, compare after, and only fill in the generic
    // line when nothing else spoke.
    u32 saveStatusSeq_ = 0;
    // The viewport the toast should appear on: with multi-viewport enabled a panel
    // dragged onto a second monitor is its own OS window, and a confirmation pinned
    // to the main viewport lands where the author is not looking - which defeats the
    // entire point of a save toast in the one setup that needs it most.
    u32 saveStatusViewport_ = 0; // ImGuiViewport::ID, 0 = the main viewport

    // The extracted per-surface savers. Each one is the SAME code its panel button
    // runs (the button now calls these), so the chord and the button can never
    // drift, and SaveAll can cover them without duplicating a sort or a cache clear.
    bool SaveCutsceneAsset();          // sorts every track by time, then writes
    bool SaveMusicAsset();             // + the .hbproj keys the music graph owns
    bool SaveCharacterAsset();         // + character::ClearCache (re-weld)
    bool SaveViewedMaterial(Engine& engine); // + re-applies it to every wearer
    bool SaveViewedAudioEvent();
    bool SaveViewedMesh();             // material slots back into the .uaf

    // -- Terrain editor (sculpt brushes) ------------------------------------------
    // When the sculpt tool is active and a Terrain entity is selected, dragging
    // LMB over the viewport raises/lowers/smooths/flattens the heightfield under
    // a falloff brush; the brush ring previews the affected area. Called from
    // BuildUI after the viewport draws.
    void UpdateTerrainTool(Engine& engine);
    bool terrainSculpt_ = false;     // tool active
    int  terrainBrush_ = 0;          // terrain::Brush: 0 raise,1 lower,2 smooth,3 flatten
    f32  terrainRadius_ = 5.0f;      // brush radius (world units)
    f32  terrainStrength_ = 6.0f;    // raise/lower units/s; smooth/flatten rate
    f32  terrainFlatten_ = 0.0f;     // captured flatten target height (stroke start)
    bool terrainStroking_ = false;   // a sculpt stroke is in progress
    bool terrainConsumedClick_ = false; // suppress entity picking this frame
    glm::vec3 terrainHit_{0.0f};     // last brush world hit (overlay)
    bool terrainHitValid_ = false;

    // -- Art Editor (surface painting) -------------------------------------------
    // A unified workflow for 2D artists: paint pigment + relief directly onto any
    // mesh's surface (UV-space texture painting). The panel holds the brush, a
    // colour wheel, a live brush preview, and the paintable-object list (per-object
    // show / hide / lock). UpdateArtTool stamps onto the canvas under the cursor.
    void DrawArtEditor(Engine& engine);
    void UpdateArtTool(Engine& engine);
    // Adds a PaintComponent + blank canvas to a MeshInstance entity.
    void MakePaintable(Scene& scene, Renderer& renderer, entt::entity e);
    // CPU geometry (positions + UVs) for an entity's mesh, resolved from MeshRef
    // ("prim:" / "uaf:") and cached by source; null when unavailable. Powers the
    // brush raycast.
    const MeshData* GetCpuMesh(Scene& scene, entt::entity e);
    std::unordered_map<std::string, MeshData> cpuMeshCache_;
    // TERRAIN is the one paintable surface GetCpuMesh can never answer for: chunk
    // meshes are generated procedurally into a GPU buffer and the CPU copy is
    // dropped, so a chunk has no MeshRef and no cacheable source. That is why
    // painting on terrain used to do nothing at all - the brush bailed out one line
    // before the code that knew about terrain.
    //
    // Resolves `e` (a chunk OR the terrain entity) to the terrain entity that owns
    // the heightfield and the paint canvas; entt::null when `e` is not terrain.
    entt::entity TerrainPaintOwner(Scene& scene, entt::entity e);
    // World-space wrapper over paint::RaycastTerrain (which is where the geometry
    // lives, and is covered by --test-terraincollide). `terrainEntity` must come from
    // TerrainPaintOwner; the hit comes back in TERRAIN-local space.
    bool RaycastTerrainSurface(Scene& scene, entt::entity terrainEntity,
                               const glm::vec3& worldOrigin, const glm::vec3& worldDir,
                               paint::PaintHit& out);

    bool paintActive_ = false;        // paint tool engaged
    bool paintErase_ = false;         // erase paint instead of adding
    bool paintAutoCreate_ = true;     // every mesh paintable: make a canvas on first stroke
    // Brush library: custom presets (persisted to <project>/brushes.json), the
    // editable working brush, and its baked tip.
    std::vector<paint::BrushDef> brushes_;
    int  brushIndex_ = 0;             // selected preset in brushes_
    paint::BrushDef brushDef_;        // editable working copy
    paint::BrushTip brushTip_;        // baked from brushDef_
    bool brushDirty_ = true;          // rebake brushTip_ from brushDef_
    bool brushesLoaded_ = false;      // library loaded for the active project
    char brushNameBuf_[64] = "My Brush";
    void EnsureBrushes();             // seed defaults / load the project library
    void LoadBrushes();
    void SaveBrushes() const;
    void SelectBrush(int index);      // load preset -> working brush + tool defaults
    void DrawBrushEditor();           // preset library + param editor (in Art Editor)
    glm::vec4 brushColor_{0.82f, 0.24f, 0.27f, 1.0f};
    bool colorPickMode_ = false;      // eyedropper armed: next scene click samples a pixel
    f32  brushRadius_ = 0.35f;        // brush radius (world units)
    f32  brushFlow_ = 0.6f;           // 0..1 paint build-up per stamp
    f32  brushHeight_ = 0.2f;         // relief raised per stamp (impasto)
    f32  brushColorVar_ = 0.25f;      // broken colour: per-dab value/warm-cool jitter (oil feel)
    // Painted PBR material (alongside the albedo colour).
    f32  brushMetallic_ = 0.0f;
    f32  brushRoughness_ = 0.5f;
    bool brushPaintColor_ = true;     // lay down albedo
    bool brushPaintMaterial_ = false; // lay down metallic/roughness/relief
    f32  paintOpacity_ = 1.0f;        // applied to new canvases
    bool paintReliefDefault_ = true;  // new canvases apply relief (toggleable)
    f32  paintHeightScale_ = 0.12f;   // relief display strength of new canvases (impasto)
    f32  paintLodBias_ = 1.0f;        // distance averaging of new canvases
    bool paintSelectedOnly_ = false;  // mask: paint only the selected object
    std::string paintActiveLayer_ = "All"; // brush layer mask ("All" = no mask)
    char paintNewLayerBuf_[64] = "Layer 1"; // name field for "New Layer"
    int  paintRes_ = 1024;            // new-canvas resolution
    bool paintStroking_ = false;      // a paint stroke is in progress
    bool paintConsumedClick_ = false; // suppress entity picking this frame

    // Paint-in-space (grease-pencil 3D strokes): drag in screen space and the stroke
    // becomes its OWN free-floating ribbon entity. The first cursor-ray hit sets a
    // DRAW PLANE (depth from the surface under it, orientation = camera-facing); every
    // later point projects onto that plane, so the stroke floats in real space and is
    // NOT conformed to the mesh. Lit + transparent + double-sided, with a baked oil
    // brush-streak texture - real brush marks in 3D, viewable from any angle.
    // Default OFF: opening the Paint tool paints the SURFACE texture (the simple,
    // expected mode). 3D brush strokes are opt-in via the "3D brush strokes" checkbox,
    // so they don't surprise the user by spawning floating ribbon geometry.
    bool paintStrokeMode_ = false;
    rhi::MeshHandle strokeQuadMesh_;  // shared oriented-quad mesh
    std::unordered_map<std::string, std::string> strokeMatCache_; // sig -> .hbmat rel path
    int strokeMatCounter_ = 0;
    void SpawnStroke(Engine& engine, const glm::vec3& hitWorld, const glm::vec3& worldNormal);
    // Builds a spline ribbon entity from the drawn path (>= 2 points), or a single
    // quad for a tap. Saves the ribbon mesh as a .uaf so the stroke persists.
    void BuildSplineStroke(Engine& engine);
    // Bakes the current brush tip+colour to a transparent stroke .hbmat (+ tip
    // .uaf), reused across identical strokes. Returns the .hbmat path rel to Assets.
    std::string EnsureStrokeMaterial();
    // Like EnsureStrokeMaterial but bakes a LONG bristle-streak texture (soft across,
    // bristle + value variation along) for ribbon strokes - reads as loaded oil paint.
    std::string EnsureRibbonMaterial();
    std::vector<glm::vec3> strokePath_;   // drawn path points (world space, on draw plane)
    std::vector<glm::vec3> strokePathN_;  // draw-plane normal at each point (camera-facing)
    bool strokeDrawing_ = false;          // a spline stroke is being drawn
    glm::vec3 strokePlaneP_{0.0f};        // draw-plane point (first-hit depth)
    glm::vec3 strokePlaneN_{0,0,1};       // draw-plane normal (camera forward at start)
    // The SURFACE the stroke was started on - the same first hit that set the draw
    // plane above, which used to be computed and thrown away. It is the stroke's
    // ZONE: strokezone::ZoneOfSurface walks up its parents for a streaming Tag, and
    // the stroke is grouped and tagged accordingly (Scene/StrokeZone.h). Stroke
    // START, not release: one ribbon is one mesh is one entity is one tag, and it
    // already lies on the plane of the surface it started on.
    entt::entity strokeHitEntity_ = entt::null;
    int strokeMeshCounter_ = 0;           // unique ribbon .uaf names
    // Both counters are seeded from what is ALREADY in Assets/Strokes/ at project
    // open. Plain `= 0` members meant every session restarted naming at
    // `ribbon_0.uaf`, so loading a saved scene and painting one stroke silently
    // overwrote an existing stroke's mesh under the entity still referencing it -
    // and `strokeMeshCounter_` also seeds the per-stroke noise, so the "random"
    // jitter repeated across sessions. Harmless only while no scene referenced a
    // stroke; zoning makes strokes content worth keeping.
    bool strokeCountersSeeded_ = false;
    void SeedStrokeCounters();
    // Parents a freshly created stroke into the group node for the zone of
    // `strokeHitEntity_`, tagging both. The ONE grouping site: the tap path and the
    // ribbon path must not disagree, and before this they did - SpawnStroke never
    // parented at all, so tap strokes were loose untagged roots.
    void AttachStrokeToZone(Engine& engine, entt::entity stroke);
    glm::vec2 paintLastUV_{0.0f};     // previous stamp UV (continuous strokes)
    bool paintHasLast_ = false;       // paintLastUV_ is valid this stroke
    int  paintSyncTick_ = 0;          // throttles GPU re-upload during a stroke
    entt::entity paintTarget_ = entt::null; // canvas being stroked (flush on release)
    glm::vec3 paintLastLocal_{0.0f};      // previous dab local position (projection mode)
    glm::vec3 paintLastNormal_{0,1,0};    // previous dab local normal (projection mode)
    f32 paintLastLocalRadius_ = 0.0f;     // previous dab local radius (projection mode)

    // Stroke recording: the in-progress stroke is accumulated here and committed to
    // the target's stroke database on release (the database is the source of truth;
    // see PaintComponent::strokes / paint::BakeFromStrokes).
    paint::Stroke curStroke_;
    bool          curStrokeActive_ = false;
    // Commits a finished stroke/fill/clear to entity `e`'s database + the global
    // paint-order log (for undo), and clears the redo stack.
    void CommitStroke(entt::entity e, PaintComponent& pc, paint::Stroke&& s);

    // Stroke-level undo: the global order of committed strokes (which entity each
    // belongs to) and the redo stack of popped strokes. Undo pops the last stroke
    // and rebakes that canvas - no pixel snapshots (the stroke DB makes that free).
    std::vector<entt::entity> paintStrokeOrder_;
    std::vector<std::pair<entt::entity, paint::Stroke>> paintStrokeRedo_;
    void PaintUndo(Engine& engine);
    void PaintRedo(Engine& engine);

    // Saves the ACTIVE scene (entities with no SceneSource tag) to `path`,
    // writing its paint canvases first, then writes each streamed-in scene's
    // entities back to its own file. Streamed entities are never merged into the
    // active scene.
    //
    // REFUSES, before writing anything at all, when the world it is looking at
    // cannot produce a complete file: loose (pre-.hbui) UI, Play mode, a world that
    // was replaced behind the editor's back, a streamed world with despawned shards,
    // or an empty world over a non-empty file. Never writes a partial scene.
    bool SaveSceneToDisk(Scene& scene, const std::filesystem::path& path);
    // "The registry currently in `scene` IS the world at currentScenePath_."
    //
    // Called from every place the EDITOR deliberately replaces the world with the
    // same level - a scene load, an undo/redo or Play->Stop restore, New Scene, and
    // a successful save. Anything else that Replaces (a checkpoint load, a level
    // bind) leaves the token behind and the next save refuses instead of writing one
    // world over another's file. See Scene::WorldToken.
    void AdoptWorld(const Scene& scene);
    // Saves whatever is open: the level (both layer files) when a level is open,
    // else the active scene in place, else opens "Save Scene As" (never saved yet).
    // The single save entry point for all the Save buttons / Ctrl+S.
    // Returns true only when a file was actually written - SaveSceneToDisk REFUSES
    // when a non-document entity carries a UI component, and callers must not
    // report success on a refusal.
    bool SaveCurrent(Scene& scene);
    // Writes every distinct streamed-in scene (SceneSource-tagged) back to its
    // own .hbscene with just that scene's entities. Returns false on any failure.
    bool SaveStreamedScenes(Scene& scene);
    // Logs the save-time shard bake's findings and keeps them for the Tags panel.
    // NON-BLOCKING by design - see tagshard::Severity.
    void ReportShardBake(const tagshard::BakeReport& rep, const std::filesystem::path& path);
    // Last shard bake's author-facing lines + per-tag numbers (the scattered-tag
    // failure mode is invisible without them). Shown in the Tags panel.
    std::vector<std::string> shardDiagnostics_;
    std::vector<tagshard::TagStat> shardStats_;
    // The last bake's SHARD BOXES, which is what makes streaming visible at all: the
    // author sets tags, the sharder decides the geometry, and without seeing the boxes
    // and the radii they are authoring blind. Refreshed on every save (and by the Tags
    // panel's "Re-bake now"), so it always describes the file on disk.
    std::vector<scene::ShardDesc> shardDescs_;

    // -- Streaming SIMULATION (editor-only) ---------------------------------------
    // The editor runs the REAL stream::Evaluate against the real baked shard boxes with
    // a movable focus, and colours the overlay by the result. It deliberately does NOT
    // spawn or despawn anything: a streamer in the editor would have to BindWorld, which
    // destroys the scene the author is editing. So this answers "which shards would be
    // resident here, and why", and --tagstreamtest / the runtime answer "does spawning
    // and despawning work". The panel says so, in those words.
    bool streamSimEnabled_ = false;
    // One-shot: the Tags panel seeds the shard preview the first time it is drawn with
    // nothing baked, so the author sees their shards without having to press a button.
    bool shardPreviewSeeded_ = false;
    bool streamSimDrawBoxes_ = true;
    bool streamSimDrawRadii_ = true;
    glm::vec3 streamSimFocus_{0.0f};
    std::vector<stream::PolicyShard> streamSimShards_; // rebuilt each evaluation
    stream::PolicyOut streamSimOut_;
    std::vector<u8> streamSimResident_; // parallel to shardDescs_: the simulated residency
    std::vector<f32> streamSimDist_;    // parallel to shardDescs_: distance to the focus
    // Re-runs the policy against shardDescs_ + the project's tag list.
    void UpdateStreamSim();
    // Draws the baked shard boxes and their load/unload radii in the viewport.
    void DrawShardOverlay(Scene& scene, Renderer& renderer);
    // Rebuilds shardDescs_/shardStats_ from the live scene WITHOUT saving, so the
    // overlay is available before the first Ctrl+S of a session.
    void RebakeShardPreview(Scene& scene);

    // -- LIVE editor zones (the streamer, for real, in the editor) -----------------
    // THE EDITOR OWNS ITS OWN STREAMER. Engine::tagStream_ is private with no
    // accessor, and Engine::UpdateTagStreaming's three guards - including the
    // `onInit_` line whose absence once let a dev-overlay bind write a surviving
    // fragment over the level - are unchanged, byte for byte. Nothing in the shipping
    // runtime path is involved in this feature at all.
    //
    // It binds with stream::BindMode::AuthorWorld: no BindWorld, no DestroyWorld, no
    // environment re-apply, no resident-slice spawn, no world:: persistence. The
    // world the author loaded stays exactly where it is and the streamer adopts it by
    // guid.
    stream::Streamer liveStream_;
    // AUTOMATIC spawn/despawn as the editor camera moves. DEFAULT OFF, deliberately:
    // content vanishing while you work is worse than content you have to ask for, and
    // an author placing a prop 300 m out would watch it despawn under the gizmo. With
    // it off the streamer is still bound and still obeys MANUAL overrides - "off"
    // means SetEnabled(false), which is the existing "pin everything loaded".
    bool liveStreamAuto_ = false;
    // Why the last bind attempt was refused, shown in the Tags panel.
    std::string liveStreamError_;
    // The Renderer a respawn instantiates through, remembered by LiveStreamBind. The
    // save path has a Scene but no Engine, and a bound streamer must be settleable
    // from there; this is how. Null whenever nothing is bound.
    Renderer* liveRenderer_ = nullptr;
    // Binds liveStream_ to currentScenePath_ and adopts the live world. REFUSES, with
    // a reason in liveStreamError_ and nothing bound, unless the scene on disk really
    // describes the world in the registry: every shard in the file must be adoptable
    // by guid, or a later respawn would duplicate content with fresh guids instead of
    // restoring it. Saving first is the fix, and the panel says so.
    bool LiveStreamBind(Scene& scene, Renderer& renderer);
    // Unbinds and scrubs the StreamShard stamps, so "not bound" really means "nothing
    // here is streamed". Safe to call unbound.
    //
    // A NON-NULL `renderer` brings every zone resident FIRST, which is what the
    // author-facing switch must do: handing back a world with holes would let the very
    // next Ctrl+S write it. Pass NULL on the post-Replace path (AdoptWorld) - the
    // registry is a different world by then, and spawning into it would inject one
    // level's zones into another.
    void LiveStreamUnbind(Scene& scene, Renderer* renderer);
    // One call per frame from BuildUI. Suspends itself during Play, a cutscene or
    // movie preview and a gizmo drag, holds the selection's shard resident, and feeds
    // the editor camera in as the streaming focus.
    void LiveStreamTick(Engine& engine);
    // Brings every shard resident right now (stream::Streamer::SpawnAllShards) and
    // returns how many could not be - which can only be Failed shards. Called before
    // ANY save and before ANY undo/Play snapshot: both must see the whole world.
    u32 LiveStreamSettle(Scene& scene, Renderer& renderer);
    // Settles live zones before a scene::SaveSceneToString snapshot that will later be
    // restored through LoadMode::Replace (the cutscene preview and the movie render).
    // CaptureSnapshot and SaveSceneToDisk do this inline; these two did not, and a
    // snapshot taken with a zone streamed out makes that zone's loss PERMANENT.
    void CaptureSceneSettle(Engine& engine);

    // -- Window menu / panel visibility ------------------------------------------
    // Every dockable panel can be shown/hidden from the Window menu (and via its
    // close button). panelOpen_ is indexed by Panel; DrawWindowMenu builds the
    // menu and "Reset Layout" re-runs the default DockBuilder arrangement.
    enum Panel {
        Panel_Viewport, Panel_Game, Panel_Hierarchy, Panel_Inspector,
        Panel_AssetViewer, Panel_ProjectSettings, Panel_PostProcess,
        Panel_Navigation, Panel_Stats, Panel_Timeline,
        Panel_Scenes, Panel_AudioMixer, Panel_Assets,
        Panel_ArtEditor, Panel_SchematicEditor, Panel_Music,
        Panel_CutsceneTimeline, Panel_DialogueEditor, Panel_InputIcons,
        Panel_Objectives, Panel_CharacterEditor, Panel_MovieRender,
        Panel_UIDocument,
        // APPEND ONLY. kNames[] is indexed BY this enum and kArtPanels[] holds its
        // VALUES, so inserting a value mid-list silently swaps two panels'
        // identities in both (stage 1 documented that hazard).
        Panel_Tags,
        Panel_UIEditor,
        Panel_Collaborate,
        Panel_People,
        Panel_Review,
        Panel_VolumeBaker,
        Panel_Count
    };
    bool panelOpen_[Panel_Count];
    bool panelsInit_ = false;     // panelOpen_ defaulted on first BuildUI
    // One-shot: on the first BuildUI frame, take the project's startupScene (which
    // the Engine already loaded into the world at boot) as currentScenePath_, so
    // Ctrl+S saves the level the author is looking at instead of opening Save As.
    bool bootSceneAdopted_ = false;
    void DrawWindowMenu();

    // -- Volume Baker panel: author a VolumeSimConfig and bake it to a `.hbvol` -------
    // The bake runs on a background fiber-job (CPU solver) so the UI stays responsive; the panel
    // polls the shared job state and writes the file on the main thread when it completes.
    void DrawVolumeBaker(Engine&);
    volume::VolumeSimConfig volBakeConfig_{};       // edit copy, seeded from the model's defaultConfig
    int                     volBakeModel_ = 0;      // index into VolumeSimRegistry::Get().Types()
    bool                    volBakeSeeded_ = false;
    int                     volBakeFrames_ = 60;    // frames to bake (start=0)
    char                    volBakePath_[256] = "Volumes/smoke.hbvol"; // rel to Assets/
    std::string             volBakeStatus_;
    // The `.hbvolsim` authoring asset currently loaded in the panel (absolute path; empty = editing an
    // unsaved default). Set by OpenVolumeSim; the Save button writes volBakeConfig_ back to it.
    std::filesystem::path   volBakeSimPath_;
    // Author a new `.hbvolsim` (default config of the default model) under `dir`. Returns the path.
    std::filesystem::path CreateVolumeSimAsset(const std::filesystem::path& dir, const std::string& name);
    // Load a `.hbvolsim` into the Volume Baker panel and open it (jumped to on create / double-click).
    void OpenVolumeSim(const std::filesystem::path& path);
    struct VolBakeJob {
        std::atomic<int>  state{1};   // 1 running, 2 done, 3 failed, 4 cancelled
        std::atomic<int>  done{0};
        std::atomic<int>  total{0};
        std::atomic<bool> cancel{false};
        std::vector<u8>   out;        // written by the job; read on the main thread once state>=2
        std::string       outAbs;     // resolved absolute output path
    };
    std::shared_ptr<VolBakeJob> volBakeJob_;
    // "Bake in place" (from the VolumeComponent inspector): when the shared bake job completes, assign
    // the resulting .hbvol (relative path) as this entity's source and switch it to baked playback.
    entt::entity volBakePendingAssign_ = entt::null;
    std::string  volBakePendingRel_;
    // Heap payload handed to the detached bake job (freed by the job). The job entry must be a plain
    // function pointer (the fiber system takes void(*)(void*)), so a static trampoline owns the work.
    struct VolBakePayload {
        std::shared_ptr<VolBakeJob> job;
        volume::VolumeSimConfig     cfg;
        int                         frames = 1;
    };
    static void RunBakeJob(void* arg); // CPU bake on a worker fiber; deletes the payload

    // -- In-scene volume live preview -----------------------------------------------
    // Runs a low-res CPU sim of the SELECTED VolumeComponent's embedded config in EDIT mode and feeds
    // it (density + temperature) at the entity's transform, so authoring is WYSIWYG in the real scene.
    // Called at the end of BuildUI (after the Engine's baked drive, before RenderScene, so it wins).
    // One at a time (the RHI feeds a single grid); play mode uses the Engine's baked-playback drive.
    void DriveVolumePreview(Engine&);
    // Shared Domain/Physics/Emitters editor for a VolumeSimConfig (used by the Volume Baker panel AND
    // the in-scene VolumeComponent inspector).
    void DrawVolumeConfigControls(volume::VolumeSimConfig& c);
    entt::entity                               volPreviewEntity_ = entt::null;
    std::unique_ptr<volume::IVolumeSimulation> volPreviewSim_;
    u64                                        volPreviewHash_ = 0;  // config+res hash; rebuild on change
    int                                        volPreviewFrame_ = 0;
    bool                                       volPreviewRestart_ = false;
    std::vector<u8>                            volPreviewDensity_; // persist through the frame (fed to RHI)
    std::vector<u8>                            volPreviewTemp_;
    int                                        volBakeInPlaceFrames_ = 90; // "Bake in place" duration (frames)
    glm::vec3                                  volPreviewMin_{0.0f};
    glm::vec3                                  volPreviewMax_{1.0f};

    // -- Undo / redo ----------------------------------------------------------------
    // Snapshot-based: the scene serializes to an in-memory .hbscene JSON string
    // BEFORE each mutation (gizmo drags, inspector edits, create/delete/spawn,
    // reparent, component add/remove, material apply); restoring re-instantiates
    // it. The serializer's persistent GPU caches make restores upload nothing.
    //
    // B12: a snapshot is the scene string PLUS every open `.hbui` document.
    // Document entities are deliberately excluded from BuildSceneJson (they are
    // asset content), so a scene-only snapshot captures nothing of a UI edit and
    // restores nothing of it - Ctrl+Z over a UI drag would silently do the wrong
    // thing, and RestoreSnapshot's Replace would leave the documents live and
    // unchanged. Putting documents back into BuildSceneJson is NOT the fix: that
    // reintroduces the duplication the skip exists to prevent.
    // PushUndo takes a Scene for its ~60 existing call sites; the document half
    // needs an Engine, so the Engine-taking overload is the real one and the
    // Scene overload forwards through `engine_` (parked by BuildUI). The
    // scene-only fallback can only be reached outside a frame, where no
    // document can be open either.
    void PushUndo(Scene& scene);
    void PushUndo(Engine& engine);
    void Undo(Engine& engine);
    void Redo(Engine& engine);
    void RestoreSnapshot(Engine& engine, const Snapshot& snapshot);
    // Scene-only overload: restores the world and LEAVES open documents alone
    // (the Replace sweep spares them). Used by the cutscene-preview revert and
    // the movie-render job, neither of which can edit a document.
    void RestoreSnapshot(Engine& engine, const std::string& sceneJson);
    std::vector<Snapshot> undoStack_;
    std::vector<Snapshot> redoStack_;
    bool gizmoEditing_ = false; // this gizmo drag already captured a snapshot
    static constexpr usize kMaxUndoSteps = 64;

    // -- Copy / paste / duplicate ---------------------------------------------------
    // The selected entity (and its descendants) serialize to an in-memory
    // .hbscene fragment; pasting re-instantiates it additively, in place, as a
    // SIBLING of the source (Unity's rule - see AttachPastedRoot), and selects it.
    // Ctrl+C / Ctrl+V / Ctrl+X / Ctrl+D + Edit menu.
    void CopySelection(Scene& scene);
    void PasteClipboard(Engine& engine);
    void DuplicateSelection(Engine& engine);
    // Instantiates a subtree JSON fragment additively, parents + selects its root,
    // and pushes one undo step. Shared by paste, duplicate, prefab drop and revert.
    // `placeAt` (when given) positions the new root there; otherwise the clone
    // keeps the source's exact local transform and lands on top of it.
    // pushUndo=false lets a caller that already snapshotted (e.g. prefab Revert)
    // record the whole operation as a single undo step.
    // `intoDocument` forces the pasted subtree to join the ACTIVE document even
    // when it carries no UI component of its own (a bare grouping node copied from
    // inside a `.hbui`); UI-carrying fragments join it either way.
    // `parentTo` is the entity the pasted ROOT becomes a child of, or entt::null
    // for a scene/document root. It cannot come from the fragment - see
    // AttachPastedRoot - so each caller supplies its own answer:
    //   Ctrl+V           -> clipboardParent_ (the source's parent at COPY time)
    //   Ctrl+D           -> the SELECTION's parent, read at duplicate time
    //   .hbprefab drop   -> entt::null (a placed prefab is a root)
    //   prefab Revert    -> the INSTANCE's own parent, captured before the destroy
    // `worldFallback` (optional) is the SOURCE root's world matrix at copy time. It
    // is used only when a requested `parentTo` is REJECTED: the clone's stored TRS is
    // local to that parent, so dropping the parent silently reinterprets it as world
    // and teleports the clone by the parent's transform (a child of a rig at x=100
    // appears at the origin, off-screen, as the new selection). With it, a dropped
    // parent keeps the clone where the author last saw it.
    //
    // RETURNS the new root, or entt::null if nothing was pasted - the four refusals
    // (empty fragment, UI-into-no-document, unparseable, instantiated nothing) leave
    // `selected_` untouched, so a caller that stamps a component "onto the paste"
    // must be told there wasn't one.
    entt::entity PasteSubtree(Engine& engine, const std::string& fragment,
                              const glm::vec3* placeAt = nullptr, bool pushUndo = true,
                              bool intoDocument = false, entt::entity parentTo = entt::null,
                              const glm::mat4* worldFallback = nullptr);
    // Parents a freshly instantiated paste ROOT and gives it a sibling index.
    // Reparent()'s rules minus the two halves a paste must not have (its PushUndo,
    // which would make one paste cost two Ctrl+Z, and its world-transform rebase,
    // which would teleport a clone whose stored TRS is ALREADY in the destination
    // parent's space). Returns the parent actually applied - entt::null when the
    // requested one was rejected, so the paste still lands, as a root.
    entt::entity AttachPastedRoot(Scene& scene, entt::entity root, entt::entity parentTo,
                                  const std::vector<entt::entity>& created,
                                  const glm::mat4* worldFallback = nullptr);
    std::string clipboard_; // last copied subtree (.hbscene JSON fragment)
    bool clipboardFromDoc_ = false; // that fragment was copied out of a .hbui
    // The parent the copied subtree's ROOT had, so a paste lands as its SIBLING
    // rather than at the scene root. Handles are only meaningful inside ONE world,
    // so the token that world had at copy time is stored with it: an undo/redo or a
    // scene load Replaces the registry and recycles handles, and a stale index+
    // version pair can silently alias a DIFFERENT entity afterwards.
    entt::entity clipboardParent_ = entt::null;
    u64 clipboardWorld_ = 0; // Scene::WorldToken() at copy time (0 = never copied)
    // THE SAME PARENT AS A STABLE IDENTITY (Scene/EntityGuid.h). The handle above is
    // unusable after a Replace, and undo/redo and Play->Stop are Replaces - so
    // "copy, press Play, press Stop, paste" dropped the parent and the reported bug
    // came back. Undo snapshots and play snapshots both go through BuildSceneJson,
    // which WRITES guids, so the same parent survives a Replace as the same Guid with
    // a fresh handle. 0 = the source root had no parent (or none was recorded).
    u64 clipboardParentGuid_ = 0;
    // The source root's WORLD matrix at copy time, for the teleport guard described
    // on PasteSubtree. Identity + `false` = nothing recorded.
    glm::mat4 clipboardWorldMatrix_{1.0f};
    bool clipboardHasWorldMatrix_ = false;
    // clipboardParent_ if `scene` is still the world it was captured in; otherwise
    // the entity carrying clipboardParentGuid_ in the CURRENT world, if it is still
    // there. What Ctrl+V passes as `parentTo`.
    entt::entity ClipboardParentFor(const Scene& scene) const;

    // -- Prefabs (.hbprefab = a saved entity subtree, reusable across scenes) ------
    // Writes the selected entity + descendants to <dir>/<name>.hbprefab.
    std::filesystem::path CreatePrefabFromSelection(Scene& scene,
                                                    const std::filesystem::path& dir,
                                                    const std::string& name);
    // Instantiates a .hbprefab into the world (at `at` if given, else as authored).
    // Tags the new root with a PrefabInstance link back to `path`.
    void InstantiatePrefab(Engine& engine, const std::filesystem::path& path,
                           const glm::vec3* at = nullptr);
    // Linked-prefab ops on the root entity of a placed instance (PrefabInstance):
    //  Apply  - overwrite the source .hbprefab with this instance's current subtree.
    //  Revert - re-instantiate from the source, keeping the root's world transform.
    // (Unpack = just drop the PrefabInstance component; done inline in the inspector.)
    void ApplyPrefabInstance(Engine& engine, entt::entity root);
    void RevertPrefabInstance(Engine& engine, entt::entity root);
    // Absolute path of a PrefabInstance's source (AssetsDir / rel).
    std::filesystem::path PrefabSourcePath(const std::string& rel) const;
    // The inspector defers Apply/Revert/Unpack here (they destroy/replace the
    // selected subtree, unsafe mid-inspector-draw); BuildUI runs it post-draw.
    int pendingPrefabAction_ = 0;                   // 0 none, 1 apply, 2 revert, 3 unpack
    entt::entity pendingPrefabEntity_ = entt::null; // target instance root

    // -- Scenes ------------------------------------------------------------------
    std::filesystem::path currentScenePath_; // last saved/loaded .hbscene
    // Scene::WorldToken as of the last load/restore/save. 0 = nothing adopted yet
    // (a fresh editor that has neither loaded nor saved), which skips the check -
    // there is no file for the world to disagree with. See AdoptWorld.
    u64 sceneWorldToken_ = 0;
    SceneStreamer streamer_;                 // additive async scene loads
    bool wantSaveSceneAs_ = false;
    // Set by the interactive Build entry points when no Release runtime exists;
    // BuildUI turns it into a confirm modal (popups cannot open inside a menu, and
    // the Build Settings panel draws after the modal block - both defer a frame).
    bool wantShipConfirm_ = false;
    char sceneSaveName_[128] = "MainScene";

    // -- Shipping ----------------------------------------------------------------
    // Build configurator window (game name, platform/backend, resolution,
    // packing options) backed by the project's BuildSettings.
    void DrawBuildSettings(Engine& engine);
    // Interaction-prompt icon mapping (dev): texture pickers for each input device.
    // Shared by the dedicated Input Icons panel and the Build Settings section;
    // returns true if any icon changed (caller saves the project).
    bool DrawInputIconsEditor(Project& project);   // per-device button icon grid
    bool DrawInputActionsEditor(Project& project);  // define actions + default bindings
    void DrawInputIconsPanel(Engine& engine); // dev-only dockable panel (Window menu)
    int inputIconDevice_ = 0;                 // Icon Manager: selected device tab
    bool showBuildSettings_ = false;
    std::string buildResult_; // last build status line (shown in Stats)

    // -- Navigation (real-time grid A*: params, rebuild, visualise, test paths) ---
    void DrawNavigation(Engine& engine);
    // Projects the A* walkable cells + test path into the viewport (debug overlay).
    void DrawNavOverlay(Scene& scene, Renderer& renderer);
    bool navBuilt_ = false;
    bool navShow_ = true;            // overlay the pathfinding debug in the viewport
    glm::vec3 navStart_{-8.0f, 0.0f, -8.0f};
    glm::vec3 navEnd_{8.0f, 0.0f, 8.0f};
    std::vector<glm::vec3> navPath_; // last queried path corners (world space)
    std::string navStatus_;
    nav::GridNavParams gridParams_;  // real-time A* pathfinder params (the live system)
    std::vector<glm::vec3> navCells_; // walkable grid cell centers (debug overlay)

    // -- Objectives browser (searchable index of task goals in the loaded scene) ---
    // Objectives ("task goals") are runtime strings (id + HUD text) that emerge from
    // Checkpoints, Interactable/Trigger actions and Schematic nodes rather than a
    // central authored list. This panel scans the loaded scene/level for every
    // objective referenced, groups by id, and lets the developer jump to each source.
    struct ObjectiveSource {
        entt::entity entity = entt::null; // owning entity (jumpable); null if none
        std::string label;                // e.g. "Checkpoint 'gate'", "Interactable", "Schematic foo.hbschem"
        bool sets = false;                // sets the objective (else completes it)
    };
    struct ObjectiveInfo {
        std::string id;                        // objective id (grouping key)
        std::string text;                      // representative HUD text (first setter with text)
        std::vector<ObjectiveSource> sources;  // every place it is set/completed
        bool hasSetter = false;                // something SetObjective's it
        bool hasCompleter = false;             // something CompleteObjective's it
    };
    std::vector<ObjectiveInfo> objectives_;  // built by RebuildObjectiveIndex (sorted by id)
    char objectiveSearch_[128] = {};         // panel + picker filter text
    // Rebuilds objectives_ from the loaded scene registry (Checkpoint/Interactable/
    // TriggerVolume) + the .hbschem graphs referenced by its SchematicComponents.
    void RebuildObjectiveIndex(Scene& scene);
    void DrawObjectives(Engine& engine);     // the dockable browser panel

    // -- Movie Render (offline trailer -> .mp4, video + audio) -------------------
    movie::MovieJob movieJob_;
    bool movieActive_ = false;               // a render is running (ticked each frame)
    std::string movieCutscene_;              // .hbcutscene rel ("" = current scene)
    std::string movieMusic_;                 // optional background-music asset rel
    char movieOutPath_[512] = "Trailers/trailer.mp4"; // rel to project root
    int movieW_ = 1920, movieH_ = 1080, movieFps_ = 30, movieWarmup_ = 12;
    float movieDuration_ = 5.0f;             // seconds (current-scene mode)
    void DrawMovieRender(Engine& engine);

    // -- Character Editor (modular-rig .hbchar authoring) ------------------------
    CharacterAsset charEdit_;          // the .hbchar currently being authored
    std::string charEditRel_;          // its path relative to Assets/ ("" = new/unsaved)
    weld::Stats charBuildStats_{};     // last seam-weld "Build & report" result
    bool charBuilt_ = false;           // charBuildStats_ is valid
    int charEditSubmesh_ = 0;          // submesh spinner shared by the variant mesh picker
    void DrawCharacterEditor(Engine& engine);
    // An objective-id field: free-typing InputText + a "Pick" popup of existing ids
    // (searchable). Returns true when `value` changed. Used by the narrative inspectors.
    bool ObjectiveIdPicker(const char* label, std::string& value, Scene& scene);

    // -- Collaborate (peer-to-peer sessions + scene history) ---------------------
    // The whole state machine lives in editor::CollabSession; these are just its front
    // door. Defined in CollabPanels.cpp, following the UIEditor.cpp precedent - Editor.cpp
    // is already on /bigobj.
    editor::CollabSession collab_;
    char collabLabelBuf_[64] = {};   // name to file a newly admitted person under
    char collabImportPath_[512] = {};
    char collabDownloadPath_[512] = {}; // where a joiner copies the project to // path of a collaborator's .hbjournal to compare
    bool wantCollabImport_ = false; // deferred: a popup cannot be opened from a button
                                    // inside another panel's draw in every case, and this
                                    // matches the editor's existing want* modal idiom
    void DrawCollaborate(Engine& engine);
    void DrawCollabPeople(Engine& engine);
    void DrawCollabReview(Engine& engine);
    void CollabTick(Engine& engine);
    // Seals a commit for a scene that has just been written. Called from the two success
    // paths of SaveSceneToDisk, never from a wrapper - Save As bypasses those.
    void CollabNoteSaved(Scene& scene, const std::filesystem::path& writtenPath);
    // Re-baselines because a DIFFERENT document is now open. Deliberately not called from
    // AdoptWorld: that also fires on undo/redo and Play->Stop, where re-baselining would
    // erase the uncommitted delta.
    void CollabNoteOpened(Scene& scene, const std::filesystem::path& path);

    bool hubMode_ = false;
    bool artMode_ = false;     // painting-focused layout (HeartbreakArtEditor.exe)
    Engine* engine_ = nullptr; // valid during BuildUI (hub hand-off)
    bool showProjectManager_ = false;
    bool recentsLoaded_ = false;
    std::vector<std::filesystem::path> recentProjects_;
    char newProjectName_[128] = "MyProject";
    char newProjectDir_[512] = {};
};

} // namespace hbe
