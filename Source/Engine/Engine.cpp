// Engine/Engine.cpp
#include "Engine/Engine.h"
#include "Engine/CutscenePlayer.h"
#include "Assets/CutsceneAsset.h"
#include "Assets/DialogueAsset.h"
#include "Assets/VFS.h"
#include "Audio/AudioSystem.h"
#include "Core/Input.h"
#include "Core/JobSystem.h"
#include "Core/Log.h"
#include "Core/Window.h"
#include "Game/GameSystems.h"
#include "Game/GameplaySystems.h"
#include "Physics/PhysicsWorld.h"
#include "Assets/MusicGraph.h"
#include "Project/Project.h"
#include "Navigation/GridNav.h"
#include "Scene/AnimationSystem.h"
#include "Scene/CameraSystem.h"
#include "Scene/CharacterController.h"
#include "Scene/Level.h"
#include "Scene/ParticleSystem.h"
#include "Scene/TerrainSystem.h"
#include "Scene/SceneSerializer.h"
#include "Scene/StreamingWorld.h"
#include "Schematic/SchematicSystem.h"
#include "UI/FontAtlas.h"
#include "UI/UIFocus.h"
#include "UI/UISystem.h"
#include "UI/UIAnimation.h"
#include "UI/UIWorld.h"
#include "Renderer/Renderer.h"
#include "RHI/RHIFactory.h"
#include "Scene/FacialSystem.h"
#include "Scene/Scene.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

namespace hbe {

namespace {
#if defined(_WIN32)
// Last-resort SEH handler. An access violation during init (e.g. a graphics driver /
// device-creation fault on another machine) would otherwise terminate the process
// SILENTLY, leaving only the pre-crash log - exactly the "it logged 'starting' and
// quit" report. Log the code + address, flush every sink (incl. the on-disk log),
// then let the OS finish. Turns a silent death into an actionable crash line.
LONG CALLBACK CrashHandler(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* rec = ep ? ep->ExceptionRecord : nullptr;
    const u32 code = rec ? rec->ExceptionCode : 0u;
    const void* addr = rec ? rec->ExceptionAddress : nullptr;

    // Resolve the faulting *instruction* to the module (DLL/exe) it lives in. This is
    // what turns "0x7FFB.. in some DLL" into an actual name - e.g. our own exe (a real
    // bug), a GPU driver (nvwgf2umx/atidxx), or kernel32 (a null handle we handed it).
    char modName[MAX_PATH] = "<unknown module>";
    uintptr_t modOffset = 0;
    if (addr) {
        HMODULE mod = nullptr;
        if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                     GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCSTR>(addr), &mod) &&
            mod) {
            char full[MAX_PATH] = {};
            if (::GetModuleFileNameA(mod, full, MAX_PATH) > 0) {
                const char* leaf = std::strrchr(full, '\\');
                std::snprintf(modName, sizeof(modName), "%s", leaf ? leaf + 1 : full);
            }
            modOffset = reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mod);
        }
    }

    // For an access violation, ExceptionInformation says read vs write and the exact
    // address touched. A tiny address = a NULL/near-null deref (our own bad pointer,
    // NOT a graphics driver) - which points the finger squarely at engine code.
    if (code == 0xC0000005u /*EXCEPTION_ACCESS_VIOLATION*/ && rec && rec->NumberParameters >= 2) {
        const unsigned long long rw = rec->ExceptionInformation[0]; // 0 read,1 write,8 DEP
        const unsigned long long bad = rec->ExceptionInformation[1];
        const char* op = rw == 1 ? "write to" : (rw == 8 ? "execute at" : "read from");
        HBE_ERROR("FATAL: access violation in {}+0x{:X} - tried to {} 0x{:016X}{}",
                  modName, modOffset, op, bad,
                  bad < 0x10000ull ? " (NULL/near-null pointer - an engine bug, not the GPU)."
                                   : " - engine terminating.");
    } else {
        HBE_ERROR("FATAL: unhandled exception 0x{:08X} in {}+0x{:X} at 0x{:016X} - terminating.",
                  code, modName, modOffset, reinterpret_cast<uintptr_t>(addr));
    }
    HBE_ERROR("Send the <exe>.log file next to the executable. If this is graphics-side, try "
              "--d3d12 / --vulkan / --opengl and update the GPU driver.");
    FlushLog();
    return EXCEPTION_EXECUTE_HANDLER; // stop searching; the process ends
}
#endif

// Applies a graphics-quality preset (0 High, 1 Medium, 2 Low) by DEGRADING the
// authored post stack: it only ever disables passes, never enables anything the
// author turned off, and never touches look values (bloom/intensities/exposure).
// High = exactly as authored. Painterly is the art style, not a perf knob -
// untouched. Re-applied every frame AFTER the project stamp + volumes so it wins
// for the frame without ever editing the authored data.
void ApplyGraphicsPreset(rhi::PostSettings& p, int preset) {
    if (preset <= 0) return; // High: the authored look, exactly
    // Medium (default in shipped builds): drop the heaviest SCREEN-SPACE passes -
    // they cost the most at native fullscreen resolution and are the least missed.
    p.ssgiEnabled = 0;
    p.motionBlurEnabled = 0;
    p.ssrEnabled = 0;   // screen-space reflections
    p.fogEnabled = 0;   // volumetric fog raymarch
    // DoF + SSAO are the remaining COVERAGE-SCALED costs (they ramp with lit-geometry
    // screen fill = the daytime dip). Dropping them frees the ~2ms to hold 120 with the
    // full-res painterly intact. Painterly is untouched (it's the art style).
    p.dofEnabled = 0;
    p.ssaoEnabled = 0;
    // Shadows are "the dominant DAYTIME GPU cost" (see Common.hlsli ShadowFactor): each
    // cascade re-rasterizes every caster (cost = cascades x casters, a per-draw descriptor
    // bind on Vulkan). Drop the 4th (farthest, coarsest) cascade at Medium - the split
    // scheme redistributes over 3 slices in Scene::MakeView so the full shadow DISTANCE is
    // kept, only distant shadows get slightly coarser. ~25% off the whole shadow pass.
    p.shadowCascades = 3;
    if (preset >= 2) { // Low: minimal post; TAA falls back to cheap FXAA
        p.ssrEnabled = 0;
        p.dofEnabled = 0;
        p.ssaoEnabled = 0;
        p.shadowCascades = 2; // half the shadow-render cost; distant shadows softer
        if (p.taaEnabled) {
            p.taaEnabled = 0;
            p.fxaaEnabled = 1;
        }
    }
}

// Maps a build-settings backend string to the RHI enum.
rhi::GraphicsAPI ApiFromString(const std::string& s) {
    if (s == "vulkan" || s == "vk") return rhi::GraphicsAPI::Vulkan;
    if (s == "opengl" || s == "gl") return rhi::GraphicsAPI::OpenGL;
    return rhi::GraphicsAPI::D3D12;
}

// Unit vector pointing TOWARD the sun for a time of day (hours, [0,24)). The arc
// is overhead at noon and underfoot at midnight, rising near 6am and setting near
// 6pm, tilted by `lat` so it crosses toward the south rather than dead overhead.
glm::vec3 SunDirFromTime(f32 hours) {
    const f32 ang = (hours - 12.0f) / 12.0f * 3.14159265f; // 0 at noon, +/-PI midnight
    const f32 ax = std::sin(ang);   // -1 at 6am, +1 at 6pm (rises -X, sets +X)
    const f32 ay = std::cos(ang);   // +1 noon (up), -1 midnight (down)
    const f32 lat = 0.55f;          // ~31 degrees of southward tilt
    const glm::vec3 v(ax, ay * std::cos(lat), -ay * std::sin(lat));
    return glm::normalize(v);
}

// Day/night cycle: advances timeOfDay (when a day length is set) and drives the
// sun direction/colour/intensity + ambient from it, so the analytic sky and the
// scene lighting stay in sync as the sun moves. Writes BOTH env.sun and any
// DirectionalLight component (which overrides env.sun in the view fill), so the
// cycle wins regardless of how the scene authored its sun. No-op unless dynamicSky.
void UpdateDayNight(Scene& scene, f32 dt) {
    SceneEnvironment& env = scene.Environment();
    if (env.dynamicSky == 0) return;
    if (env.dayLengthSeconds > 0.0f) {
        env.timeOfDay += dt * (24.0f / env.dayLengthSeconds);
        env.timeOfDay = std::fmod(env.timeOfDay, 24.0f);
        if (env.timeOfDay < 0.0f) env.timeOfDay += 24.0f;
    }
    const glm::vec3 toSun = SunDirFromTime(env.timeOfDay);
    const glm::vec3 fromLight = -toSun; // DirectionalLight points FROM the light
    const f32 elev = toSun.y;           // sun height, -1..1
    const f32 day = glm::clamp(elev * 6.0f + 0.15f, 0.0f, 1.0f); // 0 night, 1 day
    const f32 warm = glm::clamp(1.0f - glm::max(elev, 0.0f) * 3.0f, 0.0f, 1.0f);
    const glm::vec3 color = glm::mix(glm::vec3(1.0f, 0.97f, 0.92f),
                                     glm::vec3(1.0f, 0.5f, 0.22f), warm);
    const f32 intensity = 4.5f * day;                          // dims to ~0 at night
    env.sun.direction = fromLight;
    env.sun.color = color;
    env.sun.intensity = intensity;
    env.ambientIntensity = glm::mix(0.12f, 1.0f, day);         // dim moonlit floor at night
    // Exposure is forced deterministically in Scene::MakeView (after Post Volumes),
    // so a dark night can't be auto-exposed back into daylight.
    auto view = scene.Registry().view<DirectionalLightComponent>();
    for (const entt::entity e : view) {
        DirectionalLightComponent& dl = view.get<DirectionalLightComponent>(e);
        dl.direction = fromLight;
        dl.color = color;
        dl.intensity = intensity;
    }
}

// The ordered backends to try at boot for the current platform: a matching build
// profile's list, else the primary backend followed by the rest as auto-fallback.
std::vector<rhi::GraphicsAPI> ResolveBackendOrder(const BuildSettings& b) {
    using A = rhi::GraphicsAPI;
    for (const BuildProfile& p : b.profiles) {
        if (p.platform == "windows" && !p.backends.empty()) {
            std::vector<A> order;
            for (const std::string& s : p.backends) order.push_back(ApiFromString(s));
            return order;
        }
    }
    const A primary = ApiFromString(b.backend);
    std::vector<A> order{primary};
    for (A a : {A::D3D12, A::Vulkan, A::OpenGL})
        if (a != primary) order.push_back(a);
    return order;
}
} // namespace

void Engine::Quit() {
    if (window_) window_->RequestClose();
}

