// Engine/Engine.cpp
#include "Engine/Engine.h"
#include "Assets/VFS.h"
#include "Audio/AudioSystem.h"
#include "Core/Input.h"
#include "Core/JobSystem.h"
#include "Core/Log.h"
#include "Core/Window.h"
#include "Game/GameSystems.h"
#include "Physics/PhysicsWorld.h"
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
#include "UI/UISystem.h"
#include "Renderer/Renderer.h"
#include "RHI/RHIFactory.h"
#include "Scene/Scene.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
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
            if (!config.apiExplicit) config.api = ApiFromString(build.backend);
        }
    }
#endif

    HBE_INFO("Heartbreak Engine starting ({})", rhi::ToString(config.api));

    // Worker threads + fibers come up first: subsystems below parallelize onto
    // them (animation, scene streaming, culling).
    jobs::Initialize();

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
    Window window(wd);
    if (!window.GetNativeHandle().hwnd) {
        HBE_ERROR("Failed to create window.");
        return 1;
    }

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
        if (renderer.Initialize(window, api, config.enableValidation)) {
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

    // Build the scene (skipped when the studio splash already owns the world): the
    // project's main menu when set, else startup scene / model / default world. In
    // the studio-splash case the menu loads later, after the splash (FlowAfterBoot).
    if (!onInit_ && !sceneBuilt && Project::HasActive() &&
        !Project::Active().Settings().mainMenuScene.empty()) {
        const std::filesystem::path menu =
            Project::Active().AssetsDir() / Project::Active().Settings().mainMenuScene;
        if (vfs::Exists(menu) && scene::LoadScene(scene, renderer, menu)) {
            flowActive_ = true;
            gameState_ = GameState::MainMenu;
            sceneBuilt = true;
        } else {
            HBE_WARN("Main-menu scene '{}' failed to load; falling back to startup.",
                     Project::Active().Settings().mainMenuScene);
        }
    }
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
        scene::SpawnStress(scene, renderer, config.stressCount);
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

    // Route OS resize events into the renderer.
    window.SetResizeCallback([&renderer](u32 w, u32 h) { renderer.Resize(w, h); });

    // One-time application init (e.g. the editor wires up ImGui here).
    if (onInit_) onInit_(*this);

    using clock = std::chrono::high_resolution_clock;
    auto last = clock::now();
    auto fpsLast = last;
    u32 fpsFrames = 0;
    std::vector<rhi::UIVertex> uiVertices; // reused each frame
    std::vector<rhi::ParticleVertex> particleAlpha, particleAdd; // reused each frame
    glm::vec3 streamFocus(0.0f);           // last streaming focus (for stats log)
    f32 worldTestT = 0.0f;                 // smoke-test focus-sweep clock
    cam::CameraState cameraState;          // persistent camera smoothing/blend state
    bool prevGameCamEnabled = false;       // rising edge -> snap the camera

    while (true) {
        // Roll input edge state, then pump: this frame's events land in `input`.
        input.NewFrame();
        if (!window.PumpMessages()) break;

        const auto now = clock::now();
        const f32 dt = std::chrono::duration<f32>(now - last).count();
        last = now;
        dt_ = dt;

        // Periodic FPS report.
        if (++fpsFrames >= 1) {
            const f32 elapsed = std::chrono::duration<f32>(now - fpsLast).count();
            if (elapsed >= 2.0f) {
                HBE_INFO("Perf: {:.1f} FPS ({:.2f} ms/frame)", fpsFrames / elapsed,
                         1000.0f * elapsed / fpsFrames);
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
        if (devEnabled && input.IsKeyDown(Key::Ctrl) && input.WasKeyPressed(Key::Grave))
            devMenuOpen_ = !devMenuOpen_;
        if (devEnabled && devMenuOpen_) {
            if (input.WasKeyPressed(Key::F5)) SaveGame("checkpoint");
            if (input.WasKeyPressed(Key::F9)) LoadGame("checkpoint");
            if (flowActive_ && input.WasKeyPressed(Key::F2)) FlowReload();
            if (flowActive_ && input.WasKeyPressed(Key::F3)) FlowMainMenu();
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
            ui::UpdateInteraction(scene, input, pointerNorm, uiTarget, uiConfig);
        }

        // High-level game flow: reads the fresh button-click state above to drive
        // menu -> loading -> gameplay transitions (runtime only; opt-in via the
        // project's main-menu scene). Cursor lock is applied here too.
        if (flowActive_) UpdateGameFlow(dt);

        // Visual-script "Schematics" tick while the simulation runs (the editor's
        // play mode gates physics; the runtime always plays). On Start / On Update.
        schematic::Update(scene, input, dt, physics.IsRunning());
        // Player/character input intent BEFORE physics, which drives the capsule
        // CharacterVirtual (gravity + world collision). Camera-relative movement
        // uses the current view's forward.
        if (physics.IsRunning())
            character::Update(scene, input, dt, renderer.GetCamera().Forward());
        physics.Update(scene, dt);
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
        // Checkpoints: fire box-triggers the player entered, then perform any save
        // a reached checkpoint (trigger / script / schematic) requested this frame.
        if (physics.IsRunning()) {
            game::UpdateCheckpoints(scene);
            std::string cpId;
            if (game::ConsumeSaveRequest(cpId)) SaveGame("checkpoint");
        }
        if (physics.IsRunning()) anim::UpdateRotators(scene, dt);
        audio.UpdateScene(scene,
                          Project::HasActive() ? Project::Active().AssetsDir()
                                               : std::filesystem::path(),
                          renderer.GetCamera().Position(),
                          renderer.GetCamera().Forward(),
                          physics.IsRunning()); // autoplay only when the game runs
        audio.Update();
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
            renderer.SetParticles(particleAlpha, particleAdd);
        }

        // In-game UI built AFTER scripts so text/visibility edits show this
        // frame; drawn over the scene inside RenderScene.
        ui::BuildVertices(scene, renderer,
                          Project::HasActive() ? Project::Active().AssetsDir()
                                               : std::filesystem::path(),
                          uiTarget, uiConfig, uiVertices);
        BuildDevOverlay(uiVertices); // dev stats panel on top (when toggled on)
        renderer.SetUIOverlay(uiVertices);
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
        renderer.RenderScene(scene, dt);
    }

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
    if (flowActive_) { // resume gameplay (skip menus)
        gameState_ = GameState::Playing;
        SetCursorLocked(true);
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
// Drive every ProgressBar's fill (the loading bar) to `f` in [0,1].
void SetProgressFill(Scene& scene, f32 f) {
    scene.Registry().view<UIElement>().each([&](UIElement& el) {
        if (el.type == UIElement::Type::ProgressBar) el.fill = f;
    });
}
} // namespace

// Loading duration in seconds: the bar sweeps over this, then the (synchronous)
// gameplay load happens. A real async stream would key the bar off load progress.
static constexpr f32 kLoadDuration = 1.25f;
// Studio/boot splash dwell in the loop AFTER the pre-loop warmup (IBL + loads
// already ran behind the splash); just long enough to read the final status.
static constexpr f32 kBootDuration = 1.0f;

void Engine::FlowMainMenu() {
    if (!flowActive_ || !scene_ || !renderer_ || !Project::HasActive()) return;
    const std::filesystem::path menu =
        Project::Active().AssetsDir() / Project::Active().Settings().mainMenuScene;
    if (vfs::Exists(menu)) scene::LoadScene(*scene_, *renderer_, menu); // Replace
    SetCursorLocked(false);
    gameState_ = GameState::MainMenu;
    loadTimer_ = 0.0f;
}

void Engine::FlowPlay() {
    if (!flowActive_ || !scene_ || !renderer_ || !Project::HasActive()) return;
    const std::string& loadingScene = Project::Active().Settings().loadingScene;
    if (!loadingScene.empty()) {
        const std::filesystem::path loading = Project::Active().AssetsDir() / loadingScene;
        if (vfs::Exists(loading) && scene::LoadScene(*scene_, *renderer_, loading)) {
            SetProgressFill(*scene_, 0.0f);
            SetCursorLocked(false);
            loadTimer_ = 0.0f;
            gameState_ = GameState::Loading;
            return;
        }
    }
    EnterPlaying(); // no loading screen: jump straight into gameplay
}

void Engine::EnterPlaying() {
    if (!scene_ || !renderer_ || !Project::HasActive()) return;
    game::Reset(); // a fresh run: clear objectives + reached checkpoints
    const std::filesystem::path assets = Project::Active().AssetsDir();
    const ProjectSettings& s = Project::Active().Settings();

    // Gameplay scene/level REPLACES the loading screen (clears the registry).
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

    // HUD overlays the gameplay world (additive, kept resident while playing).
    if (!s.hudScene.empty()) {
        const std::filesystem::path hud = assets / s.hudScene;
        if (vfs::Exists(hud))
            scene::LoadScene(*scene_, *renderer_, hud, scene::LoadMode::Additive);
    }

    // No nav step: GridNav rebuilds from the loaded gameplay geometry each frame.

    SetCursorLocked(true); // mouse-look while playing
    loadTimer_ = 0.0f;
    gameState_ = GameState::Playing;
}

void Engine::FlowReload() {
    // Respawn / restart: same path as Play (game loading screen -> reload gameplay
    // scene + HUD). Works from Playing too.
    FlowPlay();
}

void Engine::FlowAfterBoot() {
    // Studio splash finished: go to the main menu when the project has one, else
    // straight into gameplay (FlowPlay shows the game loading screen if set).
    // --play (playOnBoot_) forces gameplay directly, skipping the menu.
    if (!playOnBoot_ && Project::HasActive() &&
        !Project::Active().Settings().mainMenuScene.empty())
        FlowMainMenu();
    else
        FlowPlay();
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
        if (el.text.find('{') == std::string::npos) { el.runtimeText.clear(); return; }
        std::string t = el.text;
        replaceAll(t, "{backend}", backend);
        replaceAll(t, "{gpu}", gpu);
        replaceAll(t, "{audio}", audioDev);
        replaceAll(t, "{version}", version);
        replaceAll(t, "{progress}", pct);
        replaceAll(t, "{objective}", game::CurrentObjectiveText()); // HUD task goal
        if (t.find("{log}") != std::string::npos) {
            // Only the single latest line (real-time status), per design.
            if (!logFetched) { logTail = RecentLog(1); logFetched = true; }
            replaceAll(t, "{log}", logTail);
        }
        el.runtimeText = std::move(t);
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
    ui::BuildVertices(*scene_, *renderer_, assets, renderer_->RenderTargetSize(),
                      uiConfig, verts);
    renderer_->SetUIOverlay(verts);
    renderer_->RenderScene(*scene_, 0.0f); // draws the splash UI + presents
}

void Engine::BuildDevOverlay(std::vector<rhi::UIVertex>& out) {
    if (!devMenuOpen_ || !renderer_ || !scene_) return;
    if (!(Project::HasActive() && Project::Active().Settings().build.devMenu)) return;
    const glm::vec2 target = renderer_->RenderTargetSize();
    if (target.x < 1.0f || target.y < 1.0f) return;

    // --- Stats ---
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
    const int objects = static_cast<int>(scene_->Registry().view<Transform>().size());
    std::string obj = game::CurrentObjectiveText();
    if (obj.empty()) obj = "(none)";

    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "== DEV MENU ==  (Ctrl+` to close)\n"
                  "FPS %.0f   |   %.2f ms\n"
                  "State: %s    Level: %s\n"
                  "Player: %.1f, %.1f, %.1f    Objects: %d\n"
                  "Objective: %s\n"
                  "[F5] Save   [F9] Load   [F2] Restart   [F3] Menu",
                  fps, dt_ * 1000.0f, st, level.c_str(), ppos.x, ppos.y, ppos.z, objects,
                  obj.c_str());
    const std::string block = buf;

    // --- Emit quads (NDC) ---
    auto ndc = [&](f32 x, f32 y) {
        return glm::vec2(x / target.x * 2.0f - 1.0f, 1.0f - y / target.y * 2.0f);
    };
    auto addRect = [&](f32 x0, f32 y0, f32 x1, f32 y1, f32 r, f32 g, f32 b, f32 a, u32 tex,
                       f32 u0, f32 v0, f32 u1, f32 v1) {
        const glm::vec2 p0 = ndc(x0, y0), p1 = ndc(x1, y1);
        const rhi::UIVertex a00{p0.x, p0.y, u0, v0, r, g, b, a, tex};
        const rhi::UIVertex a10{p1.x, p0.y, u1, v0, r, g, b, a, tex};
        const rhi::UIVertex a11{p1.x, p1.y, u1, v1, r, g, b, a, tex};
        const rhi::UIVertex a01{p0.x, p1.y, u0, v1, r, g, b, a, tex};
        out.push_back(a00); out.push_back(a10); out.push_back(a11);
        out.push_back(a00); out.push_back(a11); out.push_back(a01);
    };

    ui::FontAtlas& font = ui::SharedFont();
    font.Initialize(*renderer_);
    const f32 textPx = 18.0f, padX = 12.0f, padY = 10.0f, ox = 16.0f, oy = 16.0f;
    std::vector<ui::GlyphQuad> quads;
    f32 bw = 0.0f, bh = 0.0f;
    if (font.Ready()) font.Layout(block, textPx, quads, bw, bh);

    addRect(ox, oy, ox + bw + padX * 2.0f, oy + bh + padY * 2.0f, 0.04f, 0.05f, 0.08f, 0.85f, 0,
            0, 0, 1, 1); // panel
    const u32 ft = font.TextureIndex();
    for (const ui::GlyphQuad& q : quads)
        addRect(ox + padX + q.x0, oy + padY + q.y0, ox + padX + q.x1, oy + padY + q.y1,
                0.95f, 0.97f, 1.0f, 1.0f, ft, q.u0, q.v0, q.u1, q.v1);
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
        if (act == "play")
            FlowPlay();
        else if (act == "quit")
            Quit();
        break;
    }
    case GameState::Loading: {
        loadTimer_ += dt;
        const f32 p = glm::clamp(loadTimer_ / kLoadDuration, 0.0f, 1.0f);
        SetProgressFill(*scene_, p);
        SubstituteUITokens(p); // {progress} on the game loading screen
        if (loadTimer_ >= kLoadDuration) EnterPlaying();
        break;
    }
    case GameState::Playing: {
        const std::string act = PollClickedAction(*scene_);
        if (act == "menu")
            FlowMainMenu();
        else if (act == "restart")
            FlowReload();
        else if (act == "quit")
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
