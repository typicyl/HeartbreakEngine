// Engine/Engine.cpp
#include "Engine/Engine.h"
#include "Core/Platform.h"
#include "Engine/CutscenePlayer.h"
#include "Assets/CutsceneAsset.h"
#include "Assets/DialogueAsset.h"
#include "Assets/Fracture.h"      // --fracturetest smoke test
#include "Assets/MeshGenerator.h" // GenerateCube for the fracture test
#include "Assets/VFS.h"
#include "Audio/AudioSystem.h"
#include "Core/Input.h"
#include "Core/JobSystem.h"
#include "Core/Log.h"
#include "Core/Window.h"
#include "Game/DestructionSystem.h"
#include "Game/GameSystems.h"
#include "Game/GameplaySystems.h"
#include "Physics/PhysicsWorld.h"
#include "Assets/MusicGraph.h"
#include "Project/Project.h"
#include "Navigation/GridNav.h"
#include "Scene/AnimationSystem.h"
#include "Scene/CameraSystem.h"
#include "Scene/CharacterController.h"
#include "Scene/ParticleSystem.h"
#include "Scene/PaintSystem.h" // --test-terraincollide: terrain brush raycast + dab
#include "Scene/TerrainSystem.h"
#include "Scene/SceneSerializer.h"
#include "Scene/TagShard.h" // save-time spatial shard bake (--tagstreamtest builds one)
#include "Scene/TagTable.h" // tags::Intern/Normalize (the synthetic test world's tags)
#include "Scene/WorldState.h" // per-area revisit state (capture on exit, replay on entry)
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
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
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
            // DELIBERATELY NOT platform::ExecutablePath(). This asks a different question:
            // "which MODULE does this faulting address live in?" - the answer is a DLL as
            // often as it is the exe, which is the entire point of printing it. The platform
            // helper only ever answers for the running process. Leave this one alone.
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


// Maps a build-settings backend string to the RHI enum.
rhi::GraphicsAPI ApiFromString(const std::string& s) {
    if (s == "vulkan" || s == "vk") return rhi::GraphicsAPI::Vulkan;
    if (s == "opengl" || s == "gl") return rhi::GraphicsAPI::OpenGL;
    return rhi::GraphicsAPI::D3D12;
}