int Engine::Run(const EngineConfig& configIn) {
#if defined(_WIN32)
    // Install first so a crash ANYWHERE in boot gets logged before the process dies.
    ::SetUnhandledExceptionFilter(&CrashHandler);
#endif
    // Build stamp = the first line ALWAYS printed. If a deployed copy doesn't show the
    // date/time of the build you just made, that machine is running a STALE exe (the
    // usual cause of "I fixed it but it still crashes the same way").
    HBE_INFO("Heartbreak Engine build " __DATE__ " " __TIME__);
    EngineConfig config = configIn;

    // Open the project BEFORE the window exists so its BuildSettings can shape
    // the window/backend. The runtime also auto-discovers a `.hbproj` sitting
    // next to the executable (that is how shipped builds find their game).
    if (!config.projectPath.empty() && !Project::HasActive()) {
        Project::Active().Open(std::filesystem::path(config.projectPath));
    }
    playOnBoot_ = config.playOnBoot;
    forceTimeOfDay_ = config.forceTimeOfDay;
    forceDayLength_ = config.forceDayLength;
    forceClouds_ = config.forceClouds;

    // Register any schematics that were transpiled to native C++ and linked into
    // this executable (a baked runtime). The stub registers nothing, so the editor
    // and an un-baked runtime fall back to the interpreter. Idempotent / once.
    schematic::RegisterBakedSchematics();
#if !HBE_EDITOR
    {
        wchar_t exePath[MAX_PATH] = {};
        ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        const std::filesystem::path exeDir =
            std::filesystem::path(exePath).parent_path();
        std::error_code ec;
        bool packedProject = false;

        // 1) A loose `.hbproj` next to the exe (dev convenience).
        if (!Project::HasActive()) {
            for (const auto& it : std::filesystem::directory_iterator(exeDir, ec)) {
                if (it.is_regular_file() && it.path().extension() == ".hbproj") {
                    Project::Active().Open(it.path());
                    break;
                }
            }
        }

        // 2) A clean shipped export: only `<name>_N.uap` packs + the exe + DLLs.
        //    Find the packs, mount them, and read the project (and shaders) from
        //    them - the build ships no loose project file or shaders/ folder.
        if (!Project::HasActive()) {
            std::string baseName;
            for (const auto& it : std::filesystem::directory_iterator(exeDir, ec)) {
                if (!it.is_regular_file() || it.path().extension() != ".uap") continue;
                const std::string stem = it.path().stem().string(); // "<name>_<n>"
                const auto us = stem.rfind('_');
                if (us != std::string::npos) {
                    baseName = stem.substr(0, us);
                    break;
                }
            }
            if (!baseName.empty() && vfs::MountPacks(exeDir, baseName, exeDir / "Assets")) {
                packedProject = Project::Active().OpenPacked(exeDir);
            }
        }

        if (Project::HasActive()) {
            // Loose-project dev runs still mount the project's own packs (OpenPacked
            // already mounted in the shipped case). Editor builds never mount.
            if (!vfs::IsMounted()) {
                vfs::MountPacks(Project::Active().Root(),
                                Project::Active().Settings().name,
                                Project::Active().AssetsDir());
            }

            // Shipped builds serve shaders from the packs (no loose shaders/
            // folder). Only for a truly packed export, so dev runs with loose
            // shaders aren't slowed by pack misses.
            if (packedProject) {
                const std::filesystem::path assetsRoot = Project::Active().AssetsDir();
                rhi::SetShaderProvider(
                    [assetsRoot](const std::string& leaf, std::vector<u8>& out) -> bool {
                        auto bytes = vfs::ReadFile(assetsRoot / "Shaders" / leaf);
                        if (!bytes) return false;
                        out = std::move(*bytes);
                        return true;
                    });
            }

            // Apply the build configuration (game name / resolution / backend);
            // explicit command-line options win.
            const BuildSettings& build = Project::Active().Settings().build;
            const std::string& gameName = build.gameName.empty()
                                              ? Project::Active().Settings().name
                                              : build.gameName;
            config.title.assign(gameName.begin(), gameName.end());
            if (!config.widthExplicit && build.width > 0) config.width = build.width;
            if (!config.heightExplicit && build.height > 0) config.height = build.height;
            if (!config.fullscreenExplicit) config.fullscreen = build.fullscreen;
            if (!config.vsyncExplicit) config.vsync = build.vsync;
            if (!config.apiExplicit) config.api = ApiFromString(build.backend);
        }
    }
#endif

    HBE_INFO("Heartbreak Engine starting ({})", rhi::ToString(config.api));

    // Worker threads + fibers come up first: subsystems below parallelize onto
    // them (animation, scene streaming, culling).
    HBE_INFO("Boot: initializing job system...");
    jobs::Initialize();
    HBE_INFO("Boot: job system ready.");

    // Navigation smoke test: a headless (no window/GPU) check of the real-time
    // grid A* pathfinder routing around static + dynamic obstacles.
    if (config.navTest) {
        int code = 0;

        // --- Grid A* (real-time, dynamic obstacles, no bake) -------------------
        {
            Scene gscene;
            entt::registry& gr = gscene.Registry();
            const entt::entity floor = gscene.CreateEntity("Floor");
            Transform ft;
            ft.scale = glm::vec3(40.0f, 1.0f, 40.0f);
            gr.emplace<Transform>(floor, ft);
            gr.emplace<MeshInstance>(floor, MeshInstance{});
            gr.emplace<MeshRef>(floor, MeshRef{"prim:plane"});

            nav::GridNav gn;
            gn.EnsureBuilt(gscene, {});
            const glm::vec3 A(-18, 0, -18), B(18, 0, 18);
            const auto reaches = [&](const std::vector<glm::vec3>& p) {
                return !p.empty() &&
                       glm::distance(glm::vec2(p.back().x, p.back().z), glm::vec2(B.x, B.z)) < 2.0f;
            };
            const std::vector<nav::GridObstacle> obs = {{glm::vec3(0, 0, 0), 5.0f}};
            const auto clear = gn.FindPath(A, B, {});
            const auto around = gn.FindPath(A, B, obs);
            f32 minD = 1e9f;
            for (const glm::vec3& p : around)
                minD = std::min(minD, glm::distance(glm::vec2(p.x, p.z), glm::vec2(0.0f)));
            if (gn.Ready() && reaches(clear) && reaches(around) && minD > 4.0f) {
                HBE_INFO("GridNavTest PASS: A* reaches goal, and reroutes around a r=5 obstacle "
                         "(closest corner {:.1f}m, {} corners) - no bake.",
                         minD, static_cast<u32>(around.size()));
            } else {
                HBE_ERROR("GridNavTest FAIL: ready={} clear={} around={} minD={:.1f}.", gn.Ready(),
                          reaches(clear), reaches(around), minD);
                code = 1;
            }

            // Agent walks A->B with the obstacle present (re-plan + steering).
            const entt::entity ag = gscene.CreateEntity("Agent");
            Transform at;
            at.position = A;
            gr.emplace<Transform>(ag, at);
            NavigationAgent na;
            na.target = B;
            na.hasTarget = true;
            na.speed = 5.0f;
            gr.emplace<NavigationAgent>(ag, na);
            const entt::entity obE = gscene.CreateEntity("Obstacle");
            gr.emplace<Transform>(obE, Transform{});
            NavigationObstacle no;
            no.radius = 5.0f;
            gr.emplace<NavigationObstacle>(obE, no);
            f32 reachedAt = -1.0f, minClr = 1e9f;
            for (int i = 0; i < 2400; ++i) {
                nav::UpdateAgents(gscene, gn, 1.0f / 60.0f);
                const glm::vec3 p = gr.get<Transform>(ag).position;
                minClr = std::min(minClr, glm::distance(glm::vec2(p.x, p.z), glm::vec2(0.0f)));
                if (glm::distance(glm::vec2(p.x, p.z), glm::vec2(B.x, B.z)) < 1.0f) {
                    reachedAt = static_cast<f32>(i) / 60.0f;
                    break;
                }
            }
            if (reachedAt >= 0.0f && minClr > 3.5f) {
                HBE_INFO("GridAgentTest PASS: agent walked A->B around the obstacle in {:.1f}s "
                         "(closest approach {:.1f}m).",
                         reachedAt, minClr);
            } else {
                HBE_ERROR("GridAgentTest FAIL: reachedAt={:.1f} minClr={:.1f}.", reachedAt, minClr);
                code = 1;
            }
        }
        jobs::Shutdown();
        return code;
    }

    // Resolve the boot backend order: an explicit --d3d12/--vulkan/--opengl pins a
    // single backend; otherwise the active project's build profile (or the
    // primary-plus-fallback default) drives a try-each-in-turn chain so a machine
    // missing one API falls through to the next.
    std::vector<rhi::GraphicsAPI> backendOrder;
    if (config.apiExplicit) {
        backendOrder = {config.api};
    } else if (Project::HasActive()) {
        backendOrder = ResolveBackendOrder(Project::Active().Settings().build);
    } else {
        using A = rhi::GraphicsAPI;
        backendOrder = {config.api};
        for (A a : {A::D3D12, A::Vulkan, A::OpenGL})
            if (a != config.api) backendOrder.push_back(a);
    }
    // The editor is built on Dear ImGui; OpenGL has no editor UI backend yet, so
    // never select it for the editor (and avoid setting a GL pixel format on the
    // editor window). onFrame_ is set only by the editor.
    const bool editorMode = static_cast<bool>(onFrame_);

    WindowDesc wd;
    wd.title = config.title;
    wd.width = config.width;
    wd.height = config.height;
    wd.posX = config.posX;
    wd.posY = config.posY;
    wd.fullscreen = config.fullscreen;
    HBE_INFO("Boot: creating window ({}x{})...", config.width, config.height);
    Window window(wd);
    if (!window.GetNativeHandle().hwnd) {
        HBE_ERROR("Failed to create window.");
        return 1;
    }
    HBE_INFO("Boot: window created.");

    Renderer renderer;
    bool rendererReady = false;
    for (rhi::GraphicsAPI api : backendOrder) {
        if (!rhi::IsBackendCompiled(api)) {
            HBE_WARN("Backend {} is not compiled into this build; skipping.", rhi::ToString(api));
            continue;
        }
        if (editorMode && api == rhi::GraphicsAPI::OpenGL) {
            HBE_INFO("Editor: OpenGL has no editor UI yet; skipping it for the editor.");
            continue;
        }
        HBE_INFO("Boot: initializing {} backend...", rhi::ToString(api));
        if (config.noCull) renderer.SetCullingEnabled(false);
        if (renderer.Initialize(window, api, config.enableValidation, config.vsync)) {
            config.api = api;
            rendererReady = true;
            break;
        }
        HBE_WARN("Backend {} failed to initialize on this machine; trying the next.",
                 rhi::ToString(api));
    }
    if (!rendererReady) {
        HBE_ERROR("No usable graphics backend (tried {} option(s)).", backendOrder.size());
        return 1;
    }
    HBE_INFO("Graphics backend selected: {} ({} option(s) in the boot order).",
             rhi::ToString(config.api), backendOrder.size());
    if (config.gpuProfile) {
        renderer.SetGpuProfileEnabled(true); // --gpuprofile: per-pass GPU breakdown
        HBE_INFO("GPU profiler requested (--gpuprofile): device active={}. Per-pass timings "
                 "log every ~2s (~1-3 ms/frame; diagnosis only).",
                 renderer.GpuProfileActive() ? 1 : 0);
    }

    Input input;
    window.SetInputSink(&input);

    Scene scene;
    PhysicsWorld physics;
    AudioSystem audio;

    // Route audio through the project's mixer bus tree (engine defaults when
    // the project defines none). The editor's Audio Mixer reconfigures live.
    {
        std::vector<AudioBusDesc> buses;
        if (Project::HasActive()) {
            for (const AudioBusSetting& b : Project::Active().Settings().audioBuses) {
                buses.push_back({b.name, b.parent, b.volume, b.muted});
            }
        }
        if (buses.empty()) buses = DefaultAudioBuses();
        audio.ConfigureBuses(buses);
    }
    window_ = &window;
    renderer_ = &renderer;
    scene_ = &scene;
    input_ = &input;
    physics_ = &physics;
    audio_ = &audio;

    // The pathfinder + current level live for the whole run; the pointers let the
    // editor inspect them and the runtime switch levels.
    nav::GridNav gridNav;
    gridNav_ = &gridNav; // real-time A* the agents path on (auto-builds, no bake)
    scene::Level level;
    currentLevel_ = &level;

    bool sceneBuilt = false;

    // Studio boot splash FIRST - before the heavy environment bake + scene loads -
    // so the window shows the splash (not a black screen) while the engine warms
    // up. Load only the studio UI scene, present one frame, then do the work behind
    // it (the presented frame stays on screen through each blocking step). Runtime
    // only (the editor uses its own play mode, onInit_ set). The {log} token shows
    // the latest line as each step completes.
    bool studioSplash = false;
    if (!onInit_ && Project::HasActive() &&
        !Project::Active().Settings().studioLoadingScene.empty()) {
        const std::filesystem::path studio =
            Project::Active().AssetsDir() / Project::Active().Settings().studioLoadingScene;
        if (vfs::Exists(studio) && scene::LoadScene(scene, renderer, studio)) {
            flowActive_ = true;
            gameState_ = GameState::Booting;
            sceneBuilt = true;
            studioSplash = true;
            HBE_INFO("Boot: studio splash shown; warming up...");
            PresentBootSplash(0.05f); // visible immediately, before the IBL bake
        } else {
            HBE_WARN("Studio scene '{}' failed to load; skipping splash.",
                     Project::Active().Settings().studioLoadingScene);
        }
    }

    // Lighting environment (IBL + sky) applies to every scene, loaded or demo. The
    // studio splash (above) stays on screen through this ~second-long bake.
    if (studioSplash) HBE_INFO("Boot: building lighting environment...");
    scene::SetupEnvironment(scene, renderer);
    if (studioSplash) PresentBootSplash(0.5f); // refresh: IBL done, log advanced

    // Persistent UI scene: load ALL screens (UIPanel subtrees) ONCE and keep them
    // resident across gameplay scene swaps (tag Persistent so the gameplay Replace
    // spares them). The UIManager shows/hides panels. This is THE game-UI path;
    // without it the runtime boots straight into the startup scene below.
    if (!onInit_ && Project::HasActive() && !Project::Active().Settings().uiScene.empty()) {
        const std::filesystem::path uip =
            Project::Active().AssetsDir() / Project::Active().Settings().uiScene;
        scene::SceneData uiData;
        if (vfs::Exists(uip) && scene::ParseSceneFile(uip, uiData)) {
            scene::StagedAssets uiStaged;
            scene::StageAssets(uiData, Project::Active().AssetsDir(), uiStaged);
            std::vector<entt::entity> uiEnts;
            scene::Instantiate(scene, renderer, uiData, uiStaged, scene::LoadMode::Additive,
                               &uiEnts, "__ui");
            for (const entt::entity e : uiEnts) scene.Registry().emplace<Persistent>(e);
            uiManager_.Init(scene);
            uiManagerMode_ = true;
            flowActive_ = true;
            sceneBuilt = true; // UI-only overlay; the gameplay world loads on Play
            if (!studioSplash) { // no splash: go straight to the initial (menu) panel
                uiManager_.ShowInitial(scene);
                gameState_ = GameState::MainMenu;
            }
            HBE_INFO("Boot: persistent UI scene '{}' loaded ({} entities).",
                     Project::Active().Settings().uiScene, static_cast<u32>(uiEnts.size()));
        } else {
            HBE_WARN("UI scene '{}' failed to load.", Project::Active().Settings().uiScene);
        }
    }

    // Per-user settings (volume/graphics/brightness/captions): load + apply. Graphics +
    // brightness are re-applied every frame (see the loop); volume + captions are set once.
    if (Project::HasActive()) {
        const std::string& gn = Project::Active().Settings().build.gameName;
        userSettingsDir_ = UserSettings::ResolveDir(
            gn.empty() ? Project::Active().Settings().name : gn);
        userSettings_.Load(userSettingsDir_); // keeps defaults if absent
        // Always log the loaded preset: a SAVED usersettings.json silently overrides the
        // shipped default, which once hid that a player was running the full High stack
        // while every "Medium" optimization was tuned around a preset they weren't on.
        static const char* kPresetNames[] = {"High", "Medium", "Low"};
        const int gp = glm::clamp(userSettings_.graphicsPreset, 0, 2);
        HBE_INFO("User settings loaded: graphics preset {} ({}){}", gp, kPresetNames[gp],
                 onInit_ ? " - editor shows the authored look regardless" : "");
        audio.SetBusVolume("Master", userSettings_.masterVolume);
        audio.SetCaptionsEnabled(userSettings_.captionsEnabled);
        SyncActionMap(); // action defaults (project) + rebind overrides (user settings)
    }

    // Build the scene (skipped when the studio splash or UI scene already owns the
    // world): startup scene / model / default world.
    if (!sceneBuilt && Project::HasActive() && !Project::Active().Settings().startupScene.empty()) {
        const std::filesystem::path startup =
            Project::Active().AssetsDir() / Project::Active().Settings().startupScene;
        if (vfs::Exists(startup)) { // pack-aware (shipped builds have no loose files)
            // A level layer file (<name>.static/.dynamic) loads the whole level
            // via the Level class; a plain scene (incl. a UI/menu scene) loads on
            // its own.
            if (scene::IsLevelMember(startup)) {
                LoadLevel(scene::ResolveLevel(startup).base);
                sceneBuilt = currentLevel_->Loaded();
            } else {
                sceneBuilt = scene::LoadScene(scene, renderer, startup);
            }
        }
        if (!sceneBuilt) {
            HBE_WARN("Startup scene '{}' failed to load.",
                     Project::Active().Settings().startupScene);
        }
    }
    if (!sceneBuilt && !config.modelPath.empty()) {
        sceneBuilt = scene::LoadModel(scene, renderer, config.modelPath);
        if (!sceneBuilt) HBE_WARN("Falling back to the default scene.");
    }
    if (!sceneBuilt) {
        scene::BuildDefaultScene(scene);
    }
    if (config.stressCount > 0) {
        scene::SpawnStress(scene, renderer, config.stressCount, config.stressShared);
    }

    if (config.forceDof) scene.Environment().post.dofEnabled = 1;
    if (config.forceMotionBlur) scene.Environment().post.motionBlurEnabled = 1;
    if (config.forceSsr) scene.Environment().post.ssrEnabled = 1;
    if (config.forceAutoExposure) scene.Environment().post.autoExposureEnabled = 1;

    // World streaming (distance-based partition): a .hbworld manifest, or the
    // built-in smoke test (a line of cells the focus sweeps across). Loads run
    // on the job system; the focus point is the camera at runtime.
    StreamingWorld streamingWorld;
    streamingWorld_ = &streamingWorld; // editor loads/inspects it via GetStreamingWorld()
    if (config.worldTest) {
        const std::filesystem::path tmp =
            std::filesystem::temp_directory_path() / "hbe_worldtest";
        std::error_code wec;
        std::filesystem::create_directories(tmp, wec);
        const std::filesystem::path cellScene = tmp / "cell.hbscene";
        {
            // One reusable floor cell, placed per-cell via StreamingCell::offset.
            Scene tmpScene;
            const entt::entity e = tmpScene.CreateEntity("Floor");
            Transform tf;
            tf.scale = glm::vec3(8.0f, 1.0f, 8.0f); // a walkable patch (survives erosion)
            tmpScene.Registry().emplace<Transform>(e, tf);
            MeshInstance mi;
            mi.baseColor = glm::vec4(0.85f, 0.32f, 0.22f, 1.0f);
            tmpScene.Registry().emplace<MeshInstance>(e, mi);
            tmpScene.Registry().emplace<MeshRef>(e, MeshRef{"prim:plane"});
            scene::SaveScene(tmpScene, cellScene);
        }
        std::vector<StreamingCell> cells;
        for (int i = 0; i < 12; ++i) {
            StreamingCell c;
            c.name = "cell_" + std::to_string(i);
            c.scene = cellScene.string();
            c.center = glm::vec3(static_cast<f32>(i) * 20.0f, 0.0f, 0.0f);
            c.offset = c.center;
            c.loadRadius = 25.0f;
            c.unloadRadius = 35.0f;
            cells.push_back(std::move(c));
        }
        streamingWorld.SetCells(std::move(cells), tmp);
        HBE_INFO("StreamingWorld: smoke test, 12 cells along +X.");
    } else if (!config.worldPath.empty()) {
        const std::filesystem::path assetsDir =
            Project::HasActive()
                ? Project::Active().AssetsDir()
                : std::filesystem::path(config.worldPath).parent_path();
        streamingWorld.LoadManifest(config.worldPath, assetsDir);
    }

    // NavigationAgents path on GridNav (the real-time A*), which auto-builds from
    // static geometry each frame (see the agent update below) - no startup bake.

    // World-space UI smoke test (--uiworldtest): a lit page floating in the scene
    // with a label + slider, exercising canvas->texture->quad on both backends.
    if (config.uiWorldTest) {
        auto& reg = scene.Registry();
        const entt::entity canvasE = scene.CreateEntity("UIWorldTest");
        Transform tf;
        tf.position = {0.0f, 2.0f, 0.0f};
        // Stand the page up: local XZ faces +Y by default; pitch it 90 degrees so
        // it faces the orbiting demo camera instead of the sky.
        tf.rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        reg.emplace<Transform>(canvasE, tf);
        UICanvas canvas;
        canvas.worldSpace = true;
        canvas.refWidth = 512.0f;
        canvas.refHeight = 512.0f;
        canvas.worldWidth = 2.0f;
        canvas.emissive = 0.15f; // slight glow so it reads in the demo scene
        reg.emplace<UICanvas>(canvasE, canvas);
        const auto child = [&](UIElement el, glm::vec2 offset, glm::vec2 size,
                               entt::entity parent = entt::null) {
            const entt::entity e = scene.CreateEntity("UIWorldTestEl");
            el.offset = offset;
            el.size = size;
            reg.emplace<UIElement>(e, std::move(el));
            reg.emplace<Parent>(e, Parent{parent == entt::null ? canvasE : parent});
            return e;
        };
        UIElement panel;
        panel.type = UIElement::Type::Panel;
        panel.color = {0.93f, 0.90f, 0.82f, 1.0f}; // paper
        panel.anchorMin = {0.0f, 0.0f};
        panel.anchorMax = {1.0f, 1.0f};
        child(panel, {0.0f, 0.0f}, {0.0f, 0.0f});
        UIElement label;
        label.type = UIElement::Type::Label;
        label.text = "WORLD UI";
        label.textSize = 64.0f;
        label.color = {0.12f, 0.10f, 0.08f, 1.0f};
        label.anchorMin = {0.5f, 0.25f};
        label.anchorMax = {0.5f, 0.25f};
        child(label, {0.0f, 0.0f}, {480.0f, 90.0f});
        UIElement slider;
        slider.type = UIElement::Type::Slider;
        slider.action = "uitest:slider";
        slider.color = {0.35f, 0.30f, 0.25f, 1.0f};
        slider.fillColor = {0.80f, 0.35f, 0.20f, 1.0f};
        slider.anchorMin = {0.5f, 0.45f};
        slider.anchorMax = {0.5f, 0.45f};
        child(slider, {0.0f, 0.0f}, {400.0f, 60.0f});

        // ScrollView (U3 smoke): tall content clipped to the page's lower third,
        // wheel-scrollable when the pointer ray hits it.
        UIElement sv;
        sv.type = UIElement::Type::ScrollView;
        sv.color = {0.85f, 0.80f, 0.70f, 1.0f};
        sv.fillColor = {0.80f, 0.35f, 0.20f, 1.0f};
        sv.anchorMin = {0.5f, 0.78f};
        sv.anchorMax = {0.5f, 0.78f};
        const entt::entity svE = child(sv, {0.0f, 0.0f}, {420.0f, 170.0f});

        // TextInput (U4 smoke): click it (ray pointer) and type; Enter commits.
        UIElement ti;
        ti.type = UIElement::Type::TextInput;
        ti.placeholder = "type here...";
        ti.textSize = 30.0f;
        ti.color = {0.20f, 0.18f, 0.15f, 1.0f};
        ti.fillColor = {0.80f, 0.35f, 0.20f, 1.0f};
        ti.anchorMin = {0.5f, 0.58f};
        ti.anchorMax = {0.5f, 0.58f};
        child(ti, {0.0f, 0.0f}, {400.0f, 52.0f});

        for (int i = 0; i < 8; ++i) {
            UIElement line;
            line.type = UIElement::Type::Label;
            line.text = "scroll line " + std::to_string(i + 1);
            line.textSize = 34.0f;
            line.color = {0.15f, 0.12f, 0.10f, 1.0f};
            line.anchorMin = {0.5f, 0.0f}; // top-anchored content flows downward
            line.anchorMax = {0.5f, 0.0f};
            child(line, {0.0f, 30.0f + 55.0f * static_cast<f32>(i)}, {380.0f, 48.0f},
                  svE);
        }

        // Plain 3D text objects beside the page: one oriented, one billboard.
        {
            const entt::entity t1 = scene.CreateEntity("WorldTextTest");
            Transform t1f;
            t1f.position = {-2.5f, 2.0f, 0.0f};
            reg.emplace<Transform>(t1, t1f);
            WorldText wt1;
            wt1.text = "3D TEXT\nownz";
            wt1.size = 0.4f;
            wt1.color = {1.0f, 0.85f, 0.3f, 1.0f};
            reg.emplace<WorldText>(t1, wt1);

            const entt::entity t2 = scene.CreateEntity("WorldTextBillboard");
            Transform t2f;
            t2f.position = {2.5f, 2.0f, 0.0f};
            reg.emplace<Transform>(t2, t2f);
            WorldText wt2;
            wt2.text = "billboard";
            wt2.size = 0.3f;
            wt2.color = {0.5f, 0.9f, 1.0f, 1.0f};
            wt2.billboard = true;
            reg.emplace<WorldText>(t2, wt2);
        }
        HBE_INFO("UIWorldTest: spawned a world-space canvas (512x512, 2m wide) + "
                 "two WorldText objects.");
    }

    // Route OS resize events into the renderer.
    window.SetResizeCallback([&renderer](u32 w, u32 h) { renderer.Resize(w, h); });

    // One-time application init (e.g. the editor wires up ImGui here).
    if (onInit_) onInit_(*this);

    using clock = std::chrono::high_resolution_clock;
    auto last = clock::now();
    auto fpsLast = last;
    u32 fpsFrames = 0;
    // Per-window CPU phase timers (ms accumulated; averaged + reset each Perf report).
    f64 accGpMs = 0.0, accFacialMs = 0.0, accRenderMs = 0.0;
    std::vector<rhi::UIVertex> uiVertices; // reused each frame
    std::vector<ui::WorldUIBatch> worldUIBatches;      // world-canvas triangles (reused)
    std::vector<Renderer::WorldUIDraw> worldUIDraws;   // -> renderer, one frame each
    std::vector<rhi::ParticleVertex> particleAlpha, particleAdd; // reused each frame
    glm::vec3 streamFocus(0.0f);           // last streaming focus (for stats log)
    f32 worldTestT = 0.0f;                 // smoke-test focus-sweep clock
    cam::CameraState cameraState;          // persistent camera smoothing/blend state
    bool prevGameCamEnabled = false;       // rising edge -> snap the camera
    bool musicStarted = false;             // adaptive music armed while the game runs
    entt::entity musicZoneActive = entt::null; // last MusicZone the player was inside
    f32 dtSmooth = 0.0f;                   // EMA of frame delta (motion smoothing)
    f32 winMaxDt = 0.0f;                   // worst frame in the current report window
    u32 winJank = 0;                       // frames slower than 45 FPS this window

    while (true) {
        // Roll input edge state, then pump: this frame's events land in `input`.
        input.NewFrame();
        if (!window.PumpMessages()) break;

        const auto now = clock::now();
        f32 rawDt = std::chrono::duration<f32>(now - last).count();
        last = now;
        // Frame pacing. A stall (asset/PSO upload, window drag, a missed vsync) yields a
        // huge delta that would teleport physics/animation/camera; clamp it so a hitch
        // briefly slows time instead of jolting. Then lightly smooth (EMA) so per-frame
        // jitter - vsync beat, measurement noise - doesn't make motion shimmer. The raw
        // value still feeds the stability counters below so spikes stay visible.
        winMaxDt = glm::max(winMaxDt, rawDt);
        if (rawDt > 1.0f / 45.0f) ++winJank;
        rawDt = glm::clamp(rawDt, 0.0f, 0.1f); // <= 100 ms floor (never < 10 FPS of sim)
        dtSmooth = (dtSmooth <= 0.0f) ? rawDt : glm::mix(dtSmooth, rawDt, 0.2f);
        dt_ = dtSmooth; // REAL frame time (FPS display); dev game-speed doesn't skew it
        // Dev menu game-speed / pause scales the simulation delta (1.0 in ship builds).
        // The offline MOVIE RENDER overrides the SIMULATION delta with a fixed dt=1/fps
        // (deterministic frame-stepping) - but ONLY dt, never dt_, so caption/loading
        // clocks (which use dt_) are not time-warped.
        const f32 dt = (renderFixedDt_ > 0.0f) ? renderFixedDt_ : (dtSmooth * devTimeScale_);

        // Periodic FPS report.
        if (++fpsFrames >= 1) {
            const f32 elapsed = std::chrono::duration<f32>(now - fpsLast).count();
            if (elapsed >= 2.0f) {
                // Average AND worst-case: an average alone hides stutter. "worst" is
                // the single slowest frame in the window; "jank" counts frames under
                // 45 FPS - both > 0 with a smooth average means uneven pacing.
                const Renderer::FrameStats& rs = renderer.Stats();
                const ui::UIFrameStats& us = uiCtx_.stats;
                HBE_INFO("Perf: {:.1f} FPS avg ({:.2f} ms) | worst {:.1f} ms | {} jank "
                         "frame(s) <45 FPS | draws {}/{} ({} culled, {} runs x{} inst) | "
                         "UI {} el {} verts {} txt {} maps",
                         fpsFrames / elapsed, 1000.0f * elapsed / fpsFrames,
                         1000.0f * winMaxDt, winJank, rs.drawn, rs.total, rs.culled,
                         rs.instancedDraws, rs.totalInstances, us.elements, us.verts,
                         us.textLayouts, us.mapRebuilds);
                HBE_INFO("  CPU phases (avg/frame): gameplay {:.2f} ms | facial {:.2f} ms | "
                         "renderScene-submit {:.2f} ms",
                         accGpMs / fpsFrames, accFacialMs / fpsFrames, accRenderMs / fpsFrames);
                accGpMs = accFacialMs = accRenderMs = 0.0;
                winMaxDt = 0.0f;
                winJank = 0;
                if (streamingWorld.Active()) {
                    const StreamingWorld::Stats st = streamingWorld.GetStats();
                    HBE_INFO("Streaming: {}/{} cells loaded, {} loading, {} entities "
                             "(focus x={:.0f}).",
                             st.loaded, st.cells, st.loading, st.entities, streamFocus.x);
                }
                fpsFrames = 0;
                fpsLast = now;
            }
        }

        if (window.IsMinimized()) {
            ::Sleep(10); // nothing to draw; yield the CPU
            continue;
        }

        // Developer overlay (only when the project opts in via BuildSettings):
        // Ctrl+` toggles it; while open, function keys run dev actions. Ships in
        // the runtime build (gated by the flag), no editor required.
        const bool devEnabled =
            Project::HasActive() && Project::Active().Settings().build.devMenu;
        // Suppress the dev overlay chord + its function keys while listening for a
        // rebind, so pressing Ctrl+` (or a dev F-key) during a rebind neither toggles
        // the overlay nor fires a dev action.
        // The dev menu owns the keyboard while open: it reads RAW edges (the frame's
        // text-capture mute, set below, keeps gameplay/UI-nav/schematics quiet), and
        // its Ctrl+` toggle is raw so it still closes while the mute is engaged.
        if (devEnabled && !actionMap_.Rebinding() && input.IsKeyDownRaw(Key::Ctrl) &&
            input.WasKeyPressedRaw(Key::Grave)) {
            devMenuOpen_ = !devMenuOpen_;
            if (devMenuOpen_) { DevMenuScanLevels(); devMenuSel_ = 0; } // fresh level list on open
        }
        if (devEnabled && devMenuOpen_ && !actionMap_.Rebinding()) {
            RebuildDevMenu(); // reflects live values; consumed by BuildDevOverlay this frame
            if (!devItems_.empty()) {
                const int n = static_cast<int>(devItems_.size());
                devMenuSel_ = glm::clamp(devMenuSel_, 0, n - 1);
                const auto step = [&](int dir) {
                    for (int k = 0; k < n; ++k) { // skip non-selectable section headers
                        devMenuSel_ = (devMenuSel_ + dir + n) % n;
                        if (!devItems_[static_cast<usize>(devMenuSel_)].header) break;
                    }
                };
                if (devItems_[static_cast<usize>(devMenuSel_)].header) step(+1); // never rest on a header
                if (input.WasKeyPressedRaw(Key::Down)) step(+1);
                if (input.WasKeyPressedRaw(Key::Up)) step(-1);
                DevMenuItem& it = devItems_[static_cast<usize>(devMenuSel_)];
                if (input.WasKeyPressedRaw(Key::Enter) && it.activate) it.activate();
                if (it.adjust) {
                    if (input.WasKeyPressedRaw(Key::Right)) it.adjust(+1);
                    if (input.WasKeyPressedRaw(Key::Left)) it.adjust(-1);
                }
                // Re-bake the rows so BuildDevOverlay shows the just-changed value/state
                // (labels are baked at build time; an adjust above would otherwise lag a frame).
                if (devMenuOpen_) RebuildDevMenu();
            }
            // Quick shortcuts (also work with the menu open).
            if (input.WasKeyPressedRaw(Key::F5)) SaveGame("checkpoint");
            if (input.WasKeyPressedRaw(Key::F9)) LoadGame("checkpoint");
            if (flowActive_ && input.WasKeyPressedRaw(Key::F2)) FlowReload();
            if (flowActive_ && input.WasKeyPressedRaw(Key::F3)) FlowMainMenu();
        }

        const std::filesystem::path assetsDir =
            Project::HasActive() ? Project::Active().AssetsDir() : std::filesystem::path();

        anim::Update(scene, dt); // keyframe tracks pose entities first
        // Build/refresh any dirty chunked terrain (cheap when nothing changed).
        terrain::Update(scene, renderer);
        // Motion matching picks each animator's clip from movement intent BEFORE
        // the skeletal pose is sampled (play mode / runtime only).
        if (physics.IsRunning()) anim::UpdateMotionMatching(scene, assetsDir, dt);
        // Skeletal animation: advance Animators and rebuild joint palettes.
        anim::UpdateSkeletal(scene, assetsDir, dt);
        // Facial: lip-sync + blink + expression -> MorphState.weights (consumed by
        // CollectDrawItems -> the vertex shader's pre-skin morph accumulation). Play/
        // runtime only: in edit mode it would overwrite authored MorphState weights.
        {
            const auto _pt = clock::now();
            if (physics.IsRunning()) facial::Update(scene, dt);
            accFacialMs += std::chrono::duration<f64, std::milli>(clock::now() - _pt).count();
        }

        // The project's canvas configuration (scale mode + reference size).
        ui::CanvasConfig uiConfig;
        if (Project::HasActive()) {
            const BuildSettings& build = Project::Active().Settings().build;
            uiConfig.mode = static_cast<ui::ScaleMode>(
                glm::clamp(build.uiScaleMode, 0u, 2u));
            uiConfig.refWidth = static_cast<f32>(glm::max(build.uiRefWidth, 64u));
            uiConfig.refHeight = static_cast<f32>(glm::max(build.uiRefHeight, 64u));
        }
        const glm::vec2 uiTarget = renderer.RenderTargetSize();

        // UI interaction BEFORE scripts, so buttons report fresh hover/click
        // state to gameplay code this frame.
        {
            glm::vec2 pointerNorm(-1.0f, -1.0f);
            if (uiPointerExternal_) {
                // Editor-fed: normalized coords over the Game image.
                pointerNorm = {uiPointerU_, uiPointerV_};
            } else if (window.Width() > 0 && window.Height() > 0) {
                pointerNorm = {input.MouseX() / window.Width(),
                               input.MouseY() / window.Height()};
            }
            // World-space ("physical") UI is point-and-click: ray-pick the pages
            // only while the cursor is FREE. A locked (recentered) cursor would be
            // a de-facto crosshair, which is not the interaction model.
            ui::PointerState pointers;
            if (!IsCursorLocked())
                ui::ComputeWorldPointers(scene, renderer.GetCamera(), pointerNorm, pointers,
                                         uiCtx_);
            // Cached interaction: hit-tests last frame's layout + clears flags on
            // the touched list only (no full registry scans).
            ui::UpdateInteraction(scene, input, pointerNorm, &pointers, uiCtx_);
            // Keyboard/gamepad navigation + TextInput editing: moves the focus
            // ring, activates widgets via the same clicked/changed flags the
            // mouse sets, edits text buffers. Suspended while the editor owns
            // the keyboard (ImGui feeds SetUIKeyboardCaptured).
            const bool wasEditing = ui::WantsTextInput(uiCtx_);
            if (!uiKeyboardExternal_ && !devMenuOpen_) // dev menu owns nav while open
                ui::UpdateNavigation(scene, input, uiCtx_, dt);
            // While a text field is being edited, mute the normal keyboard
            // queries so gameplay / dev-menu / schematic key reads stay quiet
            // (the editing code uses the raw variants). Latch capture through
            // the frame the session ENDS too (wasEditing): the Enter/Escape that
            // committed/cancelled must not also trigger an Enter/Escape-bound
            // schematic or gameplay action the same frame. The edge is gone by
            // the next frame, so capture releases naturally.
            // Also mute while the dev menu is open so its raw-read navigation doesn't
            // double-drive gameplay / schematics / character input this frame.
            input.SetTextCapture(devMenuOpen_ ||
                                 (!uiKeyboardExternal_ &&
                                  (wasEditing || ui::WantsTextInput(uiCtx_))));
            PlayUISounds(); // hover/click one-shots (edge-detected)
        }

        // High-level game flow: reads the fresh button-click state above to drive
        // menu -> loading -> gameplay transitions (runtime only; opt-in via the
        // project's main-menu scene). Cursor lock is applied here too.
        // Flow transitions + captions are PRESENTATION timers, not simulation - drive
        // them off the real frame time so dev-menu pause/slow-mo (which scales `dt`)
        // never freezes a loading fade or the caption crawl.
        if (flowActive_) UpdateGameFlow(dt_);
        if (!onInit_) {             // runtime only (not the editor preview)
            ApplyChangedSettings(); // live-apply any Settings widget the user just changed
            UpdateCaptions(dt_);    // drain audio captions -> drive the caption element
        }

        // Visual-script "Schematics" tick while the simulation runs (the editor's
        // play mode gates physics; the runtime always plays). On Start / On Update.
        schematic::Update(scene, input, dt, physics.IsRunning());
        // Player/character input intent BEFORE physics, which drives the capsule
        // CharacterVirtual (gravity + world collision). Camera-relative movement
        // uses the current view's forward.
        if (physics.IsRunning())
            character::Update(scene, input, dt, renderer.GetCamera().Forward());
        physics.Update(scene, dt);
        // Gameplay band: AI + spawning/encounters + combat + player fire. Runs
        // after physics (fresh positions for line-of-sight and hit tests) and
        // BEFORE nav::UpdateAgents so an AI-set NavigationAgent target steers the
        // same frame (no one-frame lag).
        {
            const auto _pt = clock::now();
            if (physics.IsRunning())
                gameplay::Update(scene, physics, renderer, input, renderer.GetCamera(), dt);
            accGpMs += std::chrono::duration<f64, std::milli>(clock::now() - _pt).count();
        }
        // NavigationAgents steer along real-time grid A* paths while the
        // simulation runs (play mode in the editor; always in the runtime). The
        // grid auto-rebuilds from static geometry (no bake) and re-plans around
        // moving NavigationObstacles.
        if (physics.IsRunning()) {
            const std::filesystem::path navAssets =
                Project::HasActive() ? Project::Active().AssetsDir() : std::filesystem::path();
            gridNav.EnsureBuilt(scene, navAssets);
            nav::UpdateAgents(scene, gridNav, dt);
        }
        // Control rebinding (from a "rebind:<action>" UI button): capture the next
        // key/button and persist. Runs regardless of physics so it works in menus.
        rebindJustCommitted_ = false; // reset each frame; set below on a fresh commit
        if (input_ && actionMap_.Rebinding()) {
            if (actionMap_.UpdateRebind(*input_)) {
                userSettings_.inputBindings = actionMap_.Overrides();
                SaveUserSettings();
                // The just-bound key's down-edge is still live this frame; guard so
                // UpdateInteractions doesn't read it as an Interact press (double-fire).
                rebindJustCommitted_ = true;
            }
        }
        // Checkpoints: fire box-triggers the player entered, then perform any save
        // a reached checkpoint (trigger / script / schematic) requested this frame.
        if (physics.IsRunning()) {
            game::UpdateCheckpoints(scene);
            std::string cpId;
            if (game::ConsumeSaveRequest(cpId)) SaveGame("checkpoint");
            // Interactables (proximity "[E]" prompt + key) + box TriggerVolumes.
            UpdateInteractions(scene, dt);
        }
        if (physics.IsRunning()) anim::UpdateRotators(scene, dt);
        // Spatial-audio occlusion: push the project's tuning + a physics segment
        // test so 3D sources are attenuated/muffled by geometry (multi-ray leaks
        // through gaps). Only fed while the game runs (colliders live then).
        if (Project::HasActive()) {
            const AudioOcclusionSettings& os = Project::Active().Settings().occlusion;
            audio.SetOcclusion({os.enabled, os.rays, os.attenuation, os.cutoffHz, os.spread});
        }
        std::function<bool(const glm::vec3&, const glm::vec3&)> segBlocked;
        if (physics.IsRunning()) {
            segBlocked = [&physics](const glm::vec3& a, const glm::vec3& b) {
                glm::vec3 d = b - a;
                const f32 len = glm::length(d);
                if (len < 1e-3f) return false;
                return physics.Raycast(a, d / len, len) < len - 0.05f; // blocked before reaching b
            };
        }
        audio.UpdateScene(scene,
                          Project::HasActive() ? Project::Active().AssetsDir()
                                               : std::filesystem::path(),
                          renderer.GetCamera().Position(),
                          renderer.GetCamera().Forward(),
                          physics.IsRunning(), // autoplay only when the game runs
                          segBlocked, dt);
        audio.Update();
        // Adaptive music: install + start the project's graph on the edge into the
        // running game; crossfade out when it stops. Parameters/state are then driven
        // by gameplay (schematics) or the editor's Music panel preview.
        {
            const bool running = physics.IsRunning();
            if (running && !musicStarted) {
                if (Project::HasActive() && !Project::Active().Settings().musicGraph.empty()) {
                    const std::filesystem::path assets = Project::Active().AssetsDir();
                    if (auto g = assets::LoadMusicGraph(
                            assets / Project::Active().Settings().musicGraph)) {
                        audio.SetMusicGraph(*g, assets);
                        const std::string& start = Project::Active().Settings().musicStartState;
                        audio.PlayMusicState(start.empty() ? g->initialState : start, 0.5f);
                    }
                }
                musicStarted = true;
                musicZoneActive = entt::null; // re-apply the containing zone next frame
            } else if (!running && musicStarted) {
                audio.StopMusic();
                musicStarted = false;
                musicZoneActive = entt::null;
            }
            // Drain deferred music commands queued by gameplay (schematics): state
            // changes, parameter sets, and one-shot stingers.
            if (running) {
                const std::filesystem::path mAssets =
                    Project::HasActive() ? Project::Active().AssetsDir() : std::filesystem::path();
                std::string st;
                if (game::ConsumeMusicState(st)) audio.PlayMusicState(st);
                std::string pn;
                f32 pv = 0.0f;
                while (game::ConsumeMusicParameter(pn, pv)) audio.SetMusicParameter(pn, pv);
                std::string sa;
                while (game::ConsumeStinger(sa)) audio.PostStinger(mAssets / sa, "Music");
                // World music zones: the highest-priority enabled MusicZone the
                // player is inside drives the music on the ENTER edge (crossfade to
                // its state + optional parameter). PlayMusicState no-ops if already
                // there, so this won't fight a schematic that set the same state.
                {
                    glm::vec3 lp = renderer.GetCamera().Position();
                    auto players = scene.Registry().view<Transform, CharacterController>();
                    if (players.begin() != players.end())
                        lp = glm::vec3(scene.WorldMatrix(*players.begin())[3]);
                    entt::entity bestZone = entt::null;
                    int bestPri = 0;
                    for (const entt::entity ze : scene.Registry().view<MusicZone>()) {
                        MusicZone& mz = scene.Registry().get<MusicZone>(ze);
                        mz.active = false;
                        if (!mz.enabled || !scene.Registry().all_of<Transform>(ze)) continue;
                        const glm::vec3 a = glm::abs(
                            glm::vec3(glm::inverse(scene.WorldMatrix(ze)) * glm::vec4(lp, 1.0f)));
                        // >= so the LAST equal-priority overlapping zone wins (matches
                        // CameraZone); the null guard still admits a lone negative-priority zone.
                        if (a.x <= mz.halfExtents.x && a.y <= mz.halfExtents.y &&
                            a.z <= mz.halfExtents.z &&
                            (bestZone == entt::null || mz.priority >= bestPri)) {
                            bestZone = ze;
                            bestPri = mz.priority;
                        }
                    }
                    if (bestZone != entt::null) scene.Registry().get<MusicZone>(bestZone).active = true;
                    if (bestZone != musicZoneActive) {
                        musicZoneActive = bestZone;
                        if (bestZone != entt::null) {
                            const MusicZone& mz = scene.Registry().get<MusicZone>(bestZone);
                            if (!mz.musicState.empty())
                                audio.PlayMusicState(mz.musicState, mz.fadeSeconds);
                            if (!mz.parameter.empty())
                                audio.SetMusicParameter(mz.parameter, mz.parameterValue);
                        }
                    }
                }
                // One-shot voicelines from schematics: play the clip; PlayUAF
                // surfaces its baked "Speaker: caption" into the caption stack.
                std::string va;
                while (game::ConsumeVoiceline(va)) audio.PlayUAF(mAssets / va);
                // Start a requested dialogue (a .hbdialogue graph): load it and
                // enter its Start node (walks to the first Line/Choice).
                std::string dlgPath;
                if (game::ConsumeDialogue(dlgPath)) {
                    dlg::Graph g;
                    if (dlg::LoadGraph(mAssets / dlgPath, g)) {
                        ClearDialogueChoices();
                        dialogueGraph_ = std::move(g);
                        dialogueTimer_ = 0.0f;
                        EnterDialogueNode(dialogueGraph_.StartNode());
                    }
                }
                UpdateDialogue(dt); // step the active conversation over time
                // Start a requested cutscene (a .hbcutscene): take over the
                // camera; UpdateCutscene evaluates its tracks each frame. Set
                // camera BEFORE the camera-apply block below sees the disable.
                std::string cs;
                if (game::ConsumeCutscene(cs)) {
                    if (auto c = assets::LoadCutscene(mAssets / cs)) {
                        const bool takesCamera = !c->camera.empty();
                        cutscene_ = std::move(*c);
                        cutsceneTime_ = 0.0f;
                        // Only take over the camera for a cutscene that actually
                        // has a camera track (empty track = "leave the camera
                        // alone"), and capture the restore-state ONLY when we
                        // don't already own the camera. Otherwise a chained/
                        // re-triggered cutscene would clobber the original state
                        // with the already-disabled value and strand the camera.
                        if (takesCamera && !cutsceneCamOwned_) {
                            cutsceneRestoreCam_ = gameCameraEnabled_;
                            SetGameCameraEnabled(false); // engine owns the camera now
                            cutsceneCamOwned_ = true;
                        }
                    }
                }
                UpdateCutscene(dt); // evaluate camera/anim/dialogue tracks
            }
            audio.UpdateMusic(dt);
        }
        // Drain deferred UI panel commands queued by schematic UI nodes (the engine
        // owns the UIManager). Ordered FIFO: a Push then Pop the same frame nets out.
        if (uiManagerMode_) {
            game::UICommand uc;
            while (game::ConsumeUICommand(uc)) {
                switch (uc.op) {
                    case game::UICommand::Op::Show: uiManager_.Show(scene, uc.panel); break;
                    case game::UICommand::Op::Push: uiManager_.Push(scene, uc.panel); break;
                    case game::UICommand::Op::Pop:  uiManager_.Pop(scene); break;
                }
                // Keep parity with the built-in "settings" action: seed the widgets
                // whenever a schematic brings the Settings panel up.
                if (uc.op != game::UICommand::Op::Pop && uc.panel == "Settings")
                    SeedSettingsWidgets();
            }
        }
        renderer.Update(dt);
        // The active game camera (zone-selected or primary) overrides the
        // editor/orbit camera when enabled (play mode and the runtime). Snap
        // on the rising edge so entering play mode doesn't blend from the
        // editor freecam's pose.
        if (gameCameraEnabled_) {
            if (!prevGameCamEnabled) cameraState.valid = false;
            const cam::RaycastFn camRay = [&physics](const glm::vec3& o, const glm::vec3& d,
                                                     f32 m) { return physics.Raycast(o, d, m); };
            if (cam::Update(scene, renderer.GetCamera(), cameraState, dt, input, camRay)) {
                renderer.SetOrbitEnabled(false); // the game camera owns the view
            }
        }
        prevGameCamEnabled = gameCameraEnabled_;

        // Stream cells around the focus (camera at runtime; a sweep in the
        // smoke test). Async loads land on the job system; this only finalizes
        // finished loads and unloads departed cells.
        if (streamingWorld.Active()) {
            glm::vec3 focus = renderer.GetCamera().Position();
            if (config.worldTest) {
                worldTestT += dt;
                const f32 span = 220.0f; // ping-pong the focus over 0..span
                const f32 p = std::fmod(worldTestT * 24.0f, 2.0f * span);
                focus = glm::vec3(p > span ? (2.0f * span - p) : p, 0.0f, 0.0f);
            }
            streamFocus = focus;
            streamingWorld.Update(scene, renderer, focus);
        }
        // Particles: simulate (spawn + integrate) and build this frame's billboards
        // against the camera basis. Emit even in the editor so emitters preview live.
        particle::Update(scene, dt, true);
        {
            const glm::vec3 fwd = renderer.GetCamera().Forward();
            glm::vec3 pRight = glm::cross(fwd, glm::vec3(0.0f, 1.0f, 0.0f));
            pRight = glm::dot(pRight, pRight) > 1e-6f ? glm::normalize(pRight)
                                                      : glm::vec3(1.0f, 0.0f, 0.0f);
            const glm::vec3 pUp = glm::normalize(glm::cross(pRight, fwd));
            particle::BuildVertices(scene, renderer, assetsDir, pRight, pUp, particleAlpha,
                                    particleAdd);
            // 3D text objects (WorldText) ride the same depth-tested quad batch.
            ui::AppendWorldText(scene, renderer, assetsDir, pRight, pUp, particleAlpha);
            renderer.SetParticles(particleAlpha, particleAdd);
        }

        // Volumetric VFX: feed the density-splat compute. Blobs come from every
        // `volumetric`-flagged emitter (one per live particle); the raymarch pass
        // then lights + composites them. HBE_VOLTEST injects a static test plume as
        // a fallback so the splat path can be exercised with no authored emitter.
        {
            static std::vector<rhi::VolumeBlob> volumeBlobs; // persists across the frame
            rhi::VolumeParams vp{};
            const bool haveEmitters = particle::BuildVolumetricBlobs(scene, volumeBlobs, vp);
            if (!haveEmitters) {
                static const bool kVolTest = [] {
                    const char* e = std::getenv("HBE_VOLTEST");
                    return e && e[0] && e[0] != '0';
                }();
                volumeBlobs.clear();
                vp = rhi::VolumeParams{};
                if (kVolTest) {
                    for (int i = 0; i < 10; ++i) {
                        rhi::VolumeBlob b;
                        b.pos = {0.0f, 0.3f + 0.35f * static_cast<f32>(i), 0.0f};
                        b.radius = 0.6f + 0.05f * static_cast<f32>(i);
                        b.density = 0.35f; // blobs overlap heavily; keep the sum reasonable
                        b.temperature = glm::clamp(1.0f - 0.12f * static_cast<f32>(i), 0.0f, 1.0f);
                        volumeBlobs.push_back(b);
                    }
                    glm::vec3 lo(1e9f), hi(-1e9f);
                    for (const rhi::VolumeBlob& b : volumeBlobs) {
                        lo = glm::min(lo, b.pos - glm::vec3(b.radius));
                        hi = glm::max(hi, b.pos + glm::vec3(b.radius));
                    }
                    vp.boundsMin = lo - glm::vec3(0.25f); // small padding
                    vp.boundsMax = hi + glm::vec3(0.25f);
                    vp.blobCount = static_cast<u32>(volumeBlobs.size());
                    vp.densityScale = 1.0f;
                }
            }
            renderer.SetVolumeParticles(volumeBlobs, vp); // before RenderScene (Vulkan splats in BeginFrame)
        }

        // Advance UI animation clips (writes UIElement transform/color/frame fields)
        // BEFORE building the UI geometry so this frame reflects the animated pose.
        ui::UpdateAnimations(scene, dt, assetsDir);

        // World-space ("physical") UI upkeep: per-canvas render targets + the lit
        // page quads that display them. Runs in the editor too (live preview).
        ui::UpdateWorldSurfaces(scene, renderer);

        // In-game UI built AFTER scripts so text/visibility edits show this
        // frame; drawn over the scene inside RenderScene. World canvases route
        // into per-canvas texture batches instead of the screen overlay.
        ui::BuildVertices(scene, renderer,
                          Project::HasActive() ? Project::Active().AssetsDir()
                                               : std::filesystem::path(),
                          uiTarget, uiConfig, uiVertices, &worldUIBatches, uiCtx_);
        BuildFadeCurtain(uiVertices); // loading fade-to/from-black over the world + UI
        BuildDevOverlay(uiVertices);  // dev stats panel on top (when toggled on)
        renderer.SetUIOverlay(uiVertices);
        worldUIDraws.clear();
        for (const ui::WorldUIBatch& b : worldUIBatches) {
            // Empty batches still submit: DrawUIToTexture clears the target, so a
            // fresh (or just-hidden) canvas page shows transparent, never garbage.
            if (b.target.IsValid())
                worldUIDraws.push_back(
                    {b.target, b.verts.data(), static_cast<u32>(b.verts.size())});
        }
        renderer.SetWorldUI(worldUIDraws); // rendered to texture before the scene pass
        renderer.BeginUI();          // no-op in a runtime build
        if (onFrame_) onFrame_(*this); // editor builds its UI

        // Project-global rendering quality (AA + GTAO) is authoritative: stamp it
        // onto the live scene's post each frame so neither a per-scene post nor a
        // Post Volume can change anti-aliasing / ambient occlusion. The "look"
        // (bloom/fog/DoF/grade/exposure) stays scene- and volume-driven.
        if (Project::HasActive()) {
            const rhi::PostSettings& q = Project::Active().Settings().environment.post;
            rhi::PostSettings& p = scene.Environment().post;
            p.taaEnabled = q.taaEnabled;
            p.fxaaEnabled = q.fxaaEnabled;
            p.ssaoEnabled = q.ssaoEnabled;
            p.ssaoRadius = q.ssaoRadius;
            p.ssaoIntensity = q.ssaoIntensity;
        }
        // Player quality/brightness (the shipped game's Settings menu) - RUNTIME
        // ONLY: the editor always shows the authored look. The preset only
        // DEGRADES the authored post (High = untouched); brightness is a +/-1
        // photographic-stop multiplier applied at the VIEW level, never written
        // into the scene/volume exposure (which stays author-driven).
        if (!onInit_) {
            ApplyGraphicsPreset(scene.Environment().post, userSettings_.graphicsPreset);
            if (config.forceShadowCascades > 0) // TEMP perf A/B: override the preset
                scene.Environment().post.shadowCascades =
                    static_cast<u32>(config.forceShadowCascades);
            renderer.SetUserExposureScale(
                std::exp2(userSettings_.brightness * 2.0f - 1.0f)); // 0.5x..2x, 0.5=neutral
        }
        // --time / --daynight overrides survive level reloads (re-applied here,
        // after any SetupEnvironment reset).
        if (forceTimeOfDay_ >= 0.0f) {
            scene.Environment().dynamicSky = 1;
            scene.Environment().timeOfDay = forceTimeOfDay_; // held
        }
        if (forceDayLength_ >= 0.0f) {
            scene.Environment().dynamicSky = 1;
            scene.Environment().dayLengthSeconds = forceDayLength_;
        }
        if (forceClouds_ >= 0.0f) scene.Environment().cloudCoverage = forceClouds_;
        // Day/night: advance the time of day and drive the sun + ambient so the
        // analytic sky and scene lighting move together (no-op unless dynamicSky).
        UpdateDayNight(scene, dt);
        {
            const auto _pt = clock::now();
            renderer.RenderScene(scene, dt);
            accRenderMs += std::chrono::duration<f64, std::milli>(clock::now() - _pt).count();
        }
    }

    if (settingsDirty_) { userSettings_.Save(userSettingsDir_); settingsDirty_ = false; }
    streamingWorld.UnloadAll(scene); // drain in-flight loads, destroy entities
    streamingWorld_ = nullptr;
    gridNav_ = nullptr;
    currentLevel_ = nullptr;
    renderer.Shutdown();
    jobs::Shutdown();
    window_ = nullptr;
    renderer_ = nullptr;
    scene_ = nullptr;
    input_ = nullptr;
    physics_ = nullptr;
    audio_ = nullptr;
    HBE_INFO("Heartbreak Engine shut down cleanly.");
    return 0;
}

