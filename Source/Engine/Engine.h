// Engine/Engine.h - owns the window + renderer + scene and runs the main loop.
//
// The Engine itself is editor-agnostic (no ImGui / editor dependency). The
// editor application injects its UI via SetOnInit / SetOnFrame, so a shipping
// runtime build links the engine without the editor.
#pragma once

#include "Core/Types.h"
#include "Assets/CutsceneAsset.h" // active cutscene the player evaluates
#include "Dialogue/DialogueGraph.h" // active dialogue graph the conversation player walks
#include "Navigation/NavWorld.h" // persistent streamed Detour navmesh (agents path on this)
#include "Scene/ParticleGpuSim.h" // GPU particle simulation context (compute-driven)
#include "Scene/PrecipSystem.h"   // precip::PrecipField (camera-following rain/snow)
#include "Scene/OceanFFT.h"       // ocean::GpuOcean (GPU FFT ocean, compute-driven)
#include "Vegetation/VegetationWorld.h" // veg::VegetationWorld (data-driven vegetation subsystem)
#include "Vegetation/GrassGpu.h"        // veg::GrassGpuField (GPU-compute grass, --vegdemo)
#include "Vegetation/VegetationRender.h" // veg::VegetationMeshLibrary (editor-painted plants)
#include "Scene/TagStreaming.h"   // stream::Streamer (distance streaming of baked shards)
#include "RHI/RHI.h"
#include "Core/UserSettings.h" // per-user volume/graphics/brightness/captions
#include "Core/InputActions.h" // data-driven actions + rebindable bindings
#include "Interaction/Pick.h" // interact::Hit (the ONE pick pass, pages + objects)
#include "UI/Subtitles.h"  // the one subtitle / closed-caption stack
#include "UI/UIDocument.h" // ui::DocumentSet (the resident `.hbui` UI layer)
#include "UI/UIManager.h" // persistent-UI-document panel manager (game flow drives it)
#include "UI/UISystem.h"  // ui::UIContext (cached layout/interaction state)
#include "UI/UIData.h"    // ui::UIDataModel (P9.4 data-binding)

