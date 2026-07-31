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
    // Pack only assets reachable from the project's roots (its scenes, UI
    // documents, prefabs and settings) instead of everything under Assets/. The
    // reachability walk is the TRANSITIVE closure in Assets/AssetRefs.h.
    bool onlyReferenced = false;
    // Escape hatch for a cook that finds a reference naming no file. Default
    // FALSE: an unresolvable reference means the game is already broken, and
    // cook time is the last moment a human is present to hear about it, so the
    // build FAILS with the full list. True downgrades it to a warning and packs
    // whatever did resolve.
    //
    // Deliberately project-file only (no editor checkbox): it exists so a false
    // positive of the scan cannot hard-block a release at 2am, not as a setting
    // anyone should live with. Clearing a genuine dangling reference is one
    // deleted field.
    bool allowMissingRefs = false;

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

// One STREAMING TAG, authored per project. The entity carries only the tag's id
// (Components.h `struct Tag`); everything about HOW that group streams lives
// here, because the author thinks in groups, not in objects. Per-entity radii
// were the deleted `.hbworld` design and no content ever used them.
//
// The list is ORDER-SIGNIFICANT: a tag's index IS its runtime TagId (see
// tags::SeedFromProject), so rows are never silently reordered and a row is
// removed only through tags::RemoveTag, which remaps live entities.
//
// Index 0 is always "Untagged": alwaysLoaded, undeletable, and the meaning of an
// entity with no Tag component at all. tags::Normalize enforces that, drops
// nameless/duplicate rows, and clamps the hysteresis band.
struct TagDef {
    std::string name;
    // Distance from the streaming focus at which the tag's shards SPAWN, and the
    // (necessarily larger) distance at which they DESPAWN. The gap is not
    // optional: with unload <= load the player standing on the boundary spawns
    // and despawns the same shard every frame, and a spawn is a synchronous
    // Instantiate. See salvage::EnforceHysteresis (StreamingSalvage.h SALVAGE 2)
    // for the measured reason this is corrected rather than merely warned about.
    f32 loadRadius = 120.0f;
    f32 unloadRadius = 160.0f;
    i32 priority = 0;         // higher = spawned first when the frame budget throttles
    bool alwaysLoaded = false; // never streamed; spawns with the level and stays
    // ONE TAG = ONE GROUP is the DEFAULT (autoShard = false), because it is the model
    // that matches how a tag reads to an author: everything wearing this tag has one
    // combined bounding box, and the whole set spawns or despawns together on one
    // distance test. Nothing is decided per actor.
    //
    // Turning autoShard ON instead splits the tag into spatially-coherent shards at
    // save time, each streamed independently. That is strictly better for VOLUME - a
    // `Props` tag on 400 crates spread over a city would otherwise have a city-sized
    // box and so be always resident - but it is no longer "the tag" that streams, and
    // a tag can then be half-loaded. Turn it on per tag when a tag has grown big
    // enough that its combined box stops being meaningful; the bake WARNS when a
    // non-sharded tag's box gets pathologically large (see the coherence diagnostic).
    //
    // NOTE the distance test is to the combined BOX, not its centre - a long wall
    // streams in when you approach either end, not only its middle.
    bool autoShard = false;
    f32 shardCell = 0.0f;     // shard grid size; 0 = derive from loadRadius (autoShard only)
};

struct ProjectSettings {
    std::string name = "Untitled";
    std::string startupScene; // relative path under Assets (optional)
    // Studio/boot splash shown once at startup while the engine warms up (backend
    // pick, GPU/audio probe, shader warmup). Rendered BEFORE the UI document
    // exists. Can display live info via {backend}/{gpu}/{audio}/{version}/
    // {progress} text tokens; has a ProgressBar.
    //
    // A `.hbui` DOCUMENT (was: `studioLoadingScene`, a `.hbscene`). A path still
    // ending in `.hbscene` is honoured by an explicit LEGACY BRANCH at boot -
    // see the Engine's boot sequence and Project::ParseSettings. That is a real
    // branch, not a default string: a half-migrated project must still boot.
    std::string bootDocument;
    // THE game UI: ONE `.hbui` DOCUMENT PER SCREEN (MainMenu / Settings / Loading
    // / HUD / Pause ...), each holding that screen's UIPanel subtree. EVERY entry
    // is opened at boot and stays RESIDENT for the process lifetime - nothing here
    // is loaded on demand, so showing a screen is a bool write and can neither
    // pop in nor flash unstyled. Kept across gameplay scene swaps because both
    // Replace sweeps spare UIDocMember::screenOwned. The UIManager binds to the
    // whole set and shows/hides panels BY NAME across it. The "Loading" panel
    // (with a ProgressBar) is driven by the engine during level loads. Empty =
    // no menus: the runtime boots straight into startupScene.
    //
    // ORDER IS MEANINGFUL, twice over:
    //   * [0] is THE MENU DOCUMENT: its header `post` block becomes uiScenePost_,
    //     the look ApplyMenuPost replays whenever a menu is up. Four screens
    //     cannot each supply "the menu look"; the first entry wins, silently and
    //     by design (UIDocument.h decision 1).
    //   * Bind order breaks ties: the first `startVisible` panel is the initial
    //     screen, and a duplicate panel name resolves to the earlier document.
    //
    // A panel name must be UNIQUE across the set, and so must a `UIElement::action`
    // that the engine addresses globally (`setting:*`, the flow verbs, "caption").
    // Both are checked at boot; see UIManager::Init and Engine::AuditScreenActions.
    std::vector<std::string> uiDocuments;
    // LEGACY MIRROR of uiDocuments[0], kept so a single-document project (and any
    // downgrade that only knows this key) still boots. Read on load when
    // `uiDocuments` is absent; written on save as uiDocuments.front(). Nothing in
    // the engine reads it after ParseSettings - resolve screens through
    // `uiDocuments`, which is always populated when this is non-empty.
    //
    // (was: `uiScene`. Same legacy `.hbscene` branch as bootDocument.)
    std::string uiDocument;
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
    // Streaming tags. ALWAYS non-empty after a parse: index 0 is "Untagged"
    // (see TagDef). A tag's index is its runtime TagId.
    std::vector<TagDef> tags;
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
    // THE project's slot ledger - the remembered-pack-slot fallback and the
    // reservation list for everything that cannot embed an id (Assets/SlotIds.h).
    //
    // THERE IS EXACTLY ONE, AND THAT IS THE WHOLE POINT. This used to be two files:
    // `<Name>.uapmanifest` (written by --pack, and what the importer and the
    // editor's create-new paths allocated against) and `<Name>.ship.uapmanifest`
    // (written by --ship). They were two unrelated numberings of the same assets -
    // measured on the reference project, 237 of 254 shared keys had DIFFERENT slots
    // in the two files. So every import allocated a number out of the dev space,
    // stamped it into the asset, and the next ship found that number already owned
    // by something else and relocated whichever file lost - i.e. a gamma fix on one
    // texture reshuffled three packs, silently. One asset, one number, one ledger.
    //
    // The file keeps its historical `.ship.` name because it is the one that
    // describes the CURRENTLY SHIPPED layout: adopting it costs nothing, while
    // adopting the other would renumber everything already in players' hands. This
    // code does not rename files inside a user's project.
    std::filesystem::path SlotManifestPath() const {
        return root_ / (settings_.name + ".ship.uapmanifest");
    }
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