bool Engine::HasLevel() const { return currentLevel_ && currentLevel_->Loaded(); }

void Engine::LoadLevel(const std::filesystem::path& base) {
    if (!currentLevel_ || !scene_ || !renderer_) return;
    const std::filesystem::path assets = Project::HasActive()
                                             ? Project::Active().AssetsDir()
                                             : base.parent_path();
    // Switching mid-play (e.g. the dev-menu "skip to zone") must not strand a running
    // conversation or a cutscene owning the camera: Unload destroys their entities, so
    // tear down the narrative runtime first (mirrors LoadGame/FlowMainMenu).
    ResetDialogueRuntime();
    ClearCutscene();
    game::ClearTransientQueues(); // don't let a queued death/noise/spot fire into the new level
    // Switching: unload the current level (UI / other scenes stay resident).
    if (currentLevel_->Loaded()) currentLevel_->Unload(*scene_);
    currentLevel_->SetBase(base);
    currentLevel_->Load(*scene_, *renderer_, assets);

    // No nav step: GridNav auto-rebuilds from the new static geometry each frame.
}

void Engine::UnloadLevel() {
    if (currentLevel_ && scene_ && currentLevel_->Loaded()) currentLevel_->Unload(*scene_);
}

// --- Checkpoint save/load ----------------------------------------------------
namespace {
std::filesystem::path SaveFilePath(const std::string& slot) {
    if (!Project::HasActive()) return {};
    return Project::Active().Root() / "Saves" / (slot + ".hbsave");
}
} // namespace

