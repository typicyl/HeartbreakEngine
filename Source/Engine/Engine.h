// Engine/Engine.h - owns the window + renderer + scene and runs the main loop.
//
// The Engine itself is editor-agnostic (no ImGui / editor dependency). The
// editor application injects its UI via SetOnInit / SetOnFrame, so a shipping
// runtime build links the engine without the editor.
#pragma once

#include "Core/Types.h"
#include "Navigation/GridNav.h" // real-time A* pathfinder (agents path on this)
#include "RHI/RHI.h"

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

    // True when the corresponding option was given explicitly on the command
    // line; explicit options beat the project's BuildSettings in the runtime.
    bool widthExplicit = false;
    bool heightExplicit = false;
    bool apiExplicit = false;
    bool fullscreenExplicit = false;

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

    // Navigation smoke test: route the grid A* pathfinder around a static + a
    // dynamic obstacle and walk an agent to the goal, then exit.
    bool navTest = false;

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

    // High-level game flow (runtime): boots to the project's main-menu scene,
    // shows the loading scene + progress while the gameplay scene/level loads,
    // then overlays the HUD. Driven by UIElement button `action`s.
    enum class GameState { None, Booting, MainMenu, Loading, Playing };
    GameState State() const { return gameState_; }
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

    // In-game UI pointer in NORMALIZED game-image coordinates (0..1; negative
    // = no pointer). The editor feeds this from the Game panel's image rect;
    // the runtime derives it from the OS cursor over the window.
    void SetUIPointer(f32 u, f32 v) {
        uiPointerU_ = u;
        uiPointerV_ = v;
        uiPointerExternal_ = true; // an editor is feeding the pointer
    }

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
    void UpdateGameFlow(f32 dt); // per-frame; polls UI button actions + progress
    void EnterPlaying();         // loads gameplay scene/level + HUD, locks cursor
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
    bool      devMenuOpen_ = false;
    bool      gameCameraEnabled_ = true;
    f32       uiPointerU_ = -1.0f;
    f32       uiPointerV_ = -1.0f;
    bool      uiPointerExternal_ = false;
};

// Parses common command-line options into an EngineConfig:
//   [--d3d12 | --vulkan] [--width N] [--height N] [--validation] [--model PATH]
EngineConfig ParseCommandLine(int argc, char** argv);

} // namespace hbe