// Day/night cycle: advances timeOfDay (when a day length is set) and points the
// sun where that hour puts it, so the analytic sky and the scene lighting stay in
// sync as the sun moves. No-op unless dynamicSky.
//
// IT WRITES DIRECTION AND NOTHING ELSE, and that is the fix, not an omission.
// This function used to REPLACE env.ambientIntensity, env.sun.colour/intensity and
// every DirectionalLightComponent's colour/intensity with absolute constants, every
// frame. The effect was that with `dynamicSky` on, a scene's authored ambient,
// exposure and sun were DEAD DATA - overwritten before anything read them - so
// ambient swept 0.12 -> 1.0 and back once per day cycle no matter what the level
// author typed, and a sealed interior authored at 0.63 ambient rendered at 1.0.
// The curve is now applied as a MULTIPLIER at read time in Scene::MakeView, over
// the authored values, so authored lighting is authorable again and the inspector
// keeps showing the numbers the file holds.
//
// Direction stays here because the cycle genuinely owns it: a sun that does not
// move is not a day/night cycle, and the shadow cascades + analytic sky both need
// the moved sun. The components are written (not just env.sun) so the editor's
// light gizmo points where the light actually points.
void UpdateDayNight(Scene& scene, f32 dt) {
    SceneEnvironment& env = scene.Environment();
    if (env.dynamicSky == 0) return;
    if (env.dayLengthSeconds > 0.0f) {
        env.timeOfDay += dt * (24.0f / env.dayLengthSeconds);
        env.timeOfDay = std::fmod(env.timeOfDay, 24.0f);
        if (env.timeOfDay < 0.0f) env.timeOfDay += 24.0f;
    }
    // One shared evaluation of the curve (scene::EvalDayNight) - MakeView reads the
    // same function off the same timeOfDay, so the two can no longer disagree.
    const glm::vec3 fromLight = -EvalDayNight(env.timeOfDay).toSun; // lights point FROM the sun
    env.sun.direction = fromLight;
    auto view = scene.Registry().view<DirectionalLightComponent>();
    for (const entt::entity e : view) view.get<DirectionalLightComponent>(e).direction = fromLight;
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
    // AUTO-FALLBACK IS D3D12/VULKAN ONLY. OpenGL is a preview backend - no shadows,
    // no post stack, no punctual lights/probes/GI, bind-pose skinning, no particles,
    // no material flags (glass renders opaque) - so a player whose D3D12 AND Vulkan
    // both failed would silently get a visibly different product rather than an
    // error. It stays reachable when it is CHOSEN (--opengl, or a build profile
    // that names it), which is the only way a preview backend should ever ship.
    for (A a : {A::D3D12, A::Vulkan})
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
    tagStreamTest_ = config.tagStreamTest;
    forceTimeOfDay_ = config.forceTimeOfDay;
    forceDayLength_ = config.forceDayLength;
    forceClouds_ = config.forceClouds;

    // Register any schematics that were transpiled to native C++ and linked into
    // this executable (a baked runtime). The stub registers nothing, so the editor
    // and an un-baked runtime fall back to the interpreter. Idempotent / once.
    schematic::RegisterBakedSchematics();
#if !HBE_EDITOR
    {
        // A SHIPPED build finds its packs beside the executable, never via the working
        // directory - a game launched from a shortcut starts somewhere else entirely.
        const std::filesystem::path exeDir = platform::ExecutableDir();
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
            // Straight assignment now that both sides are UTF-8. This line used to widen
            // bytes one-for-one into a wstring, which silently produced MOJIBAKE in the
            // title bar for any project whose name was not pure ASCII.
            config.title = gameName;
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

    // Fracture smoke test: headless correctness check of the Voronoi fracture.
    // Computational geometry compiles happily while producing garbage, so this
    // asserts the INVARIANTS that actually matter:
    //   * volume is CONSERVED (sum of chunk volumes ~= source volume) - the single
    //     best signal that the half-space clipping is correct; a bad cap or a
    //     dropped face shows up immediately as lost volume,
    //   * every chunk is a closed non-empty mesh,
    //   * adjacency is SYMMETRIC (if a touches b, b touches a),
    //   * the result is deterministic for a fixed seed (same seed -> same volumes),
    //   * it round-trips through .hbfrac unchanged.
    if (config.fractureTest) {
        int code = 0;
        const MeshData cube = mesh::GenerateCube(2.0f); // 2x2x2 -> volume 8
        constexpr f32 kSourceVolume = 8.0f;

        const FracturePattern pats[] = {FracturePattern::Uniform, FracturePattern::Clustered,
                                        FracturePattern::Radial, FracturePattern::Slabs};
        for (const FracturePattern pat : pats) {
            FractureSettings fs;
            fs.pattern = pat;
            fs.cellCount = 24;
            fs.seed = 20260727u;
            // Keep everything so volume must balance exactly. The aggressive-cull
            // case below is what exercises the index remapping.
            fs.minChunkVolumeFrac = 0.0f;
            const auto res = assets::FractureMesh(cube, fs);
            if (!res) {
                HBE_ERROR("FractureTest[{}]: returned no asset.", FracturePatternName(pat));
                code = 1;
                continue;
            }
            f32 total = 0.0f;
            usize emptyMeshes = 0;
            for (const FractureChunk& c : res->chunks) {
                total += c.volume;
                if (c.mesh.Empty()) ++emptyMeshes;
            }
            const f32 err = std::abs(total - kSourceVolume) / kSourceVolume;
            HBE_INFO("FractureTest[{}]: {} chunks, volume {:.4f}/{:.4f} (err {:.2f}%), "
                     "{} empty meshes.",
                     FracturePatternName(pat), res->chunks.size(), total, kSourceVolume,
                     err * 100.0f, emptyMeshes);
            // Voronoi cells of a box partition it exactly, so anything worse than
            // 1% is a real clipping bug, not float noise.
            if (err > 0.01f) {
                HBE_ERROR("FractureTest[{}]: VOLUME NOT CONSERVED (err {:.2f}%).",
                          FracturePatternName(pat), err * 100.0f);
                code = 1;
            }
            if (emptyMeshes > 0) {
                HBE_ERROR("FractureTest[{}]: {} chunk(s) produced empty geometry.",
                          FracturePatternName(pat), emptyMeshes);
                code = 1;
            }
            // Adjacency symmetry.
            usize asym = 0;
            for (usize i = 0; i < res->chunks.size(); ++i) {
                for (const u32 nb : res->chunks[i].neighbours) {
                    if (nb >= res->chunks.size()) { ++asym; continue; }
                    const auto& back = res->chunks[nb].neighbours;
                    if (std::find(back.begin(), back.end(), static_cast<u32>(i)) == back.end())
                        ++asym;
                }
            }
            if (asym > 0) {
                HBE_ERROR("FractureTest[{}]: {} asymmetric adjacency link(s).",
                          FracturePatternName(pat), asym);
                code = 1;
            }

            // Determinism: the same seed must reproduce the same decomposition.
            const auto again = assets::FractureMesh(cube, fs);
            if (!again || again->chunks.size() != res->chunks.size()) {
                HBE_ERROR("FractureTest[{}]: NOT DETERMINISTIC (chunk count differs).",
                          FracturePatternName(pat));
                code = 1;
            }

            // .hbfrac round-trip.
            if (pat == FracturePattern::Uniform) {
                const std::filesystem::path tmp =
                    std::filesystem::temp_directory_path() / "hbe_fracture_test.hbfrac";
                if (!assets::SaveFracture(tmp, *res)) {
                    HBE_ERROR("FractureTest: SaveFracture failed.");
                    code = 1;
                } else if (const auto rt = assets::LoadFracture(tmp)) {
                    f32 rtVol = 0.0f;
                    for (const FractureChunk& c : rt->chunks) rtVol += c.volume;
                    const bool same = rt->chunks.size() == res->chunks.size() &&
                                      std::abs(rtVol - total) < 1e-3f;
                    HBE_INFO("FractureTest: round-trip {} ({} chunks, volume {:.4f}).",
                             same ? "OK" : "MISMATCH", rt->chunks.size(), rtVol);
                    if (!same) code = 1;
                    std::error_code rmEc;
                    std::filesystem::remove(tmp, rmEc);
                } else {
                    HBE_ERROR("FractureTest: LoadFracture failed.");
                    code = 1;
                }
            }
        }
        // --- The case the first version of this test could not catch ----------
        // Adjacency is recorded against SITE ids but consumed as CHUNK indices,
        // and those only coincide while EVERY cell survives. The checks above cull
        // nothing, so they never exercised the remap. Force heavy cell death two
        // ways - aggressive sliver culling, and a clustered pattern whose
        // coincident sites kill cells outright - then re-assert symmetry and that
        // every neighbour id is a valid, in-range, non-self chunk.
        {
            struct Case { const char* name; FracturePattern pat; f32 cull; u32 cells; };
            const Case cases[] = {
                {"aggressive-cull", FracturePattern::Uniform,   0.85f, 40},
                {"clustered-dense", FracturePattern::Clustered, 0.60f, 64},
                {"radial-dense",    FracturePattern::Radial,    0.70f, 64},
            };
            for (const Case& tc : cases) {
                FractureSettings fs;
                fs.pattern = tc.pat;
                fs.cellCount = tc.cells;
                fs.seed = 4242u;
                fs.minChunkVolumeFrac = tc.cull; // culls most chunks -> index spaces diverge
                const auto res = assets::FractureMesh(cube, fs);
                if (!res) {
                    HBE_ERROR("FractureTest[{}]: no asset.", tc.name);
                    code = 1;
                    continue;
                }
                usize bad = 0, asym = 0, self = 0;
                for (usize i = 0; i < res->chunks.size(); ++i) {
                    for (const u32 nb : res->chunks[i].neighbours) {
                        if (nb >= res->chunks.size()) { ++bad; continue; }
                        if (nb == i) { ++self; continue; }
                        const auto& back = res->chunks[nb].neighbours;
                        if (std::find(back.begin(), back.end(), static_cast<u32>(i)) ==
                            back.end())
                            ++asym;
                    }
                }
                HBE_INFO("FractureTest[{}]: {} chunks kept, {} out-of-range, {} self-links, "
                         "{} asymmetric.",
                         tc.name, res->chunks.size(), bad, self, asym);
                if (bad || asym || self) {
                    HBE_ERROR("FractureTest[{}]: ADJACENCY CORRUPTED after chunk culling.",
                              tc.name);
                    code = 1;
                }
            }
        }

        // Settings must survive .hbfrac unchanged, or "re-fracture in the editor"
        // silently reverts the artist's tuning.
        {
            FractureSettings fs;
            fs.pattern = FracturePattern::Radial;
            fs.cellCount = 18;
            fs.seed = 99u;
            fs.impactPoint = {0.25f, -0.5f, 0.75f};
            fs.radialFalloff = 3.5f;
            fs.slabAxis = {0.0f, 0.0f, 1.0f};
            fs.slabBias = 7.0f;
            fs.minChunkVolumeFrac = 0.125f;
            const auto res = assets::FractureMesh(cube, fs);
            const std::filesystem::path tmp =
                std::filesystem::temp_directory_path() / "hbe_fracture_settings.hbfrac";
            if (res && assets::SaveFracture(tmp, *res)) {
                if (const auto rt = assets::LoadFracture(tmp)) {
                    const FractureSettings& s = rt->settings;
                    const bool same =
                        s.pattern == fs.pattern && s.cellCount == fs.cellCount &&
                        s.seed == fs.seed &&
                        glm::distance(s.impactPoint, fs.impactPoint) < 1e-5f &&
                        std::abs(s.radialFalloff - fs.radialFalloff) < 1e-5f &&
                        glm::distance(s.slabAxis, fs.slabAxis) < 1e-5f &&
                        std::abs(s.slabBias - fs.slabBias) < 1e-5f &&
                        std::abs(s.minChunkVolumeFrac - fs.minChunkVolumeFrac) < 1e-5f;
                    HBE_INFO("FractureTest: settings round-trip {}", same ? "OK" : "LOSSY");
                    if (!same) code = 1;
                } else {
                    HBE_ERROR("FractureTest: settings round-trip load failed.");
                    code = 1;
                }
                std::error_code rmEc;
                std::filesystem::remove(tmp, rmEc);
            }
        }

        HBE_INFO("FractureTest: {}", code == 0 ? "ALL PASS" : "FAILED");
        jobs::Shutdown();
        return code;
    }

    // Structural-integrity smoke test (headless): the support flood fill is the
    // part of destruction most likely to be subtly wrong, and it is pure, so it can
    // be proven without a GPU or a scene. Builds a fractured wall, anchors its base,
    // and checks that severing the base actually drops what it was holding up.
    if (config.destructionTest) {
        int code = 0;
        const MeshData wall = mesh::GenerateCube(4.0f);
        FractureSettings fs;
        fs.pattern = FracturePattern::Uniform;
        fs.cellCount = 40;
        fs.seed = 7u;
        fs.minChunkVolumeFrac = 0.0f;
        auto fracOpt = assets::FractureMesh(wall, fs);
        if (!fracOpt) {
            HBE_ERROR("DestructionTest: fracture failed.");
            jobs::Shutdown();
            return 1;
        }
        FractureAsset frac = std::move(*fracOpt);
        const usize n = frac.chunks.size();

        // Anchor the bottom layer, as an editor "anchor to ground" would.
        const f32 baseY = frac.boundsMin.y + (frac.boundsMax.y - frac.boundsMin.y) * 0.25f;
        usize anchors = 0;
        for (FractureChunk& c : frac.chunks) {
            c.anchored = c.centroid.y <= baseY;
            if (c.anchored) ++anchors;
        }

        using CS = Destructible::ChunkState;
        std::vector<u8> state(n, static_cast<u8>(CS::Intact));
        std::vector<u8> sup;

        // 1. Nothing broken: every chunk must be supported, or the graph is
        //    disconnected and later results mean nothing.
        destruction::ComputeSupport(frac, state, sup);
        usize supported = 0;
        for (const u8 s : sup) supported += s ? 1 : 0;
        HBE_INFO("DestructionTest: {} chunks, {} anchored, {} supported when intact.", n,
                 anchors, supported);
        if (anchors == 0) {
            HBE_ERROR("DestructionTest: no chunks anchored - test is vacuous.");
            code = 1;
        }
        if (supported != n) {
            HBE_WARN("DestructionTest: {} chunk(s) unreachable when intact (disconnected "
                     "adjacency islands).", n - supported);
        }

        // 2. Sever every anchored chunk. With no anchors left, NOTHING may remain
        //    supported - a floating wall is the classic destruction bug.
        std::vector<u8> cut = state;
        for (usize i = 0; i < n; ++i)
            if (frac.chunks[i].anchored) cut[i] = static_cast<u8>(CS::Detached);
        destruction::ComputeSupport(frac, cut, sup);
        usize floating = 0;
        for (usize i = 0; i < n; ++i)
            if (sup[i]) ++floating;
        HBE_INFO("DestructionTest: base severed -> {} chunk(s) still supported (want 0).",
                 floating);
        if (floating != 0) {
            HBE_ERROR("DestructionTest: FLOATING GEOMETRY - support survived with no anchor.");
            code = 1;
        }

        // 3. A detached chunk must not CONDUCT support. Anchor exactly one chunk,
        //    detach it, and confirm nothing downstream stays up through the hole.
        std::vector<u8> one(n, static_cast<u8>(CS::Intact));
        for (usize i = 0; i < n; ++i) frac.chunks[i].anchored = (i == 0);
        one[0] = static_cast<u8>(CS::Detached);
        destruction::ComputeSupport(frac, one, sup);
        usize leaked = 0;
        for (const u8 s : sup) leaked += s ? 1 : 0;
        HBE_INFO("DestructionTest: sole anchor detached -> {} supported (want 0).", leaked);
        if (leaked != 0) {
            HBE_ERROR("DestructionTest: support CONDUCTED THROUGH a detached chunk.");
            code = 1;
        }

        // 4. Idempotence: re-running the solve must not change the answer (it is
        //    called every time a break happens).
        std::vector<u8> again;
        destruction::ComputeSupport(frac, one, again);
        if (again != sup) {
            HBE_ERROR("DestructionTest: ComputeSupport is not deterministic.");
            code = 1;
        }

        HBE_INFO("DestructionTest: {}", code == 0 ? "ALL PASS" : "FAILED");
        jobs::Shutdown();
        return code;
    }

    // Terrain-collider smoke test (headless: no window, no GPU, no assets).
    //
    // Terrain collision used to be one static triangle-mesh body PER CHUNK, built
    // once when the chunk was created and never touched again - so every brush
    // stroke left the collider describing ground that was no longer there. It is now
    // a single Jolt HeightFieldShape per terrain, edited in place. This test pins the
    // five properties that has to have: right heights, holes that are actually holes,
    // sculpt edits that land, a CharacterController that stands on it, and no body
    // growth however much you sculpt.
    //
    // Deliberately headless-only: the collider is built from TerrainComponent::heights,
    // never from the chunk MESHES, so no renderer is needed to exercise it.
    if (config.terrainCollideTest) {
        int code = 0;
        const auto fail = [&code](const std::string& why) {
            HBE_ERROR("TerrainCollideTest FAIL: {}", why);
            code = 1;
        };

        Scene tscene;
        entt::registry& tr = tscene.Registry();
        const entt::entity te = tscene.CreateEntity("Terrain");
        // Non-trivial placement, so a bug in world placement cannot hide behind an
        // identity transform.
        const glm::vec3 origin(10.0f, 1.0f, -5.0f);
        Transform tt;
        tt.position = origin;
        tr.emplace<Transform>(te, tt);

        TerrainComponent tc;
        tc.chunks = 4;
        tc.resolution = 16;  // GridN = 65: ODD on purpose, so Jolt's round-up to a
        tc.chunkSize = 8.0f; // block multiple (66) and its no-collision padding run.
        tc.height = 0.0f;
        tr.emplace<TerrainComponent>(te, tc);

        TerrainComponent& t = tr.get<TerrainComponent>(te);
        terrain::EnsureHeights(t);
        const i32 gridN = static_cast<i32>(t.GridN());
        const f32 step = terrain::SampleStep(t);
        const f32 half = terrain::ExtentXZ(t) * 0.5f;

        // KNOWN SHAPE: a ramp descending along -X, meeting a flat plateau at x >= 0.
        // Not flat, so a collider that quietly collapsed to one plane is caught; and
        // the plateau gives the character somewhere to stand that is not a slope it
        // would slide off (which is a Jolt CharacterVirtual behaviour, not a collider
        // property, and would be testing the wrong thing).
        constexpr f32 kSlope = 0.25f;
        const auto shapeAt = [&](f32 lx) { return lx < 0.0f ? kSlope * lx : 0.0f; };
        for (i32 gz = 0; gz < gridN; ++gz) {
            for (i32 gx = 0; gx < gridN; ++gx) {
                t.heights[static_cast<usize>(gz) * gridN + gx] =
                    shapeAt(-half + static_cast<f32>(gx) * step);
            }
        }
        terrain::MarkColliderDirty(t, 0, 0, gridN - 1, gridN - 1);

        PhysicsWorld phys;
        phys.SetRunning(false); // EDIT mode: the collider must exist without Simulate
        phys.Update(tscene, 1.0f / 60.0f);

        // Downward probe at terrain-local (lx,lz); returns the world hit or nullopt.
        const auto probe = [&](f32 lx, f32 lz) -> std::optional<PhysicsWorld::RayHit> {
            const glm::vec3 ro(origin.x + lx, origin.y + shapeAt(lx) + 10.0f, origin.z + lz);
            const PhysicsWorld::RayHit h =
                phys.RaycastDetailed(ro, glm::vec3(0.0f, -1.0f, 0.0f), 40.0f);
            if (!h.hit) return std::nullopt;
            return h;
        };
        const auto expectedY = [&](f32 lx) { return origin.y + shapeAt(lx); };

        // 1. The collider exists, is exactly ONE body, and raycasts return the
        //    authored heights (and name the terrain entity, which is what surface
        //    painting and impact routing ask for).
        if (t.colliderBodyId == TerrainComponent::kInvalidCollider) {
            fail("no heightfield collider was created for the terrain.");
        } else if (phys.BodyCount() != 1) {
            fail(std::format("expected 1 body for a 4x4-chunk terrain, got {}.",
                             phys.BodyCount()));
        }
        u32 bodyAtStart = t.colliderBodyId;
        {
            const f32 xs[] = {0.0f, 8.0f, -8.0f, -14.0f, 15.5f};
            const f32 zs[] = {0.0f, -4.0f, 4.0f, -12.0f, 12.0f};
            for (int i = 0; i < 5; ++i) {
                const auto h = probe(xs[i], zs[i]);
                if (!h) {
                    fail(std::format("ray at local ({:.1f},{:.1f}) MISSED solid ground.", xs[i],
                                     zs[i]));
                    continue;
                }
                const f32 want = expectedY(xs[i]);
                if (std::abs(h->point.y - want) > 0.05f) {
                    fail(std::format("ray at local ({:.1f},{:.1f}) hit y={:.3f}, want {:.3f}.",
                                     xs[i], zs[i], h->point.y, want));
                }
                if (h->entity != te) fail("ground hit did not resolve to the terrain entity.");
                if (h->normal.y < 0.5f) {
                    fail(std::format("ground normal points the wrong way ({:.2f}).", h->normal.y));
                }
            }
        }
        HBE_INFO("TerrainCollideTest: {0}x{0} samples, {1:.2f} m spacing, {2:.0f} m across -> "
                 "1 static heightfield body; heights match the authored ramp.",
                 gridN, step, terrain::ExtentXZ(t));

        // 2. A painted HOLE is a hole in the collider - you fall through it - and it
        //    is LOCAL: ground a few metres away still catches the ray.
        terrain::PaintHole(t, -8.0f, -8.0f, 2.0f, /*erase=*/false);
        phys.Update(tscene, 1.0f / 60.0f);
        if (probe(-8.0f, -8.0f)) {
            fail("ray through a painted hole HIT - the hole is not in the collider.");
        }
        if (!probe(-8.0f + 4.0f, -8.0f)) {
            fail("a hole removed collision 4 m away from itself.");
        }
        if (t.colliderBodyId != bodyAtStart) {
            fail("punching a hole replaced the body instead of editing the shape.");
        }
        HBE_INFO("TerrainCollideTest: hole at local (-8,-8) r=2 -> ray MISSES there, still hits "
                 "4 m away, same body.");

        // 2b. A WRONG-SIZED hole mask must be ignored wholesale, not partially
        //     applied. This is not hypothetical: the shipped reference project carries
        //     641^2 heights against a 385^2 mask (nothing resizes the mask when the
        //     resolution changes), and the GPU upload path partial-fills it. Applying
        //     that to a collider would scatter holes at the wrong coordinates - i.e.
        //     invisible pits the player falls through.
        {
            const std::vector<u8> good = t.holeMask;
            t.holeMask.assign(good.size() / 3 + 7, 255); // stale + entirely "hole"
            terrain::MarkColliderDirty(t, 0, 0, gridN - 1, gridN - 1);
            phys.Update(tscene, 1.0f / 60.0f);
            if (terrain::HoleMaskUsable(t)) fail("a wrong-sized hole mask reported as usable.");
            int missed = 0;
            const f32 xs[] = {0.0f, -8.0f, 6.0f, -13.0f};
            const f32 zs[] = {0.0f, -8.0f, 6.0f, 11.0f};
            for (int i = 0; i < 4; ++i)
                if (!probe(xs[i], zs[i])) ++missed;
            if (missed != 0) {
                fail(std::format("a stale hole mask punched {} phantom hole(s) in the collider.",
                                 missed));
            } else {
                HBE_INFO("TerrainCollideTest: stale (wrong-sized) hole mask ignored -> ground "
                         "stays solid everywhere, including where the real hole was.");
            }
            t.holeMask = good; // restore the real mask
            terrain::MarkColliderDirty(t, 0, 0, gridN - 1, gridN - 1);
            phys.Update(tscene, 1.0f / 60.0f);
            if (probe(-8.0f, -8.0f)) fail("restoring the hole mask did not restore the hole.");
            // The two mask swaps above dirtied the WHOLE field, which is deliberately
            // answered with a fresh shape (in-place editing has no advantage at that
            // size). Re-baseline before the "no rebuild" assertions below, which are
            // about SCULPT STROKES - the interactive case that must not rebuild.
            bodyAtStart = t.colliderBodyId;
        }

        // 2c. TWISTED QUADS: the collider, the sampler and the renderer must share ONE
        //     surface. The ramp above is piecewise linear in x and constant in z, so its
        //     twist term (h00 + h11 - h01 - h10) is identically ZERO in every quad - which
        //     means it cannot see a triangulation mismatch at all, and one was live: the
        //     chunk builder split quads on the ANTI-diagonal while Jolt splits on the MAIN
        //     diagonal. The two surfaces agree at every sample and differ in the quad
        //     INTERIOR by half the twist, so the player walked up invisible bumps whose
        //     worst case is the full corner-height difference. A doubly-sinusoidal field
        //     has a non-zero twist in every quad and pins all four consumers together.
        {
            const std::vector<u8> savedMask = t.holeMask;
            t.holeMask.clear(); // holes are tested elsewhere; keep this phase about geometry
            const auto twist = [&](f32 lx, f32 lz) {
                return 2.0f * std::sin(lx * 0.7f) * std::sin(lz * 0.7f);
            };
            for (i32 gz = 0; gz < gridN; ++gz) {
                for (i32 gx = 0; gx < gridN; ++gx) {
                    t.heights[static_cast<usize>(gz) * gridN + gx] =
                        twist(-half + static_cast<f32>(gx) * step, -half + static_cast<f32>(gz) * step);
                }
            }
            terrain::MarkColliderDirty(t, 0, 0, gridN - 1, gridN - 1);
            phys.Update(tscene, 1.0f / 60.0f);

            // Probe QUAD CENTRES and quad interiors - the only places the two
            // triangulations disagree. Sample offsets deliberately straddle the diagonal
            // (0.25 is in one triangle, 0.75 in the other, 0.5 is on it).
            f32 worstColl = 0.0f, worstNav = 0.0f;
            int probes = 0, misses = 0;
            for (i32 gz = 2; gz < gridN - 3; gz += 5) {
                for (i32 gx = 2; gx < gridN - 3; gx += 5) {
                    for (const glm::vec2 off : {glm::vec2(0.5f, 0.5f), glm::vec2(0.25f, 0.75f),
                                                glm::vec2(0.75f, 0.25f)}) {
                        const f32 lx = -half + (static_cast<f32>(gx) + off.x) * step;
                        const f32 lz = -half + (static_cast<f32>(gz) + off.y) * step;
                        ++probes;
                        // What the SAMPLER says (this is what paint raycasts and what
                        // nav's analytic provider evaluate).
                        f32 sh = 0.0f, dhdx = 0.0f, dhdz = 0.0f;
                        bool hole = false;
                        if (!terrain::SampleSurface(t, lx, lz, sh, dhdx, dhdz, hole)) {
                            fail("SampleSurface rejected a point inside the footprint.");
                            continue;
                        }
                        // What the COLLIDER says.
                        const glm::vec3 ro(origin.x + lx, origin.y + 20.0f, origin.z + lz);
                        const PhysicsWorld::RayHit h =
                            phys.RaycastDetailed(ro, glm::vec3(0.0f, -1.0f, 0.0f), 60.0f);
                        if (!h.hit) { ++misses; continue; }
                        worstColl = std::max(worstColl, std::abs(h.point.y - (origin.y + sh)));
                        // And what SampleHeight says (the brush raycast's own path) - it
                        // must be the same function, not a bilinear third opinion.
                        worstNav = std::max(worstNav, std::abs(terrain::SampleHeight(t, lx, lz) - sh));
                    }
                }
            }
            if (misses != 0) {
                fail(std::format("{} of {} twisted-quad probes missed the collider.", misses,
                                 probes));
            }
            // 8-bit-per-block quantization over a 128 m height range is the floor here.
            if (worstColl > 0.02f) {
                fail(std::format("collider and terrain::SampleSurface disagree by {:.4f} m on a "
                                 "twisted quad ({} probes) - the triangulations do not match.",
                                 worstColl, probes));
            } else if (worstNav > 1e-4f) {
                fail(std::format("SampleHeight and SampleSurface disagree by {:.5f} m - the "
                                 "brush raycast is on a different surface than nav/physics.",
                                 worstNav));
            } else {
                HBE_INFO("TerrainCollideTest: twisted field (sin*sin), {} quad-interior probes -> "
                         "collider vs sampler within {:.4f} m, SampleHeight vs SampleSurface "
                         "within {:.5f} m. Render/collide/paint/nav share one surface.",
                         probes, worstColl, worstNav);
            }

            // Restore the ramp + mask for the sculpt phases below, which expect it.
            for (i32 gz = 0; gz < gridN; ++gz) {
                for (i32 gx = 0; gx < gridN; ++gx) {
                    t.heights[static_cast<usize>(gz) * gridN + gx] =
                        shapeAt(-half + static_cast<f32>(gx) * step);
                }
            }
            t.holeMask = savedMask;
            terrain::MarkColliderDirty(t, 0, 0, gridN - 1, gridN - 1);
            phys.Update(tscene, 1.0f / 60.0f);
            bodyAtStart = t.colliderBodyId; // full-field dirty rects rebuild by design
        }

        // 3. A SCULPT edit changes what subsequent raycasts return, in place. This is
        //    the bug the whole change exists for: the old per-chunk mesh collider was
        //    never rebuilt, so collision silently diverged from the visible ground.
        const auto beforeHit = probe(8.0f, 8.0f);
        const f32 beforeY = beforeHit ? beforeHit->point.y : 0.0f;
        constexpr f32 kRaise = 3.0f;
        terrain::SculptHeights(t, 8.0f, 8.0f, 3.0f, kRaise, terrain::Brush::Raise, 0.0f);
        phys.Update(tscene, 1.0f / 60.0f);
        {
            const glm::vec3 ro(origin.x + 8.0f, origin.y + shapeAt(8.0f) + 20.0f, origin.z + 8.0f);
            const PhysicsWorld::RayHit h =
                phys.RaycastDetailed(ro, glm::vec3(0.0f, -1.0f, 0.0f), 60.0f);
            const f32 want = expectedY(8.0f) + kRaise;
            if (!h.hit) {
                fail("ray missed sculpted ground entirely.");
            } else if (std::abs(h.point.y - want) > 0.05f) {
                fail(std::format("sculpt did not reach the collider: hit y={:.3f} (was {:.3f}), "
                                 "want {:.3f}.",
                                 h.point.y, beforeY, want));
            } else {
                HBE_INFO("TerrainCollideTest: sculpt +{:.1f} m at local (8,8) -> collider height "
                         "{:.3f} -> {:.3f} (want {:.3f}), no rebuild.",
                         kRaise, beforeY, h.point.y, want);
            }
        }
        if (t.colliderBodyId != bodyAtStart) {
            fail("a sculpt stroke replaced the body instead of editing the shape in place.");
        }

        // 4. LEAK CHECK. A brush stroke is dozens of edits per second; if each one
        //    made a shape or a body, an afternoon of sculpting would exhaust Jolt.
        for (int i = 0; i < 200; ++i) {
            const f32 lx = -6.0f + static_cast<f32>(i % 13);
            const f32 lz = -6.0f + static_cast<f32>((i / 13) % 13);
            terrain::SculptHeights(t, lx, lz, 1.5f, 0.01f, terrain::Brush::Raise, 0.0f);
            phys.Update(tscene, 1.0f / 60.0f);
        }
        if (phys.BodyCount() != 1) {
            fail(std::format("200 sculpt edits grew the world to {} bodies.", phys.BodyCount()));
        } else if (t.colliderBodyId != bodyAtStart) {
            fail("200 sculpt edits churned the body id (shape rebuilt, not edited).");
        } else {
            HBE_INFO("TerrainCollideTest: 200 sculpt edits -> still 1 body, same shape.");
        }

        // 4b. World SCALE only lives in the ScaledShape wrapper, so changing it must
        //     swap the wrapper and KEEP the heightfield. Otherwise dragging the scale
        //     gizmo rebuilds a 641x641 field every frame (measured ~12 ms a go).
        {
            tr.get<Transform>(te).scale = glm::vec3(1.0f, 2.0f, 1.0f);
            phys.Update(tscene, 1.0f / 60.0f);
            const f32 lx = -12.0f, lz = 5.0f;
            const glm::vec3 ro(origin.x + lx, origin.y + 10.0f, origin.z + lz);
            const PhysicsWorld::RayHit h =
                phys.RaycastDetailed(ro, glm::vec3(0.0f, -1.0f, 0.0f), 60.0f);
            const f32 want = origin.y + 2.0f * shapeAt(lx); // Y doubled, XZ untouched
            if (!h.hit) {
                fail("ray missed a Y-scaled terrain entirely.");
            } else if (std::abs(h.point.y - want) > 0.05f) {
                fail(std::format("Y-scaled terrain hit y={:.3f}, want {:.3f}.", h.point.y, want));
            } else if (t.colliderBodyId != bodyAtStart) {
                fail("a scale change rebuilt the heightfield instead of swapping the wrapper.");
            } else {
                HBE_INFO("TerrainCollideTest: scale (1,2,1) -> hit y={:.3f} (want {:.3f}), "
                         "wrapper swapped, heightfield kept.",
                         h.point.y, want);
            }
            tr.get<Transform>(te).scale = glm::vec3(1.0f); // back to 1:1 for the rest
            phys.Update(tscene, 1.0f / 60.0f);
        }

        // 5. A CharacterController STANDS on it. Dropped from 5 m over ground the
        //    sculpt loop above did not touch; the capsule centre must settle half a
        //    capsule above whatever the heightfield says is there.
        const entt::entity pe = tscene.CreateEntity("Player");
        CharacterController cc;
        const f32 spawnLx = 12.0f, spawnLz = -10.0f;
        Transform pt;
        pt.position = glm::vec3(origin.x + spawnLx, origin.y + shapeAt(spawnLx) + 5.0f,
                                origin.z + spawnLz);
        tr.emplace<Transform>(pe, pt);
        tr.emplace<CharacterController>(pe, cc);
        phys.SetRunning(true);
        for (int i = 0; i < 300; ++i) phys.Update(tscene, 1.0f / 60.0f); // 5 s
        {
            const CharacterController& pc = tr.get<CharacterController>(pe);
            const glm::vec3 p = tr.get<Transform>(pe).position;
            // Expected from where it ACTUALLY ended up: a 14-degree ramp lets it creep.
            const f32 groundY = origin.y + shapeAt(p.x - origin.x);
            const f32 want = groundY + cc.height * 0.5f;
            const f32 drift = glm::distance(glm::vec2(p.x, p.z),
                                            glm::vec2(pt.position.x, pt.position.z));
            if (!pc.grounded) {
                fail(std::format("CharacterController never landed (y={:.2f}, want {:.2f}).", p.y,
                                 want));
            } else if (std::abs(p.y - want) > 0.15f) {
                fail(std::format("CharacterController rests at y={:.3f}, want {:.3f}.", p.y, want));
            } else {
                HBE_INFO("TerrainCollideTest: CharacterController fell 5 m and stands at "
                         "y={:.3f} (ground {:.3f} + half capsule {:.2f}), drifted {:.2f} m.",
                         p.y, groundY, cc.height * 0.5f, drift);
            }
        }

        // 6. SURFACE PAINTING. The other half of the reported bug: the brush resolved
        //    its target mesh through MeshRef, terrain chunks are procedural and have
        //    none, so paint::RaycastMesh was never given any terrain geometry and the
        //    brush returned one line before the code that knew about terrain. It now
        //    goes through paint::RaycastTerrain, and this checks the three things that
        //    has to get right: the hit point (cross-checked against the INDEPENDENT
        //    Jolt heightfield raycast), the terrain-wide canvas UV, and that a dab at
        //    that UV actually lands pigment on the canvas.
        {
            // An oblique ray, as a camera would give: nothing here is axis-aligned, so
            // a hit that agreed only for straight-down rays would not pass.
            const glm::vec3 aimLocal(2.0f, 0.0f, 3.0f);
            const glm::vec3 ro = origin + glm::vec3(-11.0f, 26.0f, -19.0f);
            const glm::vec3 aimWorld =
                origin + glm::vec3(aimLocal.x, shapeAt(aimLocal.x), aimLocal.z);
            const glm::vec3 rd = glm::normalize(aimWorld - ro);

            paint::PaintHit hit;
            const glm::mat4 invWorld = glm::inverse(tscene.WorldMatrix(te));
            const bool got = paint::RaycastTerrain(
                t, glm::vec3(invWorld * glm::vec4(ro, 1.0f)),
                glm::vec3(invWorld * glm::vec4(rd, 0.0f)), hit);
            const PhysicsWorld::RayHit jolt = phys.RaycastDetailed(ro, rd, 200.0f);
            if (!got) {
                fail("the paint brush still cannot raycast terrain (RaycastTerrain missed).");
            } else if (!jolt.hit) {
                fail("physics missed the same terrain ray the brush hit.");
            } else {
                const glm::vec3 brushWorld = glm::vec3(tscene.WorldMatrix(te) *
                                                       glm::vec4(hit.localPos, 1.0f));
                const f32 disagree = glm::distance(brushWorld, jolt.point);
                if (disagree > 0.05f) {
                    fail(std::format("brush hit and heightfield hit disagree by {:.3f} m.",
                                     disagree));
                }
                if (jolt.entity != te) fail("terrain ray did not resolve to the terrain entity.");
                // Terrain-wide canvas UV: the WHOLE terrain maps to [0,1]^2 (that is
                // what makes one canvas cover every chunk seamlessly).
                const f32 extent = terrain::ExtentXZ(t);
                const glm::vec2 wantUv((hit.localPos.x + extent * 0.5f) / extent,
                                       (hit.localPos.z + extent * 0.5f) / extent);
                if (glm::distance(hit.uv, wantUv) > 1e-4f || hit.uv.x < 0.0f ||
                    hit.uv.x > 1.0f || hit.uv.y < 0.0f || hit.uv.y > 1.0f) {
                    fail(std::format("terrain canvas UV wrong: ({:.4f},{:.4f}).", hit.uv.x,
                                     hit.uv.y));
                }
                if (std::abs(hit.uvPerWorld - 1.0f / extent) > 1e-6f) {
                    fail("terrain UV density is not 1/extent (brush size would be wrong).");
                }
                if (hit.localNormal.y < 0.5f) fail("terrain paint normal points the wrong way.");

                // And the dab lands. This is the end of the chain the user reported as
                // broken - a stroke on the ground that leaves no paint.
                PaintComponent canvas;
                paint::EnsureCanvas(canvas, 256);
                paint::Dab dab;
                dab.color = glm::vec4(1.0f, 0.2f, 0.1f, 1.0f);
                dab.flow = 1.0f;
                const paint::BrushTip tip = paint::MakeBrushTip(paint::BrushDef{}, 64);
                usize before = 0, after = 0;
                for (const u8 c : canvas.layers[0].color) before += c;
                paint::Stamp(canvas, 0, hit.uv, 1.0f * hit.uvPerWorld, tip, 0.0f, dab);
                for (const u8 c : canvas.layers[0].color) after += c;
                if (after <= before) {
                    fail("a brush dab at the terrain hit UV left the canvas untouched.");
                } else {
                    HBE_INFO("TerrainCollideTest: paint ray on terrain -> hit agrees with the "
                             "heightfield to {:.4f} m, canvas UV ({:.3f},{:.3f}), dab landed.",
                             disagree, hit.uv.x, hit.uv.y);
                }
            }
        }

        // 7. Destroying the terrain destroys its body (no orphan collider left behind
        //    for the player to walk on after the ground is gone).
        tr.destroy(te);
        phys.Update(tscene, 1.0f / 60.0f);
        if (phys.BodyCount() != 0) {
            fail(std::format("terrain destroyed but {} body(ies) survive.", phys.BodyCount()));
        } else {
            HBE_INFO("TerrainCollideTest: terrain entity destroyed -> 0 bodies.");
        }

        // 7. COST, at the scale that matters. The reference project's terrain is
        //    16x16 chunks of 32 m at resolution 40 => 641x641 samples over 512 m, the
        //    same field that cost 819,200 collision triangles as chunk mesh colliders.
        //    Sculpting is interactive (a stroke is one edit per frame while the mouse
        //    is down), so the per-edit number is the one that decides whether this is
        //    usable, and it has to be measured rather than assumed.
        {
            using clock = std::chrono::steady_clock;
            Scene bscene;
            entt::registry& br = bscene.Registry();
            const entt::entity be = bscene.CreateEntity("BigTerrain");
            br.emplace<Transform>(be, Transform{});
            TerrainComponent big;
            big.chunks = 16;
            big.resolution = 40;
            big.chunkSize = 32.0f;
            big.height = 0.0f;
            br.emplace<TerrainComponent>(be, big);
            TerrainComponent& bt = br.get<TerrainComponent>(be);
            terrain::EnsureHeights(bt);
            terrain::MarkColliderDirty(bt, 0, 0, static_cast<i32>(bt.GridN()) - 1,
                                       static_cast<i32>(bt.GridN()) - 1);

            PhysicsWorld bphys;
            bphys.SetRunning(false);
            const auto t0 = clock::now();
            bphys.Update(bscene, 1.0f / 60.0f); // builds the shape from scratch
            const f64 buildMs = std::chrono::duration<f64, std::milli>(clock::now() - t0).count();

            // The editor's default brush: radius 5 m, strength 6 units/s at 60 fps.
            constexpr int kEdits = 200;
            f64 sculptMs = 0.0, pushMs = 0.0;
            for (int i = 0; i < kEdits; ++i) {
                const f32 lx = -120.0f + static_cast<f32>(i) * 1.2f;
                const f32 lz = -60.0f + static_cast<f32>(i % 40) * 3.0f;
                const auto s0 = clock::now();
                terrain::SculptHeights(bt, lx, lz, 5.0f, 0.1f, terrain::Brush::Raise, 0.0f);
                const auto s1 = clock::now();
                bphys.Update(bscene, 1.0f / 60.0f);
                const auto s2 = clock::now();
                sculptMs += std::chrono::duration<f64, std::milli>(s1 - s0).count();
                pushMs += std::chrono::duration<f64, std::milli>(s2 - s1).count();
            }
            if (bphys.BodyCount() != 1) {
                fail(std::format("reference-scale terrain ended with {} bodies.",
                                 bphys.BodyCount()));
            }

            // IDLE cost. SyncTerrainColliders is now on the per-frame spine, and the
            // engine has roughly 0.4 ms of CPU headroom in Release - so the cost when
            // NOTHING is being sculpted is the number that has to be small, and it has
            // to be measured rather than assumed.
            constexpr int kIdle = 2000;
            const auto i0 = clock::now();
            for (int i = 0; i < kIdle; ++i) bphys.Update(bscene, 1.0f / 60.0f);
            const f64 idleMs =
                std::chrono::duration<f64, std::milli>(clock::now() - i0).count() / kIdle;

            HBE_INFO("TerrainCollideTest COST ({0}x{0} samples / 512 m, r=5 m brush): initial "
                     "shape build {1:.2f} ms; per sculpt edit {2:.3f} ms heightmap + {3:.3f} ms "
                     "collider = {4:.3f} ms; idle frame (nothing dirty) {5:.4f} ms.",
                     bt.GridN(), buildMs, sculptMs / kEdits, pushMs / kEdits,
                     (sculptMs + pushMs) / kEdits, idleMs);
        }

        HBE_INFO("TerrainCollideTest: {}", code == 0 ? "ALL PASS" : "FAILED");
        jobs::Shutdown();
        return code;
    }

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

        // --- TERRAIN as a nav surface, and a SCULPT that costs no rebuild ---------
        // A terrain-ONLY world is a valid world (there is no static mesh here at all),
        // which is exactly what the "AI has no walkable surface" report was about.
        {
            Scene ts;
            entt::registry& tr = ts.Registry();
            const entt::entity te = ts.CreateEntity("Terrain");
            tr.emplace<Transform>(te, Transform{});
            TerrainComponent tc;
            tc.chunks = 8;
            tc.resolution = 8;   // GridN = 65, 64 m across
            tc.chunkSize = 8.0f;
            tc.height = 0.0f;    // flat, then sculpted below
            tr.emplace<TerrainComponent>(te, tc);
            terrain::EnsureHeights(tr.get<TerrainComponent>(te));

            nav::GridNav gn;
            const bool ready = gn.EnsureBuilt(ts, {});
            const glm::vec3 A(-26, 0, -26), B(26, 0, 26);
            const auto crosses = [&](const std::vector<glm::vec3>& p) {
                return !p.empty() && glm::distance(glm::vec2(p.back().x, p.back().z),
                                                   glm::vec2(B.x, B.z)) < 2.0f;
            };
            const auto path = gn.FindPath(A, B, {});
            if (ready && gn.TerrainCount() == 1 && gn.TriangleCount() == 0 && crosses(path)) {
                HBE_INFO("NavTerrainTest PASS: terrain-only world is walkable - A* crossed 64 m "
                         "of heightfield in {} corners with ZERO triangles stored (analytic).",
                         static_cast<u32>(path.size()));
            } else {
                HBE_ERROR("NavTerrainTest FAIL: ready={} terrains={} tris={} crossed={}.", ready,
                          gn.TerrainCount(), gn.TriangleCount(), crosses(path));
                code = 1;
            }

            // Off the terrain there must be NO ground: an edge-clamped heightmap sampled
            // past its border would invent an infinite skirt of walkable floor.
            if (gn.GroundAt(500.0f, 500.0f, 0.0f).has_value()) {
                HBE_ERROR("NavTerrainTest FAIL: found walkable ground 500 m outside the terrain.");
                code = 1;
            }

            // SCULPT MID-PATH: nav borrows `heights`, so a stroke must be visible
            // immediately and must NOT trigger a full rebuild.
            const u32 rebuildsBefore = gn.RebuildCount();
            const f32 flatY = gn.GroundAt(0.0f, 0.0f, 0.0f).value_or(-999.0f);
            terrain::SculptHeights(tr.get<TerrainComponent>(te), 0.0f, 0.0f, 6.0f, 4.0f,
                                   terrain::Brush::Raise, 0.0f);
            gn.EnsureBuilt(ts, {});
            const f32 raisedY = gn.GroundAt(0.0f, 0.0f, 4.0f).value_or(-999.0f);
            if (gn.RebuildCount() != rebuildsBefore) {
                HBE_ERROR("NavSculptTest FAIL: a sculpt stroke triggered {} full rebuild(s).",
                          gn.RebuildCount() - rebuildsBefore);
                code = 1;
            } else if (raisedY - flatY < 3.0f) {
                HBE_ERROR("NavSculptTest FAIL: sculpt did not reach navigation ({:.2f} -> {:.2f}).",
                          flatY, raisedY);
                code = 1;
            } else {
                HBE_INFO("NavSculptTest PASS: +4 m sculpt visible to nav same call ({:.2f} -> "
                         "{:.2f} m) with 0 rebuilds - the borrow works.",
                         flatY, raisedY);
            }

            // A painted HOLE is not walkable ground, for AI as it is for the player.
            terrain::PaintHole(tr.get<TerrainComponent>(te), -16.0f, -16.0f, 3.0f, /*erase=*/false);
            gn.EnsureBuilt(ts, {});
            if (gn.GroundAt(-16.0f, -16.0f, 0.0f).has_value()) {
                HBE_ERROR("NavHoleTest FAIL: a painted hole is still walkable ground for AI.");
                code = 1;
            } else if (!gn.GroundAt(-16.0f + 8.0f, -16.0f, 0.0f).has_value()) {
                HBE_ERROR("NavHoleTest FAIL: a hole removed walkable ground 8 m away.");
                code = 1;
            } else {
                HBE_INFO("NavHoleTest PASS: painted hole is a hole for AI too; ground 8 m away "
                         "still walkable.");
            }
        }

        // --- SLOPE LIMIT: a cliff is a wall, a gentle ramp is a road ---------------
        // maxSlopeDeg is the terrain half of "AI must not climb what a human could not".
        // NOTE ON PARAMS: at the DEFAULTS the STEP rule is stricter than the slope rule
        // and would mask it completely - maxStep 0.5 m over a cellSize 0.5 m cell is a
        // 45 deg ceiling, below maxSlopeDeg's 50 deg, so every slope this test could
        // reject would already have been rejected as a step. maxStep is raised to 2 m
        // here so the ONLY thing that can refuse the steep ramp is the slope test.
        {
            Scene ps;
            entt::registry& pr = ps.Registry();
            const entt::entity te = ps.CreateEntity("Ramp");
            pr.emplace<Transform>(te, Transform{});
            TerrainComponent tc;
            tc.chunks = 8;
            tc.resolution = 8; // GridN = 65, 64 m across, 1 m sample step
            tc.chunkSize = 8.0f;
            tc.height = 0.0f;
            pr.emplace<TerrainComponent>(te, tc);
            TerrainComponent& t = pr.get<TerrainComponent>(te);
            terrain::EnsureHeights(t);

            // Ground at y=0 for x<=0, then a ramp of the given gradient up to an 8 m
            // plateau. Authored directly (not sculpted) so the gradient under test is
            // exact rather than whatever a brush falloff happens to produce.
            const auto ramp = [&](f32 tanTheta) {
                const i32 n = static_cast<i32>(t.GridN());
                const f32 step = terrain::SampleStep(t);
                const f32 half = terrain::ExtentXZ(t) * 0.5f;
                for (i32 gz = 0; gz < n; ++gz)
                    for (i32 gx = 0; gx < n; ++gx) {
                        const f32 lx = -half + static_cast<f32>(gx) * step;
                        t.heights[static_cast<usize>(gz) * n + gx] =
                            lx <= 0.0f ? 0.0f : std::min(8.0f, lx * tanTheta);
                    }
            };

            nav::GridNav gn;
            nav::GridNavParams sp;   // cellSize 0.5, maxSlopeDeg 50
            sp.maxStep = 2.0f;       // see the note above
            gn.SetParams(sp);

            const glm::vec3 A(-20, 0, 0), B(28, 8, 0);
            const auto reaches = [&](const std::vector<glm::vec3>& p) {
                return !p.empty() && glm::distance(glm::vec2(p.back().x, p.back().z),
                                                   glm::vec2(B.x, B.z)) < 2.0f;
            };
            const auto maxCornerX = [](const std::vector<glm::vec3>& p) {
                f32 m = -1e9f;
                for (const glm::vec3& q : p) m = std::max(m, q.x);
                return m;
            };

            ramp(0.4663f); // 25 deg - inside maxSlopeDeg
            gn.EnsureBuilt(ps, {});
            const bool gentleGround = gn.GroundAt(8.0f, 0.0f, 3.7f).has_value();
            const bool climbed = reaches(gn.FindPath(A, B, {}));

            ramp(2.1445f); // 65 deg - a cliff
            gn.EnsureBuilt(ps, {});
            const bool steepGround = gn.GroundAt(2.0f, 0.0f, 4.0f).has_value();
            const std::vector<glm::vec3> refused = gn.FindPath(A, B, {});
            const f32 stoppedAt = maxCornerX(refused);

            if (gentleGround && climbed && !steepGround && !reaches(refused) && stoppedAt < 1.5f) {
                HBE_INFO("NavSlopeTest PASS: a 25 deg ramp is walkable ground and A* climbed it "
                         "to the plateau; the same ramp at 65 deg is not ground at all and the "
                         "path stopped at the foot of it (x={:.1f}).",
                         stoppedAt);
            } else {
                HBE_ERROR("NavSlopeTest FAIL: gentleGround={} climbed={} steepGround={} "
                          "steepReached={} stoppedAt={:.1f}.",
                          gentleGround, climbed, steepGround, reaches(refused), stoppedAt);
                code = 1;
            }
        }

        // --- A HOLE is routed AROUND, not just "no ground under one sample" -------
        // NavHoleTest proves GroundAt refuses a hole; this proves the PLANNER does the
        // useful thing with that - an agent crossing a corridor with a hole in it walks
        // around the hole instead of into it.
        {
            Scene hs;
            entt::registry& hr = hs.Registry();
            const entt::entity te = hs.CreateEntity("Terrain");
            hr.emplace<Transform>(te, Transform{});
            TerrainComponent tc;
            tc.chunks = 8;
            tc.resolution = 8;
            tc.chunkSize = 8.0f;
            tc.height = 0.0f; // flat, so the ONLY thing in the way is the hole
            hr.emplace<TerrainComponent>(te, tc);
            TerrainComponent& t = hr.get<TerrainComponent>(te);
            terrain::EnsureHeights(t);

            nav::GridNav gn;
            gn.EnsureBuilt(hs, {});
            const glm::vec3 A(-20, 0, 0), B(20, 0, 0); // straight line through the origin
            const auto offAxis = [](const std::vector<glm::vec3>& p) {
                f32 m = 0.0f;
                for (const glm::vec3& q : p) m = std::max(m, std::fabs(q.z));
                return m;
            };
            const auto nearestToHole = [](const std::vector<glm::vec3>& p) {
                f32 m = 1e9f;
                for (const glm::vec3& q : p)
                    m = std::min(m, glm::distance(glm::vec2(q.x, q.z), glm::vec2(0.0f)));
                return m;
            };
            const f32 offFlat = offAxis(gn.FindPath(A, B, {}));

            terrain::PaintHole(t, 0.0f, 0.0f, 4.0f, /*erase=*/false);
            gn.EnsureBuilt(hs, {});
            const std::vector<glm::vec3> around = gn.FindPath(A, B, {});
            const bool arrives = !around.empty() &&
                                 glm::distance(glm::vec2(around.back().x, around.back().z),
                                               glm::vec2(B.x, B.z)) < 2.0f;
            const f32 clearance = nearestToHole(around);
            if (offFlat < 2.0f && arrives && clearance > 3.0f) {
                HBE_INFO("NavHolePathTest PASS: flat terrain paths straight ({:.1f} m off axis); "
                         "an r=4 painted hole diverted the path {:.1f} m off axis and no corner "
                         "came closer than {:.1f} m to the hole.",
                         offFlat, offAxis(around), clearance);
            } else {
                HBE_ERROR("NavHolePathTest FAIL: offFlat={:.1f} arrives={} clearance={:.1f}.",
                          offFlat, arrives, clearance);
                code = 1;
            }
        }

        // --- AN AGENT ACTUALLY WALKS SCULPTED TERRAIN ------------------------------
        // Everything above queries the planner. This drives the real per-frame path -
        // nav::UpdateAgents - across ground that exists only because a BRUSH put it
        // there, and checks the agent rode the sculpted surface the whole way rather
        // than gliding at its old height.
        {
            Scene ws;
            entt::registry& wr = ws.Registry();
            const entt::entity te = ws.CreateEntity("Terrain");
            wr.emplace<Transform>(te, Transform{});
            TerrainComponent tc;
            tc.chunks = 8;
            tc.resolution = 8;
            tc.chunkSize = 8.0f;
            tc.height = 0.0f;
            wr.emplace<TerrainComponent>(te, tc);
            TerrainComponent& t = wr.get<TerrainComponent>(te);
            terrain::EnsureHeights(t);
            // One broad SCULPT stroke: a 4 m mound spanning almost the whole terrain, so
            // walking around it costs far more than walking over it (83 m of detour
            // against 58 m plus an 8 m climb penalty) and the agent has to climb. The
            // smoothstep falloff peaks at 1.5*amount/radius = 0.21 -> ~12 deg, well
            // inside both maxSlopeDeg and the step rule at the DEFAULT params.
            terrain::SculptHeights(t, 0.0f, 0.0f, 28.0f, 4.0f, terrain::Brush::Raise, 0.0f);

            nav::GridNav gn;
            gn.EnsureBuilt(ws, {});
            const glm::vec3 A(-29, 0, 0), B(29, 0, 0);
            const entt::entity ag = ws.CreateEntity("Walker");
            Transform at;
            at.position = glm::vec3(A.x, terrain::SampleHeight(t, A.x, A.z), A.z);
            wr.emplace<Transform>(ag, at);
            NavigationAgent na;
            na.target = B;
            na.hasTarget = true;
            na.speed = 5.0f;
            wr.emplace<NavigationAgent>(ag, na);

            f32 reachedAt = -1.0f, peakY = -1e9f, worstDrift = 0.0f;
            for (int i = 0; i < 2400; ++i) {
                nav::UpdateAgents(ws, gn, 1.0f / 60.0f);
                const glm::vec3 p = wr.get<Transform>(ag).position;
                peakY = std::max(peakY, p.y);
                // The sculpted surface under the agent's own feet, from the heightmap the
                // brush wrote - so this is "did it ride the ground the artist made", not
                // "did nav agree with itself".
                worstDrift = std::max(worstDrift,
                                      std::fabs(p.y - terrain::SampleHeight(t, p.x, p.z)));
                if (glm::distance(glm::vec2(p.x, p.z), glm::vec2(B.x, B.z)) < 1.0f) {
                    reachedAt = static_cast<f32>(i) / 60.0f;
                    break;
                }
            }
            if (reachedAt >= 0.0f && peakY > 2.0f && worstDrift < 0.1f) {
                HBE_INFO("NavAgentTerrainTest PASS: agent walked 58 m of SCULPTED terrain in "
                         "{:.1f}s, climbed to {:.2f} m over the mound, and stayed within {:.3f} m "
                         "of the brushed surface the whole way.",
                         reachedAt, peakY, worstDrift);
            } else {
                HBE_ERROR("NavAgentTerrainTest FAIL: reachedAt={:.1f} peakY={:.2f} drift={:.3f}.",
                          reachedAt, peakY, worstDrift);
                code = 1;
            }
        }

        // --- STREAMED shard geometry: enters and leaves the index, no rebuild ------
        // The reported gap: a streamed prop was invisible to AI, so agents walked
        // through spawned geometry. Also covers the CLEARANCE rule - a prop TALLER than
        // `climb` used to be ignored entirely (walls not walkable so skipped, roof out
        // of climb range), which left the headline bug only half fixed.
        {
            Scene ss;
            entt::registry& sr = ss.Registry();
            const entt::entity floor = ss.CreateEntity("Floor");
            Transform ft;
            ft.scale = glm::vec3(40.0f, 1.0f, 40.0f);
            sr.emplace<Transform>(floor, ft);
            sr.emplace<MeshInstance>(floor, MeshInstance{});
            sr.emplace<MeshRef>(floor, MeshRef{"prim:plane"});

            nav::GridNav gn;
            gn.EnsureBuilt(ss, {});
            const u32 baseRebuilds = gn.RebuildCount();
            const int baseTris = gn.TriangleCount();
            const glm::vec3 A(-14, 0, 0), B(14, 0, 0);
            // Straight line A->B passes through the origin; a wall there must divert it.
            const auto straightish = [&](const std::vector<glm::vec3>& p) {
                f32 maxOff = 0.0f;
                for (const glm::vec3& q : p) maxOff = std::max(maxOff, std::abs(q.z));
                return maxOff;
            };
            const f32 offClear = straightish(gn.FindPath(A, B, {}));

            // Spawn a shard holding a 3 m tall, 1 m thin wall across the path. 3 m is
            // deliberately taller than GridNavParams::climb (2 m).
            const entt::entity shard = ss.CreateEntity("StreamedWall");
            Transform wt;
            wt.position = glm::vec3(0.0f, 1.5f, 0.0f);
            wt.scale = glm::vec3(1.0f, 3.0f, 24.0f);
            sr.emplace<Transform>(shard, wt);
            sr.emplace<MeshInstance>(shard, MeshInstance{});
            sr.emplace<MeshRef>(shard, MeshRef{"prim:cube"});
            sr.emplace<StreamShard>(shard, StreamShard{7});
            gn.EnsureBuilt(ss, {});

            const bool indexed = gn.HasStreamedShard(7) && gn.StreamedBlockCount() == 1 &&
                                 gn.TriangleCount() > baseTris;
            const bool noRebuild = gn.RebuildCount() == baseRebuilds;
            const f32 offWall = straightish(gn.FindPath(A, B, {}));
            if (indexed && noRebuild && offWall > 6.0f && offClear < 2.0f) {
                HBE_INFO("NavShardTest PASS: streamed shard indexed incrementally ({} -> {} tris, "
                         "0 rebuilds); a 3 m wall (taller than climb) diverted the path {:.1f} m "
                         "off the straight line (was {:.1f} m).",
                         baseTris, gn.TriangleCount(), offWall, offClear);
            } else {
                HBE_ERROR("NavShardTest FAIL: indexed={} noRebuild={} offWall={:.1f} "
                          "offClear={:.1f} blocks={}.",
                          indexed, noRebuild, offWall, offClear, gn.StreamedBlockCount());
                code = 1;
            }

            // Despawn: the triangles must LEAVE the index, still with no full rebuild.
            sr.destroy(shard);
            gn.EnsureBuilt(ss, {});
            const f32 offGone = straightish(gn.FindPath(A, B, {}));
            if (!gn.HasStreamedShard(7) && gn.StreamedBlockCount() == 0 &&
                gn.TriangleCount() == baseTris && gn.RebuildCount() == baseRebuilds &&
                offGone < 2.0f) {
                HBE_INFO("NavShardTest PASS: despawn removed the block ({} tris again, 0 "
                         "rebuilds) and the path went straight through ({:.1f} m off).",
                         gn.TriangleCount(), offGone);
            } else {
                HBE_ERROR("NavShardTest FAIL (despawn): blocks={} tris={} rebuilds={} "
                          "offGone={:.1f}.",
                          gn.StreamedBlockCount(), gn.TriangleCount(),
                          gn.RebuildCount() - baseRebuilds, offGone);
                code = 1;
            }
        }

        // --- A MOVED static mesh must invalidate the index -------------------------
        // With only entity ids in the fingerprint, dragging a wall with the gizmo left
        // nav holding its OLD triangles forever.
        {
            Scene ms;
            entt::registry& mr = ms.Registry();
            const entt::entity floor = ms.CreateEntity("Floor");
            Transform ft;
            ft.scale = glm::vec3(40.0f, 1.0f, 40.0f);
            mr.emplace<Transform>(floor, ft);
            mr.emplace<MeshInstance>(floor, MeshInstance{});
            mr.emplace<MeshRef>(floor, MeshRef{"prim:plane"});
            const entt::entity wall = ms.CreateEntity("Wall");
            Transform wt;
            wt.position = glm::vec3(0.0f, 1.5f, 0.0f);
            wt.scale = glm::vec3(1.0f, 3.0f, 24.0f);
            mr.emplace<Transform>(wall, wt);
            mr.emplace<MeshInstance>(wall, MeshInstance{});
            mr.emplace<MeshRef>(wall, MeshRef{"prim:cube"});

            nav::GridNav gn;
            gn.EnsureBuilt(ms, {});
            const auto offOf = [&](const std::vector<glm::vec3>& p) {
                f32 m = 0.0f;
                for (const glm::vec3& q : p) m = std::max(m, std::abs(q.z));
                return m;
            };
            const f32 blocked = offOf(gn.FindPath({-14, 0, 0}, {14, 0, 0}, {}));
            const u32 before = gn.RebuildCount();
            mr.get<Transform>(wall).position = glm::vec3(0.0f, 1.5f, 200.0f); // gizmo drag
            // A PLACEMENT change is caught by the amortized heavy fingerprint tier, so the
            // contract is "within kHeavyFingerprintPeriod frames", not "next frame" - the
            // per-frame cost of checking every static mesh's world matrix was 0.216 ms,
            // more than half the frame's whole CPU headroom. Assert the real window: it
            // must happen, and it must happen inside that many EnsureBuilt calls.
            int framesToNotice = -1;
            for (int i = 1; i <= 16; ++i) {
                gn.EnsureBuilt(ms, {});
                if (gn.RebuildCount() > before) { framesToNotice = i; break; }
            }
            const f32 moved = offOf(gn.FindPath({-14, 0, 0}, {14, 0, 0}, {}));
            if (blocked > 6.0f && moved < 2.0f && framesToNotice > 0 && framesToNotice <= 8) {
                HBE_INFO("NavStaleTest PASS: moving a static wall rebuilt the index after {} "
                         "frame(s) (path went {:.1f} m off -> {:.1f} m off).",
                         framesToNotice, blocked, moved);
            } else {
                HBE_ERROR("NavStaleTest FAIL: blocked={:.1f} moved={:.1f} framesToNotice={}.",
                          blocked, moved, framesToNotice);
                code = 1;
            }
        }

        // --- An UNREACHABLE target must not cost a query every frame ---------------
        // The pathology: `ag.path.empty()` was tested first and unconditionally, so an
        // agent whose goal is walled off re-ran a full maxExpand query at frame rate
        // (~4.7 ms/frame, against ~0.4 ms of headroom) forever.
        {
            Scene us;
            entt::registry& ur = us.Registry();
            const entt::entity floor = us.CreateEntity("Floor");
            Transform ft;
            ft.scale = glm::vec3(20.0f, 1.0f, 20.0f);
            ur.emplace<Transform>(floor, ft);
            ur.emplace<MeshInstance>(floor, MeshInstance{});
            ur.emplace<MeshRef>(floor, MeshRef{"prim:plane"});
            nav::GridNav gn;
            gn.EnsureBuilt(us, {});
            const entt::entity ag = us.CreateEntity("StuckAgent");
            Transform at;
            at.position = glm::vec3(0.0f, 0.0f, 0.0f);
            ur.emplace<Transform>(ag, at);
            NavigationAgent na;
            na.target = glm::vec3(4000.0f, 0.0f, 4000.0f); // far off the mesh: unreachable
            na.hasTarget = true;
            ur.emplace<NavigationAgent>(ag, na);

            u32 queries = 0;
            constexpr int kFrames = 240; // 4 s at 60 Hz
            for (int i = 0; i < kFrames; ++i) {
                nav::UpdateAgents(us, gn, 1.0f / 60.0f);
                queries += gn.LastQueryCount();
            }
            // At ~3 Hz the honest ceiling over 4 s is ~13; anything near kFrames means
            // the empty-path branch is still bypassing the cooldown.
            if (queries <= 20) {
                HBE_INFO("NavBudgetTest PASS: an agent with an unreachable target issued {} A* "
                         "queries in {} frames (~{:.1f} Hz), not one per frame.",
                         queries, kFrames, queries * 60.0f / kFrames);
            } else {
                HBE_ERROR("NavBudgetTest FAIL: {} queries in {} frames - the re-plan cooldown is "
                          "being bypassed.",
                          queries, kFrames);
                code = 1;
            }
        }

        jobs::Shutdown();
        return code;
    }

    // Navigation COST harness. Reports the per-frame nav band and the per-query cost, and
    // where two implementations of one hot path both still exist in this binary it times
    // BOTH - so the before/after is a measurement in one process rather than a claim
    // about a build that is no longer here.
    if (config.navBench) {
        using bclock = std::chrono::high_resolution_clock;
        const auto msOf = [](bclock::duration d) {
            return std::chrono::duration<f64, std::milli>(d).count();
        };

        // A world shaped like the reference project's problem case: a big terrain (the
        // walkable floor), a few thousand static meshes, and a couple of thousand
        // entities owned by stream shards.
        Scene bs;
        entt::registry& br = bs.Registry();
        const entt::entity te = bs.CreateEntity("Terrain");
        br.emplace<Transform>(te, Transform{});
        TerrainComponent btc;
        btc.chunks = 16;
        btc.resolution = 40; // GridN = 641, matching the reference project
        btc.chunkSize = 32.0f;
        btc.height = 6.0f;
        br.emplace<TerrainComponent>(te, btc);
        terrain::EnsureHeights(br.get<TerrainComponent>(te));

        constexpr int kStatic = 2000, kStreamed = 2000, kShards = 40;
        for (int i = 0; i < kStatic; ++i) {
            const entt::entity e = bs.CreateEntity("S" + std::to_string(i));
            Transform t;
            t.position = glm::vec3(static_cast<f32>((i % 50) * 8 - 200), 0.5f,
                                   static_cast<f32>((i / 50) * 8 - 160));
            br.emplace<Transform>(e, t);
            br.emplace<MeshInstance>(e, MeshInstance{});
            br.emplace<MeshRef>(e, MeshRef{"prim:cube"});
        }
        for (int i = 0; i < kStreamed; ++i) {
            const entt::entity e = bs.CreateEntity("D" + std::to_string(i));
            Transform t;
            t.position = glm::vec3(static_cast<f32>((i % 50) * 8 - 196), 0.5f,
                                   static_cast<f32>((i / 50) * 8 - 156));
            br.emplace<Transform>(e, t);
            br.emplace<MeshInstance>(e, MeshInstance{});
            br.emplace<MeshRef>(e, MeshRef{"prim:cube"});
            br.emplace<StreamShard>(e, StreamShard{static_cast<u32>(i % kShards)});
        }

        nav::GridNav gn;
        const auto tBuild = bclock::now();
        gn.EnsureBuilt(bs, {});
        const f64 firstBuildMs = msOf(bclock::now() - tBuild);
        HBE_INFO("NavBench: world = 641^2 terrain (512 m) + {} static + {} streamed meshes in {} "
                 "shards -> {} tris, {} terrain surface(s), {} block(s). First build {:.2f} ms.",
                 kStatic, kStreamed, kShards, gn.TriangleCount(), gn.TerrainCount(),
                 gn.StreamedBlockCount(), firstBuildMs);

        // 1. Steady-state per-frame cost: EnsureBuilt with nothing changed. Split into the
        //    cheap streamed gate (what runs now) and the full per-shard content
        //    fingerprint (what ran every frame before).
        constexpr int kFrames = 400;
        auto t0 = bclock::now();
        for (int i = 0; i < kFrames; ++i) gn.EnsureBuilt(bs, {});
        const f64 steadyMs = msOf(bclock::now() - t0) / kFrames;

        t0 = bclock::now();
        for (int i = 0; i < kFrames; ++i) gn.BenchStreamPoll(bs, {}, /*forceFull=*/true);
        const f64 fullPollMs = msOf(bclock::now() - t0) / kFrames;
        t0 = bclock::now();
        for (int i = 0; i < kFrames; ++i) gn.BenchStreamPoll(bs, {}, /*forceFull=*/false);
        const f64 gatePollMs = msOf(bclock::now() - t0) / kFrames;

        // 2. A terrain SCULPT must not cost a rebuild.
        const u32 rb0 = gn.RebuildCount();
        t0 = bclock::now();
        for (int i = 0; i < 100; ++i) {
            terrain::SculptHeights(br.get<TerrainComponent>(te), static_cast<f32>(i % 40) - 20.0f,
                                   0.0f, 4.0f, 0.05f, terrain::Brush::Raise, 0.0f);
            gn.EnsureBuilt(bs, {});
        }
        const f64 sculptFrameMs = msOf(bclock::now() - t0) / 100.0;
        const u32 sculptRebuilds = gn.RebuildCount() - rb0;

        // 3. Query cost, and the obstacle-index win. 50 obstacles, like the budget note.
        std::vector<nav::GridObstacle> obs;
        for (int i = 0; i < 50; ++i)
            obs.push_back({glm::vec3(static_cast<f32>(i % 10) * 6.0f - 30.0f, 0.0f,
                                     static_cast<f32>(i / 10) * 6.0f - 15.0f),
                           2.0f});
        // Start and goal must sit ON the ground: this terrain undulates +/-6 m, and a
        // start point 4 m above the surface is farther than `climb` from every walkable
        // cell, so the query would (correctly) find nothing and time nothing.
        // ...and they must be in the OPEN: the prop grid is on 8 m centres, so a point
        // that lands on one puts the agent inside a cube, where the clearance test
        // legitimately rejects its whole neighbourhood. +4 m sits mid-gap.
        const TerrainComponent& btr = br.get<TerrainComponent>(te);
        const glm::vec3 QA(-124.0f, terrain::SampleHeight(btr, -124.0f, -124.0f), -124.0f);
        const glm::vec3 QB(108.0f, terrain::SampleHeight(btr, 108.0f, 100.0f), 100.0f);
        t0 = bclock::now();
        int corners = 0;
        constexpr int kQueries = 20;
        for (int i = 0; i < kQueries; ++i) corners = static_cast<int>(gn.FindPath(QA, QB, obs).size());
        const f64 queryMs = msOf(bclock::now() - t0) / kQueries;

        // Ground sample with and without the clearance test, so the cost of making
        // non-walkable geometry block (the tall-prop fix) is a number and not a guess.
        {
            constexpr int kG = 200000;
            volatile f32 acc = 0.0f;
            auto tg = bclock::now();
            for (int i = 0; i < kG; ++i) {
                const f32 x = static_cast<f32>(i % 600) * 0.5f - 150.0f;
                const f32 z = static_cast<f32>((i / 600) % 600) * 0.5f - 150.0f;
                acc += gn.BenchGround(x, z, 0.0f, false).value_or(0.0f);
            }
            const f64 plainNs = msOf(bclock::now() - tg) * 1e6 / kG;
            tg = bclock::now();
            for (int i = 0; i < kG; ++i) {
                const f32 x = static_cast<f32>(i % 600) * 0.5f - 150.0f;
                const f32 z = static_cast<f32>((i / 600) % 600) * 0.5f - 150.0f;
                acc += gn.BenchGround(x, z, 0.0f, true).value_or(0.0f);
            }
            const f64 clearNs = msOf(bclock::now() - tg) * 1e6 / kG;
            HBE_INFO("NavBench: ground sample {:.1f} ns (no clearance) vs {:.1f} ns (with "
                     "clearance - the tall-prop blocker test).",
                     plainNs, clearNs);
        }

        // Obstacle test in isolation: the grid vs the linear scan it replaced.
        constexpr int kSamples = 200000;
        volatile int sink = 0;
        t0 = bclock::now();
        for (int i = 0; i < kSamples; ++i) {
            const f32 x = static_cast<f32>(i % 400) * 0.5f - 100.0f;
            const f32 z = static_cast<f32>((i / 400) % 400) * 0.5f - 100.0f;
            if (nav::GridNav::BenchLinearBlocked(obs, 0.4f, x, z)) ++sink;
        }
        const f64 linearNs = msOf(bclock::now() - t0) * 1e6 / kSamples;
        nav::GridNav::ObstacleGrid probe;
        probe.Build(obs, 0.4f);
        t0 = bclock::now();
        for (int i = 0; i < kSamples; ++i) {
            const f32 x = static_cast<f32>(i % 400) * 0.5f - 100.0f;
            const f32 z = static_cast<f32>((i / 400) % 400) * 0.5f - 100.0f;
            if (probe.Blocked(x, z)) ++sink;
        }
        const f64 gridNs = msOf(bclock::now() - t0) * 1e6 / kSamples;

        // 4. A stuck agent: the pathology was one full query PER FRAME, forever.
        const entt::entity ag = bs.CreateEntity("Stuck");
        Transform at;
        at.position = glm::vec3(0.0f, terrain::SampleHeight(btr, 0.0f, 0.0f), 0.0f);
        br.emplace<Transform>(ag, at);
        NavigationAgent na;
        na.target = glm::vec3(9000.0f, 0.0f, 9000.0f); // unreachable
        na.hasTarget = true;
        br.emplace<NavigationAgent>(ag, na);
        u32 stuckQueries = 0;
        t0 = bclock::now();
        for (int i = 0; i < 300; ++i) {
            gn.EnsureBuilt(bs, {});
            nav::UpdateAgents(bs, gn, 1.0f / 60.0f);
            stuckQueries += gn.LastQueryCount();
        }
        const f64 stuckFrameMs = msOf(bclock::now() - t0) / 300.0;

        HBE_INFO("NavBench RESULTS (Release, {} static tris, {} streamed entities, 50 obstacles):",
                 gn.TriangleCount(), kStreamed);
        HBE_INFO("  steady-state EnsureBuilt (nothing changed) : {:.4f} ms/frame", steadyMs);
        HBE_INFO("  streamed poll, FULL fingerprint (was)      : {:.4f} ms/frame", fullPollMs);
        HBE_INFO("  streamed poll, cheap gate (is)             : {:.4f} ms/frame", gatePollMs);
        HBE_INFO("  terrain sculpt + EnsureBuilt               : {:.4f} ms/frame, {} rebuild(s)",
                 sculptFrameMs, sculptRebuilds);
        HBE_INFO("  one ~330 m A* query ({} corners)            : {:.3f} ms", corners, queryMs);
        HBE_INFO("  obstacle test, linear scan (was)           : {:.2f} ns/sample", linearNs);
        HBE_INFO("  obstacle test, XZ grid (is)                : {:.2f} ns/sample", gridNs);
        HBE_INFO("  stuck agent, whole nav band                : {:.4f} ms/frame ({} queries in "
                 "300 frames)",
                 stuckFrameMs, stuckQueries);
        jobs::Shutdown();
        return 0;
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
        // Auto-fallback is D3D12/Vulkan only - see ResolveBackendOrder for why
        // OpenGL is never appended to a chain the user did not ask for.
        for (A a : {A::D3D12, A::Vulkan})
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

    // The pathfinder lives for the whole run; the pointer lets the editor inspect it.
    nav::GridNav gridNav;
    gridNav_ = &gridNav; // real-time A* the agents path on (auto-builds, no bake)

    bool sceneBuilt = false;

    // Studio boot splash FIRST - before the heavy environment bake + scene loads -
    // so the window shows the splash (not a black screen) while the engine warms
    // up. Load only the studio UI scene, present one frame, then do the work behind
    // it (the presented frame stays on screen through each blocking step). Runtime
    // only (the editor uses its own play mode, onInit_ set). The {log} token shows
    // the latest line as each step completes.
    bool studioSplash = false;
    if (!onInit_ && Project::HasActive() &&
        !Project::Active().Settings().bootDocument.empty()) {
        const std::string& rel = Project::Active().Settings().bootDocument;
        const std::filesystem::path studio = Project::Active().AssetsDir() / rel;
        // LEGACY BRANCH, decided by EXTENSION at load. A `.hbui` opens as a
        // document (spared by every Replace sweep, so FlowAfterBoot closes it
        // explicitly); a `.hbscene` runs the old Replace load, which is disposed
        // implicitly by FlowMainMenu's non-Persistent sweep exactly as before.
        const bool isDoc = std::filesystem::path(rel).extension() == ".hbui";
        bool loaded = false;
        if (vfs::Exists(studio)) {
            if (isDoc) {
                bootDoc_ = docs_.Open(scene, &renderer, studio, /*screenOwned*/ true);
                loaded = bootDoc_ != 0;
            } else {
                loaded = scene::LoadScene(scene, renderer, studio);
            }
        }
        if (loaded) {
            flowActive_ = true;
            gameState_ = GameState::Booting;
            sceneBuilt = true;
            studioSplash = true;
            HBE_INFO("Boot: studio splash shown ({}); warming up...",
                     isDoc ? "document" : "legacy scene");
            PresentBootSplash(0.05f); // visible immediately, before the IBL bake
        } else {
            HBE_WARN("Boot document '{}' failed to load; skipping splash.", rel);
        }
    }

    // Lighting environment (IBL + sky) applies to every scene, loaded or demo. The
    // studio splash (above) stays on screen through this ~second-long bake.
    if (studioSplash) HBE_INFO("Boot: building lighting environment...");
    scene::SetupEnvironment(scene, renderer);
    if (studioSplash) PresentBootSplash(0.5f); // refresh: IBL done, log advanced

    // 3D MAIN MENU: bind the world BEHIND THE SPLASH, not when the menu appears.
    // The splash is already covering a ~second of IBL bake, so the menu set loads
    // inside time the player is spending anyway and is STANDING when the splash
    // lifts. Binding later would show an empty menu that pops its geometry in a
    // frame or two afterwards. Safe here: BindMenuWorld is a no-op unless the
    // project asks for it, and it falls back to the flat menu rather than a black
    // screen if the scene will not bind.
    BindMenuWorld();
    if (studioSplash && menuWorldBound_) PresentBootSplash(0.75f); // world is up

    // THE resident UI layer: open the `.hbui` document holding ALL screens
    // (UIPanel subtrees) ONCE and keep it resident across gameplay scene swaps.
    // Residency is structural now - both Replace sweeps spare
    // UIDocMember::screenOwned - so there is no Persistent-stamping loop here any
    // more. The UIManager binds to the document and shows/hides its panels. This
    // is THE game-UI path; without it the runtime boots straight into the startup
    // scene below.
    if (!onInit_ && Project::HasActive() &&
        !Project::Active().Settings().uiDocuments.empty()) {
        // ONE DOCUMENT PER SCREEN, ALL RESIDENT. Opened here, in project order,
        // and never closed - showing a screen later is a bool write, so there is
        // no on-demand load to hitch, pop in or flash unstyled. Every open passes
        // a real Renderer* and preload = true, which is the contract that makes
        // that claim true (InstantiateDocument -> ui::PreloadUIAssets bakes the
        // font atlas and resolves every texture before the first frame).
        const std::vector<std::string>& rels = Project::Active().Settings().uiDocuments;
        u32 count = 0;
        u32 failures = 0;
        std::string names;
        for (const std::string& rel : rels) {
            if (rel.empty()) continue;
            const std::filesystem::path uip = Project::Active().AssetsDir() / rel;
            const bool isDoc = std::filesystem::path(rel).extension() == ".hbui";
            ui::DocHandle h = 0;
            u32 here = 0;
            bool legacy = false;
            if (vfs::Exists(uip)) {
                if (isDoc) {
                    h = docs_.Open(scene, &renderer, uip, /*screenOwned*/ true,
                                   /*preload*/ true);
                    if (h != 0) {
                        const ui::DocumentInstance* inst = docs_.Get(h);
                        here = inst ? static_cast<u32>(inst->entities.size()) : 0;
                    }
                } else {
                    // LEGACY BRANCH - a `.hbproj` that still points at the
                    // pre-document UI `.hbscene`. Load it exactly as before, then
                    // ADOPT the result as a document so UIManager::Bind, the sweep
                    // predicates and the snapshot skips all behave identically to a
                    // migrated project. Without this branch a half-migrated project
                    // boots with uiManagerMode_ false and NO MENU AT ALL.
                    scene::SceneData uiData;
                    if (scene::ParseSceneFile(uip, uiData)) {
                        scene::StagedAssets uiStaged;
                        scene::StageAssets(uiData, Project::Active().AssetsDir(), uiStaged);
                        std::vector<entt::entity> uiEnts;
                        scene::Instantiate(scene, renderer, uiData, uiStaged,
                                           scene::LoadMode::Additive, &uiEnts, "__ui");
                        ui::DocData header;
                        header.post = uiData.post;
                        header.ambientIntensity = uiData.ambientIntensity;
                        header.exposure = uiData.exposure;
                        h = docs_.AdoptLegacy(scene, uiEnts, uip, header,
                                              /*screenOwned*/ true);
                        here = static_cast<u32>(uiEnts.size());
                        legacy = true;
                    }
                }
            }
            if (h == 0) {
                HBE_WARN("UI screen document '{}' failed to load.", rel);
                ++failures;
                continue;
            }
            // The MENU document is the FIRST one that opens, and its MANDATORY
            // post block is the menu look. Four screens cannot each supply one;
            // the rest of their headers' `post` is ignored, by design.
            if (uiDocs_.empty()) {
                // AdoptLegacy stores the scene's post into the same header slot,
                // so one read covers both branches.
                if (const ui::DocData* d = docs_.Header(h)) {
                    uiScenePost_ = d->post;
                    uiScenePostValid_ = true;
                }
            }
            uiDocs_.push_back(h);
            count += here;
            if (!names.empty()) names += ", ";
            names += rel;
            if (legacy) names += " (legacy scene)";
        }
        if (!uiDocs_.empty()) {
            uiManager_.Bind(uiDocs_);
            uiManager_.Init(scene);
            uiManagerMode_ = true;
            // NO INITIAL SCREEN = A DEAD BLACK BOOT, and it is one renamed file
            // away. Only MainMenu.hbui carries startVisible; if that ONE entry
            // goes stale (a rename, a moved file, the "+ Add screen" row's empty
            // slot) the other three still open, uiManagerMode_ still latches, and
            // FlowMainMenu shows NOTHING while parking the game in MainMenu with a
            // free cursor - a black screen with zero interactive elements and no
            // way out. Pre-split this was loud and recoverable (the single
            // document failed, uiDocs_ stayed empty, FlowAfterBoot fell through to
            // FlowPlay). Restore that property: adopt the first reachable panel,
            // and if even that is impossible, hand the boot back to FlowPlay.
            if (uiManager_.Initial().empty()) {
                const std::string first = uiManager_.FirstPanelName();
                if (!first.empty()) {
                    HBE_ERROR("Boot: no screen document declares a startVisible panel "
                              "(is the main menu screen missing from the project?); "
                              "falling back to the first reachable screen '{}'.",
                              first);
                    uiManager_.SetInitial(first);
                } else {
                    HBE_ERROR("Boot: the resident screen set contains NO named panel; "
                              "booting straight into the game instead of an empty menu.");
                    uiManagerMode_ = false;
                    flowActive_ = false;
                    sceneBuilt = false;
                }
            }
        }
        if (!uiDocs_.empty() && uiManagerMode_) {
            AuditScreenActions(scene);
            flowActive_ = true;
            sceneBuilt = true; // UI-only overlay; the gameplay world loads on Play
            if (!studioSplash) { // no splash: go straight to the initial (menu) panel
                // Already bound above, behind the splash - this only runs when there
                // was no splash to hide it behind. Re-binding would UnloadAll and
                // rebuild the same world for nothing.
                if (!menuWorldBound_) BindMenuWorld();
                uiManager_.ShowInitial(scene);
                gameState_ = GameState::MainMenu;
                ApplyMenuPost(); // no-op while the backdrop owns the look
            }
            HBE_INFO("Boot: {} resident UI screen document(s) loaded ({} entities): {}.",
                     uiDocs_.size(), count, names);
            // A MISSING SCREEN IS A SHIPPING BLOCKER, not a warning: every flow verb
            // that targets it becomes a dead button in the shipped build.
            if (failures > 0)
                HBE_ERROR("Boot: {} UI screen document(s) FAILED to load; those screens "
                          "are missing and every button that opens one is dead.",
                          failures);
        } else if (uiDocs_.empty()) {
            HBE_WARN("No UI screen document loaded ({} configured).", rels.size());
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
        // Enqueue captions when EITHER category is on; subtitle::Stack does the
        // real per-kind gating (a Voiceline .uaf is speech, so it must reach the
        // stack when only Subtitles is enabled).
        audio.SetCaptionsEnabled(userSettings_.subtitlesEnabled ||
                                 userSettings_.captionsEnabled);
        SyncActionMap(); // action defaults (project) + rebind overrides (user settings)
    }

    // Build the scene (skipped when the studio splash or UI scene already owns the
    // world): startup scene / model / default world.
    if (!sceneBuilt && Project::HasActive() && !Project::Active().Settings().startupScene.empty()) {
        const std::filesystem::path startup =
            Project::Active().AssetsDir() / Project::Active().Settings().startupScene;
        // A level is ONE .hbscene: no layer files, no two-file loader.
        if (vfs::Exists(startup)) // pack-aware (shipped builds have no loose files)
            sceneBuilt = scene::LoadScene(scene, renderer, startup);
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
    if (config.stressParticles > 0) {
        scene::SpawnParticleStress(scene, config.stressParticles, config.gpuParticles,
                                   config.gpuSimParticles);
    }

    if (config.forceDof) scene.Environment().post.dofEnabled = 1;
    if (config.forceMotionBlur) scene.Environment().post.motionBlurEnabled = 1;
    if (config.forceSsr) scene.Environment().post.ssrEnabled = 1;
    if (config.forceAutoExposure) scene.Environment().post.autoExposureEnabled = 1;
    if (config.forcePainterly) {
        rhi::PostSettings& p = scene.Environment().post;
        p.painterlyEnabled = 1;
        if (config.forcePainterlyRadius >= 0.0f) p.painterlyRadius = config.forcePainterlyRadius;
        HBE_INFO("Painterly forced ON (stroke size {:.1f}, strokes {}).", p.painterlyRadius,
                 p.painterlyStrokes ? "on" : "off");
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

    // --fixed-dt: pin the simulation clock (see EngineConfig::fixedDt). After onInit_
    // so a test harness that sets its own fixed dt still wins.
    if (config.fixedDt > 0.0f && renderFixedDt_ <= 0.0f) renderFixedDt_ = config.fixedDt;

    using clock = std::chrono::high_resolution_clock;
    auto last = clock::now();
    auto fpsLast = last;
    u32 fpsFrames = 0;
    // Per-window CPU phase timers (ms accumulated; averaged + reset each Perf report).
    f64 accGpMs = 0.0, accFacialMs = 0.0, accRenderMs = 0.0;
    // Particle VERTEX EXPANSION, timed on its own. It is the phase
    // ParticleEmitter::gpuExpand moves off the CPU, so folding it into any other
    // bucket would make the one change this timer exists to measure invisible.
    f64 accVfxBuildMs = 0.0;
    // Particle SIMULATION (particle::Update: spawn scheduling + the module kernels),
    // timed separately from expansion for the same reason expansion is timed
    // separately from everything else. This is the phase ParticleEmitter::gpuSim
    // moves into a compute shader; without its own bucket the one change it exists to
    // measure would only ever show up as a diffuse frame-time difference.
    f64 accVfxSimMs = 0.0;
    // Interaction pick (ray + ONE physics occlusion cast + the page loop).
    f64 accPickMs = 0.0;
    u64 accPickCasts = 0;
    // TAG STREAMING, on its own. It is the one phase whose whole design claim is "this
    // costs almost nothing per frame", so it needs its own number rather than being
    // hidden inside a bucket that already measures milliseconds.
    f64 accStreamMs = 0.0;
    // NAVIGATION, on its own, for the same reason: its acceptance criterion is a
    // per-frame millisecond budget, and its cost scales with agents and with how many
    // A* queries the frame budget let through - neither of which is visible in any
    // other bucket. `accNavQueries` is reported alongside so a suspicious millisecond
    // count can be attributed to query volume rather than guessed at.
    f64 accNavMs = 0.0;
    u64 accNavQueries = 0;
    std::vector<rhi::UIVertex> uiVertices; // reused each frame
    std::vector<ui::WorldUIBatch> worldUIBatches;      // world-canvas triangles (reused)
    std::vector<Renderer::WorldUIDraw> worldUIDraws;   // -> renderer, one frame each
    std::vector<rhi::ParticleVertex> particleAlpha, particleAdd; // reused each frame
    cam::CameraState cameraState;          // persistent camera smoothing/blend state
    bool prevGameCamEnabled = false;       // rising edge -> snap the camera
    bool musicStarted = false;             // adaptive music armed while the game runs
    entt::entity musicZoneActive = entt::null; // last MusicZone the player was inside
    f32 dtSmooth = 0.0f;                   // EMA of frame delta (motion smoothing)
    f32 winMaxDt = 0.0f;                   // worst frame in the current report window
    u32 winJank = 0;                       // frames slower than 45 FPS this window

    // --- Tag streaming test (--tagstreamtest N) -----------------------------
    // Builds and binds the synthetic world before the first frame, so the sweep runs
    // against a real renderer. A setup failure exits non-zero rather than running a
    // meaningless test.
    int tagStreamTestExit = 0;
    if (tagStreamTest_ > 0 && !BeginTagStreamTest()) {
        HBE_ERROR("--tagstreamtest: setup failed; not running the sweep.");
        tagStreamTestExit = 1;
        tagStreamTest_ = 0;
    }

    // --- Benchmark mode (--benchmark N) -------------------------------------
    // Collect RAW per-frame times (never the smoothed dt - the EMA is for motion,
    // and averaging away spikes is exactly the wrong thing when measuring).
    std::vector<f32> benchFrameMs;
    u64 benchFrameIndex = 0;
    if (config.benchmarkFrames > 0) {
        benchFrameMs.reserve(config.benchmarkFrames);
        HBE_INFO("Benchmark: {} frames after {} warmup frames, vsync forced OFF.",
                 config.benchmarkFrames, config.benchmarkWarmup);
    }

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
        // Benchmark records the TRUE delta: the clamp below exists to stop a hitch
        // teleporting the simulation, but a measurement that clamps its own spikes
        // is measuring the clamp.
        const f32 trueFrameMs = rawDt * 1000.0f;
        rawDt = glm::clamp(rawDt, 0.0f, 0.1f); // <= 100 ms floor (never < 10 FPS of sim)
        dtSmooth = (dtSmooth <= 0.0f) ? rawDt : glm::mix(dtSmooth, rawDt, 0.2f);
        dt_ = dtSmooth; // REAL frame time (FPS display); dev game-speed doesn't skew it
        // Dev menu game-speed / pause scales the simulation delta (1.0 in ship builds).
        // The offline MOVIE RENDER overrides the SIMULATION delta with a fixed dt=1/fps
        // (deterministic frame-stepping) - but ONLY dt, never dt_, so caption/loading
        // clocks (which use dt_) are not time-warped.
        // Player pause multiplies in here rather than gating each system: one place,
        // and every consumer of the simulation delta (physics, animation, AI, nav,
        // gameplay, the cutscene clock) stops together by construction. dt_ - the
        // PRESENTATION clock used by the flow fades and the caption crawl - is
        // deliberately untouched, so the pause menu still animates.
        const f32 pauseScale = paused_ ? 0.0f : 1.0f;
        const f32 dt =
            (renderFixedDt_ > 0.0f) ? renderFixedDt_ : (dtSmooth * devTimeScale_ * pauseScale);

        // Benchmark sampling + termination. Uses the RAW delta, before clamping
        // and smoothing, so a spike is recorded as a spike.
        if (config.benchmarkFrames > 0) {
            ++benchFrameIndex;
            if (benchFrameIndex > config.benchmarkWarmup) {
                benchFrameMs.push_back(trueFrameMs);
                if (benchFrameMs.size() >= config.benchmarkFrames) {
                    ReportBenchmark(benchFrameMs, renderer, config.benchmarkCsv);
                    break; // falls into the normal shutdown path below the loop
                }
            }
        }

        // --tagstreamtest: advance the swept focus and sample the frame time. Runs here,
        // before anything reads streamFocusOverride_, and uses the RAW frame time for
        // the same reason the benchmark does - the clamp and EMA below exist to stop a
        // hitch teleporting the simulation, and a measurement that smooths its own
        // spikes is measuring the smoothing.
        if (tagStreamTestActive_ && !StepTagStreamTest(trueFrameMs)) break;

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
                         "shadow {} drawn/{} culled | UI {} el {} verts {} txt {} maps",
                         fpsFrames / elapsed, 1000.0f * elapsed / fpsFrames,
                         1000.0f * winMaxDt, winJank, rs.drawn, rs.total, rs.culled,
                         rs.instancedDraws, rs.totalInstances, rs.shadowDraws,
                         rs.shadowCulled, us.elements, us.verts,
                         us.textLayouts, us.mapRebuilds);
                HBE_INFO("  CPU phases (avg/frame): gameplay {:.2f} ms | facial {:.2f} ms | "
                         "vfx-sim {:.3f} ms | vfx-expand {:.3f} ms | stream {:.3f} ms | "
                         "nav {:.3f} ms ({:.2f} A*/frame) | pick {:.4f} ms ({:.2f} "
                         "cast/frame) | renderScene-submit {:.2f} ms | cpu-particles {}",
                         accGpMs / fpsFrames, accFacialMs / fpsFrames,
                         accVfxSimMs / fpsFrames, accVfxBuildMs / fpsFrames,
                         accStreamMs / fpsFrames, accNavMs / fpsFrames,
                         static_cast<f64>(accNavQueries) / fpsFrames,
                         accPickMs / fpsFrames,
                         static_cast<f64>(accPickCasts) / fpsFrames, accRenderMs / fpsFrames,
                         particle::LiveCount(scene));
                if (gridNav.Ready()) {
                    HBE_INFO("  Navigation: {} static tri(s) | {} terrain surface(s) | "
                             "{} streamed block(s) | {} full rebuild(s) since boot",
                             gridNav.StaticTriangleCount(), gridNav.TerrainCount(),
                             gridNav.StreamedBlockCount(), gridNav.RebuildCount());
                }
                // Streaming, when a level is actually bound. `resident/total` is the
                // whole point of the feature in one number; the maxima are the WINDOW's
                // worst single finalize/despawn and worst evaluation - the cost the
                // salvaged one-per-frame budget exists to keep bounded. Window, not
                // lifetime: one 12 ms finalize at level start would otherwise make every
                // later line report it forever, which says nothing about a NEW
                // regression during play. (--tagstreamtest reports the since-bind ones.)
                if (tagStream_.IsBound() && tagStream_.ShardCount() > 0) {
                    const stream::StreamStats& ss = tagStream_.Stats();
                    HBE_INFO("  Streaming: {}/{} shard(s) resident | {} spawn / {} despawn "
                             "(since bind) | {} eval | window max: eval {:.3f} ms, "
                             "structural {:.2f} ms | {} deferred shard(s){}",
                             tagStream_.ResidentShardCount(), tagStream_.ShardCount(),
                             ss.spawns, ss.despawns, ss.evaluations, ss.winMaxEvalMs,
                             ss.winMaxStructuralMs, ss.deferredFinalizes,
                             tagStream_.Enabled() ? "" : " | DISABLED (all pinned loaded)");
                    tagStream_.ResetWindowStats();
                }
                // GPU simulation, when any emitter uses it. `slots` is what the
                // compute pass ran over and `spawned` is this frame's births - the two
                // numbers that say whether the ring is sized right (spawned*lifetime
                // should sit just under slots).
                if (const particle::GpuSim::Stats& gs = gpuSim_.GetStats(); gs.emitters > 0) {
                    HBE_INFO("  GPU vfx sim: {} emitters | {} slots | {} spawned/frame | "
                             "{} groups{}",
                             gs.emitters, gs.slots, gs.spawned, gs.groups,
                             gs.dropped ? std::format(" | {} DROPPED (no room)", gs.dropped)
                                        : std::string());
                }
                accGpMs = accFacialMs = accRenderMs = accVfxBuildMs = accVfxSimMs = 0.0;
                accStreamMs = 0.0;
                accPickMs = 0.0;
                accPickCasts = 0;
                accNavMs = 0.0;
                accNavQueries = 0;
                winMaxDt = 0.0f;
                winJank = 0;
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
        //
        // `!onInit_` = RUNTIME ONLY. This block is inside Engine::Run, which the EDITOR
        // also drives, and it had no editor gate at all - so with devMenu:true (which
        // the reference project sets) Ctrl+` then F9 ran LoadGame inside the editor.
        // LoadGame does scene::Instantiate(LoadMode::Replace): it DESTROYED the authored
        // world and put a checkpoint snapshot in its place, while the editor went on
        // believing currentScenePath_ still described the registry. The next Ctrl+S
        // wrote the snapshot over the level. The dev-menu scene rows (LoadGameplayScene)
        // were a second door to the same state. The editor has its own menus for all of
        // this; the overlay is a shipped-build affordance and now says so.
        const bool devEnabled = !onInit_ && Project::HasActive() &&
                                Project::Active().Settings().build.devMenu;
        // Suppress the dev overlay chord + its function keys while listening for a
        // rebind, so pressing Ctrl+` (or a dev F-key) during a rebind neither toggles
        // the overlay nor fires a dev action.
        // The dev menu owns the keyboard while open: it reads RAW edges (the frame's
        // text-capture mute, set below, keeps gameplay/UI-nav/schematics quiet), and
        // its Ctrl+` toggle is raw so it still closes while the mute is engaged.
        if (devEnabled && !actionMap_.Rebinding() && input.IsKeyDownRaw(Key::Ctrl) &&
            input.WasKeyPressedRaw(Key::Grave)) {
            devMenuOpen_ = !devMenuOpen_;
            if (devMenuOpen_) { DevMenuScanScenes(); devMenuSel_ = 0; } // fresh scene list on open
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

        // `physics.IsRunning()` is the engine's one answer to "is the simulation
        // running?" - it is what already gates motion matching and facial animation
        // just below, for the same reason: in edit mode those systems would overwrite
        // AUTHORED component values with runtime ones. Animation keeps previewing at
        // rest (the Timeline transport is a live preview); the flag only stops it from
        // clearing the authored `playing` when a non-looping clip runs out.
        const bool simulating = physics.IsRunning();
        anim::Update(scene, dt, simulating); // keyframe tracks pose entities first
        // Build/refresh any dirty chunked terrain (cheap when nothing changed).
        terrain::Update(scene, renderer);
        // Motion matching picks each animator's clip from movement intent BEFORE
        // the skeletal pose is sampled (play mode / runtime only).
        if (physics.IsRunning()) anim::UpdateMotionMatching(scene, assetsDir, dt);
        // Skeletal animation: advance Animators and rebuild joint palettes.
        anim::UpdateSkeletal(scene, assetsDir, dt, simulating);
        // Facial: lip-sync + blink + expression -> MorphState.weights (consumed by
        // CollectDrawItems -> the vertex shader's pre-skin morph accumulation). Play/
        // runtime only: in edit mode it would overwrite authored MorphState weights.
        {
            const auto _pt = clock::now();
            if (physics.IsRunning()) facial::Update(scene, dt);
            accFacialMs += std::chrono::duration<f64, std::milli>(clock::now() - _pt).count();
        }

        // The project's canvas configuration (scale mode + reference size).
        //
        // DELIBERATELY the PROJECT's, not the open document's, even though a
        // `.hbui` carries its own `canvas` block. This config is the fallback for
        // CANVAS-LESS roots, and that set is not only the document's: the
        // dialogue choice buttons and the interact prompt are created bare
        // (no Parent, no UICanvas, no UIDocMember) and lay out against exactly
        // this. Routing it through a document would silently move transient
        // gameplay UI onto the menu's basis. The document's block is the basis
        // the EDITOR lays it out against, which is what makes a document
        // self-describing; the migrator seeds the two from the same place so
        // they agree by construction.
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
            // THE POINTER, resolved by device and cursor state.
            //
            //   cursor FREE (menu, dialogue choice, editor) -> the cursor itself,
            //     driving screen canvases AND world pages, pressed with LMB.
            //   cursor LOCKED (first-person gameplay)       -> the RETICLE, screen
            //     centre, driving world pages ONLY, pressed with the "Interact"
            //     ACTION (LMB is fire).
            //   GAMEPAD                                     -> the RETICLE in EITHER
            //     cursor state (a pad moves no mouse, and focus navigation never
            //     lands on a world page), unless a screen focus ring is live - then
            //     the pad is navigating a menu and screen space beats world space.
            //
            // The policy itself lives in interact::ResolvePointer - one pure
            // function, so the rule has exactly ONE definition and
            // --test-3dinteract exercises the shipped one rather than a copy.
            interact::PointerInputs pin;
            pin.external = uiPointerExternal_;
            pin.externalNorm = {uiPointerU_, uiPointerV_}; // editor Game panel
            pin.cursorLocked = IsCursorLocked();
            pin.padActive = input_ && input_->LastInputWasGamepad();
            // Last frame's focus ring. A one-frame lag on "the pad is in a menu" is
            // invisible; ui::UpdateNavigation runs later in this same block.
            pin.screenFocusActive = uiCtx_.focusVisible && uiCtx_.focused != entt::null;
            if (window.Width() > 0 && window.Height() > 0)
                pin.cursorNorm = {input.MouseX() / window.Width(),
                                  input.MouseY() / window.Height()};
            const interact::PointerMode pmode = interact::ResolvePointer(pin);
            glm::vec2 pointerNorm = pmode.worldPointer;
            const glm::vec2 screenPointer = pmode.screenPointer;
            // THE PAGE HALF IS SUPPRESSED TOO. `considerObjects` below covered only
            // the object half, so during a cutscene the reticle sat wherever the
            // cutscene camera pointed and ANY Interact press - including the press
            // that skips the cutscene - activated whatever world Button happened to
            // be framed; and during a dialogue choice a click BETWEEN the choice
            // buttons went through into a world page behind them.
            //
            // Deliberately NARROWER than InteractionsSuppressed(): `menuOpen` is
            // NOT in this list. Screen-beats-world already handles menus, and
            // folding it in would kill a world-space pause page - the exact content
            // this system exists to make possible.
            if (dialogueNode_ != 0 || cutsceneTime_ >= 0.0f || actionMap_.Rebinding() ||
                devMenuOpen_ || game::DialoguePending() || game::CutscenePending())
                pointerNorm = glm::vec2(-1.0f);

            // ONE PICK PASS for both world UI pages and 3D interactables: one ray,
            // one occlusion raycast, one winner. UpdateInteractions reads the same
            // result for the object half, so a wall terminal that is both a page and
            // an Interactable can never light up twice.
            ui::PointerState pointers;
            {
                const auto _pk = clock::now();
                interact::Params pp;
                // A suppressed interactable must not be a candidate at all: if it
                // were, it could win the ray and shadow a world UI page behind it
                // that IS live (a pause menu page, a diegetic screen).
                pp.considerObjects = !InteractionsSuppressed();
                pp.maxRange = 100.0f;
                // The player, for the Interactable proximity GATE and the
                // not-aiming fallback (nearest CharacterController, as before).
                auto players = scene.Registry().view<Transform, CharacterController>();
                if (players.begin() != players.end()) {
                    pp.hasAnchor = true;
                    pp.anchor = glm::vec3(scene.WorldMatrix(*players.begin())[3]);
                    // ...and the player's own capsule must not occlude the player's
                    // own ray (a third-person camera sits behind it).
                    pp.anchorEntity = *players.begin();
                }
                const interact::OccludeFn occ = [&physics](const glm::vec3& o,
                                                           const glm::vec3& d, f32 m) {
                    const PhysicsWorld::RayHit h = physics.RaycastDetailed(o, d, m);
                    interact::Block b;
                    b.hit = h.hit;
                    b.distance = h.distance;
                    b.entity = h.entity;
                    return b;
                };
                const interact::AcceptFn acc = [&](entt::entity e) {
                    return InteractableAvailable(scene, e);
                };
                pick_ = interact::Pick(scene, renderer.GetCamera(), pointerNorm, occ, acc,
                                       pp, &uiCtx_);
                if (pick_.kind == interact::Hit::Kind::Page)
                    pointers.worldCanvasPx[static_cast<u32>(pick_.entity)] = pick_.canvasPx;
                if (pmode.useInteractAction) {
                    // Reticle-driven pages are pressed with the Interact ACTION -
                    // the same verb, key and glyph as "[E] Talk" on a 3D object, and
                    // rebindable/pad-bound with it. LMB stays fire.
                    pointers.worldButtonOverride = true;
                    pointers.worldPressed =
                        input_ && actionMap_.Pressed(input, "Interact");
                    pointers.worldDown = input_ && actionMap_.Down(input, "Interact");
                }
                pickMs_ = std::chrono::duration<f64, std::milli>(clock::now() - _pk).count();
                pickPages_ = pick_.pagesTested;
                pickRaycasts_ = pick_.raycasts;
                accPickMs += pickMs_;
                accPickCasts += pick_.raycasts;
            }
            pointerNorm = screenPointer;
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
        }
        // Captions run in the RUNTIME always, and in the editor while PLAYING.
        //
        // They used to share the `!onInit_` gate above, which meant they never ran in
        // the editor at all - so dialogue played, pushed lines that could never
        // expire (subtitles_.Update was the only thing that ages them, and this is
        // its only call site), nothing rendered, and `SetMusicDucking` below - which
        // reads `!subtitles_.Empty()` and is NOT gated - latched the score ducked for
        // the rest of the session after the first line. That is the primary narrative
        // authoring loop, broken in the tool it is authored in.
        if (!onInit_ || physics.IsRunning()) UpdateCaptions(dt_);

        // Visual-script "Schematics" tick while the simulation runs (the editor's
        // play mode gates physics; the runtime always plays). On Start / On Update.
        schematic::Update(scene, input, dt, physics.IsRunning());
        // Player/character input intent BEFORE physics, which drives the capsule
        // CharacterVirtual (gravity + world collision). Camera-relative movement
        // uses the current view's forward.
        // In the shipped flow, player INTENT only exists while Playing: a 3D main
        // menu binds a world whose Player entity is resident, physics runs (idle
        // anim, settling props), and without this gate WASD would walk the player
        // around behind the menu. The editor keeps its old behaviour (flowActive_
        // is false there) - play-in-editor characters still move.
        if (physics.IsRunning() && (!flowActive_ || gameState_ == GameState::Playing))
            character::Update(scene, input, dt, renderer.GetCamera().Forward());
        physics.Update(scene, dt);
        // Destruction runs immediately after physics so this frame's contacts are
        // still queued: it is the only consumer of PhysicsWorld::PopContact, and a
        // contact left undrained would be delivered a frame late (or dropped when
        // the queue caps).
        if (physics.IsRunning()) destruction::Update(scene, renderer, physics, dt);
        // TAG STREAMING. This slot is not arbitrary - it is the only defensible one:
        //   * AFTER physics.Update, so this frame's lazy reaps have already run and the
        //     Jolt bodies / CharacterVirtuals / audio voices of LAST frame's despawns
        //     are gone before more entities are destroyed;
        //   * BEFORE gameplay::Update, because spawn::Update creates and destroys
        //     entities there and combat::Update runs the one-shot death dispatch. A
        //     despawn placed after it can take an entity that combat just named in a
        //     queued game::DeathRec, and that record is read NEXT frame by
        //     schematic::Update through a handle nobody re-validates.
        {
            const auto _st = clock::now();
            UpdateTagStreaming(scene, renderer);
            accStreamMs += std::chrono::duration<f64, std::milli>(clock::now() - _st).count();
        }
        // Gameplay band: AI + spawning/encounters + combat + player fire. Runs
        // after physics (fresh positions for line-of-sight and hit tests) and
        // BEFORE nav::UpdateAgents so an AI-set NavigationAgent target steers the
        // same frame (no one-frame lag).
        {
            const auto _pt = clock::now();
            // Same gate character::Update carries 28 lines up, and for the same
            // reason: a 3D main menu binds a world whose Player/AI/Spawner entities
            // are resident and physics runs. Without it the whole gameplay band ran
            // behind the menu - AI patrolled and shot, encounters cleared, and
            // checkpoints wrote `.hbsave` while the player was still on the title
            // screen. The editor keeps its old behaviour (flowActive_ is false).
            if (physics.IsRunning() && (!flowActive_ || gameState_ == GameState::Playing))
                gameplay::Update(scene, physics, renderer, input, renderer.GetCamera(), dt,
                                 ui::PointerOverInteractive(scene, uiCtx_));
            accGpMs += std::chrono::duration<f64, std::milli>(clock::now() - _pt).count();
        }
        // NavigationAgents steer along real-time grid A* paths while the
        // simulation runs (play mode in the editor; always in the runtime). The
        // grid auto-rebuilds from static geometry (no bake) and re-plans around
        // moving NavigationObstacles.
        if (physics.IsRunning()) {
            const auto _nt = clock::now();
            const std::filesystem::path navAssets =
                Project::HasActive() ? Project::Active().AssetsDir() : std::filesystem::path();
            gridNav.EnsureBuilt(scene, navAssets);
            nav::UpdateAgents(scene, gridNav, dt);
            // The nav band has its own accumulator because it is the one band whose cost
            // is driven by AGENT COUNT and by whether targets are reachable, not by scene
            // size - and because the acceptance criterion for this system is stated as a
            // per-frame millisecond budget. Without a number here it cannot be checked.
            accNavMs += std::chrono::duration<f64, std::milli>(clock::now() - _nt).count();
            accNavQueries += gridNav.LastQueryCount();
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
                // Audio-less subtitles (cutscene narration, signage): straight
                // into the one subtitle stack, formatted + gated like speech.
                game::SubtitleReq sub;
                while (game::ConsumeSubtitle(sub)) {
                    subtitle::Line line;
                    line.speaker = std::move(sub.speaker);
                    line.text = std::move(sub.text);
                    line.kind = subtitle::Kind::Dialogue;
                    line.duration = sub.duration;
                    line.priority = 20;
                    PushSubtitle(std::move(line));
                }
                // Impulse camera shake (combat impacts, cutscene shake markers)
                // into the live camera rig - one path for every shake source.
                if (f32 trauma = 0.0f; game::ConsumeCameraShake(trauma))
                    cam::AddShake(cameraState, trauma);
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
            // Duck the score while speech is on screen. A running conversation or
            // any live subtitle counts - so a barked voiceline ducks too, not just
            // scripted dialogue.
            audio.SetMusicDucking(dialogueNode_ != 0 || !subtitles_.Empty());
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
            // PLAYER LOOK IS GATED ON THE CURSOR LOCK. Mouse deltas accumulate in
            // both cursor states (Input_Win32 feeds the locked raw delta and the
            // free-cursor move into the same accumulator), so without this the
            // same motion that picks a menu item or a dialogue choice also spins
            // the camera behind it. UpdateGameFlow ran earlier this frame and has
            // already applied the free-cursor policy, so IsCursorLocked() is this
            // frame's answer. The editor never locks the cursor - that would trap
            // it away from the panels - so play-in-editor keeps look enabled.
            const bool lookEnabled = !flowActive_ || IsCursorLocked();
            // 3D menu with a NAMED camera: that entity owns the view outright -
            // cam::Update is skipped so the scene's primary/zone cameras cannot
            // fight it. Entity forward is local -Z (the engine-wide convention,
            // see CameraSystem.cpp). With no named camera the normal camera
            // system runs and the scene's primary camera frames the menu.
            const bool menuCam = menuWorldBound_ && gameState_ == GameState::MainMenu &&
                                 menuCamEntity_ != entt::null &&
                                 scene.Registry().valid(menuCamEntity_);
            if (menuCam) {
                const glm::mat4 W = scene.WorldMatrix(menuCamEntity_);
                const glm::vec3 eye(W[3]);
                const glm::vec3 fwd = -glm::normalize(glm::vec3(W[2]));
                renderer.GetCamera().LookAt(eye, eye + fwd);
                if (const CameraComponent* cc =
                        scene.Registry().try_get<CameraComponent>(menuCamEntity_))
                    renderer.GetCamera().SetFovY(cc->fovY);
                renderer.SetOrbitEnabled(false); // the menu camera owns the view
            } else if (cam::Update(scene, renderer.GetCamera(), cameraState, dt, input,
                                   camRay, renderer.GetCamera().Aspect(), lookEnabled)) {
                renderer.SetOrbitEnabled(false); // the game camera owns the view
            }
        }
        prevGameCamEnabled = gameCameraEnabled_;

        // Particles: simulate (spawn + integrate) and build this frame's billboards
        // against the camera basis. Emit even in the editor so emitters preview live.
        {
            const auto _st = clock::now();
            particle::Update(scene, dt, true);
            accVfxSimMs += std::chrono::duration<f64, std::milli>(clock::now() - _st).count();
        }
        {
            const auto _pt = clock::now();
            f64 mapWaitMs = 0.0; // GPU back-pressure, subtracted below (see the map site)
            const glm::vec3 fwd = renderer.GetCamera().Forward();
            glm::vec3 pRight = glm::cross(fwd, glm::vec3(0.0f, 1.0f, 0.0f));
            pRight = glm::dot(pRight, pRight) > 1e-6f ? glm::normalize(pRight)
                                                      : glm::vec3(1.0f, 0.0f, 0.0f);
            const glm::vec3 pUp = glm::normalize(glm::cross(pRight, fwd));
            particle::BuildVertices(scene, renderer, assetsDir, pRight, pUp, particleAlpha,
                                    particleAdd);
            // 3D text objects (WorldText) ride the same depth-tested quad batch.
            // They are arbitrary font-atlas glyph quads with no particle and no
            // emitter behind them, so they stay on the CPU path by construction.
            ui::AppendWorldText(scene, renderer, assetsDir, pRight, pUp, particleAlpha);
            renderer.SetParticles(particleAlpha, particleAdd);

            // GPU vertex expansion for emitters that opted in: upload 64-byte
            // records instead of six 40-byte world-space vertices and let the VS
            // build the quads. Mapped BEFORE RenderScene, like the QueueCompute
            // idiom - Vulkan's MapGpuBuffer waits on this slot's fence here.
            if (particle::AnyGpuExpand(scene)) {
                if (!gpuParticleBuffer_.IsValid() && !gpuParticleFailed_) {
                    rhi::GpuBufferDesc bd;
                    bd.elementCount = kGpuParticleRecordElements;
                    bd.elementStride = sizeof(vfx::GpuParticle);
                    bd.usage = rhi::GpuBufferUsage::ShaderRead | rhi::GpuBufferUsage::CpuWrite;
                    // One emitter batch is one bind, so this is the Vulkan descriptor
                    // range AND the per-emitter ceiling both backends clamp to.
                    bd.maxBindElements = rhi::kMaxGpuParticleBatchElements;
                    bd.debugName = "VfxParticleRecords";
                    gpuParticleBuffer_ = renderer.CreateGpuBuffer(bd);
                    gpuParticleFailed_ = !gpuParticleBuffer_.IsValid();
                    if (gpuParticleFailed_) {
                        HBE_WARN("Particles: GPU expansion buffer unavailable; gpuExpand "
                                 "emitters will not draw (CPU emitters are unaffected).");
                    }
                }
                if (gpuParticleBuffer_.IsValid()) {
                    // The map is NOT expansion work and is excluded from the phase
                    // timer: Vulkan's MapGpuBuffer waits on this ring slot's fence
                    // (D3D12 already waited at the end of the previous EndFrame), so
                    // leaving it in would report GPU back-pressure as CPU cost and
                    // make the two backends' vfx-expand numbers incomparable.
                    const auto _mapStart = clock::now();
                    void* dst = renderer.MapGpuBuffer(gpuParticleBuffer_);
                    mapWaitMs = std::chrono::duration<f64, std::milli>(clock::now() - _mapStart)
                                    .count();
                    if (dst) {
                        particle::BuildGpuRecords(scene, renderer, assetsDir, pRight, pUp, dst,
                                                  kGpuParticleRecordElements,
                                                  gpuParticleBatches_);
                        renderer.SetGpuParticles(gpuParticleBuffer_, gpuParticleBatches_);
                    }
                }
            }

            // GPU SIMULATION. One step further out than the block above: these
            // emitters have no CPU pool and nothing is uploaded per particle - the
            // compute dispatches queued here write the very buffer the vertex shader
            // then reads. Queued BEFORE RenderScene because both backends execute the
            // compute queue in their BeginFrame (Vulkan cannot record compute inside a
            // render pass) - the same rule SetVolumeParticles follows. It is inside
            // the vfx-expand timer on purpose: this IS the phase, and what the timer
            // should show is it collapsing to O(emitters).
            if (particle::AnyGpuSim(scene)) {
                if (gpuSim_.Update(scene, renderer, assetsDir, pRight, pUp, dt, &mapWaitMs)) {
                    renderer.SetGpuParticles(gpuSim_.Records(), gpuSim_.Batches());
                }
            }
            accVfxBuildMs +=
                std::chrono::duration<f64, std::milli>(clock::now() - _pt).count() - mapWaitMs;
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
        //
        // DECLARED, NOT STAMPED. This used to CALL ApplyGraphicsPreset on
        // scene.Environment().post, i.e. it overwrote six of the authored post flags
        // (ssgi/motionBlur/ssr/fog/dof/ssao) plus shadowCascades in the live scene,
        // every frame, in the runtime only. Three consequences, all bad: the editor
        // could not be made to show what ships without duplicating the table; the
        // shipped game's `.hbsave` recorded the DEGRADED stack as though the author
        // had written it (save at Low, and Low is baked in forever); and `post` is
        // one of the five fields --test-lightingparity stamps, so the runtime was
        // silently mutating the very value the parity test compares.
        //
        // The preset is now DECLARED on the environment and applied at READ time in
        // Scene::MakeView - after Post Volumes, exactly where it was applied before,
        // and to the same values, so the rendered result is unchanged. The authored
        // data is left alone, which is what lets the editor preview the shipped look
        // by simply declaring the same preset (View > Preview shipped quality).
        if (!onInit_) {
            scene.Environment().postQualityPreset = userSettings_.graphicsPreset;
            // TEMP perf A/B: an explicit cascade count wins over the preset's.
            scene.Environment().forceShadowCascades =
                config.forceShadowCascades > 0 ? static_cast<u32>(config.forceShadowCascades) : 0u;
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
    // Drop the streaming binding before the scene/renderer go away. Reset() drains any
    // staging job first, so no worker is left writing into freed shards while the job
    // system is being shut down.
    tagStream_.Reset();
    gridNav_ = nullptr;
    renderer.Shutdown();
    jobs::Shutdown();
    window_ = nullptr;
    renderer_ = nullptr;
    scene_ = nullptr;
    input_ = nullptr;
    physics_ = nullptr;
    audio_ = nullptr;
    HBE_INFO("Heartbreak Engine shut down cleanly.");
    // --tagstreamtest exits NON-ZERO on a failed setup or a failed assertion, so a build
    // script can gate on it instead of having to read the log.
    if (tagStreamTestFailed_) tagStreamTestExit = 1;
    return tagStreamTestExit;
}

// --- Tag streaming ------------------------------------------------------------

const std::vector<glm::vec3>& Engine::StreamFoci(const Scene& scene, const Renderer& renderer) {
    if (streamFocusOverride_) {
        streamFoci_.assign(1, *streamFocusOverride_);
        return streamFoci_;
    }
    stream::FocusPoints(scene, renderer.GetCamera().Position(), streamFoci_);
    return streamFoci_;
}

void Engine::UpdateTagStreaming(Scene& scene, Renderer& renderer) {
    if (!tagStream_.IsBound()) return; // no level bound: one branch, nothing else
    // WHEN streaming may run. In the shipping runtime that is Playing AND LOADING - and
    // Loading is not an oversight. The loading screen's reveal gate waits on
    // Streamer::IsSettled, so a streamer that does not run behind the curtain can never
    // settle: the screen would sit at 90% until the timeout and then reveal a world with
    // holes in it. (The design doc says "gated on Playing"; that is the one place it is
    // wrong, and this is why.) Anything else - MainMenu, Boot - has no bound level, so
    // the guard above already covers it.
    //
    // THE GUARD IS POSITIVE, NOT AN EXCLUSION, and that is a correction. It used to
    // read `if (flowActive_ && ... ) return;` on the reasoning that "in the EDITOR
    // flowActive_ is false and the editor loads scenes itself, so nothing is bound and
    // this never runs". Both halves were wrong: flowActive_ IS false under the editor,
    // which made the `&&` false and SKIPPED the early-out entirely, and the dev overlay
    // (F9 / the scene rows) could bind a level from inside the editor. Streaming then
    // ran on the authoring world and despawned shards by camera distance, and a Ctrl+S
    // wrote the surviving fragment over the level - silently, because the save-time
    // shard bake re-derives itself from whatever is live.
    //
    // So: streaming needs a POSITIVE reason to run. Playing and Loading are the two in
    // the shipping runtime (Loading is not an oversight - see above), plus the explicit
    // test driver. Anything else - MainMenu, Boot, and every editor frame - returns.
    if (!tagStreamTestActive_ && gameState_ != GameState::Playing &&
        gameState_ != GameState::Loading &&
        !(gameState_ == GameState::MainMenu && menuWorldBound_)) // 3D menu backdrop
        return;
    // And the editor's AUTHORING world never streams at all, whatever gameState_ says.
    // onInit_ is set only by the editor (see its other uses); --tagstreamtest runs
    // headless with no hook, so it is unaffected.
    if (onInit_ && !tagStreamTestActive_) return;
    tagStream_.Update(scene, renderer, StreamFoci(scene, renderer));
}

// --- --tagstreamtest ----------------------------------------------------------
// A SYNTHETIC world, and the reason is not convenience. The reference project has
// nothing to stream: 17 of its 18 entities total ~18 KB, and the 18th is a Terrain
// component - one monolithic 16-chunk thing that no tag can subdivide and that IS the
// world floor. Measuring streaming there would measure nothing. So this builds content
// designed to be streamed, exactly as the deleted `--worldtest` built its own world,
// and every number it prints is about THIS world. A green --tagstreamtest says the
// feature works; it says nothing whatsoever about the reference project's frame time.
//
// The shape: `objects` props spread along X in `kTagCount` semantic tags, each of which
// the save-time bake splits into spatially-coherent shards. A focus point sweeps the
// whole span and back while the renderer runs normally, so the frame times are real
// frame times with a real GPU, not a headless estimate.
// The world is a chain of ISLANDS - clusters of content separated by empty ground -
// because that is the shape real levels have and the shape sharding exists for. A
// CONTINUOUS line of props does not work as a test: the grid clustering unions every
// pair of touching cells, so a 2400 m line of "Props" correctly bakes to ONE
// 2400 m shard, which is always in range and therefore never streams. (The bake's
// validator says exactly that, loudly, which is worth knowing - it caught this test's
// first draft.) Islands spaced further apart than the clustering cell are what produce
// many small shards, and many small shards are what streaming is.
namespace {
constexpr u32 kStreamTestTagCount = 4;
constexpr f32 kStreamTestLoadRadius = 90.0f;
// Island spacing must exceed the clustering cell (which defaults to the load radius) by
// enough that two islands' cells are not even 8-neighbours, or they merge into one shard.
constexpr f32 kStreamTestIslandSpacing = 320.0f;
constexpr f32 kStreamTestPropSpacing = 4.0f; // within an island
constexpr u32 kStreamTestWarmupFrames = 240;  // discarded: PSO compiles, first-touch faults
constexpr u32 kStreamTestSampleFrames = 240;  // per measurement window
constexpr f32 kStreamTestSweepSeconds = 18.0f; // each direction; sets the focus speed
} // namespace

bool Engine::BeginTagStreamTest() {
    if (!scene_ || !renderer_ || tagStreamTest_ == 0) return false;
    const u32 objects = glm::max(tagStreamTest_, 12u);

    // TAKE THE WORLD OVER. The game flow would otherwise reach FlowMainMenu/FlowPlay on
    // the first frame and Replace-load the project's own startup scene over the
    // synthetic one. Documents are spared by every Replace sweep (that is what makes the
    // UI layer resident), so they have to be closed explicitly or they render on top of
    // the measurement.
    flowActive_ = false;
    gameState_ = GameState::None;
    if (bootDoc_ != 0) {
        docs_.Close(*scene_, bootDoc_);
        bootDoc_ = 0;
    }
    // EVERY screen, not just the menu one: with a per-screen split, closing only
    // the first would leave three residual documents rendering over the benchmark.
    if (!uiDocs_.empty()) {
        for (const ui::DocHandle h : uiDocs_) docs_.Close(*scene_, h);
        uiDocs_.clear();
        uiManager_.Bind(uiDocs_);
        uiManagerMode_ = false;
    }

    std::error_code ec;
    tagStreamTestDir_ = std::filesystem::temp_directory_path(ec) / "hbe_tagstreamtest";
    std::filesystem::remove_all(tagStreamTestDir_, ec);
    std::filesystem::create_directories(tagStreamTestDir_, ec);
    const std::filesystem::path level = tagStreamTestDir_ / "TagStreamTest.hbscene";

    // The tag list. Semantic names, deliberately - the whole premise of the bake is
    // that authors tag by MEANING and the sharder makes it spatial. "Ground" is
    // alwaysLoaded so the test also proves an always-loaded tag never streams.
    tagStreamTestTags_.clear();
    {
        TagDef untagged;
        untagged.name = tags::kUntaggedName;
        tagStreamTestTags_.push_back(untagged);
        const char* names[kStreamTestTagCount] = {"Props", "Enemies", "Debris", "Signage"};
        for (const char* n : names) {
            TagDef d;
            d.name = n;
            d.loadRadius = kStreamTestLoadRadius;
            d.unloadRadius = 0.0f; // Normalize derives the band (SALVAGE 2)
            d.priority = 0;
            tagStreamTestTags_.push_back(d);
        }
        tagStreamTestTags_[2].priority = 5; // "Enemies" load first under the throttle
        TagDef ground;
        ground.name = "Ground";
        ground.alwaysLoaded = true;
        tagStreamTestTags_.push_back(ground);
        tags::Normalize(tagStreamTestTags_);
        tags::SeedFromProject(tagStreamTestTags_);
    }

    // Author the world in a scratch Scene, bake it, save it. Building it as a FILE (not
    // straight into the live registry) is not a detour - it is the only honest test,
    // because streaming loads slices of a parsed file and the bake result IS the file.
    {
        Scene authoring;
        entt::registry& reg = authoring.Registry();
        const TagId ids[kStreamTestTagCount] = {tags::Intern("Props"), tags::Intern("Enemies"),
                                                tags::Intern("Debris"), tags::Intern("Signage")};
        const TagId ground = tags::Intern("Ground");

        const auto prop = [&](const std::string& name, const glm::vec3& p, TagId tag,
                              const glm::vec3& half) {
            const entt::entity e = authoring.CreateEntity(name);
            Transform t;
            t.position = p;
            reg.emplace<Transform>(e, t);
            reg.emplace<AABB>(e, AABB{-half, half});
            MeshInstance mi;
            reg.emplace<MeshInstance>(e, mi);
            reg.emplace<MeshRef>(e, MeshRef{"prim:cube"});
            if (tag != kTagUntagged) tags::Assign(reg, e, tag);
            return e;
        };

        // Islands of content along X, each holding a square cluster of props.
        const u32 islands = glm::clamp(objects / 40u, 6u, 16u);
        const u32 perIsland = glm::max(objects / islands, 4u);
        const u32 side = static_cast<u32>(std::ceil(std::sqrt(static_cast<f32>(perIsland))));
        tagStreamIslands_ = islands;
        tagStreamSpan_ = static_cast<f32>(islands - 1) * kStreamTestIslandSpacing;
        u32 made = 0;
        for (u32 isl = 0; isl < islands; ++isl) {
            const f32 cx = static_cast<f32>(isl) * kStreamTestIslandSpacing;
            // A floor under each island, in the alwaysLoaded tag: ground must never
            // stream out from under the focus, and that is what alwaysLoaded means.
            prop("Ground_" + std::to_string(isl), {cx, -1.0f, 0.0f}, ground,
                 glm::vec3(60.0f, 0.5f, 60.0f));
            for (u32 k = 0; k < perIsland; ++k, ++made) {
                const f32 ox = (static_cast<f32>(k % side) - static_cast<f32>(side) * 0.5f) *
                               kStreamTestPropSpacing;
                const f32 oz = (static_cast<f32>(k / side) - static_cast<f32>(side) * 0.5f) *
                               kStreamTestPropSpacing;
                // Cycle the tags WITHIN each island, so every tag appears in every
                // island and is therefore SCATTERED across the whole world - the case a
                // literal "one tag = one streaming group" cannot handle at all, and the
                // exact reason the save-time sharder exists.
                const TagId tag = ids[made % kStreamTestTagCount];
                const entt::entity e = prop("Prop_" + std::to_string(made),
                                            {cx + ox, 0.5f, oz}, tag, glm::vec3(0.5f));
                // A quarter of them carry runtime state, so the sweep exercises
                // capture/restore and not only spawn/despawn.
                if (made % 4 == 0) {
                    Health h;
                    h.max = 100.0f;
                    h.current = 100.0f;
                    reg.emplace<Health>(e, h);
                }
                // Every eighth prop gets a child, so whole SUBTREES ride their shard.
                if (made % 8 == 0) {
                    const entt::entity c =
                        authoring.CreateEntity("Prop_" + std::to_string(made) + "_top");
                    Transform ct;
                    ct.position = {0.0f, 1.2f, 0.0f};
                    reg.emplace<Transform>(c, ct);
                    reg.emplace<AABB>(c, AABB{glm::vec3(-0.3f), glm::vec3(0.3f)});
                    reg.emplace<MeshInstance>(c, MeshInstance{});
                    reg.emplace<MeshRef>(c, MeshRef{"prim:sphere"});
                    reg.emplace<Parent>(c, Parent{e});
                    tags::Assign(reg, c, tag);
                }
            }
        }
        // A directional light + a camera-less untagged marker, so the resident slice is
        // not empty and the scene lights up.
        {
            const entt::entity l = authoring.CreateEntity("Sun");
            Transform t;
            t.rotation = glm::quat(glm::vec3(glm::radians(-55.0f), glm::radians(35.0f), 0.0f));
            reg.emplace<Transform>(l, t);
            reg.emplace<DirectionalLightComponent>(l, DirectionalLightComponent{});
        }

        const tagshard::BakeReport rep = tagshard::BakeScene(authoring, tagStreamTestTags_);
        HBE_INFO("--tagstreamtest: {} object(s) in {} island(s) over {:.0f} m baked into {} "
                 "shard(s) across {} tag(s) ({} error(s), {} warning(s)).",
                 made, islands, tagStreamSpan_, rep.shards.size(), kStreamTestTagCount,
                 rep.errors, rep.warnings);
        for (const tagshard::TagStat& s : rep.stats)
            HBE_INFO("  tag '{}': {} shard(s), {} object(s), largest diagonal {:.0f} m, "
                     "coherence {:.2f}{}",
                     s.tag, s.shards, s.members, s.largestDiagonal, s.coherence,
                     s.alwaysLoaded ? " (alwaysLoaded - never streamed)" : "");
        if (!scene::SaveScene(authoring, level, {}, SceneKind::Full, &rep.shards)) {
            HBE_ERROR("--tagstreamtest: cannot write the synthetic level to '{}'.",
                      level.string());
            return false;
        }
    }

    // Bind it. assetsDir is the temp dir: the synthetic world uses only procedural
    // primitives, so nothing has to resolve off the real project's Assets/.
    if (!tagStream_.BindLevel(*scene_, *renderer_, level, tagStreamTestDir_,
                              tagStreamTestTags_)) {
        HBE_ERROR("--tagstreamtest: BindLevel failed.");
        return false;
    }
    if (!tagStream_.Trusted()) {
        HBE_ERROR("--tagstreamtest: the shard table this run just baked came back "
                  "UNTRUSTED ({}). That is a bake/parse disagreement, not a streaming bug.",
                  tagStream_.UntrustedReason());
        return false;
    }
    tagStream_.ResetStats();
    tagStreamTestActive_ = true;
    tagStreamPhase_ = 0;
    tagStreamFrame_ = 0;
    tagStreamSweepT_ = -kStreamTestIslandSpacing;
    // Off the end of the world: nothing is in range there.
    tagStreamHome_ = glm::vec3(-kStreamTestIslandSpacing, 1.0f, 0.0f);
    tagStreamFocus_ = tagStreamHome_;
    streamFocusOverride_ = &tagStreamFocus_;
    tagStreamBaseline_ = 0;
    tagStreamIdleMs_.clear();
    tagStreamSteadyMs_.clear();
    tagStreamSweepMs_.clear();
    tagStreamSpeed_ = glm::max(tagStreamSpan_ / kStreamTestSweepSeconds, 1.0f);
    HBE_INFO("--tagstreamtest: bound {} streamed shard(s), {} always-resident row(s); "
             "sweeping a focus {:.0f} m and back at {:.0f} m/s (load radius {:.0f} m).",
             tagStream_.ShardCount(), tagStream_.ResidentRowCount(), tagStreamSpan_,
             tagStreamSpeed_, kStreamTestLoadRadius);
    HBE_WARN("--tagstreamtest measures a SYNTHETIC world built for this test. It is not "
             "a measurement of any real project.");
    return true;
}

bool Engine::StepTagStreamTest(f32 trueFrameMs) {
    if (!tagStreamTestActive_ || !scene_ || !renderer_) return false;
    ++tagStreamFrame_;
    const auto liveCount = [this] {
        const entt::registry& reg = scene_->Registry();
        const auto* st = reg.storage<entt::entity>();
        usize n = 0;
        if (st)
            for (const entt::entity e : *st)
                if (reg.valid(e)) ++n;
        return n;
    };

    // The camera follows the focus so streamed geometry is actually DRAWN - an
    // unrendered spawn is not a measurement of a spawn.
    renderer_->GetCamera().LookAt(tagStreamFocus_ + glm::vec3(-28.0f, 16.0f, 34.0f),
                                  tagStreamFocus_);

    // THREE measurement windows, because two would be dishonest. Parked-empty and
    // moving differ in SCENE CONTENT as well as in streaming activity, so the pair
    // cannot be read as "the cost of streaming". Parked-INSIDE-an-island has the same
    // content as the sweep and no streaming activity, which makes it the only fair
    // comparison; parked-empty is there to show what the bound-but-idle streamer costs.
    switch (tagStreamPhase_) {
    case 0: {
        // Warm up off the end of the world (PSO compiles, first-touch page faults),
        // sampling nothing.
        if (tagStreamFrame_ >= kStreamTestWarmupFrames) {
            tagStreamBaseline_ = liveCount();
            if (tagStream_.ResidentShardCount() != 0) {
                tagStreamTestFailed_ = true;
                HBE_ERROR("--tagstreamtest: FAILED - {} shard(s) resident with the focus "
                          "off the end of the world.",
                          tagStream_.ResidentShardCount());
            }
            HBE_INFO("--tagstreamtest: baseline {} live entities, 0/{} shards resident "
                     "(warmup done).",
                     tagStreamBaseline_, tagStream_.ShardCount());
            tagStreamPhase_ = 1;
            tagStreamFrame_ = 0;
        }
        break;
    }
    case 1: {
        // WINDOW A: parked off the end. Nothing resident, nothing streaming - the cost
        // of a bound streamer that has no work.
        tagStreamIdleMs_.push_back(trueFrameMs);
        if (tagStreamFrame_ >= kStreamTestSampleFrames) {
            // Jump into the middle island and let it settle before window B.
            tagStreamFocus_ =
                glm::vec3(static_cast<f32>(tagStreamIslands_ / 2) * kStreamTestIslandSpacing,
                          1.0f, 0.0f);
            tagStreamPhase_ = 2;
            tagStreamFrame_ = 0;
        }
        break;
    }
    case 2: {
        // Settle: let the island's shards finish loading (one finalize per frame).
        if (tagStream_.IsSettled(StreamFoci(*scene_, *renderer_)) || tagStreamFrame_ > 600) {
            HBE_INFO("--tagstreamtest: parked inside one island - {}/{} shard(s) resident, "
                     "{} live entities.",
                     tagStream_.ResidentShardCount(), tagStream_.ShardCount(), liveCount());
            tagStreamPhase_ = 3;
            tagStreamFrame_ = 0;
        }
        break;
    }
    case 3: {
        // WINDOW B: parked INSIDE an island. Shards resident, nothing streaming. Same
        // kind of content the sweep draws, so B vs C isolates streaming activity.
        tagStreamSteadyMs_.push_back(trueFrameMs);
        if (tagStreamFrame_ >= kStreamTestSampleFrames) {
            tagStreamPhase_ = 4;
            tagStreamFrame_ = 0;
            tagStreamSweepT_ = tagStreamHome_.x;
            tagStreamFocus_ = tagStreamHome_;
        }
        break;
    }
    case 4:
    case 5: {
        // WINDOW C: sweep out, then back. dt_ is the smoothed frame time - fine here,
        // the focus path only has to be repeatable, and the MEASUREMENT is trueFrameMs.
        const f32 dir = tagStreamPhase_ == 4 ? 1.0f : -1.0f;
        tagStreamSweepT_ += dir * tagStreamSpeed_ * glm::min(dt_, 0.05f);
        tagStreamFocus_ = glm::vec3(tagStreamSweepT_, 1.0f, 0.0f);
        tagStreamSweepMs_.push_back(trueFrameMs);
        if (tagStreamPhase_ == 4 && tagStreamSweepT_ >= tagStreamSpan_) {
            tagStreamPhase_ = 5;
            HBE_INFO("--tagstreamtest: reached the far end - {}/{} shard(s) resident, {} "
                     "spawn / {} despawn so far.",
                     tagStream_.ResidentShardCount(), tagStream_.ShardCount(),
                     tagStream_.Stats().spawns, tagStream_.Stats().despawns);
        } else if (tagStreamPhase_ == 5 && tagStreamSweepT_ <= tagStreamHome_.x) {
            // Home again, and off the end: everything must have unloaded. Give the
            // one-per-frame budget a few frames to finish draining.
            tagStreamPhase_ = 6;
            tagStreamFrame_ = 0;
        }
        break;
    }
    default: {
        // Drain, then report.
        if (tagStream_.ResidentShardCount() == 0 || tagStreamFrame_ > 240) {
            const usize back = liveCount();
            if (tagStream_.ResidentShardCount() != 0) {
                tagStreamTestFailed_ = true;
                HBE_ERROR("--tagstreamtest: FAILED - {} shard(s) still resident after the "
                          "focus returned off the end of the world.",
                          tagStream_.ResidentShardCount());
            }
            const stream::StreamStats& ss = tagStream_.Stats();
            if (ss.spawns == 0 || ss.despawns == 0) {
                tagStreamTestFailed_ = true;
                HBE_ERROR("--tagstreamtest: FAILED - the sweep produced {} spawn(s) and {} "
                          "despawn(s); it must produce both or it measured nothing.",
                          ss.spawns, ss.despawns);
            }
            // AND IT MUST NOT THRASH. "spawns > 0 && despawns > 0" alone passes green with
            // hysteresis broken: delete the salvage::EnforceHysteresis call, or widen
            // StreamPolicy's unload test to the LOAD radius, and a focus crossing the
            // boundary spawns and despawns the same shard tens of thousands of times -
            // every assertion above still satisfied, exit code still 0. One out-and-back
            // sweep visits each streamed shard at most twice; 4x that is a generous band
            // that still catches an order-of-magnitude regression.
            const u32 bound = 4u * static_cast<u32>(tagStream_.ShardCount());
            if (ss.spawns > bound || ss.despawns > bound) {
                tagStreamTestFailed_ = true;
                HBE_ERROR("--tagstreamtest: FAILED - {} spawn / {} despawn over ONE "
                          "out-and-back sweep of {} shard(s) (bound {}); the band is "
                          "thrashing - check the load/unload hysteresis.",
                          ss.spawns, ss.despawns, tagStream_.ShardCount(), bound);
            } else
                HBE_INFO("--tagstreamtest: no thrash - {} spawn / {} despawn over {} "
                         "shard(s) is within the 4x out-and-back bound of {}.",
                         ss.spawns, ss.despawns, tagStream_.ShardCount(), bound);
            if (back != tagStreamBaseline_) {
                tagStreamTestFailed_ = true;
                HBE_ERROR("--tagstreamtest: FAILED - {} live entities after the sweep, "
                          "baseline was {} ({:+d}). Something leaked or was destroyed.",
                          back, tagStreamBaseline_,
                          static_cast<int>(back) - static_cast<int>(tagStreamBaseline_));
            } else
                HBE_INFO("--tagstreamtest: entity count returned EXACTLY to the {} "
                         "baseline.",
                         tagStreamBaseline_);
            ReportTagStreamTest();
            tagStreamTestActive_ = false;
            streamFocusOverride_ = nullptr;
            std::error_code ec;
            std::filesystem::remove_all(tagStreamTestDir_, ec);
            return false;
        }
        break;
    }
    }
    return true;
}

void Engine::ReportTagStreamTest() {
    const auto pct = [](std::vector<f32> v, f32 q) {
        if (v.empty()) return 0.0f;
        std::sort(v.begin(), v.end());
        const usize i = glm::min(static_cast<usize>(q * static_cast<f32>(v.size())), v.size() - 1);
        return v[i];
    };
    const auto mean = [](const std::vector<f32>& v) {
        if (v.empty()) return 0.0f;
        f64 s = 0.0;
        for (const f32 x : v) s += x;
        return static_cast<f32>(s / static_cast<f64>(v.size()));
    };
    const stream::StreamStats& ss = tagStream_.Stats();
    HBE_INFO("=== --tagstreamtest RESULTS (SYNTHETIC world, not a real project) ===");
    HBE_INFO("  shards: {} streamed, peak {} resident at once, {} always-resident row(s)",
             tagStream_.ShardCount(), ss.residentPeak, tagStream_.ResidentRowCount());
    HBE_INFO("  spawned {} time(s), despawned {} time(s) over {} frame(s)", ss.spawns,
             ss.despawns, ss.framesUpdated);
    HBE_INFO("  staging: {} async (job system), {} synchronous; {} deferred finalize(s), "
             "{} failure(s)",
             ss.asyncStages, ss.syncStages, ss.deferredFinalizes, ss.failures);
    HBE_INFO("  policy evaluations: {} over {} frame(s) = 1 per {:.1f} frames; "
             "{:.4f} ms avg, {:.4f} ms worst",
             ss.evaluations, ss.framesUpdated,
             ss.evaluations ? static_cast<f64>(ss.framesUpdated) / ss.evaluations : 0.0,
             ss.evaluations ? ss.totalEvalMs / ss.evaluations : 0.0, ss.maxEvalMs);
    HBE_INFO("  structural work (finalize/despawn): {:.3f} ms total, {:.3f} ms worst single, "
             "{:.4f} ms amortised per frame",
             ss.totalStructuralMs, ss.maxStructuralMs,
             ss.framesUpdated ? ss.totalStructuralMs / ss.framesUpdated : 0.0);
    const auto window = [&](const char* label, const std::vector<f32>& v) {
        HBE_INFO("  {}: {:.2f} ms mean, {:.2f} p50, {:.2f} p99, {:.2f} max over {} frames",
                 label, mean(v), pct(v, 0.5f), pct(v, 0.99f), pct(v, 1.0f), v.size());
    };
    HBE_INFO("  frame time (all windows post-warmup, RAW per-frame deltas):");
    window("A  parked, nothing resident   ", tagStreamIdleMs_);
    window("B  parked inside one island   ", tagStreamSteadyMs_);
    window("C  sweeping (streaming live)  ", tagStreamSweepMs_);
    HBE_INFO("  Read B vs C for the cost of STREAMING (same kind of content, activity is the "
             "only difference). A vs B is scene content, not streaming - do not read it as "
             "overhead.");
    HBE_WARN("  Reminder: this is a world built to be streamed. It says the FEATURE works. "
             "It says nothing about any real project's frame time.");
}

void Engine::LoadGameplayScene(const std::filesystem::path& scenePath) {
    if (!scene_ || !renderer_ || scenePath.empty()) return;
    // Switching mid-play (the dev-menu "skip to zone") must not strand a running
    // conversation or a cutscene owning the camera: the load below destroys their
    // entities, so tear down the narrative runtime first. Found the hard way -
    // mirrors LoadGameplayWorld / LoadGame / FlowMainMenu.
    ResetDialogueRuntime();
    ClearCutscene();
    game::ClearTransientQueues(); // don't let a queued death/noise/spot fire into the new scene
    // BindLevel is the level transition: it captures the outgoing level (resident set
    // AND every resident shard) before scene::BindWorld destroys it, then binds and
    // enters the destination, replaying whatever was captured there on a previous
    // visit. Without that a scene always reloads in its AUTHORED state - looted crates
    // refill, dead NPCs return. A failed parse leaves the current world untouched.
    static const std::vector<TagDef> kNoTags; // a real object: a ternary would copy
    const std::filesystem::path assets =
        Project::HasActive() ? Project::Active().AssetsDir() : std::filesystem::path();
    const std::vector<TagDef>& tags =
        Project::HasActive() ? Project::Active().Settings().tags : kNoTags;
    if (!tagStream_.BindLevel(*scene_, *renderer_, scenePath, assets, tags)) {
        HBE_WARN("LoadGameplayScene: '{}' failed to load.", scenePath.string());
        return;
    }
    currentScenePath_ = scenePath;
    // No nav step: GridNav auto-rebuilds from the new static geometry each frame.
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
    // CAPTURE BEFORE SNAPSHOTTING. Every resident shard's runtime deltas (and the
    // always-resident set's) go into world::State, which game::SerializeState writes
    // below - so the shards the snapshot deliberately omits can be rebuilt from the
    // level file plus these blobs. Nothing is destroyed.
    tagStream_.CaptureAllLoaded(*scene_);
    nlohmann::json j;
    // FORMAT VERSION 2: the snapshot no longer contains streamed-shard entities, and
    // "shards" records which shards were standing. A v1 save (no "version", or 1) still
    // loads - see LoadGame - as a complete whole-world snapshot.
    j["version"] = 2;
    // EXCLUDE streamed-shard members from the snapshot. They come back by respawning
    // their shard from the level file and replaying the blobs above, which is what keeps
    // a save bounded: otherwise every NPC a continuous spawner ever emitted is baked in
    // as an authored entity - and then the restored Spawner bursts again on top of them,
    // because Spawner::maxAlive defaults to 0 (uncapped).
    //
    // This is only correct because StreamShard is TRANSITIVE: the streamer stamps it on
    // everything a shard spawn created, and spawn::DoBurst copies it onto each spawned
    // root. A non-transitive membership tag here would leave the spawned population in
    // the snapshot and the authored NPCs out of it - the worst of both.
    const entt::registry& sreg = scene_->Registry();
    const auto notStreamed = [&sreg](entt::entity e) { return !sreg.all_of<StreamShard>(e); };
    j["scene"] = scene::SaveSceneToString(*scene_, notStreamed);
    j["game"] = game::SerializeState();             // objectives + checkpoints + world state
    j["level"] = currentScenePath_.string(); // the .hbscene the world came from (and re-binds to)
    j["shards"] = tagStream_.ResidentKeys(); // "<tag>#<index>", sorted
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
    const u32 version = j.value("version", 1u);
    // The OUTGOING level's binding is gone: the Replace below destroys its world. Drop
    // it without capturing - a load is not a level exit, and capturing here would write
    // the world the player is abandoning over the state they are restoring.
    tagStream_.Reset(scene_);
    // GAMEPLAY STATE FIRST, snapshot second. game::DeserializeState is what restores
    // world::State, and the shard respawns below replay their deltas out of it - so it
    // has to be populated before anything spawns. (It was second before because nothing
    // respawned.) The snapshot itself needs none of it: its runtime fields are baked in.
    game::DeserializeState(j.value("game", std::string()));
    scene::StagedAssets staged;
    scene::StageAssets(data, Project::Active().AssetsDir(), staged);
    scene::Instantiate(*scene_, *renderer_, data, staged, scene::LoadMode::Replace);
    // Make the area CURRENT again (so the World schematic nodes resolve "" to here)
    // WITHOUT replaying its deltas or bumping the visit count: the snapshot already
    // carries every runtime field of what it contains, and the player did not re-enter
    // the level, they reloaded inside it. BindMode::AdoptWorld is exactly that contract -
    // bind the shard table and the area over a world that is already standing, destroy
    // nothing, instantiate nothing.
    if (const std::string lvl = j.value("level", std::string()); !lvl.empty()) {
        currentScenePath_ = lvl;
        const std::filesystem::path assets = Project::Active().AssetsDir();
        if (tagStream_.BindLevel(*scene_, *renderer_, currentScenePath_, assets,
                                 Project::Active().Settings().tags,
                                 stream::BindMode::AdoptWorld)) {
            // Which shards were standing. A v2 save says so; a v1 save does not have the
            // key, and does not need it - its snapshot carried the shard members
            // themselves, so AdoptResidency finds and adopts them instead of spawning
            // anything. Either way nothing is spawned twice.
            std::vector<std::string> keys;
            if (const auto it = j.find("shards"); it != j.end() && it->is_array())
                for (const nlohmann::json& k : *it)
                    if (k.is_string()) keys.push_back(k.get<std::string>());
            tagStream_.AdoptResidency(*scene_, *renderer_, keys);
        } else {
            // The level file is gone or unparseable. The snapshot world is intact and
            // playable; it just cannot stream for the rest of this session.
            HBE_WARN("LoadGame: cannot re-bind level '{}'; the restored world will not "
                     "stream this session.",
                     currentScenePath_.string());
            world::SetCurrentArea(world::AreaIdFromPath(currentScenePath_));
        }
    } else {
        // A save with no "level" (an early checkpoint - the key was write-only until
        // now). Nothing to bind; the snapshot is the whole world, exactly as before.
        HBE_WARN("LoadGame: save has no 'level'; restored as a complete snapshot with no "
                 "streaming (a fresh save will record it).");
    }
    if (version < 2)
        HBE_INFO("LoadGame: save format v{} - a complete whole-world snapshot. Its shard "
                 "members were ADOPTED, so streaming works from here on.",
                 version);
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

// All entities inside a UIPanel subtree (root included). Scopes the loading
// bar/wheel drivers to the "Loading" panel so gameplay HUD ProgressBars are untouched.
//
// Takes the ROOT ENTITY rather than a name on purpose: the caller resolves it
// through UIManager, which is scoped to the resident SCREEN SET. The old
// by-name scan took the first matching UIPanel in the WHOLE registry, which with
// one document per screen (plus a boot splash that may also define a "Loading"
// panel) is a coin flip over which subtree drives the progress bar.
void CollectPanelSubtree(Scene& scene, entt::entity root,
                         std::vector<entt::entity>& out) {
    auto& reg = scene.Registry();
    if (root == entt::null || !reg.valid(root)) return;
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

void Engine::AuditScreenActions(Scene& scene) {
    if (uiDocs_.empty()) return;
    auto& reg = scene.Registry();
    // Only the actions the ENGINE resolves globally. A `rebind:*` or a schematic
    // selector may legitimately repeat across screens (two screens can both have
    // a "back" of their own... except "back" IS a flow verb, so it is listed and
    // duplicating it really is a bug), whereas a settings binding or the caption
    // sink is a single named channel by construction.
    // ONE definition, in ui::IsGlobalAction. This list used to exist three times
    // (here, --test-uiscreens, the migrator's collision report); they agreed, but
    // adding a verb to one would have silently weakened the other two.
    std::unordered_map<std::string, u32> seen;
    for (const entt::entity e : reg.view<UIElement>()) {
        const UIDocMember* m = reg.try_get<UIDocMember>(e);
        if (!m) continue;
        if (std::find(uiDocs_.begin(), uiDocs_.end(), m->doc) == uiDocs_.end()) continue;
        const std::string& a = reg.get<UIElement>(e).action;
        if (a.empty()) continue;
        if (ui::IsGlobalAction(a)) ++seen[a];
    }
    for (const auto& [action, n] : seen) {
        if (n <= 1) continue;
        HBE_WARN("UI: action '{}' appears {} times across the resident screen "
                 "documents. Every consumer of UIElement::action addresses it BY "
                 "STRING over the whole registry, so all {} will fire/seed/write.",
                 action, n, n);
    }
}

void Engine::BindMenuWorld() {
    menuWorldBound_ = false;
    menuCamEntity_ = entt::null;
    if (!scene_ || !renderer_ || !Project::HasActive()) return;
    const ProjectSettings& s = Project::Active().Settings();
    if (!s.menuWorld || s.startupScene.empty()) return;
    const std::filesystem::path assets = Project::Active().AssetsDir();
    const std::filesystem::path gp = assets / s.startupScene;
    if (!vfs::Exists(gp) ||
        !tagStream_.BindLevel(*scene_, *renderer_, gp, assets, s.tags,
                              stream::BindMode::MenuWorld)) {
        // Fall back to the classic flat menu - a missing scene must cost the
        // backdrop, never the menu itself.
        HBE_WARN("Flow: menuWorld is on but '{}' could not bind; using the flat menu.",
                 s.startupScene);
        tagStream_.Reset(scene_);
        return;
    }
    menuWorldBound_ = true;
    if (!s.menuCamera.empty()) {
        menuCamEntity_ = scene_->FindByName(s.menuCamera);
        if (menuCamEntity_ == entt::null ||
            !scene_->Registry().all_of<CameraComponent>(menuCamEntity_)) {
            HBE_WARN("Flow: menuCamera '{}' not found (or has no CameraComponent); "
                     "the scene's primary camera frames the menu instead.",
                     s.menuCamera);
            menuCamEntity_ = entt::null;
        }
    }
    // FORCE THE MENU SET RESIDENT. A menu's 3D geometry is not something distance
    // should decide - the menu camera may sit nowhere near it, and "the set is
    // missing until you happen to be close" is never what an author means. Forcing
    // it also pulls in whatever that tag ASSOCIATES (ShardForce::Resident adds the
    // tag to the association seed set), so a menu set built from several tags works
    // by authoring one association rather than listing them here.
    u32 forced = 0;
    if (!s.menuTag.empty()) {
        for (u32 i = 0; i < static_cast<u32>(tagStream_.ShardCount()); ++i) {
            // ShardKey is "<tag>#<index>"; the tag is everything before the '#'.
            const std::string key = tagStream_.ShardKey(i);
            const usize hash = key.rfind('#');
            if (hash != std::string::npos && key.compare(0, hash, s.menuTag) == 0) {
                tagStream_.SetShardForce(i, stream::ShardForce::Resident);
                ++forced;
            }
        }
        if (forced == 0) {
            // Not fatal: an alwaysLoaded menu tag has no shards to force (it is
            // already resident), and so does a tag whose content lives in another
            // file. Say so rather than leaving the author wondering.
            HBE_WARN("Flow: menuTag '{}' matched no streamed shard in '{}'. If that tag "
                     "is alwaysLoaded it is already resident; otherwise check the tag "
                     "name and that the scene has been saved since tagging (the shard "
                     "table is baked on save).",
                     s.menuTag, s.startupScene);
        }
    }
    HBE_INFO("Flow: menu backdrop bound ('{}'{}{}).", s.startupScene,
             menuCamEntity_ != entt::null ? ", menu camera '" + s.menuCamera + "'"
                                          : ", primary camera",
             forced ? ", " + std::to_string(forced) + " shard(s) of menuTag '" +
                          s.menuTag + "' forced resident"
                    : "");
}

void Engine::FlowMainMenu() {
    if (!flowActive_ || !scene_ || !renderer_ || !Project::HasActive()) return;
    if (!uiManagerMode_) return; // no menu concept without a UI scene
    // LEAVE THE LEVEL THROUGH THE STREAMER FIRST (plan blocker B7). The sweep below
    // destroys entities; doing that to a resident shard without capturing it would
    // silently discard every delta the player made in it - the door they opened, the
    // guard they killed - so quit-to-menu-and-Play-again would reset the world. UnloadAll
    // captures the resident set AND every resident shard, drains any staging job (so no
    // worker is left writing into a level that is going away), and then despawns.
    // Reset() forgets the binding; the next FlowPlay binds afresh.
    tagStream_.UnloadAll(*scene_);
    tagStream_.Reset(scene_);
    // Unload the gameplay world but keep the resident UI, then show the initial
    // (menu) panel. No scene swap. The spare predicate is IDENTICAL to
    // scene::Instantiate's Replace sweep - Persistent (the runtime decoration and
    // the legacy UI layer) plus UIDocMember::screenOwned (an open `.hbui`). The
    // flag lives in the component so both sites can evaluate the same test.
    auto& reg = scene_->Registry();
    std::vector<entt::entity> kill;
    for (const entt::entity e : reg.storage<entt::entity>()) {
        const UIDocMember* m = reg.try_get<UIDocMember>(e);
        if (!reg.all_of<Persistent>(e) && !(m && m->screenOwned)) kill.push_back(e);
    }
    for (const entt::entity e : kill)
        if (reg.valid(e)) reg.destroy(e);
    currentScenePath_.clear(); // no gameplay world resident any more
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
    // 3D menu: rebind the AUTHORED backdrop (MenuWorld mode - the teardown above
    // already captured the gameplay run; this bind writes no world:: state at all).
    //
    // ONLY WHEN NOTHING IS BOUND. Two paths reach here: quit-to-menu, where the
    // sweep above just destroyed the world and a rebind is the point; and
    // FlowAfterBoot, where the splash ALREADY bound it. Rebinding at boot would
    // UnloadAll and rebuild the exact world the splash spent its time loading -
    // undoing the load-behind-the-splash this feature exists for. FlowPlay clears
    // the flag, so the quit-to-menu path still binds.
    if (!menuWorldBound_) BindMenuWorld();
    uiManager_.ShowInitial(*scene_);
    SetCursorLocked(false);
    gameState_ = GameState::MainMenu;
    // Returning from gameplay: the level's post is still installed (this path
    // unloads the world without a scene swap), so restore the menu's own look.
    ApplyMenuPost();
    loadTimer_ = 0.0f;
    // Trace the state the shipped runtime actually reached, and whether the boot
    // splash is really gone (see FlowAfterBoot). Cheap: once per menu entry.
    HBE_INFO("Flow: MainMenu reached - panel '{}' active, boot document {}.",
             uiManager_.Top(), bootDoc_ == 0 ? "closed" : "STILL OPEN (BUG)");
}

void Engine::FlowPlay() {
    if (!flowActive_ || !scene_ || !renderer_ || !Project::HasActive()) return;
    // Leaving a 3D menu: the backdrop is DISPOSABLE. Its bind never entered an
    // area, so it must not be captured; Reset (no capture) forgets it and
    // LoadGameplayWorld's Fresh bind sweeps the leftover entities. Clearing the
    // flag first also restores ApplyMenuPost/streaming/camera to their flat-menu
    // behaviour if Play fails partway.
    if (menuWorldBound_) {
        // Drop the menuTag force EXPLICITLY before forgetting the binding. Reset
        // frees the shard vector the forces are indexed by, so this is belt and
        // braces - but a force that survived into the gameplay bind would pin that
        // tag resident for the whole run, and the symptom (one zone that never
        // streams out) looks nothing like its cause.
        tagStream_.ClearShardForces();
        tagStream_.Reset(scene_);
        menuWorldBound_ = false;
        menuCamEntity_ = entt::null;
    }

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
    // DROP THE OUTGOING BINDING WITHOUT CAPTURING IT, BEFORE clearing game state. A
    // restart (death respawn, F2, dev-menu "Restart level") reaches here with the
    // streamer still bound to the run being abandoned, and BindLevel's own
    // "binding while bound is a level transition" rule would then run UnloadAll -
    // capturing that run's world deltas INTO the store game::Reset() just cleared, and
    // replaying them into the fresh run. The restarted level would keep every guard the
    // player killed dead, every door they opened fired, every pickup looted, and
    // `visits` at 1 - and a door whose Interactable::fired is restored true while the
    // flag it set was cleared is a hard soft-lock. A restart is not a level exit.
    // (The main-menu path already does UnloadAll + Reset itself; LoadGame does the same
    // for the same reason.)
    tagStream_.Reset(scene_);
    game::Reset(); // a fresh run: clear objectives + reached checkpoints
    // Stop any dialogue/captions left over from a prior run (the runner state
    // is an Engine member, so it survives the scene Replace below).
    ResetDialogueRuntime();
    // Stop any in-progress cutscene and hand the camera back (else it stays
    // disabled into the new run).
    ClearCutscene();
    // Drop anything the previous run queued but never drained: a death/noise/spot
    // event or a UI/music command surviving the swap fires into the FRESH world,
    // against entity ids that now mean something else. This is the level-TRANSITION
    // teardown - the comment on the old level loader recorded that it was found the
    // hard way, and it belongs here now that a transition is a plain scene load.
    game::ClearTransientQueues();
    const std::filesystem::path assets = Project::Active().AssetsDir();
    const ProjectSettings& s = Project::Active().Settings();

    // BIND THE LEVEL. This is a tag-streaming bind, not a plain Replace-load, and the
    // difference matters even for a level with no tags:
    //   * it clears the previous world and applies the level's environment through
    //     scene::BindWorld - the same code Replace runs, factored out, because a
    //     streamed shard is a SLICE and a slice may never Replace (plan blocker B2);
    //   * it spawns the ALWAYS-RESIDENT slice (untagged entities + alwaysLoaded tags)
    //     and leaves streamed shards to the distance policy;
    //   * it ENTERS THE AREA and replays the resident set's persisted state, which is
    //     what world::RestoreArea used to do here.
    // A level with zero streaming tags has every row in the resident slice, so this is
    // byte-for-byte the world the old full Replace produced. An UNTRUSTED shard header
    // (stale bake, hand edit) also puts every row in the resident slice: a bad bake
    // costs streaming, never content.
    //
    // This runs BEFORE the loading screen goes down so terrain chunks build/settle
    // behind the loading overlay instead of popping in afterwards.
    bool loaded = false;
    if (!s.startupScene.empty()) {
        const std::filesystem::path gp = assets / s.startupScene;
        // A level is ONE .hbscene; GridNav picks up its static geometry next frame.
        if (vfs::Exists(gp) && tagStream_.BindLevel(*scene_, *renderer_, gp, assets, s.tags)) {
            loaded = true;
            currentScenePath_ = gp;
        }
    }
    if (!loaded) {
        HBE_WARN("Game flow: startup scene '{}' failed to load.", s.startupScene);
        tagStream_.Reset(scene_); // nothing bound; do not leave a half-binding behind
    }
    // (The area entry that used to be a separate world::RestoreArea call is now inside
    // BindLevel - it has to be, because a shard spawn must not bump the visit count and
    // only the bind knows which is which. game::Reset() above cleared the world state
    // for this fresh run, so the replay is a no-op on a new game and only does work
    // when a save was loaded into it first.)

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
    // CLOSE THE BOOT DOCUMENT, EXPLICITLY, BEFORE DISPATCHING. This is the single
    // easiest thing in the whole feature to get wrong.
    //
    // The splash used to be disposed IMPLICITLY, by FlowMainMenu's non-Persistent
    // sweep. Documents are spared by that sweep (that is what makes the UI layer
    // resident), so nothing destroys a boot DOCUMENT any more - it would render
    // permanently on top of the menu.
    //
    // It has to be here, above all three branches, not inside FlowMainMenu:
    //   * FlowMainMenu early-returns when uiManagerMode_ is false, so the
    //     playOnBoot_ and no-UI branches would never reach it;
    //   * both of those go through FlowPlay -> Loading -> LoadGameplayWorld,
    //     whose Replace sweep now spares screenOwned documents too.
    // (A LEGACY `.hbscene` splash has no document handle and is still swept, so
    // this is a no-op for a half-migrated project - which is correct.)
    //
    // The reaped-entity count is LOGGED because this is the one step whose failure
    // is invisible to every automated check inside a shipped build: the splash
    // would simply keep drawing over the menu. `--test-uiflow` asserts it in the
    // editor exe; this line is how you confirm it in the runtime's own log.
    if (bootDoc_ != 0 && scene_) {
        const ui::DocumentInstance* inst = docs_.Get(bootDoc_);
        const u32 reaped = inst ? static_cast<u32>(inst->entities.size()) : 0;
        docs_.Close(*scene_, bootDoc_);
        bootDoc_ = 0;
        HBE_INFO("Boot: boot document CLOSED ({} splash entities reaped).", reaped);
    }
    // Studio splash finished: show the initial (menu) panel when a UI document is
    // loaded, else boot straight into gameplay (the empty-uiDocument fallback).
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

void Engine::DevMenuScanScenes() {
    // Every .hbscene in Assets/ is a candidate "zone": a level is ONE file, so
    // there is nothing to dedup into a base name any more.
    devScenes_.clear();
    if (!Project::HasActive()) return;
    const std::filesystem::path root = Project::Active().AssetsDir();
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file() || it->path().extension() != ".hbscene") continue;
        devScenes_.push_back(it->path());
    }
    std::sort(devScenes_.begin(), devScenes_.end());
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

    // STREAMING. A shipped-build A/B for "is this bug streaming?": turning it off pins
    // every shard loaded (never unloads anything), so if the symptom survives, streaming
    // is not the cause. Only offered when the bound level actually has shards - a row
    // that does nothing is worse than no row.
    if (tagStream_.IsBound() && tagStream_.ShardCount() > 0) {
        header("STREAMING");
        char v[64];
        std::snprintf(v, sizeof(v), "%s (%u/%u resident)", tagStream_.Enabled() ? "on" : "OFF",
                      tagStream_.ResidentShardCount(),
                      static_cast<u32>(tagStream_.ShardCount()));
        value("Tag streaming", v,
              [this](int) { tagStream_.SetEnabled(!tagStream_.Enabled()); });
    }

    if (!devScenes_.empty()) {
        header("SCENES (skip to zone)");
        for (const std::filesystem::path& sp : devScenes_) {
            const std::filesystem::path s2 = sp;
            action(sp.stem().string(),
                   [this, s2] { devMenuOpen_ = false; LoadGameplayScene(s2); });
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
    const std::string level = currentScenePath_.empty()
                                  ? std::string("(none)")
                                  : currentScenePath_.stem().string();
    glm::vec3 ppos(0.0f);
    auto pv = scene_->Registry().view<Transform, CharacterController>();
    if (pv.begin() != pv.end()) ppos = glm::vec3(scene_->WorldMatrix(*pv.begin())[3]);
    std::string obj = game::CurrentObjectiveText();
    if (obj.empty()) obj = "(none)";
    const Renderer::FrameStats& rs = renderer_->Stats();
    // WHAT IS CURRENTLY RESIDENT, in the shipped build. Streaming's whole visible
    // effect is which shards exist right now, and in a shipped runtime this overlay is
    // the only place to see it. "off" is spelled out because a pinned-loaded world looks
    // identical to a working one until the player walks somewhere.
    std::string streamLine = "Streaming: (no level bound)";
    if (tagStream_.IsBound()) {
        if (tagStream_.ShardCount() == 0)
            streamLine = std::format("Streaming: none - all {} row(s) resident{}",
                                     tagStream_.ResidentRowCount(),
                                     tagStream_.Trusted() ? "" : " (shard table UNTRUSTED)");
        else
            streamLine = std::format("Streaming: {}/{} shard(s) resident  |  {} spawn / {} "
                                     "despawn  |  {}",
                                     tagStream_.ResidentShardCount(), tagStream_.ShardCount(),
                                     tagStream_.Stats().spawns, tagStream_.Stats().despawns,
                                     tagStream_.Enabled() ? "on" : "OFF (all pinned loaded)");
    }
    char buf[768];
    std::snprintf(buf, sizeof(buf),
                  "== DEV MENU ==  (Ctrl+` close  -  arrows move  -  Enter/<> use)\n"
                  "FPS %.0f  |  %.2f ms  |  Draws %u/%u (%u culled)\n"
                  "State: %s   Level: %s   Player %.1f,%.1f,%.1f\n"
                  "%s\n"
                  "Objective: %s",
                  fps, dt_ * 1000.0f, rs.drawn, rs.total, rs.culled, st, level.c_str(), ppos.x,
                  ppos.y, ppos.z, streamLine.c_str(), obj.c_str());
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
        else if (el.action == "setting:subtitles") el.toggled = userSettings_.subtitlesEnabled;
        else if (el.action == "setting:speakernames") el.toggled = userSettings_.speakerNames;
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
            if (audio_)
                audio_->SetCaptionsEnabled(userSettings_.subtitlesEnabled ||
                                           userSettings_.captionsEnabled);
        } else if (el.action == "setting:subtitles") {
            userSettings_.subtitlesEnabled = el.toggled;
            if (audio_)
                audio_->SetCaptionsEnabled(userSettings_.subtitlesEnabled ||
                                           userSettings_.captionsEnabled);
        } else if (el.action == "setting:speakernames") {
            userSettings_.speakerNames = el.toggled;
        } else {
            return;
        }
        settingsDirty_ = true; // flushed to disk on Back / quit
    });
}

void Engine::ReportBenchmark(std::vector<f32>& frameMs, const Renderer& renderer,
                             const std::string& csvPath) {
    if (frameMs.empty()) {
        HBE_WARN("Benchmark: no frames sampled.");
        return;
    }
    // Optional raw CSV first (before sorting destroys frame order).
    if (!csvPath.empty()) {
        std::ofstream csv(csvPath, std::ios::trunc);
        if (csv) {
            csv << "frame,ms\n";
            for (usize i = 0; i < frameMs.size(); ++i) csv << i << ',' << frameMs[i] << '\n';
            HBE_INFO("Benchmark: wrote {} frame times to '{}'.", frameMs.size(), csvPath);
        } else {
            HBE_WARN("Benchmark: could not write CSV '{}'.", csvPath);
        }
    }

    f64 sum = 0.0;
    for (const f32 ms : frameMs) sum += ms;
    const f64 mean = sum / static_cast<f64>(frameMs.size());

    std::vector<f32> sorted = frameMs;
    std::sort(sorted.begin(), sorted.end());
    const auto pct = [&sorted](f64 p) {
        const usize i = static_cast<usize>(p * static_cast<f64>(sorted.size() - 1) + 0.5);
        return sorted[std::min(i, sorted.size() - 1)];
    };

    // 1% LOW is reported as an FPS because that is the number a player feels;
    // a good mean with a bad 1% low is a stuttery game that benchmarks well.
    const f32 p50 = pct(0.50), p95 = pct(0.95), p99 = pct(0.99);
    const f32 lowest1 = pct(0.99); // 99th percentile FRAME TIME == 1% low FPS

    const Renderer::FrameStats& rs = renderer.Stats();
    HBE_INFO("================ BENCHMARK ({} frames) ================", frameMs.size());
    HBE_INFO("  mean   {:7.3f} ms  ({:6.1f} FPS)", mean, 1000.0 / std::max(mean, 1e-6));
    HBE_INFO("  median {:7.3f} ms  ({:6.1f} FPS)", p50, 1000.0f / std::max(p50, 1e-6f));
    HBE_INFO("  p95    {:7.3f} ms  ({:6.1f} FPS)", p95, 1000.0f / std::max(p95, 1e-6f));
    HBE_INFO("  p99    {:7.3f} ms  ({:6.1f} FPS)   <- 1% low", p99,
             1000.0f / std::max(lowest1, 1e-6f));
    HBE_INFO("  min    {:7.3f} ms  |  max {:7.3f} ms", sorted.front(), sorted.back());
    HBE_INFO("  draws  {}/{} ({} culled, {} runs x{} instances)", rs.drawn, rs.total, rs.culled,
             rs.instancedDraws, rs.totalInstances);
    HBE_INFO("  shadow {} draws submitted, {} (item,cascade) pairs culled", rs.shadowDraws,
             rs.shadowCulled);
#if defined(_DEBUG)
    HBE_WARN("  *** DEBUG BUILD - CPU timings are NOT representative. ***");
    HBE_WARN("  *** Measure with --config RelWithDebInfo or Release.   ***");
#endif
    HBE_INFO("=======================================================");
}

void Engine::PushSubtitle(subtitle::Line line) {
    // The stack owns gating (speech vs non-speech), dedup and the line cap.
    subtitles_.SetSettings(CurrentSubtitleSettings());
    subtitles_.Push(std::move(line));
}

void Engine::PushCaption(const std::string& text, f32 seconds) {
    subtitle::Line l;
    l.text = text;
    l.kind = subtitle::Kind::Dialogue;
    l.duration = seconds;
    l.priority = 10;
    PushSubtitle(std::move(l));
}

void Engine::ApplyMenuPost() {
    if (!uiScenePostValid_ || !scene_) return;
    // A 3D menu renders the STARTUP SCENE, and scene::BindWorld just applied that
    // scene's authored post/exposure/ambient. Stamping the menu document's post
    // over it would give the world the UI's look - the exact class of clobber
    // ApplyEnvironment exists to prevent.
    if (menuWorldBound_) return;
    // The menu IS the UI scene, so it renders with that scene's authored post.
    // Without this the menu inherited whatever was last stamped into the
    // environment - at boot the PROJECT default, after gameplay the LEVEL's -
    // neither of which the artist edits when authoring the menu, so the shipped
    // menu could show effects (e.g. real brush strokes) that are switched OFF
    // everywhere the artist can see them.
    scene_->Environment().post = uiScenePost_;
}

subtitle::Settings Engine::CurrentSubtitleSettings() const {
    subtitle::Settings s;
    s.subtitles = userSettings_.subtitlesEnabled;
    s.captions = userSettings_.captionsEnabled;
    s.speakerNames = userSettings_.speakerNames;
    return s;
}

void Engine::UpdateCaptions(f32 dt) {
    if (!scene_) return;
    subtitles_.SetSettings(CurrentSubtitleSettings());
    // Drain the audio system's queue. Lines arrive STRUCTURED (speaker/text/kind),
    // so the stack can gate a door-creak caption separately from a spoken line and
    // format each correctly - both used to arrive as one pre-joined string.
    if (audio_) {
        subtitle::Line cap;
        while (audio_->PopCaption(cap)) subtitles_.Push(std::move(cap));
    }
    subtitles_.Update(dt);
    // Drive the dedicated caption element (a Label with action "caption").
    const std::string& block = subtitles_.Composed();
    const bool show = !subtitles_.Empty();
    const f32 longest = subtitles_.LongestRemaining();
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
            // Structured push: speaker stays its own field so the stack formats it
            // and the SUBTITLES setting gates it (a conversation line is speech and
            // must not depend on the non-speech Closed Captions toggle, which is
            // what the old `captionsEnabled` gate here did).
            if (hasText) {
                subtitle::Line line;
                line.speaker = n->speaker;
                line.text = n->text;
                line.kind = subtitle::Kind::Dialogue;
                line.duration = hold;
                line.priority = 20; // conversation outranks barks and ambience
                PushSubtitle(std::move(line));
            }
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
                        for (const entt::entity ne : reg.view<Name>()) // NOLINT: needs a filtered match
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
    subtitles_.Clear();
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
                                glm::vec2 anchor, bool pressed) {
    if (!scene_) return;
    entt::registry& reg = scene_->Registry();
    const bool hasIcon = !iconPath.empty();
    // The same 0.72 press multiply a UI Button uses, so a 3D object and a 3D button
    // depress by the same amount - one interaction language, not two.
    constexpr f32 kPressDim = 0.72f;
    const f32 s = pressed ? kPressDim : 1.0f;

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
    lbl.color = glm::vec4(1.0f * s, 0.96f * s, 0.75f * s, 1.0f);
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
        // Dim AND shrink: the icon is usually a bright glyph plate where a 28%
        // multiply alone is easy to miss at prompt size.
        icon.color = glm::vec4(s, s, s, 1.0f);
        icon.size = pressed ? glm::vec2(46.0f, 46.0f) : glm::vec2(52.0f, 52.0f);
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

bool Engine::InteractionsSuppressed() const {
    // Don't offer interactions while a conversation/cutscene is playing OR queued
    // this frame (the deferred queues are single-slot latest-wins, so firing now
    // would clobber a schematic's just-queued convo), or while a menu overlay is
    // open over the HUD (pause/Settings) - the prompt must not draw over it and the
    // E/gamepad key must not fire behind it.
    const bool menuOpen =
        uiManagerMode_ && !uiManager_.Empty() && uiManager_.Top() != "HUD";
    return menuOpen || devMenuOpen_ || actionMap_.Rebinding() || rebindJustCommitted_ ||
           dialogueNode_ != 0 || cutsceneTime_ >= 0.0f || game::DialoguePending() ||
           game::CutscenePending();
}

bool Engine::InteractableAvailable(Scene& scene, entt::entity e) const {
    const entt::registry& reg = scene.Registry();
    const Interactable* ia = reg.valid(e) ? reg.try_get<Interactable>(e) : nullptr;
    if (!ia) return false;
    if (ia->once && ia->fired) return false;
    if (!ia->requiredFlag.empty() && game::GetFlag(ia->requiredFlag) == 0.0f) return false;
    return true;
}

void Engine::UpdateInteractions(Scene& scene, f32 dt) {
    (void)dt;
    entt::registry& reg = scene.Registry();
    if (InteractionsSuppressed()) {
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

    // THE CANDIDATE COMES FROM THE SHARED PICK PASS - this function no longer does
    // its own selection. Previously it was a pure RADIUS test with no ray, no
    // occlusion and no facing, so an NPC through a wall two metres away prompted
    // and fired, and turning your back on one kept the prompt up. interact::Pick
    // ran at the top of the frame with the same ray the world-UI pages used:
    //   * aim-first - what the reticle is on wins, so a 3D button works;
    //   * proximity fallback - "walk up to an NPC and press E" still works when you
    //     are not aiming at anything, but is now occlusion-filtered too;
    //   * and if a world UI page was NEARER, `pick_` is a Page and no object is
    //     offered at all. Exactly one affordance, never two.
    entt::entity best = entt::null;
    if (pick_.kind == interact::Hit::Kind::Object && reg.valid(pick_.entity) &&
        reg.all_of<Interactable>(pick_.entity))
        best = pick_.entity;
    if (best == entt::null) { HideInteractPrompt(); return; }
    const std::string bestPrompt = reg.get<Interactable>(best).prompt;
    const glm::vec3 bestCenter = worldCenter(best);

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
    // HOVER / PRESS / RELEASE on a whole object. `held` is read BEFORE the fire
    // below so the frame of the press already draws pressed; release is this going
    // false on a later frame, which restores the idle prompt.
    const bool held = input_ && actionMap_.Down(*input_, "Interact");
    ShowInteractPrompt(label, icon, anchor, held);

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

void Engine::SetPaused(bool on) {
    // Only Playing can be paused. Guarding here rather than at each call site means a
    // stray Pause press during a load or on the menu is a no-op instead of freezing a
    // state machine that has no way to resume itself.
    if (on && gameState_ != GameState::Playing) return;
    if (paused_ == on) return;
    paused_ = on;
    if (!scene_) return;
    // Push/pop the project's "Pause" panel when it has one. Has() rather than an
    // assumption: pause must work in a project that has not authored the menu yet
    // (it still freezes and frees the cursor), otherwise the feature looks broken
    // rather than un-authored.
    if (uiManagerMode_ && uiManager_.Has(*scene_, "Pause")) {
        if (on) uiManager_.Push(*scene_, "Pause");
        else if (uiManager_.Top() == "Pause") uiManager_.Pop(*scene_);
    }
    HBE_INFO("Flow: {}", on ? "paused" : "resumed");
}

void Engine::TogglePause() { SetPaused(!paused_); }

void Engine::SkipCutscene() {
    if (cutsceneTime_ < 0.0f) return;
    HBE_INFO("Cutscene: skipped by the player at {:.2f}s / {:.2f}s.", cutsceneTime_,
             cutscene_.duration);
    ClearCutscene();          // restores the game camera
    ResetDialogueRuntime();   // drops the conversation/subtitles the cutscene put up
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
                CollectPanelSubtree(*scene_,
                                    uiManager_.PanelEntity(*scene_, "Loading"),
                                    loadingPanelEntities_);
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

            // Hold until the world has materialized: terrain chunks built AND every
            // in-range streaming shard actually instantiated. Then nothing pops in when
            // we reveal.
            //
            // The streaming half carries all four salvaged IsSettled clauses, and the
            // one that matters most here is "Ready is NOT settled": a shard whose assets
            // finished staging but which the main thread has not instantiated yet would
            // otherwise let the screen drop exactly one frame before its geometry exists
            // - the pop the screen is there to hide. That clause is also what makes the
            // one-finalize-per-frame budget safe: deferred finalizes still hold the
            // screen up. Failed shards are deliberately ignored (waiting on a broken
            // shard would hang the screen forever).
            const bool terrainReady = terrain::IsSettled(*scene_);
            const bool streamReady =
                tagStream_.IsSettled(StreamFoci(*scene_, *renderer_));
            const bool ready = terrainReady && streamReady;

            const f32 p = (ready && loadTimer_ >= kLoadDuration)
                              ? 1.0f
                              : glm::clamp(loadTimer_ / kLoadDuration, 0.0f, 0.9f);
            SetProgressFill(*scene_, p, &loadingPanelEntities_);
            SubstituteUITokens(p); // {progress} on the game loading screen

            const bool timedOut = loadTimer_ >= kMaxLoadDuration;
            if ((ready && loadTimer_ >= kLoadDuration) || timedOut) {
                if (timedOut && !ready)
                    HBE_WARN("Loading: revealing after {:.0f}s cap (terrain={}, streaming={}, "
                             "{}/{} shard(s) resident) - something never settled.",
                             kMaxLoadDuration, terrainReady, streamReady,
                             tagStream_.ResidentShardCount(), tagStream_.ShardCount());
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

        // PAUSE + CUTSCENE SKIP. Both are polled here, in the Playing branch, because
        // both are only meaningful while the game is actually running.
        //
        // Skip takes priority over pause while a cutscene is up: one button press
        // during a cinematic should get out of the cinematic, not open a menu behind
        // it. `devMenuOpen_` suppresses both so the dev overlay's own key handling
        // does not double-fire (the same guard the rebind poll uses).
        if (!devMenuOpen_ && input_) {
            if (actionMap_.Pressed(*input_, "Skip") && CutsceneActive()) {
                SkipCutscene();
            } else if (actionMap_.Pressed(*input_, "Pause")) {
                if (CutsceneActive()) SkipCutscene();
                else TogglePause();
            }
        }

        // Free-cursor policy: any menu panel over the HUD (Settings/Pause - pushed
        // by a button OR a schematic UI node) frees the cursor for point-and-click
        // (including world-space pages); back on the HUD relocks mouse-look.
        // Reconciled per frame so every panel-change path is covered.
        const bool menuOpen =
            uiManagerMode_ && !uiManager_.Empty() && uiManager_.Top() != "HUD";
        // Dialogue choices need point-and-click too, so free the cursor for them.
        // `paused_` is listed explicitly rather than relying on the Pause panel being
        // up: pause works in a project that has not authored one yet, and a paused
        // game holding the cursor captive for mouse-look would be unescapable.
        const bool wantFreeCursor = menuOpen || dialogueChoiceActive_ || paused_;
        if (IsCursorLocked() == wantFreeCursor) SetCursorLocked(!wantFreeCursor);
        const std::string act = PollClickedAction(*scene_);
        // A "rebind:<Action>" button starts listening for the next key/button; the
        // rebind poll (top of the update) captures + persists it.
        if (!devMenuOpen_ && act.rfind("rebind:", 0) == 0) actionMap_.BeginRebind(act.substr(7));
        if (act == "menu")
            FlowMainMenu();
        else if (act == "restart")
            FlowReload();
        else if (act == "resume")
            SetPaused(false); // a Pause panel's Resume button
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
        } else if (arg == "--stress-particles" && i + 1 < argc) {
            config.stressParticles = static_cast<u32>(std::stoul(argv[++i]));
        } else if (arg == "--gpu-particles") {
            config.gpuParticles = true; // stress rig uses GPU vertex expansion
        } else if (arg == "--gpu-sim") {
            config.gpuSimParticles = true; // stress rig simulates in a compute shader
        } else if (arg == "--fixed-dt" && i + 1 < argc) {
            config.fixedDt = std::stof(argv[++i]); // deterministic sim clock for A/B runs
        } else if (arg == "--shadowcascades" && i + 1 < argc) {
            config.forceShadowCascades = std::stoi(argv[++i]); // TEMP perf A/B
        } else if (arg == "--trace") {
            // Per-frame diagnostics (shard spawn/despawn, world-state capture/restore,
            // per-slice instantiate). OFF by default because every log line is flushed
            // unbuffered AND recorded into the boot screen's {log} ring - see Core/Log.h.
            SetTraceEnabled(true);
        } else if (arg == "--gpuprofile") {
            config.gpuProfile = true; // per-pass GPU timestamp breakdown (both backends)
        } else if (arg == "--benchmark" && i + 1 < argc) {
            config.benchmarkFrames = static_cast<u32>(std::stoul(argv[++i]));
            // A present cap would measure the display, not the engine.
            config.vsync = false;
            config.vsyncExplicit = true;
        } else if (arg == "--benchmark-warmup" && i + 1 < argc) {
            config.benchmarkWarmup = static_cast<u32>(std::stoul(argv[++i]));
        } else if (arg == "--benchmark-csv" && i + 1 < argc) {
            config.benchmarkCsv = argv[++i];
        } else if (arg == "--dof") {
            config.forceDof = true;
        } else if (arg == "--motionblur") {
            config.forceMotionBlur = true;
        } else if (arg == "--ssr") {
            config.forceSsr = true;
        } else if (arg == "--autoexposure" || arg == "--autoexp") {
            config.forceAutoExposure = true;
        } else if (arg == "--painterly") {
            config.forcePainterly = true;
            // Optional numeric argument: --painterly 7
            if (i + 1 < argc && argv[i + 1][0] != '-')
                config.forcePainterlyRadius = std::stof(argv[++i]);
        } else if (arg == "--navtest") {
            config.navTest = true;
        } else if (arg == "--navbench") {
            config.navBench = true;
        } else if (arg == "--fracturetest") {
            config.fractureTest = true;
        } else if (arg == "--destructiontest") {
            config.destructionTest = true;
        } else if (arg == "--test-terraincollide") {
            config.terrainCollideTest = true;
        } else if (arg == "--play") {
            config.playOnBoot = true;
        } else if (arg == "--uiworldtest") {
            config.uiWorldTest = true; // world-space UI smoke test (lit page)
        } else if (arg == "--tagstreamtest") {
            // Optional object count; defaults to 600 (enough that each of the four tags
            // is genuinely scattered and bakes to several shards).
            config.tagStreamTest = 600;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                config.tagStreamTest = static_cast<u32>(std::stoul(argv[++i]));
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