bool Engine::SaveGame(const std::string& slot) {
    if (!scene_ || !Project::HasActive()) return false;
    nlohmann::json j;
    j["version"] = 1;
    j["scene"] = scene::SaveSceneToString(*scene_); // full registry snapshot
    j["game"] = game::SerializeState();             // objectives + checkpoints
    j["level"] = currentLevel_ ? currentLevel_->Base().string() : std::string();
    const std::filesystem::path path = SaveFilePath(slot);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        HBE_ERROR("SaveGame: cannot write '{}'.", path.string());
        return false;
    }
    const std::string text = j.dump();
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    HBE_INFO("SaveGame: saved '{}'.", path.string());
    return true;
}

bool Engine::HasSave(const std::string& slot) const {
    std::error_code ec;
    const std::filesystem::path p = SaveFilePath(slot);
    return !p.empty() && std::filesystem::exists(p, ec);
}

bool Engine::LoadGame(const std::string& slot) {
    if (!scene_ || !renderer_ || !Project::HasActive()) return false;
    const std::filesystem::path path = SaveFilePath(slot);
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        HBE_WARN("LoadGame: no save at '{}'.", path.string());
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text);
    } catch (const std::exception& e) {
        HBE_ERROR("LoadGame: parse '{}': {}", path.string(), e.what());
        return false;
    }
    // Restore the full scene snapshot (Replace), then the gameplay state.
    scene::SceneData data;
    if (!scene::ParseSceneString(j.value("scene", std::string()), data)) {
        HBE_ERROR("LoadGame: snapshot in '{}' is invalid.", path.string());
        return false;
    }
    scene::StagedAssets staged;
    scene::StageAssets(data, Project::Active().AssetsDir(), staged);
    scene::Instantiate(*scene_, *renderer_, data, staged, scene::LoadMode::Replace);
    game::DeserializeState(j.value("game", std::string()));
    // Reset the dialogue runner (Engine members survive the scene Replace): loading
    // a save mid-conversation must not resume a stale graph over the restored world
    // or hang on a Choice whose buttons the Replace destroyed.
    ResetDialogueRuntime();
    // Resume gameplay through the normal reveal so physics is un-paused, the cursor
    // locks, and any loading overlay is dropped - loading a save (e.g. dev F9) DURING
    // the loading screen would otherwise land in Playing with the sim still frozen.
    // (The Replace above already cleared the registry; EnterPlaying's overlay-destroy
    // is a valid()-guarded no-op here.) Clear the fade curtain first so a save loaded
    // mid-transition reveals instantly instead of easing in from a leftover fade.
    if (flowActive_) {
        fadeAlpha_ = 0.0f;
        loadPhase_ = LoadPhase::Begin;
        EnterPlaying();
    }
    HBE_INFO("LoadGame: restored '{}'.", path.string());
    return true;
}

