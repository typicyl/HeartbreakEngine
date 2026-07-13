// Engine/Engine.h - owns the window + renderer + scene and runs the main loop.
//
// The Engine itself is editor-agnostic (no ImGui / editor dependency). The
// editor application injects its UI via SetOnInit / SetOnFrame, so a shipping
// runtime build links the engine without the editor.
#pragma once

#include "Core/Types.h"
#include "Assets/CutsceneAsset.h" // active cutscene the player evaluates
#include "Dialogue/DialogueGraph.h" // active dialogue graph the conversation player walks
#include "Navigation/GridNav.h" // real-time A* pathfinder (agents path on this)
#include "RHI/RHI.h"
#include "Core/UserSettings.h" // per-user volume/graphics/brightness/captions
#include "Core/InputActions.h" // data-driven actions + rebindable bindings
#include "UI/UIManager.h" // persistent-UI-scene panel manager (game flow drives it)
#include "UI/UISystem.h"  // ui::UIContext (cached layout/interaction state)

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
class StreamingWorld;
namespace scene { class Level; }

struct EngineConfig {
    std::wstring title = L"Heartbreak Engine";
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
    // --stress-shared: the stress meshes share ONE mesh (identical-mesh runs -
    // the measurement rig for draw sorting / IA-rebind skipping / instancing).
    bool stressShared = false;

    // Optional streaming-world manifest (.hbworld JSON) for distance-based world
    // partition streaming. UTF-8 path.
    std::string worldPath;
    // Streaming smoke test: generates a line of cells and sweeps the streaming
    // focus across them, exercising async load/unload on the job system.
    bool worldTest = false;

    // Force post effects on for testing (all off by default in PostSettings;
    // no editor toggle yet).
    bool forceDof = false;
    bool forceMotionBlur = false;
    bool forceSsr = false;
    bool forceAutoExposure = false;
    // TEMP (perf measurement): --shadowcascades N overrides the preset's active
    // cascade count (1..4) every frame, so 4-vs-2 shadow cost can be A/B'd on one
    // machine. 0 = off (use the preset). Remove after tuning.
    int forceShadowCascades = 0;
    // --gpuprofile: enable the per-pass GPU timestamp profiler at boot (both backends).
    // Logs "[<API> GPU] total X ms | shadow .. | scene .. | ..." every ~2s so the frame's
    // dominant pass is visible. ~1-3 ms/frame while on, so it's opt-in (diagnosis only).
    bool gpuProfile = false;

    // Navigation smoke test: route the grid A* pathfinder around a static + a
    // dynamic obstacle and walk an agent to the goal, then exit.
    bool navTest = false;

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
    // Distance-based streaming world (often empty); the editor loads + inspects
    // it, and the engine streams it around the camera each frame.
    StreamingWorld& GetStreamingWorld() { return *streamingWorld_; }
    // Real-time A* pathfinder NavigationAgents path on (no bake; reroutes around
    // moving NavigationObstacles). Auto-rebuilds from static geometry each frame.
    nav::GridNav& GetGridNav() { return *gridNav_; }

    // The currently loaded level (static + dynamic layers). UI scenes are loaded
    // separately and are NOT part of a level.
    scene::Level& CurrentLevel() { return *currentLevel_; }
    bool HasLevel() const;

    // Loads/switches the active level: unloads the current one (keeping UI and
    // other scenes resident), loads `base`'s static + dynamic layers. The grid A*
    // pathfinder picks up the new geometry automatically. `base` is a level base
    // path (no suffix) or any of its layer files.
    void LoadLevel(const std::filesystem::path& base);
    // Unloads the current level's entities (UI / other scenes stay).
    void UnloadLevel();

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