#include <entt/entt.hpp> // entt::entity (loading-overlay entity tracking)

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace hbe {

class Window;
class Renderer;
class Scene;
class Input;
class PhysicsWorld;
class AudioSystem;

struct EngineConfig {
    std::string title = "Heartbreak Engine"; // UTF-8; see WindowDesc::title
    u32 width  = 1280;
    u32 height = 720;
    i32 posX = -1; // window position; -1 = OS default (see --winpos)
    i32 posY = -1;
    rhi::GraphicsAPI api = rhi::GraphicsAPI::D3D12;
    bool enableValidation = false;
    // Borderless-fullscreen window (no chrome, covers the primary monitor). The
    // runtime defaults this from the project's BuildSettings; --windowed /
    // --fullscreen override on the command line.
    bool fullscreen = false;
    // Present with vsync (default). --novsync uncaps the frame rate (tearing
    // allowed) so real GPU headroom is measurable.
    bool vsync = true;
    // --nocull disables frustum culling (A/B comparison / debugging).
    bool noCull = false;

    // True when the corresponding option was given explicitly on the command
    // line; explicit options beat the project's BuildSettings in the runtime.
    bool widthExplicit = false;
    bool heightExplicit = false;
    bool apiExplicit = false;
    bool fullscreenExplicit = false;
    bool vsyncExplicit = false;

    // Optional model file (glTF/GLB/OBJ/FBX) to load as the scene. When empty,
    // the built-in PBR sphere-grid demo scene is used. UTF-8 path.
    std::string modelPath;

    // Optional project (.hbproj) to open at startup (editor; skips the
    // Project Manager). UTF-8 path.
    std::string projectPath;

    // Editor automation: build + verify the project's asset pack, then exit.
    bool packOnly = false;
    // Editor automation: assemble the shipping Build/ folder, then exit.
    bool shipOnly = false;
    // Editor automation: import this file into the project's Assets/, then
    // exit. UTF-8 path.
    std::string importPath;

    // Spawns this many extra meshes for draw-call stress testing (0 = off).
    u32 stressCount = 0;
    u32 stressParticles = 0;   // --stress-particles N: N live particles (fill rig)
    // --gpu-particles: spawn the stress rig on the GPU vertex-expansion path
    // (ParticleEmitter::gpuExpand). The A/B switch for measuring the two paths.
    bool gpuParticles = false;
    // --gpu-sim: spawn the stress rig on the GPU SIMULATION path
    // (ParticleEmitter::gpuSim). Implies GPU expansion - the compute-written buffer
    // is what the vertex shader reads - so it is the third arm of the same A/B.
    bool gpuSimParticles = false;
    // --fixed-dt S: drive the SIMULATION delta at a fixed S seconds regardless of how
    // fast the frame actually ran. Required for an honest particle A/B: the editor
    // camera orbits on the simulation clock, so at a real dt a slower arm reaches a
    // DIFFERENT camera angle after the same warmup - and at these particle counts the
    // frame is fill-bound, so camera angle IS the measurement. Without it the same
    // configuration measured 4.6 ms in one session and 10.1 ms in another.
    f32 fixedDt = 0.0f;
    // --stress-shared: the stress meshes share ONE mesh (identical-mesh runs -
    // the measurement rig for draw sorting / IA-rebind skipping / instancing).
    bool stressShared = false;

    // Force post effects on for testing (all off by default in PostSettings;
    // no editor toggle yet).
    bool forceDof = false;
    bool forceMotionBlur = false;
    bool forceSsr = false;
    bool forceAutoExposure = false;
    // --painterly [radius]: force the painterly finish on (and optionally set the
    // stroke size). The Kuwahara gather is O(radius^2) per pixel at full res, so
    // being able to A/B it - and sweep the radius - from the command line is the
    // only way to attribute frame cost to it honestly. <0 = leave it alone.
    f32 forcePainterlyRadius = -1.0f;
    bool forcePainterly = false;
    // TEMP (perf measurement): --shadowcascades N overrides the preset's active
    // cascade count (1..4) every frame, so 4-vs-2 shadow cost can be A/B'd on one
    // machine. 0 = off (use the preset). Remove after tuning.
    int forceShadowCascades = 0;
    // --gpuprofile: enable the per-pass GPU timestamp profiler at boot (both backends).
    // Logs "[<API> GPU] total X ms | shadow .. | scene .. | ..." every ~2s so the frame's
    // dominant pass is visible. ~1-3 ms/frame while on, so it's opt-in (diagnosis only).
    bool gpuProfile = false;

    // --benchmark <frames>: run exactly this many frames, then print a timing
    // report and exit. 0 = off (normal interactive run).
    //
    // Optimization work is worthless without a repeatable number, and an
    // interactive session is not one: it varies with window focus, mouse motion,
    // how long you let it run, and whether a 2-second log window happened to
    // straddle a hitch. This mode pins all of that down - forces vsync OFF (a
    // present cap measures the display, not the engine), discards warmup frames
    // so shader/PSO compilation and first-touch page faults do not pollute the
    // sample, and reports a PERCENTILE distribution rather than an average
    // (averages hide exactly the stutter that matters).
    u32 benchmarkFrames = 0;
    // Frames discarded before measurement starts (PSO warmup, streaming settle).
    u32 benchmarkWarmup = 120;
    // Optional CSV of per-frame times written at exit (for graphing / regression).
    std::string benchmarkCsv;

    // Fracture smoke test: headless Voronoi-fracture correctness check
    // (volume conservation, adjacency symmetry, determinism, .hbfrac round-trip).
    bool fractureTest = false;

    // Runtime-destruction smoke test: activation, damage-driven detachment, and
    // structural-support collapse, headless (no window/GPU).
    bool destructionTest = false;

    // Terrain-collider smoke test: the Jolt heightfield built from a
    // TerrainComponent returns the authored heights, MISSES through a painted hole,
    // tracks a sculpt edit, keeps a CharacterController standing on it, and never
    // grows the body count no matter how many strokes land. Headless (no window/GPU).
    bool terrainCollideTest = false;

    // World-space UI smoke test: spawns a worldSpace UICanvas (a lit page with a
    // label + widgets) in the scene, exercising the canvas->texture->quad path.
    bool uiWorldTest = false;

    // Boot straight into the gameplay level, skipping the studio splash + main
    // menu (--play). Useful for testing the scene/render path without the UI.
    bool playOnBoot = false;

    // Day/night testing: --time <hours> scrubs the sky to a fixed time of day and
    // enables the dynamic sky; --daynight <seconds> sets the auto-cycle length.
    // --clouds <0..1> forces cloud coverage. Negative = unset.
    f32 forceTimeOfDay = -1.0f;
    f32 forceDayLength = -1.0f;
    f32 forceClouds = -1.0f;
    f32 forceWetness = -1.0f; // --wetness: force ground wetness each frame
    f32 forceSnow = -1.0f;    // --snow: force snow accumulation each frame
    f32 forceRain = -1.0f;    // --rain: rain at this intensity (wets ground, pools)
    bool forceVolClouds = false; // --volclouds: force volumetric clouds on
    bool spawnWater = false;     // --water: spawn a test Gerstner water plane at the origin
    bool vegDemo = false;        // --vegdemo: spawn a procedural forest on a hilly terrain
    bool vegGpuIndirect = false; // --veggpuindirect: draw grass via the compaction/indirect path
    bool fftOcean = false;       // --fftocean: drive the spawned test water with the GPU FFT
    bool forceDynIBL = false;    // --dynibl: dynamic IBL re-bake + dynamic sky
    bool forceLightning = false; // --lightning: enable lightning + set a storm

    // --tagstreamtest [objects]: TAG STREAMING demo + measurement, on a SYNTHETIC
    // world. It exists because the reference project has nothing to stream - 17 of its
    // 18 entities total ~18 KB and the 18th is a monolithic Terrain component that no
    // tag can subdivide and that IS the world floor - so any honest number for this
    // feature has to come from content built for the purpose. This is the same reason
    // the deleted `--worldtest` built its own world, and it carries the same warning:
    // a green --tagstreamtest says the FEATURE works, never that the reference project
    // got faster. 0 = off.
    u32 tagStreamTest = 0;
};

class Engine {
public:
    using Hook = std::function<void(Engine&)>;

    // Called once after the window/renderer/scene exist (e.g. editor init).
    void SetOnInit(Hook hook) { onInit_ = std::move(hook); }
    // Called every frame inside an ImGui frame, before the scene is recorded
    // (e.g. the editor builds its UI).
    void SetOnFrame(Hook hook) { onFrame_ = std::move(hook); }

    // Runs the engine to completion. Returns a process exit code.
    int Run(const EngineConfig& config);

    // Offline MOVIE RENDER: force a FIXED simulation delta (= 1/fps) so playback is
    // deterministic and frame-stepped regardless of real framerate. Only the sim
    // delta is affected (not the wall-clock dt_ used by caption/loading timers).
    // Pass <= 0 to restore real-time. Set by the editor's movie render job.
    void SetRenderFixedDt(f32 dt) { renderFixedDt_ = dt; }

    Window&       GetWindow()   { return *window_; }
    Renderer&     GetRenderer() { return *renderer_; }
    Scene&        GetScene()    { return *scene_; }
    Input&        GetInput()    { return *input_; }
    PhysicsWorld& GetPhysics()  { return *physics_; }
    AudioSystem&  GetAudio()    { return *audio_; }
    // Persistent, independently-streamed Detour navmesh NavigationAgents path on.
    // Loaded from the scene's baked .hbnav (SceneEnvironment::navSource); streams tiles
    // around the player on its own radius, decoupled from geometry streaming.
    nav::NavWorld& GetNavWorld() { return navWorld_; }
    // The data-driven vegetation subsystem (species/biome registries + plug-in backends
    // + per-shard stores). The editor's vegetation mode and tools drive it (P9).
    veg::VegetationWorld& GetVegetation() { return veg_; }
    // The GPU-compute grass field (blade count + tunable config), for the editor panel.
    veg::GrassGpuField& GetVegGrass() { return vegGrass_; }
    bool VegGrassActive() const { return vegGrassActive_; }
    // Enable/disable driving the GPU grass field. Off = the frame loop stops feeding it, so the
    // grass despawns next frame (the renderer clears its blade handle each frame). Re-enabling
    // resumes it if a terrain heightfield was set. This is the grass despawn control.
    void SetVegGrassActive(bool active) { vegGrassActive_ = active; }

    // Resets ENGINE-OWNED vegetation runtime state that is NOT part of the entity registry, so it
    // does not bleed across a scene change. The GPU grass field is the case that motivates it: it
    // is camera-centered engine state (not a scene entity), so without this it keeps rendering the
    // OLD scene's terrain heightfield over a freshly loaded scene. Call on every world replace.
    void ResetVegetationRuntime() {
        vegGrassActive_ = false;
        vegGrass_.SetTerrain({}, 0.0f, 0, 0.0f); // drop the previous scene's heightfield
    }
    // Persistent per-species mesh cache shared by every PAINTED plant (editor brush + rehydrate).
    // Engine-owned so painted entities keep valid mesh handles; cleared before renderer shutdown.
    veg::VegetationMeshLibrary& GetVegPaintLib() { return vegPaintLib_; }
    // Spawns the procedural demo forest + GPU grass into the current scene (the --vegdemo
    // content; also driven by the editor's Vegetation panel button).
    void SpawnDemoVegetation();

    // Mouse-look cursor lock (hide + recenter). Driven by the game flow (locked
    // while Playing) and callable directly.
    void SetCursorLocked(bool locked);
    bool IsCursorLocked() const;

    // High-level game flow (runtime): boots to the persistent UI scene's initial
    // (menu) panel, shows the "Loading" panel + progress while the gameplay
    // scene/level loads, then reveals the "HUD" panel. Driven by UIElement button
    // `action`s. No UI scene = boot straight into the startup scene.
    enum class GameState { None, Booting, MainMenu, Loading, Playing };
    GameState State() const { return gameState_; }

    // Sub-phases of GameState::Loading, driving the fade sequence: go black on Play ->
    // load the world behind the curtain -> fade the loading screen in -> ease the wheel
    // in while it streams -> fade to black -> reveal (gameplay eases in from black).
    enum class LoadPhase { Begin, FadeIn, Wheel, FadeOut };
    void FlowPlay();      // menu -> loading -> gameplay (+HUD), lock cursor
    void FlowMainMenu();  // back to the main menu, unlock cursor
    void FlowReload();    // respawn/restart: game loading screen -> reload gameplay

    // --- Player pause -------------------------------------------------------
    // PAUSE IS NOT A GameState. It is a modal overlay on Playing: Loading and
    // MainMenu cannot be paused, and making it a fifth enumerator would have meant
    // auditing every `gameState_ == Playing` test in the engine (the gameplay band,
    // the character band, the streaming gate, the cursor policy) for a case none of
    // them wanted.
    //
    // Paused means the SIMULATION delta is zero - physics, animation, AI, gameplay
    // and nav all stand still - while the PRESENTATION clock (dt_) keeps running, so
    // the pause menu still fades and animates and the caption crawl does not freeze
    // mid-word. That is the same split the dev menu's devTimeScale_ already relies
    // on; this reuses the mechanism rather than adding a second one.
    //
    // Pushes a UI panel named "Pause" when the project has one. It still pauses
    // (and frees the cursor) when it does not, so pause works before the menu is
    // authored instead of silently doing nothing.
    bool Paused() const { return paused_; }
    void SetPaused(bool on);
    void TogglePause();

    // --- Cutscene skip ------------------------------------------------------
    bool CutsceneActive() const { return cutsceneTime_ >= 0.0f; }
    // Abandon the running cutscene at the player's request. Restores the game
    // camera and drops whatever the cutscene put on screen.
    //
    // Deliberately does NOT fast-forward the remaining markers. A cutscene's markers
    // are camera keys, animation clips, shakes, subtitles and dialogue - all purely
    // presentational. Story state (flags, objectives, the next beat) is written by
    // the SCHEMATIC that launched the cutscene, and PlayCutscene is fire-and-forget,
    // so that schematic has already moved on. Firing the remaining markers would
    // therefore dump every skipped line onto the screen at once - the exact thing
    // the player asked not to see.
    void SkipCutscene();

    // Checkpoint save/load: writes/reads <projectRoot>/Saves/<slot>.hbsave (a full
    // scene snapshot + game::SerializeState). SaveGame is what a reached checkpoint
    // triggers; LoadGame restores the world + objectives + checkpoints and resumes
    // Playing. Returns false on IO failure (logged). HasSave checks existence.
    bool SaveGame(const std::string& slot = "checkpoint");
    bool LoadGame(const std::string& slot = "checkpoint");
    bool HasSave(const std::string& slot = "checkpoint") const;

    // Last frame's delta time in seconds (valid inside hooks).
    f32 DeltaTime() const { return dt_; }

    // Ends the main loop after the current frame (e.g. the Hub hands off to
    // the editor process).
    void Quit();

    // When enabled, the scene's primary CameraComponent drives the render
    // camera each frame. The runtime leaves this on; the editor enables it
    // only in play mode (the scene view keeps the editor camera otherwise).
    void SetGameCameraEnabled(bool enabled) { gameCameraEnabled_ = enabled; }
    bool IsGameCameraEnabled() const { return gameCameraEnabled_; }

    // Abort any in-progress cutscene and hand the camera back to its owner.
    // The editor calls this when entering/leaving play mode so a cutscene that
    // was mid-flight when the user hit Stop cannot silently resume on the next
    // Play (which would hijack the viewport camera with no schematic trigger).
    void ClearCutscene();

    // Drop any in-progress conversation + choice buttons + interaction prompt +
    // captions. The editor calls this on Play/Stop (dialogue runner state is an
    // Engine member that survives a scene Replace, so a conversation left running
    // when the user hits Stop would otherwise resume over the next Play).
    void ResetDialogueRuntime();

    // In-game UI pointer in NORMALIZED game-image coordinates (0..1; negative
    // = no pointer). The editor feeds this from the Game panel's image rect;
    // the runtime derives it from the OS cursor over the window.
    void SetUIPointer(f32 u, f32 v) {
        uiPointerU_ = u;
        uiPointerV_ = v;
        uiPointerExternal_ = true; // an editor is feeding the pointer
    }

    // Editor keyboard gate (mirror of the external pointer): while captured,
    // in-game UI keyboard/gamepad navigation and TextInput editing are
    // suspended so ImGui typing and editor shortcuts never leak into the game
    // UI. The editor sets this every frame; the standalone runtime leaves it
    // false.
    void SetUIKeyboardCaptured(bool on) { uiKeyboardExternal_ = on; }

    // Push a line onto the ONE subtitle/caption stack. Keep the speaker in its own
    // field rather than pre-joining it - the stack formats per kind and gates
    // speech (Subtitles) separately from non-speech (Closed Captions).
    void PushSubtitle(subtitle::Line line);
    // Convenience for plain speech with no speaker; `seconds` <= 0 auto-derives a
    // readable duration from the text length.
    void PushCaption(const std::string& text, f32 seconds = 0.0f);
    // The live subtitle stack (the editor's preview reads it).
    const subtitle::Stack& Subtitles() const { return subtitles_; }

private:
    Hook onInit_;
    Hook onFrame_;
    Window*   window_ = nullptr;
    Renderer* renderer_ = nullptr;
    Scene*    scene_ = nullptr;
    Input*    input_ = nullptr;
    PhysicsWorld* physics_ = nullptr;
    AudioSystem*  audio_ = nullptr;
    ui::UIManager uiManager_;       // resident-UI panel manager (when uiDocument set)
    // Every OPEN `.hbui`. The engine owns exactly two: the boot splash and the
    // resident UI layer. The editor opens more into the same set (it authors
    // documents against the live scene), which is why it is exposed below.
    ui::DocumentSet docs_;
    ui::DocHandle bootDoc_ = 0; // studio splash; CLOSED EXPLICITLY in FlowAfterBoot
    // THE SCREEN SET: one open `.hbui` per screen (ProjectSettings::uiDocuments),
    // in project order. [0] is the menu document - its `post` becomes
    // uiScenePost_. All resident for the process lifetime; the UIManager binds to
    // the whole vector and addresses panels by name across it.
    std::vector<ui::DocHandle> uiDocs_;

    // THIS FRAME'S PICK. One ray, one occlusion cast, one winner - a world UI page
    // OR a 3D Interactable, never both. Produced at the top of the frame (before
    // ui::UpdateInteraction consumes the page half) and read again by
    // UpdateInteractions for the object half, so the two can never disagree about
    // what the player is pointing at.
    interact::Hit pick_;
    // Rolling cost of that pass, in milliseconds (the pick + its physics cast).
    // Reported by --benchmark and the frame-time overlay.
    f64 pickMs_ = 0.0;
    u32 pickPages_ = 0, pickRaycasts_ = 0;

public:
    // The interactive thing under the pointer/reticle this frame.
    const interact::Hit& CurrentPick() const { return pick_; }
    f64 PickCostMs() const { return pickMs_; }

    // The open-document set. The editor's UI Document panel opens/saves/closes
    // documents through this, and its undo stack snapshots them alongside the
    // scene (a document's entities are excluded from the scene snapshot by
    // design, so nothing else would capture a UI edit).
    ui::DocumentSet& Documents() { return docs_; }
    // The MENU document (screen 0), or 0. Kept for callers that legitimately want
    // "the one document that supplies the menu look"; anything asking "is this
    // entity part of the game UI" must use UIDocuments()/IsEngineDocument().
    ui::DocHandle UIDocument() const { return uiDocs_.empty() ? 0u : uiDocs_.front(); }
    const std::vector<ui::DocHandle>& UIDocuments() const { return uiDocs_; }
    ui::DocHandle BootDocument() const { return bootDoc_; }
    // True for a document the ENGINE owns (the splash or any screen). The editor
    // must not offer Save/Close on these - with a per-screen split there are now
    // several of them, and checking only UIDocument() would let three of four
    // screens be closed out from under the running flow.
    bool IsEngineDocument(ui::DocHandle d) const {
        if (d == 0) return false;
        if (d == bootDoc_) return true;
        for (const ui::DocHandle h : uiDocs_)
            if (h == d) return true;
        return false;
    }
    ui::UIManager& GetUIManager() { return uiManager_; }

private:
    // The UI scene's OWN authored post stack. The UI scene loads ADDITIVE (it has
    // to - it coexists with gameplay), and an additive load deliberately does not
    // touch the environment, so its authored look was being thrown away and the
    // menu rendered with whatever SetupEnvironment last stamped: the PROJECT
    // default. That silently diverges from the scene the artist is actually
    // looking at in the editor. Captured on load, applied whenever the MENU is on
    // screen (and only then - during gameplay the level's post owns the look).
    rhi::PostSettings uiScenePost_;
    bool uiScenePostValid_ = false;

    // 3D MAIN MENU (ProjectSettings::menuWorld). True while the startup scene is
    // bound as a MENU BACKDROP (stream::BindMode::MenuWorld - no visits, no
    // captures). Gates: ApplyMenuPost becomes a no-op (the scene's authored look
    // wins), streaming runs during MainMenu, and the camera below owns the view.
    bool menuWorldBound_ = false;
    // ProjectSettings::menuCamera resolved by name at bind. entt::null = let the
    // camera system pick the scene's primary camera instead.
    entt::entity menuCamEntity_ = entt::null;
    // Binds/rebinds the menu backdrop. Safe to call when menuWorld is off (no-op);
    // a failed bind falls back to the classic flat menu rather than a black screen.
    void BindMenuWorld();
    void ApplyMenuPost(); // env.post <- uiScenePost_ (no-op when not captured)
    ui::UIContext uiCtx_;           // cached UI layout/interaction/text state
    ui::UIDataModel uiDataModel_;   // P9.4 data-binding: keyed store the UI reads each frame
    // GPU VERTEX EXPANSION (ParticleEmitter::gpuExpand). One per-frame-in-flight
    // ring of 64-byte records: [emitter record][its particles] per batch. Created
    // lazily on the first frame an opted-in emitter exists, so projects that never
    // use it pay nothing. 262144 elements = 16 MB/slot, ~262k particles - versus the
    // CPU path's 6 MB vertex ring, which truncates at ~26k.
    static constexpr u32 kGpuParticleRecordElements = 262144;
    rhi::GpuBufferHandle gpuParticleBuffer_{};
    std::vector<rhi::GpuParticleBatch> gpuParticleBatches_; // reused each frame
    bool gpuParticleFailed_ = false;                        // creation failed; do not retry
    // GPU SIMULATION (ParticleEmitter::gpuSim). Owns its own device-local record
    // buffer - the compute pass writes it and the vertex shader reads the same bytes,
    // so nothing is uploaded per frame at all. Created lazily too.
    particle::GpuSim gpuSim_;
    precip::PrecipField precip_; // runtime camera-following rain/snow (not serialized)
    ocean::GpuOcean ocean_;      // runtime GPU FFT ocean (compute buffers/pipelines; lazy)
    veg::VegetationWorld veg_;   // data-driven vegetation subsystem (registries + backends + per-shard stores)
    veg::GrassGpuField vegGrass_; // GPU-compute grass field (driven while --vegdemo is active)
    bool vegGrassActive_ = false; // --vegdemo enabled the GPU grass field
    veg::VegetationMeshLibrary vegPaintLib_; // shared mesh cache for editor-painted plants

public:
    // Exposed for --test-vfxsim (it reads the simulation buffer back).
    particle::GpuSim& GetGpuSim() { return gpuSim_; }

private:
    bool uiManagerMode_ = false;    // true once a project uiScene is loaded + managed
    UserSettings userSettings_;              // per-user options (volume/graphics/brightness/captions)
    std::filesystem::path userSettingsDir_;  // where usersettings.json lives
    bool settingsDirty_ = false;             // pending user-settings save (flushed on Back/quit)
    // The ONE subtitle/closed-caption stack: dialogue lines, cutscene markers,
    // schematic voicelines and .uaf audio captions all land here as structured
    // subtitle::Line values. Bottom-anchored "caption" Label shows the composed
    // block; each line expires on its own timer.
    subtitle::Stack subtitles_;
    // Active dialogue (a .hbdialogue graph run by a schematic): the conversation
    // player walks the graph - Line nodes push captions + play clips, Choice nodes
    // spawn clickable buttons and branch, Condition/SetFlag read/write story flags.
    dlg::Graph dialogueGraph_;
    u32 dialogueNode_ = 0;          // current node id (0 = no conversation running)
    f32 dialogueTimer_ = 0.0f;      // seconds until the current Line auto-advances
    bool dialogueChoiceActive_ = false; // waiting on the player to pick a Choice option
    // Active cutscene (a .hbcutscene run by a schematic): the player takes over
    // the camera and evaluates the tracks each frame until the duration elapses.
    CutsceneAsset cutscene_;
    f32 cutsceneTime_ = -1.0f;      // -1 = no cutscene; else seconds along the timeline
    bool cutsceneRestoreCam_ = true; // gameCameraEnabled_ to restore when it ends
    bool cutsceneCamOwned_ = false;  // true while a cutscene has taken over the camera
    bool paused_ = false;            // player pause (see Paused/SetPaused above)
    void SeedSettingsWidgets();  // fill "setting:*" widgets from userSettings_ (on Settings shown)
    void ApplyChangedSettings(); // read changed "setting:*" widgets -> apply live + mark dirty
    // BOOT-TIME DUPLICATE SCAN over the resident screen set. Every consumer of
    // UIElement::action addresses elements BY STRING over the whole registry and
    // is completely indifferent to which document they came from - which is what
    // lets the four screens split with zero changes to any of them. The one new
    // invariant a split can violate is UNIQUENESS: two screens each owning a
    // `setting:volume` slider would both be seeded and both write back. Warns; it
    // cannot refuse, because a duplicate is authored content, not a crash.
    void AuditScreenActions(Scene& scene);
    // --benchmark: percentile report (mean/median/p95/1% low) + optional CSV.
    // Percentiles, not an average: an average hides the stutter players feel.
    static void ReportBenchmark(std::vector<f32>& frameMs, const Renderer& renderer,
                                const std::string& csvPath);
    subtitle::Settings CurrentSubtitleSettings() const; // from userSettings_
    void UpdateCaptions(f32 dt); // drain audio captions -> drive the "caption" UI element
    void UpdateDialogue(f32 dt); // step the active .hbdialogue graph (lines/choices/branches)
    void EnterDialogueNode(u32 nodeId); // process a node (chains through Condition/SetFlag)
    void SpawnDialogueChoices(const dlg::Node& node); // create clickable choice buttons
    void ClearDialogueChoices();  // destroy any spawned choice buttons
    bool DialogueChoicesActive() const { return dialogueChoiceActive_; }
    // Interaction: proximity prompts on Interactable objects/NPCs + box TriggerVolumes;
    // both fire a game:: action (start a dialogue/cutscene, set a flag/objective).
    void UpdateInteractions(Scene& scene, f32 dt);
    // True while 3D interactables must be OFF: a menu overlay over the HUD, the
    // dev menu, a rebind, or a conversation/cutscene playing or queued. Shared by
    // the pick pass (so a suppressed object cannot steal the pointer from a world
    // page behind it) and by UpdateInteractions itself.
    bool InteractionsSuppressed() const;
    // Per-item gating for one Interactable: `once`+fired, requiredFlag, and an
    // already-collected pickup. The AcceptFn the pick pass filters candidates with.
    bool InteractableAvailable(Scene& scene, entt::entity e) const;
    // Drive the transient prompt UI at a screen anchor (canvas fraction, y-down) so
    // it sits over the target's world centre. `iconPath` (a texture .uaf rel. Assets,
    // empty = none) shows the configured device button icon centred on the object,
    // with `text` (the verb) below it; when empty, `text` carries the "[E]" glyph.
    // `pressed` = the Interact action is HELD on this object right now, which draws
    // the prompt in its pressed state. Without it a 3D object had a hover state and
    // an activation and nothing in between, so a hold read as a dead key.
    void ShowInteractPrompt(const std::string& text, const std::string& iconPath,
                            glm::vec2 anchor, bool pressed);
    void HideInteractPrompt();
    entt::entity interactPrompt_ = entt::null; // transient prompt TEXT entity (toggled)
    entt::entity interactIcon_ = entt::null;   // transient prompt ICON image entity (toggled)
    // Data-driven input actions + rebindable bindings. Definitions come from the
    // project, per-user overrides from UserSettings; the editor Input Actions panel
    // and the runtime rebind menu drive it. Call SyncActionMap after either changes.
    input::ActionMap actionMap_;
    bool rebindJustCommitted_ = false; // true the frame a rebind committed; suppresses the
                                       // just-bound key from double-firing as an Interact press
public:
    input::ActionMap& Actions() { return actionMap_; }
    void SyncActionMap(); // (re)load action defs from the project + overrides from settings
    void SaveUserSettings(); // persist userSettings_ (rebinds, options) to disk
private:
    void UpdateCutscene(f32 dt); // evaluate the active .hbcutscene camera/anim/dialogue tracks
    void PlayUISounds();         // play UIElement hover/click sounds (edge-detected)
    // Owned by value: the navmesh manager for the current level. Loads/streams the
    // scene's .hbnav (SceneEnvironment::navSource); see Engine::Run's sim band.
    nav::NavWorld navWorld_;
    f32       dt_ = 0.0f;

    // Game flow runtime state (only active when the project sets a main-menu
    // scene; the editor uses its own play mode instead).
    bool      flowActive_ = false;
    bool      playOnBoot_ = false; // --play: FlowAfterBoot jumps straight to gameplay
    f32       forceTimeOfDay_ = -1.0f; // --time: hold the sky at this hour each frame
    f32       forceDayLength_ = -1.0f; // --daynight: auto-cycle seconds each frame
    f32       forceClouds_ = -1.0f;    // --clouds: force cloud coverage each frame
    f32       iblRebakeTimer_ = 0.0f;  // throttle accumulator for the dynamic-IBL re-bake
    glm::vec3 iblLastSunDir_{0.0f};    // sun direction at the last dynamic-IBL re-bake
    f32       forceWetness_ = -1.0f;   // --wetness: force ground wetness each frame
    f32       forceSnow_ = -1.0f;      // --snow: force snow accumulation each frame
    f32       forceRain_ = -1.0f;      // --rain: rain intensity each frame
    bool      forceVolClouds_ = false; // --volclouds: force volumetric clouds on
    bool      forceDynIBL_ = false;    // --dynibl: dynamic IBL re-bake + dynamic sky
    bool      forceLightning_ = false; // --lightning: enable lightning + a storm
    GameState gameState_ = GameState::None;
    f32       loadTimer_ = 0.0f;
    LoadPhase loadPhase_ = LoadPhase::Begin; // fade sub-phase within GameState::Loading
    f32       fadeAlpha_ = 0.0f;  // full-screen black curtain: 0 clear .. 1 opaque black
    f32       wheelAlpha_ = 0.0f; // loading wheel/progress fade-in factor (0..1)
    // Entities of the "Loading" UIPanel subtree (resident in the persistent UI
    // scene), collected when the panel is shown so the progress/wheel drivers only
    // touch the loading screen (never gameplay HUD bars). Cleared on reveal - the
    // panel entities themselves are persistent and are merely deactivated.
    std::vector<entt::entity> loadingPanelEntities_;
    void UpdateGameFlow(f32 dt); // per-frame; polls UI button actions + progress
    void LoadGameplayWorld();    // instantiate the startup scene + HUD (no state flip)
    // Swaps the gameplay world to another .hbscene mid-play (the dev menu's
    // "skip to zone"). Runs the same narrative/queue teardown LoadGameplayWorld
    // does, captures the area being left and replays the destination's saved
    // state, then Replace-loads `scenePath`. A level is ONE scene file, so this
    // is a plain scene load - there are no layers to compose.
    void LoadGameplayScene(const std::filesystem::path& scenePath);
    // The .hbscene the gameplay world was loaded from - the "where am I" key.
    // Written into the .hbsave under "level" (which tag streaming now also READS: it is
    // the file BindLevel re-binds on a load) and shown by the dev overlay. Empty until
    // a gameplay world loads.
    std::filesystem::path currentScenePath_;

    // --- Tag streaming ------------------------------------------------------
    // Owns the bound level's shard residency. BindLevel replaces the plain
    // Replace-load of the startup scene (it applies the environment through
    // scene::BindWorld, spawns the always-resident slice, and enters the persistence
    // area), and UpdateTagStreaming drives it once a frame from the player + camera.
    stream::Streamer tagStream_;
    std::vector<glm::vec3> streamFoci_; // reused each frame; no per-frame allocation
    // --tagstreamtest overrides the focus with a swept point, so the measurement is a
    // repeatable path rather than wherever a camera happened to drift. nullptr = the
    // real player/camera foci.
    const glm::vec3* streamFocusOverride_ = nullptr;
    // Runs one streaming update. Called from the frame loop between destruction:: and
    // gameplay:: - see the call site for why that is the only defensible slot.
    void UpdateTagStreaming(Scene& scene, Renderer& renderer);
    // Fills streamFoci_ (or the override) and returns it.
    const std::vector<glm::vec3>& StreamFoci(const Scene& scene, const Renderer& renderer);

    // --- --tagstreamtest ----------------------------------------------------
    u32 tagStreamTest_ = 0;              // object count (0 = the mode is off)
    bool tagStreamTestActive_ = false;
    bool tagStreamTestFailed_ = false;   // any assertion failed -> Run returns non-zero
    std::filesystem::path tagStreamTestDir_; // temp dir holding the synthetic level
    std::vector<TagDef> tagStreamTestTags_;
    glm::vec3 tagStreamFocus_{0.0f};
    glm::vec3 tagStreamHome_{0.0f}; // off the end of the world: nothing in range there
    // 0 warmup, 1 window A (parked empty), 2 settle, 3 window B (parked inside),
    // 4 sweep out, 5 sweep back, 6 drain + report.
    u32 tagStreamPhase_ = 0;
    u32 tagStreamFrame_ = 0;
    u32 tagStreamIslands_ = 0;
    usize tagStreamBaseline_ = 0; // live entity count with nothing streamed in
    f32 tagStreamSweepT_ = 0.0f;
    f32 tagStreamSpan_ = 0.0f;    // world extent the focus sweeps across
    f32 tagStreamSpeed_ = 0.0f;   // m/s, derived from the span so the run is bounded
    std::vector<f32> tagStreamIdleMs_;   // A: parked, nothing resident
    std::vector<f32> tagStreamSteadyMs_; // B: parked inside an island, nothing streaming
    std::vector<f32> tagStreamSweepMs_;  // C: focus moving, streaming live
    // Builds the synthetic level on disk, bakes its shards and binds it. Returns false
    // (and logs) if anything about that fails, so the caller can exit non-zero.
    bool BeginTagStreamTest();
    // One frame of the sweep. Returns false when the test is finished.
    bool StepTagStreamTest(f32 trueFrameMs);
    void ReportTagStreamTest();
    void EnterPlaying();         // reveal: remove loading overlay, resume sim, lock cursor
    void FlowAfterBoot();        // studio splash done -> main menu (or gameplay)
    // Fills UIElement::runtimeText for {backend}/{gpu}/{audio}/{version}/{progress}/
    // {log} tokens (studio/loading screens). `progress` is 0..1.
    void SubstituteUITokens(f32 progress);
    // Renders ONE studio-splash frame on the main thread (used during boot, before
    // the loop, so the splash is visible while the environment/scenes load).
    void PresentBootSplash(f32 progress);
    // Developer overlay (shipped builds when BuildSettings.devMenu): appends an
    // immediate stats panel + hotkey list to the UI overlay. Toggled by Ctrl+`.
    void BuildDevOverlay(std::vector<rhi::UIVertex>& out);
    // Full-screen black fade curtain (loading transitions); appends a quad at fadeAlpha_.
    void BuildFadeCurtain(std::vector<rhi::UIVertex>& out);
    bool      devMenuOpen_ = false;

    // --- Developer debug menu (TLOU-style, shipped-build, gated by BuildSettings.devMenu) ---
    // A keyboard-navigated command list rendered by BuildDevOverlay: skip zones,
    // control time/speed, cycle music, save/load, restart, etc. Rebuilt each frame
    // the menu is open; navigated with the arrow keys (Enter activates, Left/Right
    // adjust a value row).
    struct DevMenuItem {
        std::string label;
        std::string value;                 // right-hand value (value rows); empty for actions
        bool header = false;               // non-selectable section title
        std::function<void()> activate;    // Enter (action rows)
        std::function<void(int)> adjust;   // Left/Right, arg -1/+1 (value rows)
    };
    std::vector<DevMenuItem> devItems_;                 // rebuilt each open frame
    std::vector<std::filesystem::path> devScenes_;      // .hbscene paths (cached on open)
    int  devMenuSel_ = 0;                               // selected row
    f32  devTimeScale_ = 1.0f;                          // game speed (0 = paused)
    f32  renderFixedDt_ = -1.0f;                        // movie render: fixed sim dt (<0 = off)
    int  devSkyRestore_ = -1;                           // authored dynamicSky before a dev time force (-1 = not forced by the menu)
    void RebuildDevMenu();                              // populate devItems_
    void DevMenuScanScenes();                           // fill devScenes_ (on open)
    bool      gameCameraEnabled_ = true;
    f32       uiPointerU_ = -1.0f;
    f32       uiPointerV_ = -1.0f;
    bool      uiPointerExternal_ = false;
    bool      uiKeyboardExternal_ = false; // editor owns the keyboard (ImGui)
};

// Parses common command-line options into an EngineConfig:
//   [--d3d12 | --vulkan] [--width N] [--height N] [--validation] [--model PATH]
EngineConfig ParseCommandLine(int argc, char** argv);

} // namespace hbe