void Engine::SetCursorLocked(bool locked) {
    if (window_) window_->SetCursorLocked(locked);
}
bool Engine::IsCursorLocked() const {
    return window_ && window_->IsCursorLocked();
}

namespace {
// First UIElement clicked this frame that carries a built-in game-flow action.
std::string PollClickedAction(Scene& scene) {
    std::string action;
    scene.Registry().view<UIElement>().each([&](const UIElement& el) {
        if (action.empty() && el.clicked && !el.action.empty()) action = el.action;
    });
    return action;
}
// True when `only` is null (apply to all) or `e` is one of the listed entities. Used
// to confine the loading-bar drivers to the loading overlay so they don't touch the
// gameplay HUD's own ProgressBars (health/ammo bars) that share the registry.
bool InSet(entt::entity e, const std::vector<entt::entity>* only) {
    return !only || std::find(only->begin(), only->end(), e) != only->end();
}

// Fade the loading bar/wheel by setting its alpha to `a` in [0,1] so the "wheel" can
// ease in after the loading screen itself has appeared. Scoped to `only` when given.
void SetWheelAlpha(Scene& scene, f32 a, const std::vector<entt::entity>* only = nullptr) {
    scene.Registry().view<UIElement>().each([&](entt::entity e, UIElement& el) {
        if (el.type == UIElement::Type::ProgressBar && InSet(e, only)) {
            el.color.a = a;
            el.fillColor.a = a;
        }
    });
}

// Drive the loading bar's fill to `f` in [0,1]. Scoped to `only` when given (else all
// ProgressBars - used by the boot splash, which has no gameplay HUD in the scene).
void SetProgressFill(Scene& scene, f32 f, const std::vector<entt::entity>* only = nullptr) {
    scene.Registry().view<UIElement>().each([&](entt::entity e, UIElement& el) {
        if (el.type == UIElement::Type::ProgressBar && InSet(e, only)) el.fill = f;
    });
}

// All entities inside the named UIPanel subtree (root included). Scopes the loading
// bar/wheel drivers to the "Loading" panel so gameplay HUD ProgressBars are untouched.
void CollectPanelSubtree(Scene& scene, const std::string& panel,
                         std::vector<entt::entity>& out) {
    auto& reg = scene.Registry();
    entt::entity root = entt::null;
    for (const entt::entity e : reg.view<UIPanel>()) {
        if (reg.get<UIPanel>(e).name == panel) { root = e; break; }
    }
    if (root == entt::null) return;
    for (const entt::entity e : reg.storage<entt::entity>()) {
        entt::entity cur = e;
        for (int depth = 0; cur != entt::null && depth < 64; ++depth) {
            if (cur == root) { out.push_back(e); break; }
            const Parent* p = reg.try_get<Parent>(cur);
            cur = (p && reg.valid(p->entity)) ? p->entity : entt::null;
        }
    }
}
} // namespace

// MINIMUM loading-screen dwell in seconds: the bar sweeps over this, but the screen
// also stays up until the world has actually materialized (terrain built + streamed
// cells resident) so nothing pops in after it clears.
static constexpr f32 kLoadDuration = 1.25f;
// Safety cap: reveal even if the world never reports "settled" (a broken cell, a
// stuck terrain build), so a content bug can't hang on the loading screen forever.
static constexpr f32 kMaxLoadDuration = 30.0f;
// Loading-transition fade durations (seconds).
static constexpr f32 kFadeInDur = 0.4f;      // black curtain lifts: loading screen appears
static constexpr f32 kWheelFadeDur = 0.35f;  // loading wheel eases in
static constexpr f32 kFadeOutDur = 0.4f;     // curtain drops to black before gameplay
static constexpr f32 kGameFadeInDur = 0.5f;  // gameplay eases in from black once revealed
// Studio/boot splash dwell in the loop AFTER the pre-loop warmup (IBL + loads
// already ran behind the splash); just long enough to read the final status.
static constexpr f32 kBootDuration = 1.0f;

void Engine::FlowMainMenu() {
    if (!flowActive_ || !scene_ || !renderer_ || !Project::HasActive()) return;
    if (!uiManagerMode_) return; // no menu concept without a UI scene
    // Unload the gameplay world (destroy non-Persistent entities) but keep the
    // resident UI, then show the initial (menu) panel. No scene swap.
    auto& reg = scene_->Registry();
    std::vector<entt::entity> kill;
    for (const entt::entity e : reg.storage<entt::entity>())
        if (!reg.all_of<Persistent>(e)) kill.push_back(e);
    for (const entt::entity e : kill)
        if (reg.valid(e)) reg.destroy(e);
    if (currentLevel_ && currentLevel_->Loaded()) currentLevel_->Unload(*scene_);
    loadingPanelEntities_.clear();
    // Leaving gameplay: stop any in-progress dialogue and clear its captions so
    // they don't linger/resume over the menu.
    ResetDialogueRuntime();
    // Stop any in-progress cutscene and restore the camera before the menu.
    if (cutsceneCamOwned_) SetGameCameraEnabled(cutsceneRestoreCam_);
    cutsceneCamOwned_ = false;
    cutsceneTime_ = -1.0f;
    if (physics_) physics_->SetRunning(true);
    fadeAlpha_ = 0.0f; // clear any leftover fade curtain from an interrupted load
    uiManager_.ShowInitial(*scene_);
    SetCursorLocked(false);
    gameState_ = GameState::MainMenu;
    loadTimer_ = 0.0f;
}

void Engine::FlowPlay() {
    if (!flowActive_ || !scene_ || !renderer_ || !Project::HasActive()) return;

    // The "Loading" UIPanel (in the persistent UI scene) is the loading screen.
    const bool hasLoading = uiManagerMode_ && uiManager_.Has(*scene_, "Loading");
    if (hasLoading) {
        // Snap to black THIS frame; the heavy world load + loading-overlay stand-up
        // happen NEXT frame in the Begin phase, hidden behind the curtain, which then
        // lifts to fade the loading screen in. Gameplay sim stays frozen until reveal.
        fadeAlpha_ = 1.0f;
        wheelAlpha_ = 0.0f;
        loadPhase_ = LoadPhase::Begin;
        if (physics_) physics_->SetRunning(false);
        SetCursorLocked(false);
        loadTimer_ = 0.0f;
        gameState_ = GameState::Loading;
        return;
    }
    // No loading screen configured: load + play immediately (no fade).
    LoadGameplayWorld();
    EnterPlaying();
}

void Engine::LoadGameplayWorld() {
    if (!scene_ || !renderer_ || !Project::HasActive()) return;
    game::Reset(); // a fresh run: clear objectives + reached checkpoints
    // Stop any dialogue/captions left over from a prior run (the runner state
    // is an Engine member, so it survives the scene Replace below).
    ResetDialogueRuntime();
    // Stop any in-progress cutscene and hand the camera back (else it stays
    // disabled into the new run).
    if (cutsceneCamOwned_) SetGameCameraEnabled(cutsceneRestoreCam_);
    cutsceneCamOwned_ = false;
    cutsceneTime_ = -1.0f;
    const std::filesystem::path assets = Project::Active().AssetsDir();
    const ProjectSettings& s = Project::Active().Settings();

    // Gameplay scene/level REPLACES the current world (clears the registry). This
    // runs BEFORE the loading screen goes down so terrain chunks and streamed cells
    // build/settle behind the loading overlay instead of popping in afterwards.
    bool loaded = false;
    if (!s.startupScene.empty()) {
        const std::filesystem::path gp = assets / s.startupScene;
        if (vfs::Exists(gp)) {
            if (scene::IsLevelMember(gp)) {
                // First layer replaces, others stack; GridNav picks it up next frame.
                loaded = scene::LoadLevel(*scene_, *renderer_, scene::ResolveLevel(gp));
            } else {
                loaded = scene::LoadScene(*scene_, *renderer_, gp); // Replace
            }
        }
    }
    if (!loaded) HBE_WARN("Game flow: startup scene '{}' failed to load.", s.startupScene);

    // The HUD is a resident UIPanel in the persistent UI scene (shown on reveal in
    // EnterPlaying) - no per-scene HUD load.
    // No nav step: GridNav rebuilds from the loaded gameplay geometry each frame.
}

void Engine::EnterPlaying() {
    if (!scene_ || !renderer_) return;
    // Reveal the fully-streamed world and hand control to the player. The world was
    // already instantiated by LoadGameplayWorld(); Show("HUD") below hides the Loading
    // panel (its entities are persistent - never destroy them, just deactivate).
    loadingPanelEntities_.clear();

    SetCursorLocked(true);                    // mouse-look while playing
    if (physics_) physics_->SetRunning(true); // gameplay simulation begins now
    if (uiManagerMode_) uiManager_.Show(*scene_, "HUD"); // reveal HUD panel, hide menus
    loadTimer_ = 0.0f;
    gameState_ = GameState::Playing;
}

void Engine::FlowReload() {
    // Respawn / restart: same path as Play (game loading screen -> reload gameplay
    // scene + HUD). Works from Playing too.
    FlowPlay();
}

void Engine::FlowAfterBoot() {
    // Studio splash finished: show the initial (menu) panel when a UI scene is
    // loaded, else boot straight into gameplay (the empty-uiScene fallback).
    // --play (playOnBoot_) forces gameplay directly, skipping the menu.
    if (playOnBoot_) {
        FlowPlay();
    } else if (uiManagerMode_) {
        FlowMainMenu();
    } else {
        FlowPlay();
    }
}

void Engine::SubstituteUITokens(f32 progress) {
    if (!scene_) return;
    // Resolve the capability strings once; only boot/loading screens use tokens.
    const std::string backend = renderer_ ? rhi::ToString(renderer_->API()) : std::string();
    const std::string gpu = renderer_ ? renderer_->AdapterName() : std::string();
    const std::string audioDev = audio_ ? audio_->DeviceName() : std::string();
    const std::string version =
        Project::HasActive() ? Project::Active().Settings().build.version : std::string();
    const int pctI = static_cast<int>(glm::clamp(progress, 0.0f, 1.0f) * 100.0f + 0.5f);
    const std::string pct = std::to_string(pctI) + "%";

    const auto replaceAll = [](std::string& s, const char* from, const std::string& to) {
        const usize n = std::strlen(from);
        for (usize p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size()))
            s.replace(p, n, to);
    };
    // {log} = the live boot console (last lines). Resolve lazily: only fetch the
    // (locked) log tail if some element actually uses it.
    std::string logTail;
    bool logFetched = false;
    scene_->Registry().view<UIElement>().each([&](UIElement& el) {
        if (el.text.find('{') == std::string::npos) {
            if (!el.runtimeText.empty()) el.runtimeText.clear();
            return;
        }
        std::string t = el.text;
        replaceAll(t, "{backend}", backend);
        replaceAll(t, "{gpu}", gpu);
        replaceAll(t, "{audio}", audioDev);
        replaceAll(t, "{version}", version);
        replaceAll(t, "{progress}", pct);
        replaceAll(t, "{objective}", game::CurrentObjectiveText()); // HUD task goal
        replaceAll(t, "{equipped}", game::EquippedWeapon());        // selected weapon id
        // {item:<id>} -> current inventory count of that item (scavenge HUD).
        for (usize p = t.find("{item:"); p != std::string::npos; p = t.find("{item:", p)) {
            const usize end = t.find('}', p);
            if (end == std::string::npos) break;
            const std::string id = t.substr(p + 6, end - (p + 6));
            const std::string val = std::to_string(game::ItemCount(id));
            t.replace(p, end - p + 1, val);
            p += val.size();
        }
        if (t.find("{log}") != std::string::npos) {
            // Only the single latest line (real-time status), per design.
            if (!logFetched) { logTail = RecentLog(1); logFetched = true; }
            replaceAll(t, "{log}", logTail);
        }
        if (el.runtimeText != t) el.runtimeText = std::move(t); // only on change (skip re-layout)
    });
}

void Engine::PresentBootSplash(f32 progress) {
    if (!window_ || !renderer_ || !scene_) return;
    if (!window_->PumpMessages()) return; // window closing -> the loop will exit
    if (window_->IsMinimized()) return;

    ui::CanvasConfig uiConfig;
    if (Project::HasActive()) {
        const BuildSettings& b = Project::Active().Settings().build;
        uiConfig.mode = static_cast<ui::ScaleMode>(glm::clamp(b.uiScaleMode, 0u, 2u));
        uiConfig.refWidth = static_cast<f32>(glm::max(b.uiRefWidth, 64u));
        uiConfig.refHeight = static_cast<f32>(glm::max(b.uiRefHeight, 64u));
    }
    SetProgressFill(*scene_, progress);
    SubstituteUITokens(progress);

    static std::vector<rhi::UIVertex> verts; // reused across the few boot frames
    const std::filesystem::path assets =
        Project::HasActive() ? Project::Active().AssetsDir() : std::filesystem::path();
    // Prime animators at t=0 so splash elements with OnShow clips render the
    // authored first keyframe (not their un-animated base pose).
    ui::UpdateAnimations(*scene_, 0.0f, assets);
    ui::BuildVertices(*scene_, *renderer_, assets, renderer_->RenderTargetSize(),
                      uiConfig, verts);
    renderer_->SetUIOverlay(verts);
    renderer_->RenderScene(*scene_, 0.0f); // draws the splash UI + presents
}