    // Push a subtitle/caption line onto the on-screen stack (used by the dialogue
    // runner and game code). Pre-format the speaker in (e.g. "Nara: Hello");
    // `seconds` <= 0 auto-derives a readable duration from the text length.
    void PushCaption(const std::string& text, f32 seconds = 0.0f);

private:
    Hook onInit_;
    Hook onFrame_;
    Window*   window_ = nullptr;
    Renderer* renderer_ = nullptr;
    Scene*    scene_ = nullptr;
    Input*    input_ = nullptr;
    PhysicsWorld* physics_ = nullptr;
    AudioSystem*  audio_ = nullptr;
    StreamingWorld* streamingWorld_ = nullptr;
    ui::UIManager uiManager_;       // persistent-UI-scene panel manager (when uiScene set)
    ui::UIContext uiCtx_;           // cached UI layout/interaction/text state
    bool uiManagerMode_ = false;    // true once a project uiScene is loaded + managed
    UserSettings userSettings_;              // per-user options (volume/graphics/brightness/captions)
    std::filesystem::path userSettingsDir_;  // where usersettings.json lives
    bool settingsDirty_ = false;             // pending user-settings save (flushed on Back/quit)
    // Closed captions / subtitles: a STACK of active lines, each expiring on its
    // own timer. Newest is appended last, so the "caption" Label (bottom-anchored)
    // grows upward as lines arrive and drops them as they expire.
    struct ActiveCaption {
        std::string text;   // already formatted ("Speaker: line")
        f32 timer = 0.0f;   // seconds remaining
    };
    std::vector<ActiveCaption> captions_;
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
    void SeedSettingsWidgets();  // fill "setting:*" widgets from userSettings_ (on Settings shown)
    void ApplyChangedSettings(); // read changed "setting:*" widgets -> apply live + mark dirty
    void UpdateCaptions(f32 dt); // drain audio captions -> drive the "caption" UI element
    void UpdateDialogue(f32 dt); // step the active .hbdialogue graph (lines/choices/branches)
    void EnterDialogueNode(u32 nodeId); // process a node (chains through Condition/SetFlag)
    void SpawnDialogueChoices(const dlg::Node& node); // create clickable choice buttons
    void ClearDialogueChoices();  // destroy any spawned choice buttons
    bool DialogueChoicesActive() const { return dialogueChoiceActive_; }
    // Interaction: proximity prompts on Interactable objects/NPCs + box TriggerVolumes;
    // both fire a game:: action (start a dialogue/cutscene, set a flag/objective).
    void UpdateInteractions(Scene& scene, f32 dt);
    // Drive the transient prompt UI at a screen anchor (canvas fraction, y-down) so
    // it sits over the target's world centre. `iconPath` (a texture .uaf rel. Assets,
    // empty = none) shows the configured device button icon centred on the object,
    // with `text` (the verb) below it; when empty, `text` carries the "[E]" glyph.
    void ShowInteractPrompt(const std::string& text, const std::string& iconPath, glm::vec2 anchor);
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
    nav::GridNav* gridNav_ = nullptr;
    scene::Level* currentLevel_ = nullptr;
    f32       dt_ = 0.0f;

    // Game flow runtime state (only active when the project sets a main-menu
    // scene; the editor uses its own play mode instead).
    bool      flowActive_ = false;
    bool      playOnBoot_ = false; // --play: FlowAfterBoot jumps straight to gameplay
    f32       forceTimeOfDay_ = -1.0f; // --time: hold the sky at this hour each frame
    f32       forceDayLength_ = -1.0f; // --daynight: auto-cycle seconds each frame
    f32       forceClouds_ = -1.0f;    // --clouds: force cloud coverage each frame
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
    void LoadGameplayWorld();    // instantiate startup level/scene + HUD (no state flip)
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
    std::vector<std::filesystem::path> devLevels_;      // level bases (cached on open)
    int  devMenuSel_ = 0;                               // selected row
    f32  devTimeScale_ = 1.0f;                          // game speed (0 = paused)
    f32  renderFixedDt_ = -1.0f;                        // movie render: fixed sim dt (<0 = off)
    int  devSkyRestore_ = -1;                           // authored dynamicSky before a dev time force (-1 = not forced by the menu)
    void RebuildDevMenu();                              // populate devItems_
    void DevMenuScanLevels();                           // fill devLevels_ (on open)
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
