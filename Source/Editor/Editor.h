// Editor/Editor.h - Dear ImGui editor: hierarchy, inspector, gizmo, stats.
#pragma once

#include "Assets/AudioEvent.h"
#include "Assets/CutsceneAsset.h"
#include "Assets/DialogueAsset.h"
#include "Dialogue/DialogueGraph.h" // branching-dialogue graph editor
#include "Assets/MaterialAsset.h"
#include "Assets/UAF.h"
#include "Core/Types.h"
#include "Navigation/GridNav.h"
#include "Renderer/CameraController.h"
#include "RHI/RHI.h"
#include "Assets/MusicGraph.h"
#include "Scene/Components.h"
#include "Scene/PaintSystem.h"
#include "Scene/SceneStreamer.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <filesystem>
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
    // Assets-relative paths referenced by the project's scenes: the scenes
    // themselves, meshes + their textures, materials + their textures, audio.
    static std::set<std::string> CollectReferencedAssets();

private:
    void DrawViewport(Engine& engine);
    void DrawHierarchy(Scene& scene, Renderer& renderer);
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
    int  gizmoMode_ = 0;       // 0 = translate, 1 = rotate, 2 = scale
    // Grid snapping for the transform gizmo: round translation to gizmoSnapStep_
    // metres, rotation to gizmoSnapAngle_ degrees, scale to gizmoSnapScale_.
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
    bool vpHovered_ = false; // mouse over the rendered image item
    bool vpClicked_ = false; // LMB pressed on the image this frame
    bool vpVisible_ = false; // the Viewport image was actually drawn this frame
    bool layoutBuilt_ = false;

    // -- Game view / play mode -------------------------------------------------
    // Unity-style Game tab: Play snapshots the scene and starts the simulation
    // (physics + scripts); Stop restores the snapshot; Pause freezes both.
    void DrawGameView(Engine& engine);
    void EnterPlayMode(Engine& engine);
    void StopPlayMode(Engine& engine);
    bool playMode_ = false;
    bool playPaused_ = false;
    std::string playSnapshot_;      // scene state at Play, restored on Stop
    std::string gameStateSnapshot_; // game:: story state (flags/objectives) at Play, restored on Stop
    bool focusGameView_ = false; // focus the Game tab next frame
    bool focusViewport_ = false; // focus the Viewport tab next frame

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
    f32 csZoom_ = 90.0f;                          // pixels per second
    f32 csScroll_ = 0.0f;                         // seconds at the lane's left edge
    // Selected item: kind 0=camera key, 1=transform key, 2=clip marker,
    // 3=dialogue marker; track = anim-track index (kinds 1/2); index = slot.
    int csSelKind_ = -1, csSelTrack_ = -1, csSelIndex_ = -1;
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
    bool scenesScanned_ = false;
    std::filesystem::path renameScene_;  // pending rename target
    char renameBuf_[128] = {};

    // -- Schematic (visual scripting) editor -----------------------------------------
    // Node-graph editor for the project's .hbschem assets.
    void DrawSchematicEditor(Engine& engine);
    void DrawSchematicCanvas();        // the node canvas (links/nodes/interaction)
    void OpenSchematic(const std::filesystem::path& path);
    void SaveSchematic();
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
    void SaveDialogue();                     // writes dlgGraph_ back to editedDialoguePath_
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
    // project settings (.hbproj: environment, build, audio, startup scene).
    void SaveAll(Engine& engine);

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
    int strokeMeshCounter_ = 0;           // unique ribbon .uaf names
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
    bool SaveSceneToDisk(Scene& scene, const std::filesystem::path& path);
    // Saves whatever is open: the level (both layer files) when a level is open,
    // else the active scene in place, else opens "Save Scene As" (never saved yet).
    // The single save entry point for all the Save buttons / Ctrl+S.
    void SaveCurrent(Scene& scene);
    // Writes every distinct streamed-in scene (SceneSource-tagged) back to its
    // own .hbscene with just that scene's entities. Returns false on any failure.
    bool SaveStreamedScenes(Scene& scene);

    // -- Window menu / panel visibility ------------------------------------------
    // Every dockable panel can be shown/hidden from the Window menu (and via its
    // close button). panelOpen_ is indexed by Panel; DrawWindowMenu builds the
    // menu and "Reset Layout" re-runs the default DockBuilder arrangement.
    enum Panel {
        Panel_Viewport, Panel_Game, Panel_Hierarchy, Panel_Inspector,
        Panel_AssetViewer, Panel_ProjectSettings, Panel_PostProcess,
        Panel_Navigation, Panel_Streaming, Panel_Stats, Panel_Timeline,
        Panel_Scenes, Panel_AudioMixer, Panel_Assets,
        Panel_ArtEditor, Panel_SchematicEditor, Panel_Music,
        Panel_CutsceneTimeline, Panel_DialogueEditor, Panel_InputIcons,
        Panel_Count
    };
    bool panelOpen_[Panel_Count];
    bool panelsInit_ = false;     // panelOpen_ defaulted on first BuildUI
    void DrawWindowMenu();

    // -- Undo / redo ----------------------------------------------------------------
    // Snapshot-based: the scene serializes to an in-memory .hbscene JSON string
    // BEFORE each mutation (gizmo drags, inspector edits, create/delete/spawn,
    // reparent, component add/remove, material apply); restoring re-instantiates
    // it. The serializer's persistent GPU caches make restores upload nothing.
    void PushUndo(Scene& scene);
    void Undo(Engine& engine);
    void Redo(Engine& engine);
    void RestoreSnapshot(Engine& engine, const std::string& snapshot);
    std::vector<std::string> undoStack_;
    std::vector<std::string> redoStack_;
    bool gizmoEditing_ = false; // this gizmo drag already captured a snapshot
    static constexpr usize kMaxUndoSteps = 64;

    // -- Copy / paste / duplicate ---------------------------------------------------
    // The selected entity (and its descendants) serialize to an in-memory
    // .hbscene fragment; pasting re-instantiates it additively, nudged off the
    // original and freshly selected. Ctrl+C / Ctrl+V / Ctrl+X / Ctrl+D + Edit menu.
    void CopySelection(Scene& scene);
    void PasteClipboard(Engine& engine);
    void DuplicateSelection(Engine& engine);
    // Instantiates a serialized subtree additively, offsets + selects its root,
    // and pushes one undo step. Shared by paste and duplicate.
    // Instantiates a subtree JSON fragment additively. `placeAt` (when given)
    // positions the new root there; otherwise the root is nudged off the original.
    // pushUndo=false lets a caller that already snapshotted (e.g. prefab Revert)
    // record the whole operation as a single undo step.
    void PasteSubtree(Engine& engine, const std::string& fragment,
                      const glm::vec3* placeAt = nullptr, bool pushUndo = true);
    std::string clipboard_; // last copied subtree (.hbscene JSON fragment)

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
    SceneStreamer streamer_;                 // additive async scene loads
    bool wantSaveSceneAs_ = false;
    char sceneSaveName_[128] = "MainScene";

    // -- Levels (static + dynamic split, automatic) -----------------------------
    // A level is ONE world in the editor; SceneLayer (per entity) decides static
    // vs dynamic and Save partitions the whole scene into the two .hbscene files.
    // Loads a level into the world. `additive` stacks it onto the current world
    // (compose several levels); otherwise it replaces. Becomes the active level.
    void OpenLevel(Engine& engine, const scene::LevelPaths& level, bool additive = false);
    // Creates the empty static + dynamic layer files for `base`, then opens it.
    void CreateLevel(Engine& engine, const std::filesystem::path& base);
    // Tags `e`'s WHOLE hierarchy into the given level's `kind` layer file
    // (SceneSource = <base>.<kind>.hbscene) + SceneLayer{kind}, so it saves to the
    // right file and the navmesh reads the layer. The hierarchy moves as a unit.
    void AssignToLevel(Scene& scene, entt::entity e, const std::filesystem::path& base,
                       SceneKind kind);
    // Convenience: change `e`'s layer within ITS current level (combo / drag).
    void AssignToLayer(Scene& scene, entt::entity e, SceneKind kind);
    // Auto-classifies a hierarchy from its components (Animator/CharacterController
    // /dynamic body/script/nav agent/UI -> Dynamic; otherwise Static).
    SceneKind ClassifyLayer(Scene& scene, entt::entity root) const;
    // The root of `e`'s hierarchy (walks up Parent links).
    entt::entity RootOf(Scene& scene, entt::entity e) const;
    // The layer `e` belongs to: its ROOT's SceneLayer if set, else classified.
    SceneKind EffectiveLayer(Scene& scene, entt::entity e) const;
    // The level base `e` belongs to: from its root's SceneSource if it is a level
    // layer file, else the active level (currentLevel_).
    std::filesystem::path LevelBaseOf(Scene& scene, entt::entity e) const;
    // Gives every level entity a definite SceneSource (its level layer FILE) +
    // SceneLayer, filling only the untagged ones - so nothing is ever lost on save
    // and the hierarchy/navmesh agree. No-op outside a level / during play.
    void EnsureLevelMembership(Scene& scene);
    scene::LevelPaths currentLevel_; // active level (base path); empty = none
    bool levelOpen_ = false;
    bool wantNewLevel_ = false;
    bool startupSynced_ = false; // adopted the project's startup scene/level once
    char levelNameBuf_[128] = "Level1";

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

    // -- Streaming world (load + inspect cells; engine streams around camera) ------
    void DrawStreaming(Engine& engine);
    std::string streamStatus_;

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