void Engine::DevMenuScanLevels() {
    devLevels_.clear();
    if (!Project::HasActive()) return;
    const std::filesystem::path root = Project::Active().AssetsDir();
    std::error_code ec;
    std::vector<std::filesystem::path> bases;
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file() || it->path().extension() != ".hbscene") continue;
        bases.push_back(scene::ResolveLevel(it->path()).base); // dedup members -> one base
    }
    std::sort(bases.begin(), bases.end());
    bases.erase(std::unique(bases.begin(), bases.end()), bases.end());
    devLevels_ = std::move(bases);
}

void Engine::RebuildDevMenu() {
    devItems_.clear();
    const auto header = [&](std::string s) {
        DevMenuItem it;
        it.label = std::move(s);
        it.header = true;
        devItems_.push_back(std::move(it));
    };
    const auto action = [&](std::string s, std::function<void()> fn) {
        DevMenuItem it;
        it.label = std::move(s);
        it.activate = std::move(fn);
        devItems_.push_back(std::move(it));
    };
    const auto value = [&](std::string s, std::string v, std::function<void(int)> fn) {
        DevMenuItem it;
        it.label = std::move(s);
        it.value = std::move(v);
        it.adjust = std::move(fn);
        devItems_.push_back(std::move(it));
    };

    header("FLOW");
    action("Restart level", [this] { devMenuOpen_ = false; FlowReload(); });
    action("Main menu", [this] { devMenuOpen_ = false; FlowMainMenu(); });
    action("Save checkpoint", [this] { SaveGame("checkpoint"); });
    action("Load checkpoint", [this] { LoadGame("checkpoint"); });

    if (!devLevels_.empty()) {
        header("LEVELS (skip to zone)");
        for (const std::filesystem::path& base : devLevels_) {
            const std::filesystem::path b = base;
            action(base.filename().string(), [this, b] { devMenuOpen_ = false; LoadLevel(b); });
        }
    }

    header("TIME");
    {
        const f32 tod = scene_ ? scene_->Environment().timeOfDay : 12.0f;
        char v[32];
        std::snprintf(v, sizeof(v), "%.1f h", tod);
        value("Time of day", v, [this](int d) {
            // Remember the authored sky mode the first time we force, so "release"
            // can restore it (forcing sets dynamicSky=1 each frame in the render loop).
            if (scene_ && devSkyRestore_ < 0) devSkyRestore_ = scene_->Environment().dynamicSky;
            f32 t = (forceTimeOfDay_ >= 0.0f) ? forceTimeOfDay_
                    : (scene_ ? scene_->Environment().timeOfDay : 12.0f);
            t = std::fmod(t + static_cast<f32>(d) + 24.0f, 24.0f);
            forceTimeOfDay_ = t; // held each frame by the environment re-apply
            if (scene_) scene_->Environment().timeOfDay = t;
        });
        // Release a dev-menu time override (devSkyRestore_ >= 0 means WE forced it, not
        // the --time CLI flag): stop holding the hour + restore the authored sky mode.
        if (forceTimeOfDay_ >= 0.0f && devSkyRestore_ >= 0) {
            action("Time of day: release (auto)", [this] {
                forceTimeOfDay_ = -1.0f;
                if (scene_) scene_->Environment().dynamicSky = devSkyRestore_;
                devSkyRestore_ = -1;
            });
        }
        char sv[32];
        std::snprintf(sv, sizeof(sv), "%.2fx", devTimeScale_);
        value("Game speed", sv,
              [this](int d) { devTimeScale_ = glm::clamp(devTimeScale_ + static_cast<f32>(d) * 0.25f,
                                                         0.0f, 4.0f); });
        action(devTimeScale_ <= 0.001f ? "Resume (unpause)" : "Pause",
               [this] { devTimeScale_ = devTimeScale_ <= 0.001f ? 1.0f : 0.0f; });
    }

    if (audio_ && audio_->HasMusicGraph()) {
        const std::vector<std::string> states = audio_->MusicStateNames();
        if (!states.empty()) {
            header("MUSIC");
            const std::string cur = audio_->CurrentMusicState();
            value("State", cur.empty() ? "(none)" : cur, [this, states](int d) {
                const std::string c = audio_->CurrentMusicState();
                int idx = 0;
                for (int i = 0; i < static_cast<int>(states.size()); ++i)
                    if (states[static_cast<usize>(i)] == c) { idx = i; break; }
                idx = (idx + d + static_cast<int>(states.size())) % static_cast<int>(states.size());
                audio_->PlayMusicState(states[static_cast<usize>(idx)]);
            });
        }
    }
}

void Engine::BuildDevOverlay(std::vector<rhi::UIVertex>& out) {
    if (!devMenuOpen_ || !renderer_ || !scene_) return;
    if (!(Project::HasActive() && Project::Active().Settings().build.devMenu)) return;
    const glm::vec2 target = renderer_->RenderTargetSize();
    if (target.x < 1.0f || target.y < 1.0f) return;
    ui::FontAtlas& font = ui::SharedFont();
    font.Initialize(*renderer_);
    if (!font.Ready()) return;

    // --- Stats header string ---
    const f32 fps = dt_ > 1e-5f ? 1.0f / dt_ : 0.0f;
    const char* st = "None";
    switch (gameState_) {
        case GameState::Booting:  st = "Booting"; break;
        case GameState::MainMenu: st = "MainMenu"; break;
        case GameState::Loading:  st = "Loading"; break;
        case GameState::Playing:  st = "Playing"; break;
        default: break;
    }
    const std::string level =
        (currentLevel_ && currentLevel_->Loaded()) ? currentLevel_->Name() : std::string("(none)");
    glm::vec3 ppos(0.0f);
    auto pv = scene_->Registry().view<Transform, CharacterController>();
    if (pv.begin() != pv.end()) ppos = glm::vec3(scene_->WorldMatrix(*pv.begin())[3]);
    std::string obj = game::CurrentObjectiveText();
    if (obj.empty()) obj = "(none)";
    const Renderer::FrameStats& rs = renderer_->Stats();
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "== DEV MENU ==  (Ctrl+` close  -  arrows move  -  Enter/<> use)\n"
                  "FPS %.0f  |  %.2f ms  |  Draws %u/%u (%u culled)\n"
                  "State: %s   Level: %s   Player %.1f,%.1f,%.1f\n"
                  "Objective: %s",
                  fps, dt_ * 1000.0f, rs.drawn, rs.total, rs.culled, st, level.c_str(), ppos.x,
                  ppos.y, ppos.z, obj.c_str());
    const std::string stats = buf;

    // --- Emit helpers (NDC) ---
    const auto ndc = [&](f32 x, f32 y) {
        return glm::vec2(x / target.x * 2.0f - 1.0f, 1.0f - y / target.y * 2.0f);
    };
    const auto addRect = [&](f32 x0, f32 y0, f32 x1, f32 y1, f32 r, f32 g, f32 b, f32 a, u32 tex,
                             f32 u0, f32 v0, f32 u1, f32 v1) {
        const glm::vec2 p0 = ndc(x0, y0), p1 = ndc(x1, y1);
        const rhi::UIVertex a00{p0.x, p0.y, u0, v0, r, g, b, a, tex};
        const rhi::UIVertex a10{p1.x, p0.y, u1, v0, r, g, b, a, tex};
        const rhi::UIVertex a11{p1.x, p1.y, u1, v1, r, g, b, a, tex};
        const rhi::UIVertex a01{p0.x, p1.y, u0, v1, r, g, b, a, tex};
        out.push_back(a00); out.push_back(a10); out.push_back(a11);
        out.push_back(a00); out.push_back(a11); out.push_back(a01);
    };
    const u32 ft = font.TextureIndex();
    const f32 textPx = 17.0f, padX = 12.0f, padY = 10.0f, ox = 16.0f, oy = 16.0f;
    const auto measure = [&](const std::string& s) {
        std::vector<ui::GlyphQuad> q;
        f32 w = 0.0f, h = 0.0f;
        font.Layout(s, textPx, q, w, h);
        return glm::vec2(w, h);
    };
    const auto drawText = [&](f32 x, f32 y, const std::string& s, f32 r, f32 g, f32 b) {
        std::vector<ui::GlyphQuad> q;
        f32 w = 0.0f, h = 0.0f;
        font.Layout(s, textPx, q, w, h);
        for (const ui::GlyphQuad& gq : q)
            addRect(x + gq.x0, y + gq.y0, x + gq.x1, y + gq.y1, r, g, b, 1.0f, ft, gq.u0, gq.v0,
                    gq.u1, gq.v1);
    };

    // One display string per menu row.
    const auto rowText = [&](const DevMenuItem& it, bool sel) {
        if (it.header) return it.label;
        std::string s = (sel ? "> " : "  ") + it.label;
        if (!it.value.empty()) s += "   [" + it.value + "]";
        return s;
    };

    // --- Measure for the panel size ---
    const glm::vec2 statsSz = measure(stats);
    const f32 lineH = textPx + 7.0f;
    f32 maxW = statsSz.x;
    for (const DevMenuItem& it : devItems_) maxW = glm::max(maxW, measure(rowText(it, false)).x + 14.0f);
    const f32 gap = 8.0f;
    const f32 contentH = statsSz.y + gap + static_cast<f32>(devItems_.size()) * lineH;

    // Panel.
    addRect(ox, oy, ox + maxW + padX * 2.0f, oy + contentH + padY * 2.0f, 0.04f, 0.05f, 0.08f, 0.9f,
            0, 0, 0, 1, 1);

    // Stats (dim), then the menu rows.
    drawText(ox + padX, oy + padY, stats, 0.85f, 0.88f, 0.95f);
    f32 y = oy + padY + statsSz.y + gap;
    for (int i = 0; i < static_cast<int>(devItems_.size()); ++i) {
        const DevMenuItem& it = devItems_[static_cast<usize>(i)];
        const bool sel = (i == devMenuSel_) && !it.header;
        if (sel) // highlight bar behind the selected row
            addRect(ox + padX - 4.0f, y - 1.0f, ox + maxW + padX + 4.0f, y + lineH - 2.0f, 0.20f,
                    0.35f, 0.55f, 0.85f, 0, 0, 0, 1, 1);
        f32 r = 0.92f, g = 0.94f, b = 0.98f;
        if (it.header) { r = 0.55f; g = 0.75f; b = 1.0f; }
        else if (sel)  { r = 1.0f; g = 1.0f; b = 0.7f; }
        drawText(ox + padX, y, rowText(it, sel), r, g, b);
        y += lineH;
    }
}

void Engine::BuildFadeCurtain(std::vector<rhi::UIVertex>& out) {
    if (fadeAlpha_ <= 0.001f) return;
    const f32 a = glm::clamp(fadeAlpha_, 0.0f, 1.0f);
    // Full-screen black quad in NDC (tex 0 = the 1x1 white texture), drawn over the
    // whole frame. Same winding/UV convention as BuildDevOverlay's addRect.
    const rhi::UIVertex v00{-1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, a, 0};
    const rhi::UIVertex v10{1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, a, 0};
    const rhi::UIVertex v11{1.0f, -1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, a, 0};
    const rhi::UIVertex v01{-1.0f, -1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, a, 0};
    out.push_back(v00); out.push_back(v10); out.push_back(v11);
    out.push_back(v00); out.push_back(v11); out.push_back(v01);
}

void Engine::SeedSettingsWidgets() {
    if (!scene_) return; // fill the Settings widgets from the persisted user options
    scene_->Registry().view<UIElement>().each([&](UIElement& el) {
        if (el.action == "setting:volume") el.value = userSettings_.masterVolume;
        else if (el.action == "setting:brightness") el.value = userSettings_.brightness;
        else if (el.action == "setting:graphics") el.selected = userSettings_.graphicsPreset;
        else if (el.action == "setting:captions") el.toggled = userSettings_.captionsEnabled;
    });
}

void Engine::ApplyChangedSettings() {
    if (!scene_) return; // apply any "setting:*" widget the user changed this frame
    scene_->Registry().view<UIElement>().each([&](UIElement& el) {
        if (!el.changed || el.action.rfind("setting:", 0) != 0) return;
        if (el.action == "setting:volume") {
            userSettings_.masterVolume = el.value;
            if (audio_) audio_->SetBusVolume("Master", el.value);
        } else if (el.action == "setting:brightness") {
            userSettings_.brightness = el.value; // applied to exposure each frame
        } else if (el.action == "setting:graphics") {
            userSettings_.graphicsPreset = el.selected; // applied to post each frame
        } else if (el.action == "setting:captions") {
            userSettings_.captionsEnabled = el.toggled;
            if (audio_) audio_->SetCaptionsEnabled(el.toggled);
        } else {
            return;
        }
        settingsDirty_ = true; // flushed to disk on Back / quit
    });
}

void Engine::PushCaption(const std::string& text, f32 seconds) {
    if (text.empty()) return;
    // Auto-derive a readable dwell from length when unspecified (~200 wpm).
    const f32 dwell = seconds > 0.0f
                          ? seconds
                          : glm::clamp(1.8f + 0.05f * static_cast<f32>(text.size()),
                                       2.5f, 9.0f);
    captions_.push_back({text, dwell});
    // Cap the visible stack so it never overflows the caption Label rect; the
    // oldest line drops early if too many pile up at once.
    constexpr usize kMaxCaptions = 4;
    if (captions_.size() > kMaxCaptions)
        captions_.erase(captions_.begin(), captions_.end() - kMaxCaptions);
}

void Engine::UpdateCaptions(f32 dt) {
    if (!scene_) return;
    // Drain the audio system's queue (AudioSource voicelines only enqueue when
    // captions are enabled, so that path already respects the accessibility
    // toggle; the dialogue runner pushes directly via PushCaption).
    if (audio_) {
        std::string cap;
        while (audio_->PopCaption(cap)) PushCaption(cap, 0.0f);
    }
    // Age out expired lines (each on its own timer).
    for (ActiveCaption& c : captions_) c.timer -= dt;
    captions_.erase(std::remove_if(captions_.begin(), captions_.end(),
                                   [](const ActiveCaption& c) { return c.timer <= 0.0f; }),
                    captions_.end());
    // Build the multi-line block (oldest at top, newest at bottom) and drive the
    // dedicated caption element (a Label with action "caption").
    std::string block;
    f32 longest = 0.0f;
    for (const ActiveCaption& c : captions_) {
        if (!block.empty()) block += '\n';
        block += c.text;
        longest = glm::max(longest, c.timer); // the last line to expire
    }
    const bool show = !captions_.empty();
    scene_->Registry().view<UIElement>().each([&](UIElement& el) {
        if (el.action != "caption") return;
        el.visible = show;
        if (show) {
            el.runtimeText = block;
            // Fade the whole block out over the final 0.5s of the LONGEST-lived
            // remaining line (keying on the newest line could fade the block
            // while an older line is still fully valid).
            el.color.a = longest > 0.5f ? 1.0f : glm::max(longest / 0.5f, 0.0f);
        }
    });
}

void Engine::PlayUISounds() {
    if (!scene_ || !audio_) return;
    // Runtime only: the editor drives the Game view as a pure preview, so
    // authoring (mousing over the preview) must not emit the game's UI SFX.
    // Mirrors the !onInit_ gate on ApplyChangedSettings/UpdateCaptions.
    if (onInit_) return;
    const std::filesystem::path assets =
        Project::HasActive() ? Project::Active().AssetsDir() : std::filesystem::path();
    if (assets.empty()) return;
    // Hover SFX only when the pointer actually moved this frame: `hovered` is a
    // level state re-asserted every frame, so a menu opening under a resting
    // cursor would otherwise blip. (Clicks are real events - always play.)
    const bool moved =
        input_ && (input_->MouseDeltaX() != 0.0f || input_->MouseDeltaY() != 0.0f);
    // One cheap per-frame scan; maintains the hover edge for every widget so a
    // hover-out resets it.
    scene_->Registry().view<UIElement>().each([&](UIElement& el) {
        if (el.clicked && !el.clickSound.empty()) audio_->PlayUAF(assets / el.clickSound);
        if (moved && el.hovered && !el.prevHovered && !el.hoverSound.empty())
            audio_->PlayUAF(assets / el.hoverSound);
        el.prevHovered = el.hovered;
    });
}

void Engine::UpdateDialogue(f32 dt) {
    if (dialogueNode_ == 0) return; // nothing running

    // Waiting on a Choice: poll the spawned buttons; the first clicked one wins.
    if (dialogueChoiceActive_) {
        if (!scene_) return;
        int pickedPin = -1;
        scene_->Registry().view<DialogueChoiceButton, UIElement>().each(
            [&](const DialogueChoiceButton& b, const UIElement& el) {
                if (pickedPin < 0 && el.clicked) pickedPin = static_cast<int>(b.outPin);
            });
        if (pickedPin >= 0) {
            const u32 next = dialogueGraph_.Follow(dialogueNode_, static_cast<u32>(pickedPin));
            ClearDialogueChoices(); // also clears dialogueChoiceActive_
            EnterDialogueNode(next);
        }
        return;
    }

    // A Line is holding on screen; advance via its single out pin when it lapses.
    dialogueTimer_ -= dt;
    if (dialogueTimer_ > 0.0f) return;
    EnterDialogueNode(dialogueGraph_.Follow(dialogueNode_, 0));
}

