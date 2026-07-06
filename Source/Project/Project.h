// Project/Project.h - the active game project (assets root, settings).
//
// A project is a directory containing a `.hbproj` file and an `Assets/` folder.
// The editor opens/creates projects; the runtime loads from the project's
// (eventually packaged) assets. All imported content lives under Assets/ as
// `.uaf` files (see Assets/UAF.h).
#pragma once

#include "Core/Types.h"
#include "Core/InputActions.h" // input::ActionDef (data-driven action defaults)
#include "RHI/RHI.h" // rhi::PostSettings

#include <glm/glm.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace hbe {

// Procedural gradient skybox + sun disc. Editing these and rebuilding
// regenerates the scene's sky background and the image-based lighting derived
// from it (Renderer/IBL). A "custom skybox" without authoring an HDR file.
struct SkySettings {
    glm::vec3 horizonColor{0.75f, 0.80f, 0.90f};
    glm::vec3 zenithColor{0.18f, 0.36f, 0.72f};
    glm::vec3 groundColor{0.22f, 0.20f, 0.18f};
    glm::vec3 sunDirection{0.5f, 0.8f, 0.35f}; // points toward the sun
    glm::vec3 sunTint{1.0f, 0.92f, 0.78f};
    f32 sunIntensity = 40.0f; // HDR sun-disc brightness baked into the sky
    f32 skyIntensity = 1.0f;  // overall sky/ambient brightness multiplier
};

// Project-wide environment defaults applied to every scene at startup: the
// procedural sky/IBL, the fallback directional sun, ambient, and exposure.
// Scene files may still override exposure/ambient/post per scene.
struct EnvironmentSettings {
    SkySettings sky;
    glm::vec3 sunColor{1.0f, 0.98f, 0.95f}; // directional light colour
    f32 sunLightIntensity = 4.0f;           // directional light intensity
    f32 ambientIntensity = 1.0f;            // IBL/ambient contribution
    f32 exposure = 1.0f;

    // Day/night cycle. When `dynamicSky` is on, the engine renders the sky
    // analytically from the sun and drives the sun + ambient from `timeOfDay`
    // (hours, [0,24)), advancing it over `dayLengthSeconds` real seconds per cycle
    // (0 = held). Off = the authored static sun above is used.
    f32 timeOfDay = 10.0f;
    f32 dayLengthSeconds = 0.0f;
    u32 dynamicSky = 0;

    // Weather: cloud cover/density for the analytic sky + an overcast gray-out.
    f32 cloudCoverage = 0.0f;
    f32 cloudDensity = 0.6f;
    f32 overcast = 0.0f;
    f32 windAngle = 45.0f;  // direction clouds drift toward (degrees)
    f32 windSpeed = 0.01f;  // cloud drift speed

    // Project-wide HDR post-process stack (bloom/AO/SSR/fog/grade/...). Applied
    // to the scene by SetupEnvironment; edited in the Project Settings window.
    rhi::PostSettings post;
};

// One per-platform build profile: the ordered graphics backends to try at boot.
// The first that initializes on the player's machine wins, so a PC where D3D12
// fails can fall back to Vulkan, then OpenGL. Empty backends => the order is
// derived from BuildSettings::backend (primary first, then the rest).
struct BuildProfile {
    std::string platform = "windows";  // target platform key (only "windows" today)
    std::vector<std::string> backends; // ordered: "d3d12" | "vulkan" | "opengl"
};

// Shipping-build configuration, edited in the editor's Build Settings window
// and applied by the runtime at startup (window title/size, backend).
struct BuildSettings {
    std::string gameName;          // window title / product name ("" = project name)
    std::string company;
    std::string version = "1.0.0";
    std::string backend = "d3d12"; // primary backend: "d3d12" | "vulkan" | "opengl"
    // Per-platform backend fallback profiles. When the running platform matches a
    // profile, its ordered `backends` drive boot selection; otherwise the order is
    // derived from `backend`. Lets a build try D3D12 -> Vulkan -> OpenGL in turn.
    std::vector<BuildProfile> profiles;
    u32 width = 1280;
    u32 height = 720;
    // Shipped builds launch borderless-fullscreen by default (the window covers
    // the primary monitor with no chrome); width/height then size the swapchain
    // only when windowed. The editor is always windowed.
    bool fullscreen = true;
    // Present with vsync (default). Off = uncapped frame rate (tearing allowed);
    // --vsync/--novsync override on the command line.
    bool vsync = true;
    // Ship cooked .uap packs only (no loose Assets/ folder in the build).
    bool packAssets = true;
    // LZMS-compress pack contents (smaller packs, slower cook).
    bool compressAssets = true;
    // Pack only assets referenced by the project's scenes (plus the scenes
    // and materials themselves) instead of everything under Assets/.
    bool onlyReferenced = false;

    // In-game UI canvas: scale mode (0 = stretch, 1 = match height,
    // 2 = pixel-perfect) and the reference resolution UI is authored at.
    u32 uiScaleMode = 1;
    u32 uiRefWidth = 1920;
    u32 uiRefHeight = 1080;

    // When true, the SHIPPED runtime includes the developer overlay (stats +
    // save/load/reload tools), toggled in-game with Ctrl + ` (backtick). Leave
    // off for a public build.
    bool devMenu = false;
};

// One mixer bus persisted with the project (see AudioSystem::ConfigureBuses).
struct AudioBusSetting {
    std::string name;
    std::string parent = "Master";
    f32 volume = 1.0f;
    bool muted = false;
};