// Walk the graph from `nodeId`, chaining through instantaneous nodes (Start /
// Condition / SetFlag) until we hit a Line (waits on its hold) or a Choice (waits
// on the player), or run out of graph. A step guard stops an authored cycle from
// hanging the frame.
void Engine::EnterDialogueNode(u32 nodeId) {
    int guard = 0;
    while (nodeId != 0 && guard++ < 512) {
        const dlg::Node* n = dialogueGraph_.Find(nodeId);
        if (!n) { nodeId = 0; break; }
        dialogueNode_ = nodeId;
        if (n->type == dlg::NodeType::Start) {
            nodeId = dialogueGraph_.Follow(nodeId, 0);
        } else if (n->type == dlg::NodeType::SetFlag) {
            game::SetFlag(n->setFlag, n->setValue);
            nodeId = dialogueGraph_.Follow(nodeId, 0);
        } else if (n->type == dlg::NodeType::Condition) {
            const f32 v = game::GetFlag(n->flag);
            bool pass = false;
            switch (n->op) {
                case dlg::CmpOp::NotZero: pass = v != 0.0f; break;
                case dlg::CmpOp::Equal: pass = v == n->value; break;
                case dlg::CmpOp::NotEqual: pass = v != n->value; break;
                case dlg::CmpOp::Greater: pass = v > n->value; break;
                case dlg::CmpOp::Less: pass = v < n->value; break;
                case dlg::CmpOp::GreaterEqual: pass = v >= n->value; break;
                case dlg::CmpOp::LessEqual: pass = v <= n->value; break;
            }
            nodeId = dialogueGraph_.Follow(nodeId, pass ? 0u : 1u); // pin 0 True, 1 False
        } else if (n->type == dlg::NodeType::Line) {
            const std::filesystem::path assets =
                Project::HasActive() ? Project::Active().AssetsDir() : std::filesystem::path();
            const bool hasText = !n->text.empty();
            // Resolve the hold FIRST so the caption dwell and the node-advance timer
            // share one duration - otherwise an authored hold advances the line while
            // its caption lingers (stacking) or vanishes early (long holds).
            f32 hold = n->hold;
            if (hold <= 0.0f) {
                const usize len = hasText ? n->text.size() : n->speaker.size() + 8;
                hold = glm::clamp(1.8f + 0.05f * static_cast<f32>(len), 2.5f, 9.0f);
            }
            hold = glm::max(hold, 0.25f);
            if (hasText && userSettings_.captionsEnabled)
                PushCaption(n->speaker.empty() ? n->text : n->speaker + ": " + n->text, hold);
            if (!n->clip.empty() && audio_ && !assets.empty()) {
                // Dialogue actor: if an entity voices this line's speaker, play the
                // clip 3D from its world position (explicit DialogueActor first, then
                // any entity whose Name matches); otherwise fall back to flat 2D.
                bool spatial = false;
                if (scene_ && !n->speaker.empty()) {
                    entt::registry& reg = scene_->Registry();
                    entt::entity actor = entt::null;
                    std::string bus = "Dialogue";
                    f32 minD = 1.0f, maxD = 35.0f;
                    for (const entt::entity ae : reg.view<DialogueActor>()) {
                        const DialogueActor& da = reg.get<DialogueActor>(ae);
                        const std::string key =
                            da.speaker.empty()
                                ? (reg.all_of<Name>(ae) ? reg.get<Name>(ae).value : std::string())
                                : da.speaker;
                        if (key == n->speaker) {
                            actor = ae;
                            bus = da.bus;
                            minD = da.minDistance;
                            maxD = da.maxDistance;
                            break;
                        }
                    }
                    if (actor == entt::null)
                        for (const entt::entity ne : reg.view<Name>())
                            if (reg.get<Name>(ne).value == n->speaker) { actor = ne; break; }
                    // Drive the speaker's mouth from this line's audio amplitude.
                    if (actor != entt::null && !n->clip.empty())
                        facial::StartLipSync(*scene_, actor, assets, n->clip);
                    if (actor != entt::null && reg.all_of<Transform>(actor)) {
                        const glm::vec3 pos = glm::vec3(scene_->WorldMatrix(actor)[3]);
                        spatial = audio_->PlayUAFAt(assets / n->clip, pos, bus, minD, maxD, !hasText);
                    }
                }
                if (!spatial) audio_->PlayUAF(assets / n->clip, {}, /*caption=*/!hasText);
            }
            dialogueTimer_ = hold;
            return; // wait out the hold (UpdateDialogue advances when it lapses)
        } else if (n->type == dlg::NodeType::Choice) {
            SpawnDialogueChoices(*n);
            if (dialogueChoiceActive_) return;    // wait for the player's pick
            nodeId = 0;                            // no valid option -> end
        } else {                                   // End (or unknown)
            nodeId = 0;
        }
    }
    // Conversation finished (End, dead-end wire, or the cycle guard tripped).
    dialogueNode_ = 0;
    ClearDialogueChoices();
}

void Engine::SpawnDialogueChoices(const dlg::Node& node) {
    ClearDialogueChoices();
    if (!scene_) return;
    entt::registry& reg = scene_->Registry();

    // Collect the options that pass their showIf gate, keeping each one's ORIGINAL
    // out-pin index so a click follows the correct branch.
    struct Vis { u32 pin; const dlg::ChoiceOption* opt; };
    std::vector<Vis> visible;
    for (u32 i = 0; i < node.choices.size(); ++i) {
        const dlg::ChoiceOption& c = node.choices[i];
        if (!c.showIf.empty() && game::GetFlag(c.showIf) == 0.0f) continue;
        visible.push_back({i, &c});
    }
    if (visible.empty()) return; // caller ends the conversation (dialogueChoiceActive_ stays false)

    dialogueChoiceActive_ = true;
    const f32 kW = 660.0f, kH = 54.0f, kGap = 10.0f;
    const int count = static_cast<int>(visible.size());
    const f32 totalH = count * kH + (count - 1) * kGap;
    // Centre the stack, biased a little below the screen middle (above captions).
    f32 y = -totalH * 0.5f + kH * 0.5f + 90.0f;
    for (const Vis& v : visible) {
        const entt::entity e = reg.create();
        UIElement el;
        el.type = UIElement::Type::Button;
        el.text = v.opt->text;
        el.anchorMin = el.anchorMax = el.pivot = glm::vec2(0.5f, 0.5f);
        el.offset = glm::vec2(0.0f, y);
        el.size = glm::vec2(kW, kH);
        el.color = glm::vec4(0.10f, 0.11f, 0.14f, 0.92f);
        el.hoverColor = glm::vec4(0.22f, 0.36f, 0.55f, 0.98f);
        el.pressedColor = glm::vec4(0.12f, 0.22f, 0.36f, 1.0f);
        el.textSize = 26.0f;
        el.hAlign = UIElement::HAlign::Center;
        el.action = "__dlgchoice";   // ignored by the game-flow action switch
        reg.emplace<UIElement>(e, std::move(el));
        reg.emplace<DialogueChoiceButton>(e, DialogueChoiceButton{v.pin});
        y += kH + kGap;
    }
}

void Engine::ClearDialogueChoices() {
    dialogueChoiceActive_ = false;
    if (!scene_) return;
    entt::registry& reg = scene_->Registry();
    std::vector<entt::entity> del;
    reg.view<DialogueChoiceButton>().each(
        [&](entt::entity e, const DialogueChoiceButton&) { del.push_back(e); });
    for (const entt::entity e : del) reg.destroy(e);
}

void Engine::ResetDialogueRuntime() {
    dialogueNode_ = 0;
    dialogueTimer_ = 0.0f;
    dialogueGraph_ = dlg::Graph{};
    ClearDialogueChoices(); // also clears dialogueChoiceActive_ + destroys choice buttons
    HideInteractPrompt();
    captions_.clear();
}

void Engine::SyncActionMap() {
    if (Project::HasActive()) actionMap_.SetDefinitions(Project::Active().Settings().inputActions);
    actionMap_.SetOverrides(userSettings_.inputBindings);
    // The interaction system queries the action literally named "Interact"; warn if the
    // project has none (renamed/removed in the editor) so the broken prompt is diagnosable.
    bool hasInteract = false;
    for (const input::ActionDef& d : actionMap_.Definitions())
        if (d.name == "Interact") { hasInteract = true; break; }
    if (!hasInteract)
        HBE_WARN("Input: no action named 'Interact' - the interaction prompt/key will not work. "
                 "Add it back in the Input panel.");
}

void Engine::SaveUserSettings() {
    if (!userSettingsDir_.empty()) userSettings_.Save(userSettingsDir_);
}

void Engine::ShowInteractPrompt(const std::string& text, const std::string& iconPath,
                                glm::vec2 anchor) {
    if (!scene_) return;
    entt::registry& reg = scene_->Registry();
    const bool hasIcon = !iconPath.empty();

    // Text label (the verb, or the "[E] verb" glyph fallback).
    if (interactPrompt_ == entt::null || !reg.valid(interactPrompt_)) {
        interactPrompt_ = reg.create();
        UIElement el;
        el.type = UIElement::Type::Label;
        el.size = glm::vec2(360.0f, 40.0f);
        el.color = glm::vec4(1.0f, 0.96f, 0.75f, 1.0f);
        el.textSize = 26.0f;
        el.hAlign = UIElement::HAlign::Center;
        reg.emplace<UIElement>(interactPrompt_, std::move(el));
        reg.emplace<InteractPromptTag>(interactPrompt_); // excluded from saves
    }
    UIElement& lbl = reg.get<UIElement>(interactPrompt_);
    lbl.anchorMin = lbl.anchorMax = anchor; // point-anchor at the object centre
    lbl.runtimeText = text;
    lbl.visible = true;
    // With an icon, the verb sits just BELOW the (centred) icon; without one, the
    // "[E] verb" text is centred on the object.
    lbl.pivot = hasIcon ? glm::vec2(0.5f, 0.0f) : glm::vec2(0.5f, 0.5f);
    lbl.offset = hasIcon ? glm::vec2(0.0f, 28.0f) : glm::vec2(0.0f, 0.0f);

    // Device button icon, centred on the object.
    if (hasIcon) {
        if (interactIcon_ == entt::null || !reg.valid(interactIcon_)) {
            interactIcon_ = reg.create();
            UIElement el;
            el.type = UIElement::Type::Image;
            el.pivot = glm::vec2(0.5f, 0.5f);
            el.size = glm::vec2(52.0f, 52.0f);
            el.color = glm::vec4(1.0f);
            reg.emplace<UIElement>(interactIcon_, std::move(el));
            reg.emplace<InteractPromptTag>(interactIcon_);
        }
        UIElement& icon = reg.get<UIElement>(interactIcon_);
        if (icon.texture != iconPath) {
            icon.texture = iconPath;
            icon.textureResolved = false; // re-resolve through the UI texture cache
        }
        icon.anchorMin = icon.anchorMax = anchor;
        icon.visible = true;
    } else if (interactIcon_ != entt::null && reg.valid(interactIcon_)) {
        reg.get<UIElement>(interactIcon_).visible = false;
    }
}

void Engine::HideInteractPrompt() {
    if (!scene_) return;
    entt::registry& reg = scene_->Registry();
    if (interactPrompt_ != entt::null && reg.valid(interactPrompt_))
        reg.get<UIElement>(interactPrompt_).visible = false;
    if (interactIcon_ != entt::null && reg.valid(interactIcon_))
        reg.get<UIElement>(interactIcon_).visible = false;
}

void Engine::UpdateInteractions(Scene& scene, f32 dt) {
    (void)dt;
    entt::registry& reg = scene.Registry();
    // Don't offer interactions while a conversation/cutscene is playing OR queued
    // this frame (the deferred queues are single-slot latest-wins, so firing now
    // would clobber a schematic's just-queued convo), or while a menu overlay is
    // open over the HUD (pause/Settings) - the prompt must not draw over it and the
    // E/gamepad key must not fire behind it.
    const bool menuOpen =
        uiManagerMode_ && !uiManager_.Empty() && uiManager_.Top() != "HUD";
    if (menuOpen || devMenuOpen_ || actionMap_.Rebinding() || rebindJustCommitted_ ||
        dialogueNode_ != 0 || cutsceneTime_ >= 0.0f || game::DialoguePending() ||
        game::CutscenePending()) {
        HideInteractPrompt();
        return;
    }

    // Player = the first CharacterController (matches game::UpdateCheckpoints).
    auto players = reg.view<Transform, CharacterController>();
    if (players.begin() == players.end()) { HideInteractPrompt(); return; }
    const glm::vec3 player = glm::vec3(scene.WorldMatrix(*players.begin())[3]);

    // Dispatch an action through the deferred game:: queues (identical to a schematic
    // firing it). Returns true if it started a blocking convo/cutscene.
    const auto fire = [&](InteractAction a, const std::string& asset, const std::string& flag,
                          f32 val, const std::string& text, const std::string& itemId,
                          u32 itemCount, const std::string& pickupId) -> bool {
        switch (a) {
            case InteractAction::Dialogue: if (!asset.empty()) game::PlayDialogue(asset); break;
            case InteractAction::Cutscene: if (!asset.empty()) game::PlayCutscene(asset); break;
            case InteractAction::SetFlag: game::SetFlag(flag, val); break;
            case InteractAction::SetObjective: if (!flag.empty()) game::SetObjective(flag, text); break;
            case InteractAction::CompleteObjective: if (!flag.empty()) game::CompleteObjective(flag); break;
            case InteractAction::GrantItem:
                if (!itemId.empty()) {
                    game::AddItem(itemId, itemCount);
                    // Authoritative "gone" state: a persistent flag (survives save/load
                    // AND a full level reload, unlike the scene snapshot alone).
                    if (!pickupId.empty()) game::SetFlag("picked." + pickupId, 1.0f);
                }
                break;
            case InteractAction::None: default: break;
        }
        return a == InteractAction::Dialogue || a == InteractAction::Cutscene;
    };

    // Remove pickups already collected this playthrough (the picked.<id> flag). Runs
    // each frame but is cheap; destroys on the first frame after a reload/level load.
    {
        std::vector<entt::entity> picked;
        for (const entt::entity e : reg.view<Interactable>()) {
            const Interactable& ia = reg.get<Interactable>(e);
            if (ia.action == InteractAction::GrantItem && !ia.pickupId.empty() &&
                game::GetFlag("picked." + ia.pickupId) != 0.0f)
                picked.push_back(e);
        }
        for (const entt::entity e : picked)
            if (reg.valid(e)) reg.destroy(e);
    }

    // Box triggers: fire on the ENTER edge.
    for (const entt::entity e : reg.view<TriggerVolume>()) {
        TriggerVolume& tv = reg.get<TriggerVolume>(e);
        if ((tv.once && tv.fired) ||
            (!tv.requiredFlag.empty() && game::GetFlag(tv.requiredFlag) == 0.0f) ||
            (tv.action == InteractAction::GrantItem && !tv.pickupId.empty() &&
             game::GetFlag("picked." + tv.pickupId) != 0.0f)) { // pickup already taken
            tv.inside = false;
            continue;
        }
        const glm::vec3 c = glm::vec3(scene.WorldMatrix(e)[3]);
        const glm::vec3 d = glm::abs(player - c);
        const bool inside =
            d.x <= tv.halfExtents.x && d.y <= tv.halfExtents.y && d.z <= tv.halfExtents.z;
        const bool enter = inside && !tv.inside;
        tv.inside = inside;
        if (enter) {
            tv.fired = true;
            if (fire(tv.action, tv.asset, tv.flag, tv.flagValue, tv.text, tv.itemId, tv.itemCount,
                     tv.pickupId)) {
                HideInteractPrompt();
                return; // a trigger started a convo/cutscene - stop here this frame
            }
        }
    }

    // Interactables: pick the nearest available one in range, prompt, fire on the key.
    // The interactable's world position = its geometry CENTER (AABB center under the
    // world transform), so the prompt sits dead-centre on the object rather than at
    // the pivot; falls back to the transform origin when there's no bounds.
    const auto worldCenter = [&](entt::entity e) -> glm::vec3 {
        const glm::mat4 m = scene.WorldMatrix(e);
        if (const AABB* bb = reg.try_get<AABB>(e))
            return glm::vec3(m * glm::vec4((bb->min + bb->max) * 0.5f, 1.0f));
        return glm::vec3(m[3]);
    };

    entt::entity best = entt::null;
    f32 bestD2 = 0.0f;
    std::string bestPrompt;
    glm::vec3 bestCenter(0.0f);
    for (const entt::entity e : reg.view<Interactable>()) {
        const Interactable& ia = reg.get<Interactable>(e);
        if (ia.once && ia.fired) continue;
        if (!ia.requiredFlag.empty() && game::GetFlag(ia.requiredFlag) == 0.0f) continue;
        const glm::vec3 center = worldCenter(e);
        const glm::vec3 rel = center - player;
        const f32 d2 = glm::dot(rel, rel);
        if (d2 <= ia.range * ia.range && (best == entt::null || d2 < bestD2)) {
            best = e;
            bestD2 = d2;
            bestPrompt = ia.prompt;
            bestCenter = center;
        }
    }
    if (best == entt::null) { HideInteractPrompt(); return; }

    // Project the object CENTRE to a screen anchor so the prompt sits over it.
    // Behind the camera -> hide; off to a side -> clamp on-screen.
    glm::vec2 anchor(0.5f, 0.85f);
    if (renderer_) {
        const glm::mat4 vp = renderer_->GetCamera().ViewProjection();
        const glm::vec4 clip = vp * glm::vec4(bestCenter, 1.0f);
        if (clip.w <= 1e-4f) { HideInteractPrompt(); return; }
        const glm::vec2 ndc(clip.x / clip.w, clip.y / clip.w);
        anchor = glm::clamp(glm::vec2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f), glm::vec2(0.04f),
                            glm::vec2(0.96f)); // y-down canvas fraction
    }

    // Device-adaptive prompt driven by the "Interact" action's CURRENT binding:
    // on a gamepad show the bound pad button, on keyboard the bound key. Prefer the
    // project's per-device icon IMAGE for that button (fallback: general icon); if
    // none is set, show a text glyph of the button name.
    const bool gamepad = input_ && input_->LastInputWasGamepad();
    const input::Binding bind = actionMap_.Current("Interact");
    std::string icon;
    std::string glyph;
    // Platform-agnostic mode: one universal icon for every device/button (a single
    // "just press this" symbol matching the game's art style).
    const bool useGeneral =
        Project::HasActive() && Project::Active().Settings().inputIcons.useGeneralAlways;
    if (useGeneral) {
        icon = Project::HasActive() ? Project::Active().Settings().inputIcons.general : "";
        // No image set -> fall back to the current binding's text glyph so there's
        // still something to press-label.
        glyph = gamepad ? "" : input::KeyName(bind.key);
    } else if (gamepad && bind.pad != 0) {
        const InputIcons* ic = Project::HasActive() ? &Project::Active().Settings().inputIcons : nullptr;
        const DeviceGlyphs* dev = nullptr;
        // brandName stays null for every button EXCEPT the PlayStation face buttons;
        // a null brand falls through to the generic PadButtons() label (A/B/LB/D-Up/...),
        // so non-PlayStation pads show the correct button rather than a fixed letter.
        const char* brandName = nullptr;
        switch (input_->GamepadBrand()) {
            case Input::PadBrand::PlayStation:
                dev = ic ? &ic->playstation : nullptr;
                brandName = (bind.pad == static_cast<u32>(Gamepad_X)) ? "Square"
                            : (bind.pad == static_cast<u32>(Gamepad_Y)) ? "Triangle"
                            : (bind.pad == static_cast<u32>(Gamepad_A)) ? "Cross"
                            : (bind.pad == static_cast<u32>(Gamepad_B)) ? "Circle" : nullptr;
                break;
            case Input::PadBrand::Nintendo: dev = ic ? &ic->nintendo : nullptr; break;
            case Input::PadBrand::Generic: dev = ic ? &ic->generic : nullptr; break;
            case Input::PadBrand::Xbox:
            default: dev = ic ? &ic->xbox : nullptr; break;
        }
        if (dev)
            if (const std::string* p = dev->Find(bind.pad)) icon = *p;
        // Text-glyph fallback: brand-specific name if known, else the generic label.
        glyph = brandName ? brandName : "?";
        if (!brandName || glyph == "?") {
            for (const input::PadButtonInfo& pb : input::PadButtons())
                if (pb.bit == bind.pad) { glyph = pb.name; break; }
        }
    } else if (gamepad) {
        // Gamepad in hand but the Interact action has no pad binding (e.g. rebound to a
        // keyboard-only key): show the neutral general icon, NOT a keyboard key glyph.
        glyph = "?"; // -> falls back to the general icon below if one is set
    } else {
        if (Project::HasActive())
            if (const std::string* p = Project::Active().Settings().inputIcons.keyboard.Find(
                    static_cast<u32>(bind.key)))
                icon = *p;
        glyph = input::KeyName(bind.key);
    }
    if (icon.empty() && Project::HasActive()) icon = Project::Active().Settings().inputIcons.general;
    if (glyph.empty()) glyph = "?";
    const std::string label =
        icon.empty() ? (std::string("[") + glyph + "] " + bestPrompt) : bestPrompt;
    ShowInteractPrompt(label, icon, anchor);

    const bool press = input_ && actionMap_.Pressed(*input_, "Interact");
    if (press) {
        Interactable& ia = reg.get<Interactable>(best);
        fire(ia.action, ia.asset, ia.flag, ia.flagValue, ia.text, ia.itemId, ia.itemCount,
             ia.pickupId);
        ia.fired = true;
        HideInteractPrompt();
    }
}

void Engine::UpdateCutscene(f32 dt) {
    if (cutsceneTime_ < 0.0f || !scene_ || !renderer_) return;
    const f32 prev = cutsceneTime_;
    cutsceneTime_ += dt;
    const f32 t = cutsceneTime_;

    // Pose the scene at t (camera only while we own it), then fire any events
    // crossed in [prev, t). Both live in cutscene::, shared with the editor's
    // timeline preview.
    cutscene::Evaluate(cutscene_, t, *scene_, renderer_->GetCamera(), cutsceneCamOwned_);
    cutscene::FireMarkers(cutscene_, prev, t, *scene_);

    // End: restore the game camera (rising edge snaps it back next apply).
    if (t >= cutscene_.duration) {
        cutsceneTime_ = -1.0f;
        if (cutsceneCamOwned_) {
            SetGameCameraEnabled(cutsceneRestoreCam_);
            cutsceneCamOwned_ = false;
        }
    }
}

void Engine::ClearCutscene() {
    if (cutsceneCamOwned_) {
        SetGameCameraEnabled(cutsceneRestoreCam_);
        cutsceneCamOwned_ = false;
    }
    cutsceneTime_ = -1.0f;
}

void Engine::UpdateGameFlow(f32 dt) {
    if (!flowActive_ || !scene_) return;
    switch (gameState_) {
    case GameState::Booting: {
        loadTimer_ += dt;
        const f32 p = glm::clamp(loadTimer_ / kBootDuration, 0.0f, 1.0f);
        SetProgressFill(*scene_, p);
        SubstituteUITokens(p); // backend/gpu/audio/version/progress
        if (loadTimer_ >= kBootDuration) FlowAfterBoot();
        break;
    }
    case GameState::MainMenu: {
        const std::string act = PollClickedAction(*scene_);
        // A "rebind:<Action>" button starts listening for the next key/button; the
        // rebind poll (top of the update) captures + persists it.
        if (!devMenuOpen_ && act.rfind("rebind:", 0) == 0) actionMap_.BeginRebind(act.substr(7));
        if (act == "play")
            FlowPlay();
        else if (act == "settings" && uiManager_.Has(*scene_, "Settings")) {
            uiManager_.Push(*scene_, "Settings"); // overlay Settings; "back" pops it
            SeedSettingsWidgets();
        } else if (act == "back") {
            uiManager_.Pop(*scene_);
            if (settingsDirty_) { userSettings_.Save(userSettingsDir_); settingsDirty_ = false; }
        } else if (act == "quit")
            Quit();
        break;
    }
    case GameState::Loading: {
        switch (loadPhase_) {
        case LoadPhase::Begin: {
            // First Loading frame - the screen is already black (set in FlowPlay). Do
            // the heavy world load behind the curtain, then show the resident Loading
            // panel and start fading the loading screen in.
            LoadGameplayWorld();
            bool shown = false;
            if (uiManagerMode_ && uiManager_.Has(*scene_, "Loading")) {
                uiManager_.Show(*scene_, "Loading");
                // Scope the progress/wheel drivers to the panel's subtree so gameplay
                // HUD bars are never clobbered (the panel entities are persistent -
                // Show("HUD") on reveal hides them, nothing is destroyed).
                loadingPanelEntities_.clear();
                CollectPanelSubtree(*scene_, "Loading", loadingPanelEntities_);
                SetProgressFill(*scene_, 0.0f, &loadingPanelEntities_);
                SetWheelAlpha(*scene_, 0.0f, &loadingPanelEntities_); // hidden until it eases in
                shown = true;
            }
            loadTimer_ = 0.0f;
            loadPhase_ = shown ? LoadPhase::FadeIn : LoadPhase::FadeOut;
            break;
        }
        case LoadPhase::FadeIn: {
            // Lift the black curtain so the loading screen appears (wheel still hidden).
            fadeAlpha_ = glm::max(fadeAlpha_ - dt / kFadeInDur, 0.0f);
            if (fadeAlpha_ <= 0.0f) { loadTimer_ = 0.0f; loadPhase_ = LoadPhase::Wheel; }
            break;
        }
        case LoadPhase::Wheel: {
            loadTimer_ += dt;
            // Ease the wheel in while the world streams behind the visible screen.
            wheelAlpha_ = glm::min(wheelAlpha_ + dt / kWheelFadeDur, 1.0f);
            SetWheelAlpha(*scene_, wheelAlpha_, &loadingPanelEntities_);

            // Hold until the world has materialized: terrain chunks built AND streamed
            // cells resident around the spawn (cam::Update parks the camera there while
            // the sim is frozen). Then nothing pops in when we reveal.
            const glm::vec3 focus =
                renderer_ ? renderer_->GetCamera().Position() : glm::vec3(0.0f);
            const bool terrainReady = terrain::IsSettled(*scene_);
            const bool streamReady = !(streamingWorld_ && streamingWorld_->Active()) ||
                                     streamingWorld_->IsSettled(focus);
            const bool ready = terrainReady && streamReady;

            const f32 p = (ready && loadTimer_ >= kLoadDuration)
                              ? 1.0f
                              : glm::clamp(loadTimer_ / kLoadDuration, 0.0f, 0.9f);
            SetProgressFill(*scene_, p, &loadingPanelEntities_);
            SubstituteUITokens(p); // {progress} on the game loading screen

            const bool timedOut = loadTimer_ >= kMaxLoadDuration;
            if ((ready && loadTimer_ >= kLoadDuration) || timedOut) {
                if (timedOut && !ready)
                    HBE_WARN("Loading: revealing after {:.0f}s cap (terrain={}, stream={}) - "
                             "something never settled.",
                             kMaxLoadDuration, terrainReady, streamReady);
                loadPhase_ = LoadPhase::FadeOut;
            }
            break;
        }
        case LoadPhase::FadeOut: {
            // Fade to black, then reveal the fully-resident world behind the curtain.
            fadeAlpha_ = glm::min(fadeAlpha_ + dt / kFadeOutDur, 1.0f);
            if (fadeAlpha_ >= 1.0f) EnterPlaying();
            break;
        }
        }
        break;
    }
    case GameState::Playing: {
        // Ease gameplay in from the fade-to-black the loading transition left behind.
        if (fadeAlpha_ > 0.0f)
            fadeAlpha_ = glm::max(fadeAlpha_ - dt / kGameFadeInDur, 0.0f);
        // Refresh live HUD tokens each frame so {objective}, {item:<id>} and {equipped}
        // reflect current game state (they were previously resolved only on load).
        SubstituteUITokens(1.0f);
        // Free-cursor policy: any menu panel over the HUD (Settings/Pause - pushed
        // by a button OR a schematic UI node) frees the cursor for point-and-click
        // (including world-space pages); back on the HUD relocks mouse-look.
        // Reconciled per frame so every panel-change path is covered.
        const bool menuOpen =
            uiManagerMode_ && !uiManager_.Empty() && uiManager_.Top() != "HUD";
        // Dialogue choices need point-and-click too, so free the cursor for them.
        const bool wantFreeCursor = menuOpen || dialogueChoiceActive_;
        if (IsCursorLocked() == wantFreeCursor) SetCursorLocked(!wantFreeCursor);
        const std::string act = PollClickedAction(*scene_);
        // A "rebind:<Action>" button starts listening for the next key/button; the
        // rebind poll (top of the update) captures + persists it.
        if (!devMenuOpen_ && act.rfind("rebind:", 0) == 0) actionMap_.BeginRebind(act.substr(7));
        if (act == "menu")
            FlowMainMenu();
        else if (act == "restart")
            FlowReload();
        else if (act == "settings" && uiManager_.Has(*scene_, "Settings")) {
            uiManager_.Push(*scene_, "Settings"); // pause-menu style overlay; "back" pops
            SeedSettingsWidgets();
        } else if (act == "back") {
            uiManager_.Pop(*scene_);
            if (settingsDirty_) { userSettings_.Save(userSettingsDir_); settingsDirty_ = false; }
        } else if (act == "quit")
            Quit();
        break;
    }
    case GameState::None:
        break;
    }
}

EngineConfig ParseCommandLine(int argc, char** argv) {
    EngineConfig config;
    config.api = rhi::GraphicsAPI::D3D12;
    // Validation/debug layers are opt-in (--validation): they add large
    // per-draw-call CPU overhead, so they are off by default even in Debug.

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--vulkan" || arg == "--vk") {
            config.api = rhi::GraphicsAPI::Vulkan;
            config.apiExplicit = true;
        } else if (arg == "--d3d12" || arg == "--dx12") {
            config.api = rhi::GraphicsAPI::D3D12;
            config.apiExplicit = true;
        } else if (arg == "--opengl" || arg == "--gl") {
            config.api = rhi::GraphicsAPI::OpenGL;
            config.apiExplicit = true;
        } else if (arg == "--validation") {
            config.enableValidation = true;
        } else if (arg == "--no-validation") {
            config.enableValidation = false;
        } else if (arg == "--width" && i + 1 < argc) {
            config.width = static_cast<u32>(std::stoul(argv[++i]));
            config.widthExplicit = true;
        } else if (arg == "--height" && i + 1 < argc) {
            config.height = static_cast<u32>(std::stoul(argv[++i]));
            config.heightExplicit = true;
        } else if (arg == "--fullscreen") {
            config.fullscreen = true;
            config.fullscreenExplicit = true;
        } else if (arg == "--windowed") {
            config.fullscreen = false;
            config.fullscreenExplicit = true;
        } else if (arg == "--vsync") {
            config.vsync = true;
            config.vsyncExplicit = true;
        } else if (arg == "--novsync") {
            config.vsync = false; // uncapped present (perf measurement / player choice)
            config.vsyncExplicit = true;
        } else if (arg == "--nocull") {
            config.noCull = true; // frustum-culling kill switch (A/B)
        } else if (arg == "--model" && i + 1 < argc) {
            config.modelPath = argv[++i];
        } else if (arg == "--project" && i + 1 < argc) {
            config.projectPath = argv[++i];
        } else if (arg == "--pack") {
            config.packOnly = true;
        } else if (arg == "--ship") {
            config.shipOnly = true;
        } else if (arg == "--import" && i + 1 < argc) {
            config.importPath = argv[++i];
        } else if (arg == "--winpos" && i + 2 < argc) {
            config.posX = std::stoi(argv[++i]);
            config.posY = std::stoi(argv[++i]);
        } else if (arg == "--stress" && i + 1 < argc) {
            config.stressCount = static_cast<u32>(std::stoul(argv[++i]));
        } else if (arg == "--stress-shared" && i + 1 < argc) {
            config.stressCount = static_cast<u32>(std::stoul(argv[++i]));
            config.stressShared = true; // one shared mesh (sort/instancing rig)
        } else if (arg == "--shadowcascades" && i + 1 < argc) {
            config.forceShadowCascades = std::stoi(argv[++i]); // TEMP perf A/B
        } else if (arg == "--gpuprofile") {
            config.gpuProfile = true; // per-pass GPU timestamp breakdown (both backends)
        } else if (arg == "--world" && i + 1 < argc) {
            config.worldPath = argv[++i];
        } else if (arg == "--worldtest") {
            config.worldTest = true;
        } else if (arg == "--dof") {
            config.forceDof = true;
        } else if (arg == "--motionblur") {
            config.forceMotionBlur = true;
        } else if (arg == "--ssr") {
            config.forceSsr = true;
        } else if (arg == "--autoexposure" || arg == "--autoexp") {
            config.forceAutoExposure = true;
        } else if (arg == "--navtest") {
            config.navTest = true;
        } else if (arg == "--play") {
            config.playOnBoot = true;
        } else if (arg == "--uiworldtest") {
            config.uiWorldTest = true; // world-space UI smoke test (lit page)
        } else if (arg == "--time" && i + 1 < argc) {
            config.forceTimeOfDay = std::stof(argv[++i]); // scrub the day/night sky
        } else if (arg == "--daynight" && i + 1 < argc) {
            config.forceDayLength = std::stof(argv[++i]); // auto-cycle seconds
        } else if (arg == "--clouds" && i + 1 < argc) {
            config.forceClouds = std::stof(argv[++i]); // cloud coverage 0..1
        }
    }

    if (!rhi::IsBackendCompiled(config.api)) {
        config.api = rhi::IsBackendCompiled(rhi::GraphicsAPI::D3D12)
                         ? rhi::GraphicsAPI::D3D12
                         : rhi::GraphicsAPI::Vulkan;
    }
    return config;
}

} // namespace hbe