// Project-wide spatial-audio occlusion tuning. When enabled, world geometry
// between a spatial source and the listener attenuates + muffles it (multi-ray so
// sound leaks through gaps). Tests against physics colliders.
struct AudioOcclusionSettings {
    bool enabled = false;         // off by default (opt-in per project)
    int rays = 4;                 // 1 = straight line; more = smoother gap leakage
    f32 attenuation = 0.35f;      // volume floor at full occlusion (0..1)
    f32 cutoffHz = 700.0f;        // low-pass cutoff (Hz) at full occlusion
    f32 spread = 0.7f;            // offset-ray ring radius (m) for gap detection
};

// One device's button/key icon set: id -> texture `.uaf` path (relative to Assets).
// `id` is a GamepadButton bit (pad devices) or a (u32)Key (keyboard). Sparse - only
// the buttons the artist supplied art for. See input::PadButtons() / input::KeyName().
struct DeviceGlyphs {
    std::vector<std::pair<u32, std::string>> icons;
    const std::string* Find(u32 id) const {
        for (const auto& e : icons)
            if (e.first == id) return &e.second;
        return nullptr;
    }
    void Set(u32 id, const std::string& tex) { // upsert; empty path removes the entry
        for (usize i = 0; i < icons.size(); ++i)
            if (icons[i].first == id) {
                if (tex.empty()) icons.erase(icons.begin() + static_cast<std::ptrdiff_t>(i));
                else icons[i].second = tex;
                return;
            }
        if (!tex.empty()) icons.emplace_back(id, tex);
    }
};

// Full input-glyph library shipped with the project: per-device button/key icon art
// (the "unique art style" set), plus a general fallback icon and a game logo. The
// interact prompt shows the icon for an action's CURRENT bound button on the active
// device (fallback: general -> text glyph). Edited in the Icon Manager panel +
// Build Settings; serialized in the .hbproj.
struct InputIcons {
    std::string general; // fallback icon (device/button with no specific art)
    std::string logo;    // game / brand logo icon (available for menus & HUD)
    DeviceGlyphs keyboard, xbox, playstation, nintendo, generic;
    // When set, prompts ALWAYS show the single `general` icon regardless of the active
    // device or bound button - a platform-agnostic "just press this" glyph for a game
    // whose art style uses one universal interact symbol.
    bool useGeneralAlways = false;
};

struct ProjectSettings {
    std::string name = "Untitled";
    std::string startupScene; // relative path under Assets (optional)
    // Studio/boot splash shown once at startup while the engine warms up (backend
    // pick, GPU/audio probe, shader warmup). Rendered BEFORE the UI scene exists,
    // so it stays a standalone scene. Can display live info via {backend}/{gpu}/
    // {audio}/{version}/{progress} text tokens; has a ProgressBar.
    std::string studioLoadingScene;
    // THE game UI: one persistent scene holding ALL screens as UIPanel subtrees
    // (MainMenu / Settings / HUD / Pause / Loading). Loaded once at boot, kept
    // resident across gameplay scene swaps; the UIManager shows/hides panels.
    // The "Loading" panel (with a ProgressBar) is driven by the engine during
    // level loads. Empty = no menus: the runtime boots straight into startupScene.
    std::string uiScene;
    BuildSettings build;
    // Project-wide sky/lighting (applied by scene::SetupEnvironment).
    EnvironmentSettings environment;
    // The project's audio mixer (empty = engine defaults: Music/SFX/Ambience).
    std::vector<AudioBusSetting> audioBuses;
    // Spatial-audio occlusion (geometry muffles/attenuates 3D sources).
    AudioOcclusionSettings occlusion;
    // Adaptive-music graph (.hbmusic, relative to Assets). When set, the runtime
    // installs it on boot and crossfades into `musicStartState` when the game runs.
    std::string musicGraph;
    std::string musicStartState; // state played on game start (empty = graph default)
    InputIcons inputIcons;       // per-device prompt icon library (ships with the game)
    // Data-driven input actions: named actions + default key/gamepad binding. Players
    // rebind them at runtime (overrides in UserSettings); the interact prompt shows
    // the icon for an action's current binding. Seeded with "Interact" on load.
    std::vector<input::ActionDef> inputActions;
};

class Project {
public:
    // The active project (one at a time, like most editors).
    static Project& Active() { return s_active; }
    static bool HasActive() { return !s_active.root_.empty(); }

    // Opens an existing `.hbproj`. Returns false on failure.
    bool Open(const std::filesystem::path& projectFile);

    // Opens a project whose `.hbproj` was packed (virtual path "__project.hbproj")
    // into the asset packs already mounted at `mountDir` (a shipped build). The
    // project root becomes `mountDir`; settings are read from the pack via the
    // VFS. Returns false when no packed project file is found.
    bool OpenPacked(const std::filesystem::path& mountDir);

    // Creates a new project directory + `.hbproj` + `Assets/`. Returns false on
    // failure. On success the new project becomes active.
    bool Create(const std::filesystem::path& directory, const std::string& name);

    bool Save() const;

    const std::filesystem::path& Root() const { return root_; }
    std::filesystem::path AssetsDir() const { return root_ / "Assets"; }
    std::filesystem::path ProjectFile() const { return projectFile_; }
    const ProjectSettings& Settings() const { return settings_; }
    ProjectSettings& Settings() { return settings_; }

    // Path of an asset relative to AssetsDir() (for display / referencing).
    std::string RelativeAssetPath(const std::filesystem::path& absolute) const;

private:
    static Project s_active;

    std::filesystem::path root_;
    std::filesystem::path projectFile_;
    ProjectSettings settings_;
};

} // namespace hbe
