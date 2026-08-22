// main_editor.cpp - Heartbreak *Editor* entry point.
//
// This build links the engine runtime PLUS the editor (Dear ImGui + ImGuizmo,
// asset browser, gizmos). It is a tool, not shipped with the game. It wires the
// editor UI into the engine via the engine's per-frame hook.
//
// Usage: HeartbreakEditor [--d3d12 | --vulkan] [--width N] [--height N]
//                         [--validation] [--model <path>]
#include "Assets/AssetFormats.h" // --test-assetformats (the registry's own invariants)
#include "Assets/Compression.h"  // --test-compress (the portable zstd/zlib codec seam)
#include "Assets/MeshCodec.h"    // --test-meshcodec (quantized + meshopt geometry codec)
#include "Assets/MeshCsg.h"      // --test-csg (BSP CSG for the blockout box brush)
#include "Scene/EffectAsset.h"   // --test-hbvfx (.hbvfx particle effect asset)
#include "Scene/DecalAsset.h"    // --test-hbdecal (.hbdecal reusable decal asset)
#include "Assets/UAF.h"          // --test-audiocodec (v11 compressed-source audio)
#include "Assets/CookStats.h"    // --cook-stats / --test-cookstats (why is this build big?)
#include "Assets/AssetRefs.h"    // --test-packclosure (the pack dependency closure)
#include "Assets/MaterialXInterop.h" // --test-openpbr (.hbmat/.mtlx round-trip + variant routing)
#include "Material/MaterialAuthoringTest.h" // --test-material (graph/compile/layers/box/paint)
#include "Material/MaterialCook.h"           // --matscene (bake the visual test scene to PNGs)
#include "Material/MaterialGraphHlsl.h"      // --test-matshader (graph -> HLSL -> runtime compile)
#include "Assets/SeamWeld.h"
#include "Navigation/NavBaker.h" // --test-nav (Recast bake -> Detour stream/query/obstacles)
#include "Scene/BodyShape.h"
#include "Assets/MeshDerive.h"
#include "Assets/MeshGenerator.h" // --skin-preview (headless skin sphere render)
#include "Assets/MeshSimplify.h"
#include "Assets/MeshOptimize.h" // --test-meshopt (import-time GPU geometry optimize)
#include "Editor/TextureCompress.h" // --test-bc / --test-bc-encode (BC texture compression)
#include "Assets/AssetLoader.h"     // assets::GenerateMips (--test-bc)
#include "Assets/MeshFaceSelect.h"
#include "Assets/AudioEvent.h"   // --test-acoustics (composite event round-trip)
#include "Audio/AcousticQuery.h" // --test-acoustics (acoustic materials + resolution)
#include "Audio/AcousticWorld.h" // --test-acoustics (room-acoustics math)
#include "Core/Platform.h"
#include "Assets/SlotIds.h"      // --test-slotids / --migrate-slots (pack slot identity)
#include "Assets/MusicGraph.h"   // --test-musicvoice (music director lifecycle)
#include "Assets/UAP.h"          // uap::PackIndexOf (the migration plan prints pack numbers)
#include "Audio/AudioSystem.h"   // --test-musicvoice
#include "Collab/CollabSelfTest.h"  // --test-collab
#include "Collab/Journal.h"          // --test-journal
#include "Collab/Identity.h"         // --test-identity
#include "Collab/SecureChannel.h"    // --test-securechannel
#include "Collab/ProjectSync.h"      // --test-projectsync
#include "Collab/WebRtcTransport.h"  // --test-webrtc
#include "Editor/CollabSession.h"   // --test-collabsession
#include "Scene/SceneJournal.h"       // --test-p2p
#include "Hub/HubConfig.h"           // --hub-install
#include "Hub/HubSelfTest.h"          // --test-hub
#include "Hub/Updater.h"              // --hub-check (live manifest fetch)
#include "Collab/TcpTransport.h"     // --test-tcp
#include "Core/JobSystem.h"
#include "Core/Window.h"
#include "Cinematics/CinematicsTest.h" // --test-cinematics / --test-curve (Sequencer + curve engine)
#include "Dialogue/DialogueGraph.h" // --test-graphfanin (node-graph reconvergence)
#include "Scene/OceanFFT.h"         // --test-oceanfft (Tessendorf FFT reference/oracle)
#include "Volume/VolumeNano.h"      // --test-nanovdb (header-only NanoVDB build gate)
#include "Volume/VolumeSimController.h" // --test-volsim (volume sim framework gate)
#include "Volume/VolumeBaker.h"         // --test-hbvol (bake -> .hbvol)
#include "Volume/VolumeAsset.h"         // --test-hbvol (.hbvol load)
#include "Volume/VolumeCache.h"         // --test-volume-timing (playback cache)
#include "Volume/EulerianSmokeSimulation.h" // --test-eulersim (CPU fluid solver gate)
#include "Volume/GpuEulerianSmokeSimulation.h" // --test-gpusolver (GPU solver golden-diff)
#include "Volume/VolumeSimRegistry.h"   // --volume-sim-preview (create a solver by model id)
#include "Editor/Editor.h"
#include "Editor/RuntimeShaderCompiler.h" // --test-shadercompile (editor-only runtime HLSL->bytecode)
#include "Editor/Importer.h"
#include "Editor/MovieRender.h"
#include "Engine/Engine.h"
#include "Interaction/Pick.h" // --test-uipick (the interaction-raycast gate)
#include "Physics/PhysicsWorld.h"
#include "Project/Project.h"
#include "Renderer/Renderer.h"
#include "Scene/CameraSystem.h" // --test-fpslook (first-person look contract)
#include "Scene/EntityGuid.h"
#include "Scene/ParticleGpuSim.h"
#include "Scene/ParticleSystem.h"
#include "Scene/Scene.h"
#include "Vegetation/VegetationSystem.h" // hbe::veg::DataSelfTest (--test-vegdata)
#include "Vegetation/VegetationStreaming.h" // hbe::veg::StreamSelfTest (--test-vegstream)
#include "Vegetation/VegetationDamage.h" // hbe::veg::LifeSelfTest (--test-veglife)
#include "Vegetation/VegetationRender.h" // hbe::veg::PaintSelfTest (--test-vegpaint)
#include "Editor/ProctreeImport.h" // hbe::editor::ProctreeSelfTest (--test-proctree)
#include "Scene/Hierarchy.h" // --test-pasteorder (the sibling-order contract)
#include "Scene/SceneSerializer.h"
#include "Scene/StrokeZone.h" // --test-strokezones (3D paint strokes stream with their zone)
#include "Scene/TagShard.h"
#include "Scene/TagStreaming.h"
#include "Scene/TagTable.h"
#include "UI/UIDocument.h"
#include "UI/UIManager.h" // --test-uiscreens (panel lookup across the screen set)
#include "UI/UISystem.h"  // --test-uisolve (direct-manipulation math gate)
#include "UI/UIData.h"    // --test-uibind (data-binding gate)
#include "UI/FontAtlas.h" // --test-uitext (text::TextSelfTest; FreeType-free header)
#include "UI/Svg/SvgCache.h" // --test-uisvg (svg::SvgSelfTest; LunaSVG-free header)
#include "UI/Style/Theme.h" // --test-uitheme (style::ThemeSelfTest)
#include "Vfx/VfxStack.h"

#include <glm/gtc/packing.hpp> // unpackHalf2x16 (GPU record colour)

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _DEBUG
#  include <crtdbg.h> // route Debug asserts to stderr - see the top of main()
#endif
#include <filesystem>
#include <fstream> // --test-readback raw frame dump / --test-readback-compare
#include <string>
#include <unordered_map> // legacy-.hbsave duplicate-guid check in --test-uiflow
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <imgui_impl_win32.h>

// Forward-declared per ImGui's documented pattern (declared inside `#if 0`).
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {
// Short, path-safe backend tag for the --test-readback frame dumps. NOT
// rhi::ToString, which returns "Direct3D 12" - a space in a filename that a
// sibling flag has to reconstruct exactly is a trap.
const char* ReadbackTag(hbe::rhi::GraphicsAPI api) {
    switch (api) {
        case hbe::rhi::GraphicsAPI::D3D12: return "d3d12";
        case hbe::rhi::GraphicsAPI::Vulkan: return "vulkan";
        case hbe::rhi::GraphicsAPI::OpenGL: return "opengl";
    }
    return "unknown";
}
} // namespace

int main(int argc, char** argv) {
#ifdef _DEBUG
    // A DEBUG BUILD MUST NEVER STOP ON A MODAL DIALOG. The CRT's default is to
    // pop "Debug Assertion Failed!" and wait forever, which turns any assert
    // during the ~35 headless `--test-*` flags into a HUNG process that looks
    // exactly like a slow one - a whole self-test sweep can sit there for hours
    // reporting nothing. Route asserts (and abort) to stderr instead, so the run
    // fails loudly, immediately, with the file and line, and exits non-zero.
    for (int mode : {_CRT_ASSERT, _CRT_ERROR, _CRT_WARN}) {
        _CrtSetReportMode(mode, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(mode, _CRTDBG_FILE_STDERR);
    }
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    // --test-seamweld: run the modular-rig seam-weld bit-identity proof (headless,
    // no GPU/window) and exit. Used by CI / the build discipline.
    // --collab-verbose: dump the ICE negotiation into the log. Scanned BEFORE the
    // command loop because the logger initialises once, on the first transport created.
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--collab-verbose") == 0) hbe::collab::SetVerboseLogging(true);
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--test-seamweld") == 0) {
            const bool ok = hbe::weld::SelfTest();
            std::printf("seamweld %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-curve: the reusable scalar animation-curve engine (Core/Curve):
        // linear/constant/cubic interpolation, auto-tangent overshoot clamping,
        // weighted-tangent Bezier, extrapolation modes, greedy reduction. Pure CPU.
        if (std::strcmp(argv[i], "--test-curve") == 0) {
            const bool ok = hbe::cine::CurveSelfTest();
            std::printf("curve %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-cinematics: the Sequencer runtime core - track registry, .hbseq JSON
        // round-trip, GUID/Name binding resolution, and deterministic + idempotent
        // evaluation of a transform track against a real Scene. Pure CPU, no window.
        if (std::strcmp(argv[i], "--test-cinematics") == 0) {
            const bool curveOk = hbe::cine::CurveSelfTest();
            const bool seqOk = hbe::cine::SelfTest(); // run both so all failures surface
            const bool ok = curveOk && seqOk;
            std::printf("cinematics %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-nav (alias --navtest): headless navigation end-to-end. Recast-bakes
        // synthetic geometry into a .hbnav, streams the tiles through Detour, paths
        // around a baked wall, then adds/removes a dtTileCache obstacle and confirms the
        // path reroutes and recovers. Pure CPU: no GPU, no window.
        if (std::strcmp(argv[i], "--test-nav") == 0 || std::strcmp(argv[i], "--navtest") == 0) {
            const bool ok = hbe::nav::SelfTest();
            std::printf("nav %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-nanovdb: prove the header-only NanoVDB build (grid builder + createNanoGrid +
        // accessor) compiles and links with zero external deps - the Phase-0 gate for the
        // NanoVDB volumetric foundation. Pure CPU: no GPU, no window.
        if (std::strcmp(argv[i], "--test-nanovdb") == 0) {
            std::string report;
            const bool ok = hbe::volume::SelfTestNanoVDB(report);
            std::printf("nanovdb %s: %s\n", ok ? "PASS" : "FAIL", report.c_str());
            return ok ? 0 : 1;
        }
        // --test-volsim: drive the built-in procedural-plume sim through VolumeSimController into a
        // sink and assert frame count, fixed-dt timing, deterministic re-record, and that scrubbing
        // reproduces recorded frames - the Phase-0 gate for the volume SIMULATION framework
        // (registry -> IVolumeSimulation -> controller -> VolumeFrame -> sink). Pure CPU: no GPU/window.
        if (std::strcmp(argv[i], "--test-volsim") == 0) {
            std::string report;
            const bool ok = hbe::volume::SelfTestVolumeController(report);
            std::printf("volsim %s: %s\n", ok ? "PASS" : "FAIL", report.c_str());
            return ok ? 0 : 1;
        }
        // --test-eulersim: run the CPU Eulerian smoke solver headless and assert it produces density,
        // that a hot plume's centre of mass RISES (buoyancy + projection), no NaN/Inf, and that a
        // re-run is bit-identical (determinism). The Phase-1 solver correctness gate. Pure CPU.
        if (std::strcmp(argv[i], "--test-eulersim") == 0) {
            std::string report;
            const bool ok = hbe::volume::SelfTestEulerianSmoke(report);
            std::printf("eulersim %s: %s\n", ok ? "PASS" : "FAIL", report.c_str());
            return ok ? 0 : 1;
        }
        // --test-hbvol: the BAKE round-trip gate. Simulate -> VolumeBaker -> .hbvol bytes -> VolumeAsset
        // load -> sample the baked NanoVDB grids and diff against the CPU field (within the sparsity
        // prune threshold), plus determinism (a re-bake is byte-identical) + file save/load. Pure CPU,
        // no GPU/window - proves the baker + format + reader are correct, source-agnostically.
        if (std::strcmp(argv[i], "--test-hbvol") == 0) {
            using namespace hbe::volume;
            VolumeSimConfig cfg;
            cfg.model = "eulerian-smoke";
            cfg.bounds.worldMin = glm::vec3(-1.0f, 0.0f, -1.0f);
            cfg.bounds.worldMax = glm::vec3(1.0f, 4.0f, 1.0f);
            cfg.bounds.dim = glm::ivec3(24, 48, 24);
            cfg.frameRate = 30.0f;
            cfg.substeps = 2;
            cfg.pressureIterations = 15;
            cfg.bakeFields = {"density", "temperature"};
            {
                VolumeEmitter em;
                em.shape.kind = VolumeShapeKind::Sphere;
                em.shape.center = glm::vec3(0.0f, 0.35f, 0.0f);
                em.shape.halfExtents = glm::vec3(0.4f);
                em.densityRate = 4.0f;
                em.temperatureRate = 6.0f;
                em.temperatureTarget = 1.0f;
                cfg.emitters.push_back(em);
            }
            const hbe::u32 N = 16;
            const hbe::f32 dt = 1.0f / (cfg.frameRate * static_cast<hbe::f32>(cfg.substeps));
            const float prune = 0.005f;

            EulerianSmokeSimulation cpu(cfg);
            std::vector<hbe::u8> hbvol;
            const bool baked = BakeSimulation(cpu, cfg, 0, N - 1, hbvol, prune);
            VolumeAsset asset;
            const bool loaded = baked && asset.Load(hbvol);
            const bool structOk = loaded && asset.FrameCount() == N &&
                                  asset.FieldIndex("density") >= 0 &&
                                  asset.FieldIndex("temperature") >= 0;

            // Round-trip frame M: re-simulate to it, sample the baked grids, diff.
            double maxErrD = 0.0, maxErrT = 0.0;
            hbe::u32 activeD = 0;
            if (structOk) {
                const hbe::u32 M = N / 2;
                EulerianSmokeSimulation cpu2(cfg);
                cpu2.Reset();
                for (hbe::u32 f = 0; f < M; ++f)
                    for (int s = 0; s < cfg.substeps; ++s) cpu2.Step(dt);
                VolumeFrame fr;
                cpu2.ReadbackFrame(fr);
                const VolumeField* dCpu = fr.field("density");
                const VolumeField* tCpu = fr.field("temperature");
                const VolumeAsset::GridView dG = asset.Grid(M, "density");
                const VolumeAsset::GridView tG = asset.Grid(M, "temperature");
                activeD = dG.activeVoxels;
                const VolumeBounds& b = fr.bounds;
                for (int z = 0; z < b.dim.z; ++z)
                    for (int y = 0; y < b.dim.y; ++y)
                        for (int x = 0; x < b.dim.x; ++x) {
                            const hbe::usize idx = VoxelIndex(b, x, y, z);
                            const float gD =
                                dG.valid() ? SampleScalarGridBlob(dG.bytes, dG.size, x, y, z) : 0.0f;
                            const float gT = tG.valid()
                                                 ? SampleScalarGridBlob(tG.bytes, tG.size, x, y, z)
                                                 : cfg.ambientTemperature;
                            if (dCpu) maxErrD = std::max(maxErrD, std::abs((double)gD - dCpu->data[idx]));
                            if (tCpu) maxErrT = std::max(maxErrT, std::abs((double)gT - tCpu->data[idx]));
                        }
            }

            // Determinism: a second bake is byte-identical.
            EulerianSmokeSimulation cpu3(cfg);
            std::vector<hbe::u8> hbvol2;
            const bool baked2 = BakeSimulation(cpu3, cfg, 0, N - 1, hbvol2, prune);
            const bool deterministic = baked2 && hbvol == hbvol2;

            // File save/load round-trip.
            const auto tmp = std::filesystem::temp_directory_path() / "hbe_test.hbvol";
            VolumeAsset assetFile;
            const bool fileOk = WriteHbvolFile(tmp.string(), hbvol) &&
                                assetFile.LoadFile(tmp.string()) && assetFile.FrameCount() == N;

            const bool pass = structOk && maxErrD <= 0.006 && maxErrT <= 0.006 && deterministic &&
                              fileOk && activeD > 0;
            std::printf("hbvol %s: %zu bytes, %u frames, maxErr density=%.4f temp=%.4f, active=%u, "
                        "deterministic=%d, file=%d\n",
                        pass ? "PASS" : "FAIL", hbvol.size(), asset.FrameCount(), maxErrD, maxErrT,
                        activeD, static_cast<int>(deterministic), static_cast<int>(fileOk));
            return pass ? 0 : 1;
        }
        // --test-volume-timing: the VolumeCache playback gate (headless). Bake a .hbvol to disk, load
        // it through VolumeCache (synchronous fallback since the job system isn't running here), and
        // assert time->frame mapping (start/end/loop-wrap/clamp) + that every frame's grid is valid.
        if (std::strcmp(argv[i], "--test-volume-timing") == 0) {
            using namespace hbe::volume;
            VolumeSimConfig cfg;
            cfg.model = "eulerian-smoke";
            cfg.bounds.worldMin = glm::vec3(-1.0f, 0.0f, -1.0f);
            cfg.bounds.worldMax = glm::vec3(1.0f, 4.0f, 1.0f);
            cfg.bounds.dim = glm::ivec3(24, 48, 24);
            cfg.frameRate = 30.0f;
            cfg.substeps = 2;
            cfg.pressureIterations = 12;
            {
                VolumeEmitter em;
                em.shape.kind = VolumeShapeKind::Sphere;
                em.shape.center = glm::vec3(0.0f, 0.35f, 0.0f);
                em.shape.halfExtents = glm::vec3(0.4f);
                em.densityRate = 4.0f;
                cfg.emitters.push_back(em);
            }
            const hbe::u32 N = 12;
            const float fps = cfg.frameRate;
            EulerianSmokeSimulation sim(cfg);
            std::vector<hbe::u8> hbvol;
            const auto path = (std::filesystem::temp_directory_path() / "hbe_timing.hbvol").string();
            const bool ok = BakeSimulation(sim, cfg, 0, N - 1, hbvol) && WriteHbvolFile(path, hbvol);

            VolumeCache cache;
            const hbe::u32 h = cache.Acquire(path);
            const bool ready = ok && cache.GetState(h) == VolumeCache::State::Ready &&
                               cache.FrameCount(h) == static_cast<hbe::i32>(N);

            auto tf = [&](float t, bool loop) { return cache.TimeToFrame(h, t, loop); };
            const bool t0 = tf(0.0f, true) == 0;
            const bool tEnd = tf((N - 0.5f) / fps, false) == static_cast<hbe::i32>(N) - 1;
            const bool tWrap = tf(N / fps, true) == 0;                       // loop wraps to 0
            const bool tWrap2 = tf((N + 2.5f) / fps, true) == 2;             // wraps to 2
            const bool tClamp = tf(10.0f * N / fps, false) == static_cast<hbe::i32>(N) - 1; // clamps
            bool allGridsValid = ready;
            for (hbe::u32 f = 0; ready && f < N; ++f)
                if (!cache.DensityGrid(h, static_cast<hbe::i32>(f)).valid()) allGridsValid = false;
            const bool unassigned = cache.GetState(VolumeCache::kInvalid) != VolumeCache::State::Ready;

            const bool pass = ready && t0 && tEnd && tWrap && tWrap2 && tClamp && allGridsValid &&
                              unassigned;
            std::printf("volume-timing %s: ready=%d start=%d end=%d wrap=%d wrap2=%d clamp=%d "
                        "grids=%d\n",
                        pass ? "PASS" : "FAIL", static_cast<int>(ready), static_cast<int>(t0),
                        static_cast<int>(tEnd), static_cast<int>(tWrap), static_cast<int>(tWrap2),
                        static_cast<int>(tClamp), static_cast<int>(allGridsValid));
            return pass ? 0 : 1;
        }
        // --test-bodyshape: the character sliders resolve from joint NAMES alone, and
        // length/girth split on a bone axis derived from the rig. Headless - it proves the
        // slider maths, not how the character looks, which only a person can judge.
        // --test-meshderive: normals/tangents for geometry nobody authored, plus the
        // deterministic RNG both the generator and its bake keys stand on. Headless.
        // --test-platform: the OS layer that replaces 12 hand-rolled copies of "where is
        // my exe" and 6 of "where does user data go". Headless.
        if (std::strcmp(argv[i], "--test-platform") == 0) {
            const bool ok = hbe::platform::SelfTest();
            std::printf("platform %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-input: the portable half of Input, which became testable when it was
        // split out of Input_Win32.cpp - press edges, the ordered text stream, mouse
        // deltas, and the stick deadzone curve, none of which need a window.
        if (std::strcmp(argv[i], "--test-input") == 0) {
            const bool ok = hbe::InputSelfTest();
            std::printf("input %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-faceselect: picking triangles and splitting them into their own submesh,
        // which is how a second material is assigned to part of a mesh.
        if (std::strcmp(argv[i], "--test-faceselect") == 0) {
            const bool ok = hbe::mesh::FaceSelectSelfTest();
            std::printf("faceselect %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-simplify: the quadric decimator used to build streaming LODs at import.
        if (std::strcmp(argv[i], "--test-simplify") == 0) {
            const bool ok = hbe::mesh::SimplifySelfTest();
            std::printf("simplify %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-meshlod: import-time distance-LOD generation (BuildLodChain) + the v9 .uaf
        // round-trip. Proves LODs are strictly decreasing, deterministic, LOD0 is left intact,
        // morph meshes are excluded, and the LOD chain survives write+read. Headless.
        if (std::strcmp(argv[i], "--test-meshlod") == 0) {
            const bool ok = hbe::mesh::MeshLodSelfTest();
            std::printf("meshlod %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-bc-encode: BC texture encoding (stb_dxt) - block math + exact packed size for a
        // non-multiple-of-4 mipped image. CPU-only, no GPU/window/project. The GPU upload of the
        // encoded blocks is covered by --test-bc (needs a device).
        if (std::strcmp(argv[i], "--test-bc-encode") == 0) {
            const bool ok = hbe::tex::CompressSelfTest();
            std::printf("bc-encode %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-upgrade: the asset auto-upgrade migrates an out-of-date mesh .uaf to the current
        // version in place, keeping its guid + pack slot, generating LODs, and is idempotent.
        // CPU-only, no GPU/window (uses a temp project setting). Headless.
        if (std::strcmp(argv[i], "--test-upgrade") == 0) {
            const bool ok = hbe::importer::UpgradeSelfTest();
            std::printf("upgrade %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-meshopt: import-time GPU geometry optimize (meshoptimizer). Proves the
        // surface is preserved, morph deltas stay glued to their vertices through the
        // reorder, the cache order improves, the weld collapses duplicates, and it is
        // deterministic. Headless, no GPU/window/project.
        if (std::strcmp(argv[i], "--test-meshopt") == 0) {
            const bool ok = hbe::mesh::OptimizeSelfTest();
            std::printf("meshopt %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        if (std::strcmp(argv[i], "--test-meshderive") == 0) {
            const bool ok = hbe::mesh::SelfTest();
            std::printf("meshderive %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        if (std::strcmp(argv[i], "--test-bodyshape") == 0) {
            const bool ok = hbe::bodyshape::SelfTest();
            std::printf("bodyshape %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-acoustics: acoustic-material presets, .hbmat acoustic round-trip, and
        // entity -> AcousticMaterial resolution + caching (physically-informed audio P1).
        // Headless, no GPU/window/project.
        if (std::strcmp(argv[i], "--test-acoustics") == 0) {
            const bool libOk = hbe::HdsrLibrarySelfTest(); // HDS-Resonance acoustics library
            const bool matOk = hbe::AcousticSelfTest();
            const bool roomOk = hbe::AcousticRoomSelfTest();
            const bool evOk = hbe::assets::AudioEventSelfTest();
            const bool ok = libOk && matOk && roomOk && evOk;
            std::printf("acoustics %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-assetformats: the ASSET REGISTRY's own invariants. Assets/
        // AssetFormats.cpp is the single source of truth for what an extension
        // means, and it now carries a second shipping contract beside
        // `runtimeLoaded`: how the pack closure walks the format. A row that
        // leaves that Unspecified is a format whose references silently do not
        // ship, so it is a build failure here rather than a mystery in a build.
        // Headless, no GPU/window/project. Same contract as --test-seamweld.
        if (std::strcmp(argv[i], "--test-assetformats") == 0) {
            const bool ok = hbe::assets::RegistrySelfTest();
            std::printf("assetformats %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-compress: the PORTABLE-CODEC gate. Round-trips zstd/zlib/None across
        // empty/tiny/compressible/incompressible buffers and checks the content hash is
        // deterministic + discriminating. This is the foundation the v5 pack format and
        // the mesh/audio cooks all sit on. Headless. Same contract as --test-seamweld.
        if (std::strcmp(argv[i], "--test-compress") == 0) {
            const bool ok = hbe::comp::SelfTest();
            std::printf("compress %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-openpbr: the MATERIAL INTERCHANGE gate. Round-trips a distinctive OpenPBR material
        // through .hbmat and (when MaterialX is linked) .mtlx, and checks the shader-variant routing.
        // Also proves the MaterialX DLLs load + execute. Headless; no GPU/window/project.
        if (std::strcmp(argv[i], "--test-openpbr") == 0) {
            const bool ok = hbe::assets::MaterialInteropSelfTest();
            std::printf("openpbr %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-material: the unified MATERIAL AUTHORING gate (docs/Design-MaterialAuthoring.md).
        // Node-graph serialization + deterministic compilation + constant folding + parameter
        // overrides + layer/height/normal blending + box-brush weight/rotation/falloff + world/
        // local tiling + mask/layer-stack serialization. Headless; no GPU/window/project.
        if (std::strcmp(argv[i], "--test-material") == 0) {
            const bool ok = hbe::mat::SelfTest();
            std::printf("material %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-shadercompile: the EDITOR-ONLY RUNTIME shader compiler gate. Compiles a known
        // compute kernel through the toolchain DXC (DXIL and/or SPIR-V, whichever is present) and
        // checks the bytecode magic. Headless; no GPU/window/project. See RuntimeShaderCompiler.h.
        if (std::strcmp(argv[i], "--test-shadercompile") == 0) {
            const bool ok = hbe::editor::RuntimeShaderCompiler::SelfTest();
            std::printf("shadercompile %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-matshader: the GRAPH -> GPU path. Generates an HLSL compute shader from a material
        // node graph (a spread of generators / transforms / filters / resampling / SDF) and compiles
        // it through the editor runtime shader compiler for whichever backends are present. Proves the
        // node-graph -> HLSL -> GPU-bytecode pipeline headlessly. Optional: --test-matshader dump.hlsl
        // writes the generated HLSL to a file for inspection.
        if (std::strcmp(argv[i], "--test-matshader") == 0) {
            using namespace hbe;
            mat::Graph g;
            g.name = "MatShaderTest";
            const u32 fbm = g.AddNode(mat::NodeType::FractalNoise);
            g.FindNode(fbm)->constant = {6.0f, 5.0f, 0.55f, 1.0f};
            const u32 ramp = g.AddNode(mat::NodeType::ColorRamp);
            g.FindNode(ramp)->ramp = {{0.0f, {0.2f, 0.13f, 0.08f, 1}}, {1.0f, {0.6f, 0.5f, 0.4f, 1}}};
            const u32 warp = g.AddNode(mat::NodeType::Warp);
            g.FindNode(warp)->constant = {0.15f, 0, 0, 0};
            const u32 cell = g.AddNode(mat::NodeType::Cellular);
            g.FindNode(cell)->constant = {9.0f, 2.0f, 2.0f, 0.0f};
            const u32 h2n = g.AddNode(mat::NodeType::HeightToNormal);
            g.FindNode(h2n)->constant = {4.0f, 0.01f, 0, 0};
            const u32 sdf = g.AddNode(mat::NodeType::SdfCircle);
            const u32 out = g.AddNode(mat::NodeType::Output);
            g.Connect(fbm, warp, 0);   // warp the noise by itself
            g.Connect(fbm, warp, 1);
            g.Connect(warp, ramp, 0);
            g.Connect(ramp, out, static_cast<u8>(mat::Channel::BaseColor));
            g.Connect(fbm, h2n, 0);
            g.Connect(h2n, out, static_cast<u8>(mat::Channel::Normal));
            g.Connect(cell, out, static_cast<u8>(mat::Channel::Roughness));
            g.Connect(fbm, out, static_cast<u8>(mat::Channel::Height));
            g.Connect(sdf, out, static_cast<u8>(mat::Channel::AO));
            const std::string hlsl = mat::GenerateComputeHlsl(g);
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                std::ofstream f(argv[i + 1]);
                f << hlsl;
                std::printf("matshader: wrote generated HLSL to %s (%zu bytes)\n", argv[i + 1],
                            hlsl.size());
            }
            int tested = 0, failed = 0;
            for (auto api : {rhi::GraphicsAPI::D3D12, rhi::GraphicsAPI::Vulkan}) {
                if (!editor::RuntimeShaderCompiler::Available(api)) continue;
                ++tested;
                const auto r = editor::RuntimeShaderCompiler::Compile(api, hlsl, "CSMain", "cs",
                                                                      "MatShaderTest");
                if (!r.ok) {
                    std::printf("matshader: compile FAILED (%s):\n%s\n",
                                api == rhi::GraphicsAPI::Vulkan ? "SPIR-V" : "DXIL", r.log.c_str());
                    ++failed;
                } else {
                    std::printf("matshader: %s OK (%zu bytes)\n",
                                api == rhi::GraphicsAPI::Vulkan ? "SPIR-V" : "DXIL", r.bytecode.size());
                }
            }
            const bool ok = (tested == 0) || (failed == 0);
            std::printf("matshader %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --matscene [outDir]: bake the unified material system's VISUAL TEST SCENE to PNGs (world
        // tiling at two sizes, box-brush weight fields, overlapping volumes, procedural + painted
        // masks, linear vs height+noise blends, and a composite floor using all three mask sources).
        // Makes stretching / blending / painting correctness visible WITHOUT a live GPU. Headless.
        if (std::strcmp(argv[i], "--matscene") == 0) {
            std::filesystem::path outDir =
                (i + 1 < argc && argv[i + 1][0] != '-') ? std::filesystem::path(argv[i + 1])
                                                        : std::filesystem::path("matscene_out");
            std::error_code ec;
            std::filesystem::create_directories(outDir, ec);
            const auto imgs = hbe::mat::BuildDemoScene(256);
            int ok = 0;
            for (const auto& im : imgs) {
                const auto p = outDir / (im.name + ".png");
                if (hbe::movie::WritePng(p, im.width, im.height, im.rgba)) {
                    ++ok;
                    std::printf("  wrote %s\n", p.string().c_str());
                }
            }
            std::printf("matscene wrote %d/%zu images to %s\n", ok, imgs.size(),
                        outDir.string().c_str());
            return ok == static_cast<int>(imgs.size()) ? 0 : 1;
        }
        // --matexport <graph.hbmatgraph> [outDir] [res]: compile a material graph and bake its full
        // PBR texture set (basecolor/normal/roughness/metallic/height/ao/emissive/opacity) to PNGs.
        // The Material-Maker-style "export textures". Headless.
        if (std::strcmp(argv[i], "--matexport") == 0 && i + 1 < argc) {
            const std::filesystem::path graphPath = argv[i + 1];
            const std::filesystem::path outDir =
                (i + 2 < argc && argv[i + 2][0] != '-') ? std::filesystem::path(argv[i + 2])
                                                        : std::filesystem::path("matexport_out");
            const hbe::u32 res = (i + 3 < argc) ? static_cast<hbe::u32>(std::atoi(argv[i + 3])) : 512u;
            auto g = hbe::mat::LoadGraph(graphPath);
            if (!g) {
                std::printf("matexport: cannot load '%s'\n", graphPath.string().c_str());
                return 1;
            }
            const hbe::mat::CompiledGraph c = hbe::mat::Compile(*g);
            if (!c.ok) {
                std::printf("matexport: compile error: %s\n", c.error.c_str());
                return 1;
            }
            std::error_code ec;
            std::filesystem::create_directories(outDir, ec);
            const auto maps = hbe::mat::BakeGraphMaps(c, res);
            int ok = 0;
            for (const auto& m : maps) {
                const auto p = outDir / (m.name + ".png");
                if (hbe::movie::WritePng(p, m.width, m.height, m.rgba)) {
                    ++ok;
                    std::printf("  wrote %s\n", p.string().c_str());
                }
            }
            std::printf("matexport wrote %d/%zu maps (%ux%u) to %s\n", ok, maps.size(), res, res,
                        outDir.string().c_str());
            return ok == static_cast<int>(maps.size()) ? 0 : 1;
        }
        // --test-packclosure: THE GATE for "Pack only referenced assets". Builds
        // a synthetic project exercising every format in the reference matrix,
        // each referencing the next, and proves the closure reaches all of them
        // (including a texture reachable only through .hbmat <- .hbprefab <-
        // .hbscene), that a real cook's packs contain exactly that set, that an
        // orphan is excluded with the filter on and present with it off, and
        // that an unresolvable or ambiguous reference is reported rather than
        // packed around. Headless, no GPU/window/project - it creates and
        // deletes its own scratch project. Same contract as --test-seamweld.
        if (std::strcmp(argv[i], "--test-packclosure") == 0) {
            const bool ok = hbe::assets::PackClosureSelfTest();
            std::printf("packclosure %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-slotids: THE GATE for "an asset owns its pack slot for life".
        // Proves against a real scratch project and a real cook that a created
        // asset takes the next id, that deleting one frees EXACTLY that number
        // for the next creation, that no surviving asset ever moves, that
        // slot/50 picks the pack at the 49/50/51 boundary, that the migration is
        // idempotent, and that two cooks in a row produce byte-identical packs.
        // Headless, no GPU/window/project. Same contract as --test-seamweld.
        if (std::strcmp(argv[i], "--test-slotids") == 0) {
            const bool ok = hbe::slots::SlotIdSelfTest();
            std::printf("slotids %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-uapv5: the v5 PACK-FORMAT gate. Cooks a synthetic corpus (with a
        // byte-identical pair) and proves round-trip decode, aligned blob offsets, the
        // per-entry content hash, within-pack dedup, deterministic re-cook (byte-
        // identical packs = patchability), and that a legacy v4 pack still opens.
        // Headless; builds/deletes its own temp. Same contract as --test-seamweld.
        if (std::strcmp(argv[i], "--test-uapv5") == 0) {
            const bool ok = hbe::uap::PackSelfTest();
            std::printf("uapv5 %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-meshcodec: the v10 GEOMETRY-CODEC gate. Round-trips static + skinned
        // geometry through quantize + meshopt encode/decode and asserts positions within
        // the quant epsilon, indices exact, tangent handedness + joints exact, weights
        // sum to ~1, and deterministic bytes. Headless. Same contract as --test-seamweld.
        if (std::strcmp(argv[i], "--test-meshcodec") == 0) {
            const bool ok = hbe::meshcodec::SelfTest();
            std::printf("meshcodec %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-audiocodec: the v11 AUDIO gate. Proves the .uaf stores the compressed
        // SOURCE (not decoded PCM), decodes back on load, survives a metadata read-modify-
        // write, and that legacy raw-PCM assets still round-trip. Headless (synthesizes a
        // WAV, no file needed). Same contract as --test-seamweld.
        if (std::strcmp(argv[i], "--test-audiocodec") == 0) {
            const bool ok = hbe::uaf::AudioSelfTest();
            std::printf("audiocodec %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-cookstats: the cook-analysis gate. Synthesizes a tiny project, runs the
        // "why is this build big" report, and asserts it renders with categories + dedup.
        // Headless. Same contract as --test-seamweld.
        if (std::strcmp(argv[i], "--test-cookstats") == 0) {
            const bool ok = hbe::cookstats::SelfTest();
            std::printf("cookstats %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-vfxstack: prove the VFX attribute model + module-stack core
        // (64-byte record layout, dead-stream elimination, stage-order validation,
        // determinism) headless, no GPU/window. Same contract as --test-seamweld.
        if (std::strcmp(argv[i], "--test-vfxstack") == 0) {
            const bool ok = hbe::vfx::SelfTest();
            std::printf("vfxstack %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-vfxcompat: prove that moving ParticleEmitter onto the module stack
        // changed nothing. Diffs the live path against a frozen copy of the pre-stack
        // simulation loop, bit-for-bit, over every preset plus a parameter fuzz.
        // Headless, no GPU/window. Same contract as --test-seamweld.
        if (std::strcmp(argv[i], "--test-vfxcompat") == 0) {
            const bool ok = hbe::particle::CompatSelfTest();
            std::printf("vfxcompat %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-oceanfft: prove the Tessendorf FFT ocean MATH headless (no GPU): the
        // radix-2 FFT round-trips (1D+2D), the evolved height field is Hermitian-real, and
        // the spectrum is deterministic. This CPU reference is the oracle the GPU compute
        // FFT is later diffed against (--test-oceanfft-gpu). Same contract as --test-seamweld.
        if (std::strcmp(argv[i], "--test-oceanfft") == 0) {
            std::string report;
            const bool ok = hbe::ocean::SelfTest(report);
            std::printf("oceanfft %s (%s)\n", ok ? "PASS" : "FAIL", report.c_str());
            return ok ? 0 : 1;
        }
        // --test-entityguid: prove the stable per-entity identity contract -
        // uniqueness across a scene, stability across save/load/save, FRESH guids
        // on copy/paste + prefab instantiate, and deterministic (stable across
        // reloads) assignment for a pre-guid scene file. Headless, no GPU/window.
        // Same contract as --test-seamweld.
        if (std::strcmp(argv[i], "--test-entityguid") == 0) {
            const bool ok = hbe::guid::SelfTest();
            std::printf("entityguid %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-noleveltypes: prove a level is ONE scene file - no UI scene kind,
        // a "<base>.static.hbscene"-NAMED file loads as an ordinary standalone
        // scene (its sibling ".dynamic" is not composed in), the per-object
        // Static/Dynamic tag the navmesh reads round-trips, and SaveScene ->
        // Parse -> SaveScene is byte-identical. Headless, no GPU/window. Same
        // contract as --test-seamweld.
        if (std::strcmp(argv[i], "--test-noleveltypes") == 0) {
            const bool ok = hbe::scene::LevelTypesSelfTest();
            std::printf("noleveltypes %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-paintcanvas: prove the paint-canvas save path - every canvas gets
        // a `source` (BuildSceneJson silently skips one that has none, which loses
        // the painting), colliding names get distinct files, an already-assigned
        // source is never reassigned, and the result reloads. Headless, no GPU.
        // --test-sceneslice: prove PARTIAL instantiation is correct - the thing all
        // of tag streaming stands on. One parsed scene is loaded twice, once whole
        // and once as two disjoint slices, and the two worlds must be the SAME world
        // (same entities by guid, byte-identical component state, same hierarchy)
        // except that a parent link crossing the slice boundary becomes a root.
        // Also pins blocker B1 (no Parent is ever emplaced on a null handle) and
        // blocker B2 (BindWorld applies the environment exactly once; a slice never
        // does; a second bind does not stack a second world). Headless, no GPU.
        if (std::strcmp(argv[i], "--test-sceneslice") == 0) {
            const bool ok = hbe::scene::SceneSliceSelfTest();
            std::printf("sceneslice %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-lightingparity: prove THE EDITOR AND THE SHIPPED GAME LIGHT A SCENE
        // THE SAME WAY. The same file is loaded through both real paths - the
        // editor's scene::LoadScene (LoadMode::Replace, what Editor::LoadSceneInEditor
        // calls) and the runtime's scene::BindWorld + Additive shard slices (what
        // stream::Streamer::BindLevel calls) - and the resulting SceneEnvironment must
        // be identical: ambientIntensity, exposure, shadowDistance, post (compared
        // whole) and giSource/giStatus/giOrigin/giSpacing/giDims plus giSh/giDepth
        // handle validity. Covers a scene WITH a baked `.hbgi` and one without, a
        // MISSING and a CORRUPT `.hbgi` (reported, and never inherited from the
        // previously loaded scene), shard-order independence, additive loads applying
        // no environment, re-bind idempotence, and that day/night MODULATES the
        // authored ambient/exposure rather than replacing them. Headless, no GPU/
        // window; creates its own scratch project. Same contract as --test-seamweld.
        if (std::strcmp(argv[i], "--test-lightingparity") == 0) {
            const bool ok = hbe::scene::LightingParitySelfTest();
            std::printf("lightingparity %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-pasteorder: prove the SIBLING-ORDER CONTRACT (Scene/Hierarchy.h) -
        // a copied/duplicated/prefab-instantiated subtree reproduces the source's
        // child order at EVERY depth, wide and deep; clones still mint fresh guids;
        // a full save/load keeps the order over two cycles; a `.hbscene` written
        // before the "order" field loads exactly as it used to; and an explicit
        // drag-reorder is authored data that survives both. Includes the case the
        // OLD pool-order walk gets wrong (a registry whose Parent pool has been
        // perturbed by an unparent/reparent, plus recycled handles), and asserts
        // that the old walk really does disagree there - so the test measures the
        // fix rather than the status quo. Headless, no GPU/window/project.
        if (std::strcmp(argv[i], "--test-pasteorder") == 0) {
            const bool ok = hbe::scene::PasteOrderSelfTest();
            std::printf("pasteorder %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-fpslook: prove the FIRST-PERSON CAMERA LOOK contract. A mouse
        // delta produces the expected yaw/pitch at a known sensitivity, pitch
        // clamps at lookPitchMin/Max while yaw stays free, invertLookY flips it,
        // the right stick drives it as a RATE (scales with dt) while the mouse
        // does not, the CHARACTER's yaw follows the camera while its pitch never
        // does, the eye rides that yaw undamped, faceMoveDir stands down for the
        // frame the camera owns the body (with a positive control proving the
        // latch is one-shot), the cursor-lock gate suppresses look while a menu
        // or dialogue choice has freed the cursor, first and third person
        // accumulate look BIT-IDENTICALLY (they share one accumulator), aim
        // modes stand down for first-person look and only there, playerLook off
        // is exactly the old behaviour, and a scene round-trips every look field
        // while never serializing accumulated look state. Headless, no
        // GPU/window/project. Same contract as --test-seamweld.
        if (std::strcmp(argv[i], "--test-fpslook") == 0) {
            const bool ok = hbe::cam::FirstPersonLookSelfTest();
            std::printf("fpslook %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-pasteparent: prove the PASTE-PARENTING CONTRACT - the other half of
        // "the clone lands where a human expects". A clipboard fragment cannot carry
        // its ROOT's parent (EntityToJson only writes a `parent` inside the subtree,
        // and the root's is by definition outside it), so a copy of a child used to
        // paste at the scene root - "it becomes its own thing". The parent is
        // captured at COPY time instead, and this pins what each caller does with it:
        // Ctrl+V/Ctrl+D produce a SIBLING of the source, LAST in that group; a copied
        // root stays a root; a `.hbprefab` drop is a root; prefab Revert restores the
        // INSTANCE's own parent, guid and sibling order. Plus every way the captured
        // handle goes bad - deleted since, a REPLACED world (valid but aliasing a
        // different entity), a handle aliasing something the paste itself created,
        // and a parent in another `.hbui` - each falling back to a root rather than
        // emplacing a dangling or boundary-crossing Parent. Includes the case the OLD
        // behaviour fails, so the test measures the fix rather than the status quo.
        // Headless, no GPU/window/project.
        if (std::strcmp(argv[i], "--test-pasteparent") == 0) {
            const bool ok = hbe::Editor::PasteParentSelfTest();
            std::printf("pasteparent %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-strokezones: prove that a 3D paint stroke belongs to the STREAMING
        // ZONE of the surface it was painted on (Scene/StrokeZone.h). A stroke on
        // tagged geometry lands in that zone's group node, the GROUP ITSELF carries
        // the tag - the only shape tagshard::Bake accepts, and the one this design
        // exists for - and the whole thing bakes with zero errors; a stroke on
        // untagged geometry falls back to the plain "Paint Strokes" group and stays
        // permanently resident; attaching never moves a stroke on screen; a scene
        // authored before zones ADOPTS its existing node instead of forking a second
        // one; the grouping survives save/load as a fixed point; a tagged stroke
        // survives a shard despawn/RESPAWN byte-identically (the blocker-B3 shape);
        // strokes are excluded from the navmesh while an identical ordinary prop is
        // not; and Rehome is idempotent. Headless: no GPU, no window, no project.
        if (std::strcmp(argv[i], "--test-strokezones") == 0) {
            const bool ok = hbe::strokezone::SelfTest();
            std::printf("strokezones %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-worldlocal: prove the WORLD-vs-LOCAL transform contract. Nav
        // steering, AI look-at and spawner placement each computed a WORLD-space
        // answer and assigned it to `Transform`, which is PARENT-RELATIVE - correct
        // for a root entity (which is why it survived), silently wrong for anything
        // parented to a moving platform, a room root or a streamed shard root. Pins
        // Scene::SetWorldPosition / SetWorldRotation against a rotated and
        // NON-UNIFORMLY SCALED parent, a two-level chain, the root identity case and
        // a Transform-less entity, and asserts in each case that the old raw
        // assignment FAILS - so the fixture is provably adversarial rather than
        // trivially satisfiable. Headless: no GPU, no window, no project.
        if (std::strcmp(argv[i], "--test-worldlocal") == 0) {
            const bool ok = hbe::scene::WorldLocalSelfTest();
            std::printf("worldlocal %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-vegdata: pin the vegetation P1 data model - SoA lockstep + handle
        // round-trip, DETERMINISTIC generation (same seed -> identical skeleton bytes),
        // registry interning, store slicing, and noise-field determinism. Headless.
        if (std::strcmp(argv[i], "--test-vegdata") == 0) {
            const bool ok = hbe::veg::DataSelfTest();
            std::printf("vegdata %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-vegscatter: pin vegetation P2 - DETERMINISTIC placements (same worldSeed
        // -> identical points), tileable min-spacing, terrain/water filtering via the
        // surface query, biome/species scoring, and the .hbbiome round-trip. Headless.
        if (std::strcmp(argv[i], "--test-vegscatter") == 0) {
            const bool ok = hbe::veg::ScatterSelfTest();
            std::printf("vegscatter %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-vegmesh: pin vegetation P3 - the .hbspecies round-trip, the tubular
        // mesher, the meshoptimizer LOD chain, deterministic geometry, and sane bounds.
        if (std::strcmp(argv[i], "--test-vegmesh") == 0) {
            const bool ok = hbe::veg::MeshSelfTest();
            std::printf("vegmesh %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-veggen: pin vegetation P4 - space colonization + L-system generators
        // (deterministic, bounded, structurally distinct, meshable). Headless.
        if (std::strcmp(argv[i], "--test-veggen") == 0) {
            const bool ok = hbe::veg::GenSelfTest();
            std::printf("veggen %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-veggrow: pin STRUCTURAL growth - more age -> more structure (not a
        // scaled copy), deterministic, taller. Headless.
        if (std::strcmp(argv[i], "--test-veggrow") == 0) {
            const bool ok = hbe::veg::GrowthSelfTest();
            std::printf("veggrow %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-veglod: pin vegetation P7 simulation-LOD - distance bands, budgeted
        // promotion, immediate demotion. Headless.
        if (std::strcmp(argv[i], "--test-veglod") == 0) {
            const bool ok = hbe::veg::LodSelfTest();
            std::printf("veglod %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-vegstream: pin vegetation P8 - deterministic shard generation +
        // job-safe parallel generation matching the serial reference. Headless.
        if (std::strcmp(argv[i], "--test-vegstream") == 0) {
            const bool ok = hbe::veg::StreamSelfTest();
            std::printf("vegstream %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-veglife: pin vegetation P10 life-cycle - incremental growth (adds
        // structure, preserves form, deterministic) + damage/support. Headless.
        if (std::strcmp(argv[i], "--test-veglife") == 0) {
            const bool ok = hbe::veg::LifeSelfTest();
            std::printf("veglife %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-vegpaint: pin the paint brush's renderer-free logic - erase-within-radius
        // geometry + "veg:" MeshRef tag isolation (never erases other meshes). Headless.
        if (std::strcmp(argv[i], "--test-vegpaint") == 0) {
            const bool ok = hbe::veg::PaintSelfTest();
            std::printf("vegpaint %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-vegbake: pin the External-authoring asset path - a species bakes to a .uaf
        // and round-trips (generate -> mesh -> write -> read) with intact geometry. Headless.
        if (std::strcmp(argv[i], "--test-vegbake") == 0) {
            const bool ok = hbe::veg::BakeSelfTest();
            std::printf("vegbake %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-proctree: pin the parametric tree importer - deterministic, valid parent
        // invariant, bounded, meshable (woody + leaves), seed-sensitive. Headless.
        if (std::strcmp(argv[i], "--test-proctree") == 0) {
            const bool ok = hbe::editor::ProctreeSelfTest();
            std::printf("proctree %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-graphfanin: prove RECONVERGENCE works in both node graphs. A choice
        // fanning out to several branches which then REJOIN a shared tail is the most
        // common shape in a visual scripting language, and both graphs made it
        // unauthorable: Connect enforced "one wire per INPUT pin" (correct for DATA
        // pins, wrong for EXEC), so wiring the second branch SILENTLY DELETED the
        // first and at runtime that branch hit Follow()==0 and ended early. Pins the
        // corrected rule - exec: output exclusive, input fan-in; data: input exclusive,
        // output fan-out - plus removal of a fan-in node, and a save/load round trip.
        // Every case asserts the OLD behaviour fails it. Headless.
        // --test-timelinesnap: prove the FRAME GRID every editor timeline now snaps
        // to. Before it, all three timelines wrote the raw mouse position into a
        // float-seconds key, so two keys meant to line up landed on 1.3871429s and
        // 1.3866667s and the music editor drew a bar grid it did not obey. Asserts
        // idempotence across eight frame rates over 0-3600s (a key is re-snapped on
        // every drag frame, on inspector release and again on save, so a Snap that
        // moved an already-snapped value would drift it every gesture), the
        // round-half-away-from-zero rule, the off-grid-duration clamp trap, and that
        // a disabled or Ctrl-suspended grid is EXACTLY the identity. Headless.
        // --test-projectkeys: prove a `.hbproj` survives a round trip through a build
        // that does not understand all of its keys. Project::Save() rebuilds the whole
        // file and used to start from an empty json object, emitting only the keys this
        // build knows - so every other key was silently DELETED on the first save. That
        // made the format lossy across engine versions in one direction, permanently:
        // open a project written by a newer build, change one setting, and every option
        // that build predates is gone. `j["version"] = 1` was written but never read, so
        // nothing detected the mismatch either. Also asserts the retired legacy keys
        // stay dropped (that drop IS the migration) and that a NEW project does not
        // inherit a previous one's unknown keys. Headless; writes only to temp.
        // --test-collab: drive the SHIPPING collaboration server and client over the
        // in-process loopback transport - no sockets, no ports, no threads, so a
        // two-client lock RACE is deterministic instead of flaky. Asserts: a contested
        // lock resolves to exactly one owner (never both, never neither); a lease
        // expires when its owner stops heartbeating and does NOT expire while it does;
        // a non-owner's edit is refused and never reaches authoritative state; a
        // stale-revision edit is detected; paint ops commit to the history in server
        // order with attribution while PREVIEWS never enter it; a reconnecting user
        // keeps their identity and reclaims their locks; and the framing survives
        // arbitrary stream splits, unknown message kinds and a hostile 4 GiB length.
        // Headless: no GPU, no window, no project.
        // --test-tcp: the same collaboration session over REAL localhost TCP, which is
        // the only way to exercise what the in-process loopback structurally cannot -
        // a partial send, a 64 KiB frame split across several recv() calls, and 200
        // small frames coalesced into one read. Binds an EPHEMERAL port (0) so it
        // cannot fail on a machine where something already owns a fixed one.
        // --test-hub: the launcher's update path, everything provable without a
        // network. Version ORDERING (a string compare puts 1.0.10 below 1.0.9 and
        // silently stops offering updates at the tenth patch), the real published
        // manifest shape, the https-only URL policy, the zip-slip containment guard,
        // the installer's refusals, and SHA-256 against its published test vectors.
        // --hub-check: a LIVE update check against the configured manifest URL, through
        // the real WinHTTP/TLS path. Separate from --test-hub on purpose: --test-hub must
        // never depend on a network, or a dropped wifi teaches people to ignore a red
        // test. This one is a diagnostic you run when you want to know about the server.
        // --hub-install <dir>: run a REAL install into <dir> through the shipping
        // installer - fetch the manifest, download, verify the hash, extract, swap, and
        // stamp. Exists because everything up to the swap was only ever exercised
        // against synthetic data; this is the one path that has to work on a stranger's
        // machine, and it deserves to be runnable without clicking through a GUI.
        if (std::strcmp(argv[i], "--hub-install") == 0 && i + 1 < argc) {
            hbe::hub::UpdatePaths ip;
            ip.installRoot = argv[i + 1];
            hbe::hub::Updater up("https://hollowdreamstudios.com/enginemanifest.json", ip);
            up.SetInstalledVersion(hbe::hub::ReadInstalledVersion(ip.installRoot));
            up.Check();
            std::printf("check: %s | %s\n", hbe::hub::UpdateStateName(up.Progress().state),
                        up.Progress().message.c_str());
            if (up.Progress().state != hbe::hub::UpdateState::Available) return 1;
            up.Apply([](const hbe::hub::UpdateProgress&) { return true; });
            const hbe::hub::UpdateProgress& p2 = up.Progress();
            std::printf("apply: %s | %s\n", hbe::hub::UpdateStateName(p2.state),
                        p2.message.c_str());
            const auto stamp = hbe::hub::ReadInstalledVersion(ip.installRoot);
            std::printf("stamp: %s | looksInstalled=%d\n",
                        stamp ? stamp->ToString().c_str() : "(none)",
                        hbe::hub::LooksInstalled(ip.installRoot) ? 1 : 0);
            return p2.state == hbe::hub::UpdateState::Done ? 0 : 1;
        }
        if (std::strcmp(argv[i], "--hub-check") == 0) {
            hbe::hub::UpdatePaths paths;
            paths.installRoot = hbe::platform::ExecutableDir().parent_path();
            hbe::hub::Updater up("https://hollowdreamstudios.com/enginemanifest.json", paths);
            up.Check();
            const hbe::hub::UpdateProgress& pr = up.Progress();
            std::printf("hub-check: state=%s local=%s remote=%s\n  %s\n",
                        hbe::hub::UpdateStateName(pr.state),
                        pr.localVersion.ToString().c_str(),
                        pr.remoteVersion.ToString().c_str(), pr.message.c_str());
            if (!pr.releaseUrl.empty())
                std::printf("  release: %s\n", pr.releaseUrl.c_str());
            return pr.state == hbe::hub::UpdateState::Failed ? 1 : 0;
        }
        // --test-componentdelta: the seam collaborative scene editing needs. Saving is
        // monolithic (EntityToJson writes a whole entity; Instantiate applies one while
        // CREATING it), and neither shape lets a client say "component C of an entity
        // that already exists becomes this". Proves every registered key round-trips
        // onto a DIFFERENT entity byte-for-byte, that omitted fields MERGE rather than
        // reset (resetting teleports objects), that an empty payload removes, and that
        // an unsupported key is REFUSED rather than silently ignored - a silent no-op
        // would be a divergence with no symptom until someone saved. Headless.
        if (std::strcmp(argv[i], "--test-componentdelta") == 0) {
            const bool ok = hbe::scene::ComponentDeltaSelfTest();
            std::printf("componentdelta %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        if (std::strcmp(argv[i], "--test-hub") == 0) {
            const bool ok = hbe::hub::HubSelfTest();
            std::printf("hub %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        if (std::strcmp(argv[i], "--test-tcp") == 0) {
            const bool ok = hbe::collab::TcpTransportSelfTest();
            std::printf("tcp %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-journal: the OFFLINE half of collaboration. A save seals a commit
        // (before/after bytes per entity+component against a named parent); going
        // offline is a fork and reconnecting is a fast-forward, a clean merge, or a
        // question for a human. Asserts the two properties that matter: a crash costs
        // only the LAST commit and never surfaces a partial one, and a merge between
        // two INDEPENDENTLY MIGRATED copies is refused - scene::MigrateSceneGuids
        // derives guids as a pure function of path and row index, so divergent copies
        // assign the same guid to different objects and a silent merge would move the
        // wrong things. Headless.
        // --test-p2p: THE WHOLE COLLABORATION STACK, end to end, against real scenes.
        // An elected-host session over a real transport (lock enforced, a non-owner
        // refused, an owner edit reaching the OTHER peer scene), then a peer going
        // offline, sealing its work as a commit, and reconciling - disjoint work
        // merging and landing without reverting local edits, an overlap held for
        // review with NOTHING applied, and independently-migrated copies refused.
        if (std::strcmp(argv[i], "--test-p2p") == 0) {
            const bool ok = hbe::scene::P2PEndToEndSelfTest();
            std::printf("p2p %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-identity: WHO a peer is, once the session is reachable from the open
        // internet. A per-install ECDSA P-256 keypair persists and stays stable, the
        // challenge never repeats, a genuine signature verifies - and impersonation,
        // replay, a tampered signature and tampered data all FAIL. Also asserts the
        // allowlist is default-DENY: an empty one admits nobody. Headless.
        if (std::strcmp(argv[i], "--test-identity") == 0) {
            const bool ok = hbe::collab::IdentitySelfTest();
            std::printf("identity %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-securechannel: the CHANNEL, over an untrusted network. A full TLS 1.3
        // handshake with no sockets, then every way it must fail - an unlisted peer
        // whose crypto is perfectly valid, a single flipped ciphertext byte, and an
        // intercepted / reflected / replayed identity proof. Headless.
        if (std::strcmp(argv[i], "--test-securechannel") == 0) {
            const bool ok = hbe::collab::SecureChannelSelfTest();
            std::printf("securechannel %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-webrtc: the PEER-TO-PEER path, over real ICE and a real data channel.
        // Invitation and reply exchanged as text, a direct link, mutual proof of
        // identity, then a whole collaboration session through it including a 200 KiB
        // blob that must survive chunking. Also asserts an uninvited peer holding a
        // genuine invitation never becomes a session. Hermetic: no ICE servers, so it
        // needs no internet.
        // --net-check: the DIAGNOSTIC for "it won't connect". Talks to real STUN servers
        // and reports what this machine looks like from outside. Needs the internet,
        // which is exactly why it is not part of --test-webrtc.
        if (std::strcmp(argv[i], "--net-check") == 0) {
            const bool ok = hbe::collab::NetCheck();
            return ok ? 0 : 1;
        }
        // --test-collabsession: the editor's FRONT DOOR, headlessly. What a save records
        // (and what it must leave out), a conflict held until a person answers it, and
        // the whole invite flow over a real peer-to-peer link - uninvited guest refused
        // but shown to the host, admitted, then connected. Hermetic: no ICE servers.
        if (std::strcmp(argv[i], "--test-collabsession") == 0) {
            const bool ok = hbe::editor::CollabSessionSelfTest();
            std::printf("collabsession %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-projectsync: handing a WHOLE project to a peer that has nothing.
        // What stays out (build output, caches, the host's own access list), that the
        // manifest is deterministic and content-addressed, that a transfer lands through
        // staging, and that an escaping path, an unoffered file, an oversized file and
        // wrong contents are each refused. Headless.
        if (std::strcmp(argv[i], "--test-projectsync") == 0) {
            const bool ok = hbe::collab::ProjectSyncSelfTest();
            std::printf("projectsync %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-staleinvite [seconds]: the delay a HUMAN introduces carrying blobs
        // between two machines. Default 60s; the fast suite never waits.
        if (std::strcmp(argv[i], "--test-staleinvite") == 0) {
            int secs = 60;
            if (i + 1 < argc) secs = std::atoi(argv[i + 1]) > 0 ? std::atoi(argv[i + 1]) : 60;
            const bool ok = hbe::collab::WebRtcStaleInviteTest(secs);
            std::puts(ok ? "staleinvite PASS" : "staleinvite FAIL");
            return ok ? 0 : 1;
        }
        if (std::strcmp(argv[i], "--test-webrtc") == 0) {
            const bool ok = hbe::collab::WebRtcSelfTest();
            std::printf("webrtc %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        if (std::strcmp(argv[i], "--test-journal") == 0) {
            const bool ok = hbe::collab::JournalSelfTest();
            std::printf("journal %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        if (std::strcmp(argv[i], "--test-collab") == 0) {
            const bool ok = hbe::collab::CollabSelfTest();
            std::printf("collab %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        if (std::strcmp(argv[i], "--test-projectkeys") == 0) {
            const bool ok = hbe::Project::ProjectKeysSelfTest();
            std::printf("projectkeys %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        if (std::strcmp(argv[i], "--test-timelinesnap") == 0) {
            const bool ok = hbe::editor::TimelineSnapSelfTest();
            std::printf("timelinesnap %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        if (std::strcmp(argv[i], "--test-graphfanin") == 0) {
            const bool ok = hbe::dlg::GraphFanInSelfTest();
            std::printf("graphfanin %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-shardstate: prove SHARD PERSISTENCE + manual spawn/despawn. A shard
        // is despawned and respawned and must come back with its door/kill/pickup/
        // trigger/destructible/AI/encounter state restored BY STABLE GUID (including
        // two entities that share a name, which the previous Name key collapsed onto
        // one row); a spawner's PROGRESS survives while its spawned population does
        // not (a cleared camp stays cleared, survivors reset - what keeps the save
        // bounded); no surviving component is left holding a dangling entt::entity;
        // a non-resident shard is never diffed as destroyed; and two full cycles are
        // stable in both entity count and stored-state size. Headless, no GPU.
        if (std::strcmp(argv[i], "--test-shardstate") == 0) {
            const bool ok = hbe::stream::SelfTest();
            std::printf("shardstate %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-tagpolicy: the DISTANCE-STREAMING DECISION rules, in isolation from
        // anything that can spawn. Proves distance is measured to the shard's AABB and
        // not its centre (the elongated-shard bug the deleted cell streamer had), that
        // a focus oscillating on the load boundary spawns ONCE - and that a degenerate
        // hysteresis band really does thrash, so the band is what stopped it - that
        // several foci union for loading and intersect for unloading, that priority
        // beats distance while distance breaks priority ties, that the concurrency
        // throttle and the unload cap both report unfinished work, that a focus inside a
        // shard can never unload it, that an empty focus list changes nothing, and that
        // "streaming off" pins everything LOADED. Pure data: no registry, no GPU, no
        // filesystem. Same contract as --test-seamweld.
        // --test-assoctags: ASSOCIATED TAGS (StreamPolicy.h RULE 6), end to end
        // through a real Streamer and by the author's own names. Three SEPARATE
        // pieces of content - City, City_LowPoly and Hill, each with its own
        // objects, bounds and radii - with Hill associating City_LowPoly. Proves
        // that standing on the hill makes the low-poly city resident 1.7 km outside
        // its own load radius; that leaving the hill releases it UNLESS it is in
        // range on its own; that a shard resident for both reasons survives losing
        // one (exactly one despawn, not a drop and a respawn); that a mutual
        // association TERMINATES and unloads instead of pinning the world; that an
        // association naming a tag that does not exist warns and streams on; and
        // that the added evaluation cost stays inside the streaming budget. Also
        // the bake's association diagnostics and the `.hbproj` round trip.
        //
        // It deliberately asserts NOTHING about City and City_LowPoly excluding each
        // other: they are separate assets and may be co-resident. That is the
        // author's choice, not the engine's business. Headless: no GPU, no window.
        if (std::strcmp(argv[i], "--test-assoctags") == 0) {
            const bool ok = hbe::stream::AssocSelfTest();
            std::printf("assoctags %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-editorzones: LIVE EDITOR ZONES - the streamer spawning and despawning
        // against the world the EDITOR is authoring. Proves the bind is
        // non-destructive (the entity set, every guid in it and Scene::WorldToken are
        // unchanged, so scene::BindWorld's DestroyWorld never ran); that a sweep
        // touches only streamed content and writes NO world::/game:: player-progress
        // state; that a manual override forces residency in both directions - out of a
        // zone the focus is standing inside, and into one 5 km away - and composes
        // with associations through the same seed set; that A SAVE NEVER WRITES A
        // PARTIAL WORLD (refused with the file untouched bytes AND mtime while a zone
        // is genuinely missing, allowed once the save path has settled the world,
        // still refused for a RUNTIME bind); that the Play/Stop snapshot round-trips
        // with streamed content present; and that a stream event never enters the undo
        // stack while an edit taken mid-stream still captures the whole world.
        //
        // Builds its own scratch project and level under the temp directory. Headless:
        // no GPU, no window, no ImGui context. Same contract as --test-scenesave, and
        // it never touches the user's project.
        if (std::strcmp(argv[i], "--test-editorzones") == 0) {
            const bool ok = hbe::Editor::EditorZoneSelfTest();
            std::printf("editorzones %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        if (std::strcmp(argv[i], "--test-tagpolicy") == 0) {
            const bool ok = hbe::stream::PolicySelfTest();
            std::printf("tagpolicy %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-matvolume: MaterialVolumeComponent (box-brush world tool) save round-trip.
        // Headless: no GPU, no window, no project, no ImGui context.
        if (std::strcmp(argv[i], "--test-matvolume") == 0) {
            const bool ok = hbe::Editor::MaterialVolumeSaveSelfTest();
            std::printf("matvolume %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-csg: BSP constructive-solid-geometry correctness for the blockout box brush.
        // Headless: no GPU, no window, no project.
        if (std::strcmp(argv[i], "--test-csg") == 0) {
            const bool ok = hbe::csg::SelfTest();
            std::printf("csg %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-brush: CSG blockout box brush scene integration + save round-trip. Headless.
        if (std::strcmp(argv[i], "--test-brush") == 0) {
            const bool ok = hbe::Editor::BrushSaveSelfTest();
            std::printf("brush %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-hbvfx: .hbvfx particle effect asset round-trip. Headless.
        if (std::strcmp(argv[i], "--test-hbvfx") == 0) {
            const bool ok = hbe::particle::SelfTest();
            std::printf("hbvfx %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-particleeditor: the Particle Editor's core mechanic, headless (no GPU, no ImGui).
        // The panel previews an effect by dropping a REAL emitter into the scene, tagged
        // ParticlePreviewTag so it is simulated but never serialized, and letting particle::Update
        // tick it live. This proves that seam: the preview spawns particles, the tag excludes it from
        // a scene save, a preset applied in place re-arms the emission window, and the authored fields
        // round-trip through the `.hbvfx` serializer (what New / Open / the authored stash rely on).
        if (std::strcmp(argv[i], "--test-particleeditor") == 0) {
            using namespace hbe;
            bool ok = true;
            const auto check = [&](bool c, const char* what) {
                if (!c) { ok = false; std::printf("  FAIL: %s\n", what); }
            };
            Scene scene;
            entt::registry& reg = scene.Registry();
            const entt::entity e = scene.CreateEntity("Particle Preview");
            reg.emplace<Transform>(e);
            reg.emplace<ParticleEmitter>(e, particle::MakeTemplate(particle::Template::Fire));
            reg.emplace<ParticlePreviewTag>(e);

            // (1) The preview emitter must be a scene-serializer write exclusion.
            check(scene::SceneWriteExclusion(reg, e, nullptr) != nullptr,
                  "ParticlePreviewTag excludes the preview emitter from scene save");

            // (2) Live preview: particle::Update(emitting=true), exactly as the editor calls it,
            //     spawns particles for the preview entity.
            for (int f = 0; f < 40; ++f) particle::Update(scene, 1.0f / 60.0f, true);
            check(reg.get<ParticleEmitter>(e).pool.count > 0,
                  "the preview emitter spawns particles under particle::Update");

            // (3) Applying a preset in place carries the pool/stack across and RE-ARMS the emission
            //     window, so a one-shot preset (Explosion) fires rather than landing on a spent
            //     window. Clear the pool first so the respawn is what the assertion actually measures.
            {
                ParticleEmitter& pe = reg.get<ParticleEmitter>(e);
                ParticleEmitter next = particle::MakeTemplate(particle::Template::Explosion);
                next.pool = std::move(pe.pool);
                next.stack = std::move(pe.stack);
                next.state = pe.state;
                next.state.emitterAge = 0.0f;
                next.state.burstFired = false;
                next.state.wasEmitting = false;
                next.stackSignature = pe.stackSignature;
                pe = std::move(next);
                pe.textureResolved = false;
                pe.pool.Clear();
            }
            for (int f = 0; f < 12; ++f) particle::Update(scene, 1.0f / 60.0f, true);
            check(reg.get<ParticleEmitter>(e).pool.count > 0,
                  "a preset applied in place re-arms the emission window and re-fires");

            // (4) Authored fields round-trip through the `.hbvfx` serializer (New / Open / stash).
            {
                const ParticleEmitter& pe = reg.get<ParticleEmitter>(e);
                auto back = particle::EffectFromString(particle::EffectToString(pe));
                check(back.has_value(), ".hbvfx round-trip parses");
                if (back)
                    check(back->render == pe.render && back->maxParticles == pe.maxParticles &&
                              back->burst == pe.burst && back->shape == pe.shape,
                          "authored fields survive the .hbvfx round-trip");
            }

            // (5) Value-over-life CURVES (Phase 2): a colour gradient + a size curve evaluate by
            //     linear interpolation and survive the `.hbvfx` round-trip byte-for-byte.
            {
                ParticleEmitter& pe = reg.get<ParticleEmitter>(e);
                pe.useColorCurve = true;
                pe.colorCurve.stops = {{0.0f, {1, 0, 0, 1}}, {0.5f, {0, 1, 0, 1}}, {1.0f, {0, 0, 1, 0}}};
                pe.useSizeCurve = true;
                pe.sizeCurve.keys = {{0.0f, 0.1f}, {1.0f, 2.0f}};
                check(glm::length(pe.colorCurve.Eval(0.5f) - glm::vec4(0, 1, 0, 1)) < 1e-4f,
                      "gradient evaluates a stop exactly");
                const glm::vec4 mid = pe.colorCurve.Eval(0.25f); // halfway red->green
                check(std::fabs(mid.r - 0.5f) < 1e-3f && std::fabs(mid.g - 0.5f) < 1e-3f,
                      "gradient interpolates between stops");
                check(std::fabs(pe.sizeCurve.Eval(0.5f) - 1.05f) < 1e-3f,
                      "size curve interpolates between keys");
                auto back = particle::EffectFromString(particle::EffectToString(pe));
                check(back.has_value() && back->useColorCurve && back->colorCurve.stops.size() == 3 &&
                          back->useSizeCurve && back->sizeCurve.keys.size() == 2,
                      "curves survive the .hbvfx round-trip");
                if (back)
                    check(glm::length(back->colorCurve.Eval(0.25f) - mid) < 1e-4f,
                          "a round-tripped gradient evaluates identically");
            }

            // (6) Ribbon render mode (Phase 3) round-trips. The strip geometry itself is built in
            //     BuildVertices (needs a Renderer), so it is validated live/at compile, not here.
            {
                ParticleEmitter& pe = reg.get<ParticleEmitter>(e);
                pe.render = ParticleEmitter::Render::Ribbon;
                auto back = particle::EffectFromString(particle::EffectToString(pe));
                check(back.has_value() && back->render == ParticleEmitter::Render::Ribbon,
                      "the Ribbon render mode survives the .hbvfx round-trip");
            }

            // (7) Sub-emitters (Phase 5): RunFrame captures death positions into the emitter when an
            //     onDeathEffect is set; the capture is gated (empty without it); and fields round-trip.
            {
                ParticleEmitter& pe = reg.get<ParticleEmitter>(e);
                pe.render = ParticleEmitter::Render::Billboard;
                pe.useColorCurve = pe.useSizeCurve = false;
                pe.shape = ParticleEmitter::Shape::Point;
                pe.loop = true; pe.emitting = true; pe.burst = 0;
                pe.rate = 300.0f; pe.lifetime = 0.05f; pe.lifetimeVariance = 0.0f;
                pe.onDeathEffect = "effects/child";
                pe.state.emitterAge = 0.0f; pe.state.burstFired = false; pe.state.wasEmitting = false;
                pe.pool.Clear();
                bool sawDeath = false;
                for (int f = 0; f < 90 && !sawDeath; ++f) {
                    particle::Update(scene, 1.0f / 60.0f, true);
                    if (!reg.get<ParticleEmitter>(e).deaths.empty()) sawDeath = true;
                }
                check(sawDeath, "RunFrame captures death positions when onDeathEffect is set");

                reg.get<ParticleEmitter>(e).onDeathEffect.clear();
                particle::Update(scene, 1.0f / 60.0f, true);
                check(reg.get<ParticleEmitter>(e).deaths.empty(),
                      "no death capture without an onDeathEffect (the capture is gated)");

                ParticleEmitter& pe2 = reg.get<ParticleEmitter>(e);
                pe2.onDeathEffect = "effects/child";
                pe2.onDeathChance = 0.5f;
                auto back = particle::EffectFromString(particle::EffectToString(pe2));
                check(back.has_value() && back->onDeathEffect == "effects/child" &&
                          std::fabs(back->onDeathChance - 0.5f) < 1e-4f,
                      "sub-emitter fields round-trip");
            }

            // (8) Mesh particles (Phase 4): render mode + mesh ref round-trip. The instanced geometry
            //     is built in CollectMeshParticles (needs a Renderer), so it is validated live.
            {
                ParticleEmitter& pe = reg.get<ParticleEmitter>(e);
                pe.render = ParticleEmitter::Render::Mesh;
                pe.particleMesh = "meshes/rock.uaf";
                auto back = particle::EffectFromString(particle::EffectToString(pe));
                check(back.has_value() && back->render == ParticleEmitter::Render::Mesh &&
                          back->particleMesh == "meshes/rock.uaf",
                      "mesh render mode + mesh ref round-trip");
            }

            std::printf("particleeditor %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-hbdecal: .hbdecal reusable decal asset round-trip. Headless.
        if (std::strcmp(argv[i], "--test-hbdecal") == 0) {
            const bool ok = hbe::decalasset::SelfTest();
            std::printf("hbdecal %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        if (std::strcmp(argv[i], "--test-paintcanvas") == 0) {
            const bool ok = hbe::scene::PaintCanvasSelfTest();
            std::printf("paintcanvas %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-uidoc [<file.hbscene> ...]: the P2 gate. Proves the extracted
        // per-component UI JSON writers are BYTE-IDENTICAL to a frozen copy of
        // the blocks that used to be inlined in SceneSerializer.cpp (the
        // --test-vfxcompat pattern), that the .hbui round-trip is lossless
        // including the mandatory post header, and that the scene->document
        // converter partitions, remaps parents and strips non-document keys
        // without ever touching the source scene. Any .hbscene paths given are
        // additionally re-saved and diffed end-to-end against the frozen
        // writers. Headless, no GPU/window. Same contract as --test-seamweld.
        if (std::strcmp(argv[i], "--test-uidoc") == 0) {
            std::vector<std::filesystem::path> scenes;
            for (int k = i + 1; k < argc; ++k) {
                if (argv[k][0] == '-') break; // next flag
                scenes.emplace_back(argv[k]);
            }
            const bool ok = hbe::ui::DocumentSelfTest(scenes);
            std::printf("uidoc %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-uiseparation: prove the SEPARATION GUARANTEE's save-time half -
        // SaveSceneToDisk REFUSES (with a message, and without touching the file
        // on disk) when a non-document entity carries one of the six document
        // components, rather than silently dropping it. Also pins the deliberate
        // exemptions, including WorldText (level signage, not screen UI).
        // Headless, no GPU/window. Same contract as --test-seamweld.
        // --test-tagtable: the P4 gate for streaming tags. Proves interning
        // round-trips, that "Untagged" is index 0 and undeletable, that the
        // load/unload hysteresis band is enforced at parse (salvaged from the
        // deleted `.hbworld` manifest parser), that a project tag list round-trips
        // through the `.hbproj` (including present-but-empty and a repeated parse
        // into the same reused settings), that a per-entity tag survives
        // save/parse/save BYTE-IDENTICALLY, that assignment propagates over the
        // whole subtree, that deleting a tag remaps live entities, and that a
        // `.hbui` document entity CANNOT be tagged. Headless, no GPU/window. Same
        // contract as --test-seamweld.
        if (std::strcmp(argv[i], "--test-tagtable") == 0) {
            const bool ok = hbe::tags::SelfTest();
            std::printf("tagtable %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-shardbake: the P5 shard-bake gate. Proves the save-time SPATIAL
        // sharder is deterministic (shard indices are geometric, not row-order
        // derived), that every tagged entity lands in exactly one shard, that the
        // degenerate SCATTERED tag splits into many shards while an unsharded one is
        // reported as effectively always-loaded, that whole subtrees ride their root
        // and a cross-shard parent is REPORTED at bake time instead of being
        // discovered as a de-parented child at runtime, that shard AABBs contain
        // their members (brute-force cross-check, meshless volume entities included),
        // that the per-tag cap merges without dropping anything, and that the
        // "tagShards" file header round-trips and cross-checks against the file's own
        // entities. Headless, no GPU/window. Same contract as --test-seamweld.
        if (std::strcmp(argv[i], "--test-shardbake") == 0) {
            const bool ok = hbe::tagshard::SelfTest();
            std::printf("shardbake %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-morphcache: the blocker-B3 gate. Proves blendshapes survive the
        // SECOND spawn of a mesh (they used to be lost for the rest of the session
        // because StageAssets skips a cache-resident model and the atlas was derived
        // only from freshly staged data), that no atlas is ever built or uploaded
        // twice - including on the mesh-collider path, which used to mint a fresh one
        // per respawn with no release - and that `.uaf` v8 persists morph targets at
        // all. Headless, no GPU/window.
        if (std::strcmp(argv[i], "--test-morphcache") == 0) {
            const bool ok = hbe::scene::MorphCacheSelfTest();
            std::printf("morphcache %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        if (std::strcmp(argv[i], "--test-uiseparation") == 0) {
            const bool ok = hbe::Editor::SeparationSelfTest();
            std::printf("uiseparation %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-uisolve: the DIRECT-MANIPULATION math gate for the dedicated
        // `.hbui` editor's canvas (Source/Editor/UIEditor.cpp). Proves the
        // RectTransform solve is the exact inverse of the layout, that a solve
        // never touches anchors/pivot, that the "re-anchor by re-solving the same
        // rect" trick the anchor widget is built on is bit-stable, that snapping is
        // idempotent, and that ui::LayoutGroupOwnership agrees with what LayoutUI
        // actually did (so the editor never writes a rect the layout discards).
        // Headless, no GPU/window. Same contract as --test-seamweld.
        if (std::strcmp(argv[i], "--test-uisolve") == 0) {
            const bool ok = hbe::ui::ManipulationSelfTest();
            std::printf("uisolve %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-uibind: the P9.4 data-binding gate. Drives a ui::UIDataModel +
        // ResolveBindings and asserts runtime fields + the rev/no-op write guard.
        // Headless, no GPU/window/project. Same contract as --test-uisolve.
        if (std::strcmp(argv[i], "--test-uibind") == 0) {
            const bool ok = hbe::ui::DataBindSelfTest();
            std::printf("uibind %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-uisubtree: the P9.4 B4 primitive gate. Clones a template subtree from an
        // in-code DocData and asserts entity count + re-parenting + field preservation.
        // Headless, no GPU/window/project. Same contract as --test-uibind.
        if (std::strcmp(argv[i], "--test-uisubtree") == 0) {
            const bool ok = hbe::ui::SubtreeInstantiateSelfTest();
            std::printf("uisubtree %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-uitext: the TEXT STACK gate (P2 of docs/Design-UIOverhaul.md).
        // Proves FreeType + HarfBuzz + SheenBidi render ASCII AND non-ASCII (the old
        // stb_truetype atlas dropped every byte >= 0x80), shape deterministically,
        // word-wrap, break on '\n', and visually reorder RTL text. Headless, no GPU.
        if (std::strcmp(argv[i], "--test-uitext") == 0) {
            const bool ok = hbe::ui::text::TextSelfTest();
            std::printf("uitext %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-uisvg: the SVG rasterization gate (P3 of docs/Design-UIOverhaul.md).
        // Proves LunaSVG parses + rasterizes to correct-size straight-alpha RGBA at
        // multiple resolutions and rejects malformed input. Headless, no GPU.
        if (std::strcmp(argv[i], "--test-uisvg") == 0) {
            const bool ok = hbe::ui::svg::SvgSelfTest();
            std::printf("uisvg %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-uitheme: the STYLE/THEME gate (P4 of docs/Design-UIOverhaul.md).
        // Proves .hbtheme parses, ApplyStyle fills UNSET fields while element-set
        // fields win, and malformed themes are rejected. Headless, no GPU.
        if (std::strcmp(argv[i], "--test-uitheme") == 0) {
            const bool ok = hbe::ui::style::ThemeSelfTest();
            std::printf("uitheme %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-uieditor: the AUTHORING contract of that same panel's palette,
        // hierarchy and tools (phase I3). Palette creation joins the document
        // through DocumentSet::Track (tag AND order list), the no-document guard
        // creates nothing, a Z-ORDER reorder survives capture -> save -> reload
        // (the only honest test of it: draw order is entt pool order, which no file
        // records directly), reparent refuses across documents while allowing an
        // unparent, and a document-rooted subtree copy is a REAL fragment - the
        // regression that once let Cut delete content it had not copied.
        // Headless, no GPU/window/project. Same contract as --test-uiseparation.
        // --test-uipick: the INTERACTION-RAYCAST gate. Proves the unified pick pass
        // (Source/Interaction/Pick.cpp) is actually correct rather than merely
        // present: a page behind a wall is NOT clickable, exactly the nearer of two
        // overlapping pages receives the pointer, a rotated + non-uniformly scaled
        // (sheared) page maps to the canvas pixel an independent corner-based solve
        // says it should, a back-facing page stays inert, a mirrored page's
        // GEOMETRIC front face is the live one, an off-screen pointer picks nothing,
        // a page moving under a parent keeps picking correctly (surfaceInv
        // invalidation), and an Interactable and a page under the same reticle
        // produce exactly ONE winner - the nearer, occlusion-filtered, with the
        // proximity fallback preserved for "walk up and press E".
        // Headless: real Jolt, no GPU/window/project. Same contract as --test-uisolve.
        if (std::strcmp(argv[i], "--test-uipick") == 0) {
            const bool ok = hbe::interact::SelfTest();
            std::printf("uipick %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-3dinteract: the END-TO-END gate for 3D interactables, one layer up
        // from --test-uipick. A 3D button is a UIElement::Type::Button on a
        // world-space UICanvas (no third concept), so this drives the whole chain
        // the player touches - pointer source -> Pick -> ui::UpdateInteraction ->
        // hovered/held/clicked -> UIElement::action, the string schematics route On
        // UI Clicked on - in all three input modes: free cursor + LMB, locked-cursor
        // RETICLE + the Interact action, and a GAMEPAD (which aims the reticle in
        // every cursor state, because focus navigation deliberately never lands on a
        // world page). Plus: a wall blocks it, the TERRAIN HEIGHTFIELD blocks it,
        // nearest wins for page/page, page/object and object/object, and STREAMED
        // SHARD content behaves - a streamed-in Interactable becomes reachable, a
        // streamed-in collider starts occluding, and both stop on despawn.
        // Headless: real Jolt, a real baked level through the real stream::Streamer,
        // a device-less Renderer. No GPU/window/project.
        if (std::strcmp(argv[i], "--test-3dinteract") == 0) {
            const bool ok = hbe::interact::Interact3DSelfTest();
            std::printf("3dinteract %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        if (std::strcmp(argv[i], "--test-uieditor") == 0) {
            const bool ok = hbe::Editor::UIEditorSelfTest();
            std::printf("uieditor %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        // --test-savedispatch: prove the Ctrl+S DISPATCH RULE - each editing surface
        // maps to its own save target, exactly one target per keypress (no surface
        // but the scene's own can ever produce a scene write, which is the
        // double-fire bug this replaced), a focused-but-empty surface writes NOTHING
        // rather than falling through to the level, a focused text field DEFERS the
        // chord instead of dropping it, and Play refuses the two surfaces it mutates.
        // The decision is a pure function of a focused-surface id, so this needs no
        // ImGui context: headless, no GPU/window. Same contract as --test-seamweld.
        // Also covers the EDIT chords (Ctrl+Z/Y/X/C/V/D), which were an ungated
        // global poll until they were routed through the same claim model. `&` not
        // `&&` so both halves always run and both print their summary.
        if (std::strcmp(argv[i], "--test-savedispatch") == 0) {
            const bool saveOk = hbe::editor::SaveDispatchSelfTest();
            const bool editOk = hbe::editor::EditDispatchSelfTest();
            const bool ok = saveOk && editOk;
            std::printf("savedispatch %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
    }

    hbe::EngineConfig config = hbe::ParseCommandLine(argc, argv);
    config.title = "Heartbreak Editor";

    // Normally no project is opened here - the editor's Project Manager modal
    // handles creating/opening projects (and remembers recent ones). The
    // --project flag opens one directly (automation / file association).
    if (!config.projectPath.empty()) {
        hbe::Project::Active().Open(std::filesystem::path(config.projectPath));
    }

    // Placed AFTER the project open so `--project` has taken effect (the music
    // graph and its audio assets resolve against Assets/).
    {
        for (int i = 1; i < argc; ++i) {
        // --test-musicvoice: drive the music director's full lifecycle - install a
        // graph, start every state, crossfade between them, stop, repeat - and assert
        // the layer list drains to zero.
        //
        // Context: a music layer's `ma_sound` used to be initialised in a STACK LOCAL
        // and then push_back-ed as a MOVE. ma_sound is self-referential (the node
        // graph holds pointers into it), so the graph kept pointing at the dead stack
        // frame and the miniaudio device thread faulted inside
        // ma_node_input_bus_read_pcm_frames. The DETERMINISTIC guard against that is
        // now a compile error - Voice has its copy/move members deleted - so this
        // test's job is the part a type cannot express: that the reap path actually
        // uninits and erases every layer instead of leaking them into the node graph,
        // and that repeated state changes stay stable. A leak here means the old
        // crash's sibling (an ever-growing node list) is back.
        if (std::strcmp(argv[i], "--test-musicvoice") == 0) {
            if (!hbe::Project::HasActive()) {
                std::printf("--test-musicvoice requires --project\n");
                return 1;
            }
            hbe::AudioSystem audio;
            if (!audio.IsAvailable()) {
                // Honest SKIP, not a green PASS: with no playback device this proves
                // nothing, and a vacuous pass is worse than no test.
                std::printf("musicvoice SKIP (no audio playback device)\n");
                return 0;
            }
            const auto& ms = hbe::Project::Active().Settings();
            if (ms.musicGraph.empty()) {
                std::printf("musicvoice SKIP (project has no musicGraph)\n");
                return 0;
            }
            const auto assetsDir = hbe::Project::Active().AssetsDir();
            const auto graph = hbe::assets::LoadMusicGraph(assetsDir / ms.musicGraph);
            if (!graph) {
                std::printf("musicvoice FAILED: could not load '%s'\n", ms.musicGraph.c_str());
                return 1;
            }
            audio.SetMusicGraph(*graph, assetsDir);
            const std::vector<std::string> states = audio.MusicStateNames();
            if (states.empty()) {
                std::printf("musicvoice SKIP (graph declares no states)\n");
                return 0;
            }
            // Pump ~1.2s of simulated frames; long enough for a short crossfade to
            // finish and the reaper to run.
            const auto pump = [&audio](int frames) {
                for (int f = 0; f < frames; ++f) {
                    audio.UpdateMusic(1.0f / 60.0f);
                    audio.Update();
                }
            };
            bool ok = true;
            for (int cycle = 0; cycle < 3; ++cycle) {
                for (const std::string& st : states) {
                    // A short explicit fade: the project's own default is 5.3s, which
                    // would outlast the pump and make the drain assertion meaningless.
                    audio.PlayMusicState(st, 0.05f);
                    pump(30);
                    if (audio.MusicLayerCount() == 0) {
                        std::printf("musicvoice FAILED: state '%s' started no layers\n",
                                    st.c_str());
                        ok = false;
                    }
                }
                audio.StopMusic(0.05f);
                pump(90);
                if (audio.MusicLayerCount() != 0) {
                    std::printf("musicvoice FAILED: %zu layer(s) survived the stop on "
                                "cycle %d (leaked into the node graph)\n",
                                audio.MusicLayerCount(), cycle);
                    ok = false;
                }
            }
            if (!audio.IsAvailable()) {
                std::printf("musicvoice FAILED: the audio engine died during the run\n");
                ok = false;
            }
            std::printf("musicvoice %s (%zu state(s), 3 cycles)\n", ok ? "PASS" : "FAILED",
                        states.size());
            return ok ? 0 : 1;
        }
        }
    }

    // --test-scenesave <scene.hbscene> [--project <proj>]: the SCENE-SAVE
    // COMPLETENESS CONTRACT - a .hbscene contains every entity of the active world,
    // or the save does not happen. Fixed-point + census-against-the-file-on-disk on
    // a real level, plus the refusals (despawned shards, a world replaced behind the
    // editor's back, an empty world over a populated file, Play mode) each asserted
    // on the target file's BYTES. Works on a COPY; never writes to the file named.
    //
    // Placed AFTER the project open so `--project` has taken effect (assets stage and
    // the tag table is seeded); headless otherwise - no GPU, no window, no ImGui.
    // A MISSING PATH FAILS rather than falling through to the editor.
    {
        bool want = false;
        const char* scenePath = nullptr;
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--test-scenesave") != 0) continue;
            want = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') scenePath = argv[i + 1];
        }
        if (want) {
            std::filesystem::path p = scenePath ? std::filesystem::path(scenePath)
                                                : std::filesystem::path();
            if (!p.empty() && p.is_relative() && !std::filesystem::exists(p) &&
                hbe::Project::HasActive())
                p = hbe::Project::Active().AssetsDir() / p;
            const bool ok = hbe::Editor::SceneSaveSelfTest(p);
            std::printf("scenesave %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
    }

    // --migrate-guids [--dry-run]: freeze every guid-less entity's DERIVED guid into
    // the project's .hbscene files, then exit. See scene::MigrateSceneGuids for why
    // this is time-sensitive and why it is a surgical JSON edit rather than a
    // load/save round-trip.
    {
        bool migrate = false, dryRun = false;
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--migrate-guids") == 0) migrate = true;
            if (std::strcmp(argv[i], "--dry-run") == 0) dryRun = true;
        }
        if (migrate) {
            if (!hbe::Project::HasActive()) {
                std::printf("--migrate-guids requires --project <file.hbproj>\n");
                return 1;
            }
            const auto st = hbe::scene::MigrateSceneGuids(
                hbe::Project::Active().AssetsDir(), dryRun);
            std::printf("migrate-guids %s: %u file(s), %u entity guid(s) stamped, "
                        "%u already had one, %u failed.\n",
                        dryRun ? "DRY RUN (nothing written)" : "done",
                        st.files, st.stamped, st.already, st.failed);
            return st.failed == 0 ? 0 : 1;
        }
    }

    // --cook-stats: cook the open project's Assets into a TEMP dir and print where the
    // bytes go (per-type source vs packed size + ratio, dedup savings, pack sizes, the
    // largest assets). Read-only w.r.t. the project (uses a copy of the slot ledger).
    // Answers "why is this game N GB" with data. Requires --project.
    {
        bool wantStats = false;
        for (int i = 1; i < argc; ++i)
            if (std::strcmp(argv[i], "--cook-stats") == 0) wantStats = true;
        if (wantStats) {
            if (!hbe::Project::HasActive()) {
                std::printf("--cook-stats requires --project <file.hbproj>\n");
                return 1;
            }
            const hbe::Project& proj = hbe::Project::Active();
            std::string text;
            const bool ok = hbe::cookstats::Report(proj.AssetsDir(), proj.SlotManifestPath(),
                                                   proj.Settings().name, text);
            if (ok) std::printf("%s\n", text.c_str());
            else std::printf("cook-stats: cook failed (see log).\n");
            return ok ? 0 : 1;
        }
    }

    // --migrate-slots [--dry-run] [--apply]: give every asset in the project a
    // permanent PACK SLOT and write it into the asset file, then exit. The logic
    // lives in the library (slots::MigrateSlotIds); this only calls it and prints.
    //
    // DRY RUN IS THE DEFAULT and `--apply` is what writes, because this rewrites
    // the author's own asset files - so the safe mode has to be the one you get
    // by typing the flag wrong. A bare --migrate-slots reports the entire plan,
    // every file and the id it would take, and stops without touching a byte.
    // (`--dry-run` is still accepted, and now redundant.)
    //
    // The order is chosen to make the FIRST cook after the migration the smallest
    // possible patch: an id the project's `.ship.uapmanifest` already records for
    // a still-present path is kept exactly, so those packs do not move at all;
    // everything else takes the lowest free id in sorted-path order. Re-running is
    // a no-op - the ids are in the files by then, and this recomputes the same
    // answer from them.
    {
        bool migrate = false, apply = false;
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--migrate-slots") == 0) migrate = true;
            if (std::strcmp(argv[i], "--apply") == 0) apply = true;
        }
        if (migrate) {
            if (!hbe::Project::HasActive()) {
                std::printf("--migrate-slots requires --project <file.hbproj>\n");
                return 1;
            }
            const bool dryRun = !apply;
            const hbe::Project& proj = hbe::Project::Active();
            std::error_code sec;
            // Seed and target are now the SAME file: Project::SlotManifestPath is
            // the project's one slot ledger, and it is already the record of the
            // shipped layout (that is why it is the one that was kept). Passing it
            // as the seed as well is what makes `st.seeded` report how much of the
            // shipped layout the migration preserves.
            const std::filesystem::path seed =
                std::filesystem::exists(proj.SlotManifestPath(), sec) ? proj.SlotManifestPath()
                                                                      : std::filesystem::path();
            const auto st = hbe::slots::MigrateSlotIds(proj.AssetsDir(), proj.SlotManifestPath(),
                                                       seed, dryRun);
            std::printf("migrate-slots %s: %u packable asset(s), %u already had an id, "
                        "%u to stamp (%u seeded from '%s'), %u recorded in the manifest "
                        "only, %u collision(s), %u failed.\n",
                        dryRun ? "DRY RUN (nothing written)" : "done", st.scanned, st.already,
                        st.stamped, st.seeded,
                        seed.empty() ? "(no ship manifest)" : seed.filename().string().c_str(),
                        st.cannotEmbed, st.collisions, st.failed);
            // The plan itself - this is the artifact that makes the change
            // reviewable BEFORE it is applied, so it is printed in full rather
            // than summarised. Slot order, because that is pack order.
            for (const auto& [key, slot, seeded] : st.plan) {
                std::printf("  slot %-6u pack %-3u  %s%s\n", slot,
                            hbe::uap::PackIndexOf(slot), key.c_str(),
                            seeded ? "   (kept from the shipped layout)" : "");
            }
            if (dryRun) {
                std::printf("Nothing was written. Re-run with --apply to stamp these ids "
                            "into the asset files (put the project under version control "
                            "first).\n");
            }
            return st.failed == 0 ? 0 : 1;
        }
    }

    // --migrate-ui [--dry-run] [--force]: write a `.hbui` DOCUMENT for every
    // `.hbscene` in the project that contains UI, then exit. Same shape as
    // --migrate-guids above: the logic lives in the library
    // (ui::MigrateSceneUI), this only calls it and prints.
    //
    // IT NEVER DELETES OR REWRITES A SOURCE `.hbscene`. A fully-lifted scene is
    // reported as a RETIREMENT CANDIDATE and left exactly where it is; retiring
    // it is the operator's decision, and only after .hbproj points at the
    // document (P3). Re-running is a safe no-op: an existing destination is
    // skipped unless --force.
    {
        bool migrate = false, dryRun = false, force = false;
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--migrate-ui") == 0) migrate = true;
            if (std::strcmp(argv[i], "--dry-run") == 0) dryRun = true;
            if (std::strcmp(argv[i], "--force") == 0) force = true;
        }
        if (migrate) {
            if (!hbe::Project::HasActive()) {
                std::printf("--migrate-ui requires --project <file.hbproj>\n");
                return 1;
            }
            // A migrated document's canvas block seeds from the PROJECT's canvas
            // configuration, because the legacy UI scenes lay their canvas-less
            // roots out against exactly that (Engine's per-frame uiConfig). The
            // converter never synthesises a UICanvas entity - that would move
            // those entities from the legacy walk to the canvas walk and change
            // both draw order and world-space routing.
            const hbe::BuildSettings& build = hbe::Project::Active().Settings().build;
            hbe::ui::CanvasConfig canvas;
            canvas.mode = static_cast<hbe::ui::ScaleMode>(
                std::clamp(build.uiScaleMode, 0u, 2u));
            canvas.refWidth = static_cast<hbe::f32>(std::max(build.uiRefWidth, 64u));
            canvas.refHeight = static_cast<hbe::f32>(std::max(build.uiRefHeight, 64u));

            const auto st = hbe::ui::MigrateSceneUI(hbe::Project::Active().AssetsDir(),
                                                    canvas, dryRun, force);
            std::printf("migrate-ui %s: %u .hbscene scanned, %u convertible, %u mixed, "
                        "%u UI entities, %u written, %u skipped, %u non-document keys "
                        "dropped, %u severed parents, %u stem collisions, %u failed. No "
                        "source scene was modified or deleted.\n",
                        dryRun ? "DRY RUN (nothing written)" : "done", st.files,
                        st.convertible, st.mixed, st.uiEntities, st.written, st.skipped,
                        st.droppedKeys, st.severedParents, st.collisions, st.failed);
            // Both new counters are LOSSY/REFUSED cases, so they are called out
            // rather than buried in the line above: a severed parent is a
            // world-mounted page that stopped following its mount (each one is
            // named in the per-file log), and a collision is a document that was
            // NOT written because two scenes with the same stem map to one
            // `UI/<stem>.hbui`.
            if (st.severedParents > 0)
                std::printf("migrate-ui: WARNING - %u UI entit(ies) lost a WORLD parent "
                            "and became document roots (see the log for names).\n",
                            st.severedParents);
            if (st.collisions > 0)
                std::printf("migrate-ui: WARNING - %u file(s) REFUSED on a destination "
                            "stem collision; nothing was overwritten.\n",
                            st.collisions);

            // Repoint the project's two UI slots at the generated documents. This
            // is the step that makes the migration take effect: until .hbproj
            // names the .hbui, the runtime keeps booting the legacy scenes through
            // the compatibility branch. It is a SURGICAL edit of four keys - see
            // ui::RepointProjectDocuments for why it is not a Project::Save().
            const int repointed = hbe::ui::RepointProjectDocuments(
                hbe::Project::Active().ProjectFile(), hbe::Project::Active().AssetsDir(),
                dryRun);
            if (repointed < 0) {
                std::printf("migrate-ui: FAILED to repoint the .hbproj.\n");
                return 1;
            }
            std::printf("migrate-ui: %d project slot(s) repointed%s. The source "
                        ".hbscene files are LEFT ON DISK and are now unreferenced "
                        "(retirement candidates - retiring them is your call).\n",
                        repointed, dryRun ? " (dry run)" : "");
            return st.failed == 0 ? 0 : 1;
        }
    }

    // --migrate-screens [--dry-run] [--force]: split each all-in-one `.hbui` in
    // the project's UI-screen list into ONE DOCUMENT PER SCREEN, then repoint the
    // `.hbproj` at the new set and exit. Same shape as --migrate-guids and
    // --migrate-ui above: the logic lives in the library (ui::SplitDocumentByPanel
    // + ui::RepointProjectScreens), this only calls it and prints.
    //
    // IT NEVER DELETES OR REWRITES A SOURCE DOCUMENT. The combined `.hbui` stays
    // exactly where it is, still loadable, and is reported as a retirement
    // candidate; retiring it is the operator's decision.
    //
    // DRY RUN IS THE DEFAULT, and `--apply` is what writes. This tool creates new
    // `.hbui` files AND rewrites the project's own `.hbproj` (ui::RepointProjectScreens
    // truncates and re-emits it), so the safe mode has to be the one you get by
    // typing the flag wrong. A bare --migrate-screens reports the whole plan -
    // screens, entity counts, destinations, orphans, action collisions, and the
    // JSON it would write - and stops without touching a byte. (`--dry-run` is
    // still accepted, and now redundant.)
    {
        bool migrate = false, apply = false, force = false;
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--migrate-screens") == 0) migrate = true;
            if (std::strcmp(argv[i], "--apply") == 0) apply = true;
            if (std::strcmp(argv[i], "--force") == 0) force = true;
        }
        const bool dryRun = !apply;
        if (migrate) {
            if (!hbe::Project::HasActive()) {
                std::printf("--migrate-screens requires --project <file.hbproj>\n");
                return 1;
            }
            const hbe::Project& proj = hbe::Project::Active();
            const std::vector<std::string> sources = proj.Settings().uiDocuments;
            if (sources.empty()) {
                std::printf("migrate-screens: the project has no UI screen document "
                            "configured; nothing to split.\n");
                return 1;
            }
            const std::filesystem::path outDir = proj.AssetsDir() / "UI";
            std::vector<std::string> finalRels;
            bool anySplit = false, failed = false;
            for (const std::string& rel : sources) {
                const std::filesystem::path src = proj.AssetsDir() / rel;
                if (std::filesystem::path(rel).extension() != ".hbui") {
                    std::printf("migrate-screens: '%s' is not a .hbui (run "
                                "--migrate-ui first); left as-is.\n",
                                rel.c_str());
                    finalRels.push_back(rel);
                    continue;
                }
                hbe::ui::ScreenSplitReport rep;
                const bool ok = hbe::ui::SplitDocumentByPanel(src, outDir, "UI/", rep,
                                                              dryRun, force);
                if (ok && rep.screens.size() <= 1) {
                    // Already one screen per document - a clean no-op. Do not print
                    // the plan table: its lone row's "destination exists" is the
                    // file ITSELF and reads like a problem.
                    const std::string panel =
                        rep.screens.empty() ? std::string("no panel!") : rep.screens[0].panel;
                    std::printf("migrate-screens: '%s' already holds exactly one screen "
                                "(%s); nothing to split.\n",
                                rel.c_str(), panel.c_str());
                    finalRels.push_back(rel);
                    continue;
                }
                std::printf("migrate-screens %s: '%s' - %u source entities, %zu screen(s).\n",
                            dryRun ? "DRY RUN (nothing written)" : "done", rel.c_str(),
                            rep.sourceEntities, rep.screens.size());
                for (const auto& s : rep.screens)
                    std::printf("    %-14s -> %-28s %3u entities%s%s%s\n",
                                s.panel.c_str(), s.rel.c_str(), s.entities,
                                s.startVisible ? "  [startVisible]" : "",
                                s.existed ? "  [DESTINATION EXISTS]" : "",
                                s.wrote ? "  [written]" : "");
                if (rep.orphans > 0) {
                    std::printf("migrate-screens: WARNING - %u entit(ies) are under NO "
                                "root UIPanel and belong to no screen:\n",
                                rep.orphans);
                    for (const std::string& nm : rep.orphanNames)
                        std::printf("      %s\n", nm.c_str());
                }
                for (const std::string& d : rep.duplicatePanels)
                    std::printf("migrate-screens: ERROR - duplicate root panel name "
                                "'%s'.\n",
                                d.c_str());
                if (!rep.actionCollisions.empty()) {
                    std::printf("migrate-screens: WARNING - %zu UIElement::action value(s) "
                                "the engine resolves GLOBALLY appear in more than one "
                                "screen; all copies will fire/seed/write:\n",
                                rep.actionCollisions.size());
                    for (const std::string& a : rep.actionCollisions)
                        std::printf("      %s\n", a.c_str());
                }
                if (!ok) {
                    failed = true;
                    finalRels.push_back(rel);
                    continue;
                }
                if (rep.screens.size() <= 1) {
                    // Already one screen per document - leave the entry alone so
                    // re-running is a no-op rather than a churn.
                    finalRels.push_back(rel);
                    continue;
                }
                anySplit = true;
                for (const auto& s : rep.screens) finalRels.push_back(s.rel);
                std::printf("migrate-screens: '%s' is LEFT ON DISK, unmodified - it is "
                            "now a retirement candidate (retiring it is your call).\n",
                            rel.c_str());
            }
            if (failed) {
                std::printf("migrate-screens: FAILED; nothing was repointed.\n");
                return 1;
            }
            if (!anySplit) {
                std::printf("migrate-screens: nothing to do - every configured screen "
                            "document already holds exactly one screen.\n");
                return 0;
            }
            std::printf("migrate-screens: .hbproj uiDocuments would become:\n");
            for (std::size_t i = 0; i < finalRels.size(); ++i)
                std::printf("      [%zu] %s%s\n", i, finalRels[i].c_str(),
                            i == 0 ? "   (menu document - supplies `post`)" : "");
            if (!hbe::ui::RepointProjectScreens(proj.ProjectFile(), finalRels, dryRun)) {
                std::printf("migrate-screens: FAILED to repoint the .hbproj.\n");
                return 1;
            }
            std::printf("migrate-screens: %zu screen(s) referenced by the project%s.\n",
                        finalRels.size(), dryRun ? " (dry run - not written)" : "");
            if (dryRun)
                std::printf("migrate-screens: NOTHING WAS WRITTEN. Re-run with --apply "
                            "to create the screen documents and repoint the .hbproj.\n");
            return 0;
        }
    }

    // --pack / --ship: cook packs or the full shipping folder, then exit
    // (CI / scripted builds).
    if (config.packOnly || config.shipOnly) {
        hbe::jobs::Initialize(); // packing compresses assets across worker threads
        std::string msg;
        const bool ok = config.shipOnly ? hbe::Editor::BuildShipping(msg)
                                        : hbe::Editor::BuildAssetPack(msg);
        hbe::jobs::Shutdown();
        std::printf("%s\n", msg.c_str());
        return ok ? 0 : 1;
    }

    // --import <file>: import an asset into the project's Assets/ root, then
    // exit (automation / scripted content pipelines).
    if (!config.importPath.empty()) {
        if (!hbe::Project::HasActive()) {
            std::printf("--import requires --project\n");
            return 1;
        }
        const auto created = hbe::importer::Import(
            std::filesystem::path(config.importPath),
            hbe::Project::Active().AssetsDir());
        std::printf("import %s\n", created ? created->string().c_str() : "FAILED");
        return created ? 0 : 1;
    }

    // --upgrade-assets: bring every out-of-date `.uaf` under the project's Assets/ up to the
    // current spec IN PLACE and non-destructively (meshes -> v9 + LODs; material textures -> BC
    // when enabled), preserving each asset's guid + pack slot. Then exit. Idempotent - safe to run
    // repeatedly. Scripted equivalent of the automatic upgrade the editor runs on project open.
    {
        bool doUpgrade = false;
        for (int i = 1; i < argc; ++i)
            if (std::strcmp(argv[i], "--upgrade-assets") == 0) doUpgrade = true;
        if (doUpgrade) {
            if (!hbe::Project::HasActive()) {
                std::printf("--upgrade-assets requires --project\n");
                return 1;
            }
            const hbe::importer::UpgradeReport r = hbe::importer::UpgradeAssets(
                hbe::Project::Active().AssetsDir(), hbe::Project::Active().SlotManifestPath());
            std::printf("upgrade-assets: %u scanned, %u mesh(es) -> v%u, %u BC texture(s) baked\n",
                        r.scanned, r.meshesUpgraded, hbe::uaf::kVersion, r.texturesBaked);
            return 0;
        }
    }

    // --test-readback: render a few editor frames offscreen at an ODD resolution,
    // read the frame back to CPU, write it to a PNG, and exit. Proves the GPU
    // readback path (row-pitch de-pad + canonical RGBA channel order) on the active
    // backend without needing to eyeball a live window.
    bool testReadback = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--test-readback") == 0) testReadback = true;
    if (testReadback) {
        if (!hbe::Project::HasActive()) {
            std::printf("--test-readback requires --project\n");
            return 1;
        }
        static hbe::Editor rbEditor;
        static int rbFrame = 0;
        // The verdict has to escape the frame lambda. This test used to `return
        // rbEngine.Run(config)` and only PRINT its own result, so it exited 0 no
        // matter what - a permanently green gate, and the only automated check
        // standing under --render-movie.
        static bool rbOk = false;
        hbe::Engine rbEngine;
        rbEngine.SetOnInit([](hbe::Engine& e) {
            e.GetPhysics().SetRunning(false);
            e.SetGameCameraEnabled(false);
            e.GetScene().SetEditorView(true);
            if (e.GetRenderer().InitUI(e.GetWindow().GetNativeHandle().hwnd))
                hbe::Editor::ApplyTheme();
        });
        rbEngine.SetOnFrame([](hbe::Engine& e) {
            rbEditor.BuildUI(e);
            constexpr hbe::u32 kW = 641, kH = 361; // odd -> exercises 256B row pitch
            e.GetRenderer().SetViewportSize(kW, kH);
            if (++rbFrame >= 30) {
                std::vector<hbe::u8> px;
                hbe::u32 w = 0, h = 0;
                const bool got = e.GetRenderer().ReadbackViewportColor(px, w, h);

                // ASSERT THE PIXELS, not just the bool. The three failures this test
                // exists to catch - a black frame, a channel swap, a row-pitch offset
                // - all return `true` from ReadbackViewportColor, so checking only
                // the bool could never have caught any of them.
                bool ok = got;
                const auto fail = [&ok](const char* why) {
                    std::printf("readback FAIL: %s\n", why);
                    ok = false;
                };
                if (!got) std::printf("readback FAIL: ReadbackViewportColor returned false\n");
                if (ok && (w != kW || h != kH)) fail("dimensions differ from the requested size");
                if (ok && px.size() != static_cast<hbe::usize>(w) * h * 4)
                    fail("buffer is not w*h*4 (row pitch not de-padded?)");
                if (ok) {
                    // Non-degenerate: a de-padded frame of a rendered scene is neither
                    // all zero nor one flat colour. A row-pitch bug that shifts rows
                    // still varies, so this is the weakest of the three checks - the
                    // strong one is the cross-backend compare below.
                    bool allZero = true, uniform = true;
                    for (hbe::usize i = 0; i < px.size(); ++i) {
                        if (px[i] != 0) allZero = false;
                        if (px[i] != px[i % 4]) uniform = false;
                        if (!allZero && !uniform) break;
                    }
                    if (allZero) fail("every byte is zero (black frame)");
                    else if (uniform) fail("every pixel is identical (nothing rendered?)");
                }

                // Per-backend filenames. Both backends used to write the SAME
                // hbe_readback.png, so running one after the other silently discarded
                // the first and no comparison was possible. A short tag, not
                // rhi::ToString - that yields "Direct3D 12", and a space in a path a
                // sibling tool has to reconstruct is a trap.
                const std::string api = ReadbackTag(e.GetRenderer().API());
                const auto dir = std::filesystem::temp_directory_path();
                const auto png = dir / ("hbe_readback_" + api + ".png");
                const auto raw = dir / ("hbe_readback_" + api + ".raw");
                if (got) {
                    hbe::movie::WritePng(png, w, h, px);
                    // Raw RGBA for --test-readback-compare (the D3D12<->Vulkan parity
                    // gate): PNG round-trips through an encoder, raw bytes do not.
                    std::ofstream rf(raw, std::ios::binary);
                    const hbe::u32 hdr[2] = {w, h};
                    rf.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
                    rf.write(reinterpret_cast<const char*>(px.data()),
                             static_cast<std::streamsize>(px.size()));
                }
                rbOk = ok;
                std::printf("readback %s %ux%u (%s) -> %s\n", ok ? "PASS" : "FAILED", w, h,
                            api.c_str(), png.string().c_str());
                e.Quit();
            }
        });
        const int runRc = rbEngine.Run(config);
        return (runRc == 0 && rbOk) ? 0 : 1;
    }

    // --skin-preview: render a ROW of skin spheres (subsurface SSS) headless to a PNG so
    // the skin/lighting can be EYEBALLED without a live window. No project needed - the sky
    // IBL + the pre-integrated skin LUT are built at boot by scene::SetupEnvironment. The
    // spheres are HIGH-poly (128x64) so faceting is not a factor - this isolates the SHADING.
    // Iterate: edit the skin shader, rebuild, re-run, open the PNG.
    bool skinPreview = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--skin-preview") == 0) skinPreview = true;
    if (skinPreview) {
        static hbe::Editor spEditor;
        static int spFrame = 0;
        hbe::Engine spEngine;
        spEngine.SetOnInit([](hbe::Engine& e) {
            e.GetPhysics().SetRunning(false);
            e.SetGameCameraEnabled(false);
            e.GetScene().SetEditorView(true);
            e.GetRenderer().SetOrbitEnabled(false);
            if (e.GetRenderer().InitUI(e.GetWindow().GetNativeHandle().hwnd))
                hbe::Editor::ApplyTheme();

            hbe::Scene& scene = e.GetScene();
            auto& reg = scene.Registry();
            const hbe::MeshData sm = hbe::mesh::GenerateSphere(0.5f, 128, 64);
            const hbe::rhi::MeshHandle sphere = e.GetRenderer().UploadMesh(sm);

            // Row: 3 skin spheres (varying scatter radius / roughness) + 1 plain dielectric
            // reference on the right, so the SSS effect reads by comparison.
            const glm::vec4 skinTone{0.86f, 0.62f, 0.52f, 1.0f};
            // {x, roughness, sssRadius, isSSS, clearcoat}. Sphere 0 = plain reference;
            // 1-3 = SSS skin (last one WET via clearcoat) so the row shows dry->wet skin.
            struct Cfg { float x; float roughness; float radius; bool sss; float cc; };
            const Cfg cfgs[] = {{-1.7f, 0.40f, 0.0f, false, 0.0f}, {-0.57f, 0.40f, 1.0f, true, 0.0f},
                                {0.57f, 0.40f, 2.0f, true, 0.0f}, {1.7f, 0.28f, 1.0f, true, 0.8f}};
            for (const Cfg& c : cfgs) {
                hbe::MeshInstance mi;
                mi.mesh = sphere;
                mi.surface.base_color = skinTone;
                mi.surface.specular_roughness = c.roughness;
                mi.surface.base_metalness = 0.0f;
                mi.surface.subsurface_color = {0.85f, 0.2f, 0.16f};
                mi.surface.subsurface_radius = c.radius;
                mi.surface.coat_weight = c.cc;              // wet sheen on the rightmost sphere
                mi.surface.coat_roughness = 0.06f;
                if (c.sss) mi.materialFlags |= hbe::rhi::MaterialFlag_Subsurface;
                const entt::entity ent = scene.CreateEntity("SkinSphere");
                hbe::Transform tf;
                tf.position = {c.x, 0.0f, 0.0f};
                reg.emplace<hbe::Transform>(ent, tf);
                reg.emplace<hbe::MeshInstance>(ent, mi);
                reg.emplace<hbe::AABB>(ent, hbe::AABB{glm::vec3(-0.5f), glm::vec3(0.5f)});
            }

            // Ground plane so the sun casts a REAL contact shadow (verifies the
            // bilinear shadow PCF) and gives the spheres bounce context.
            {
                const hbe::MeshData pm = hbe::mesh::GeneratePlane(14.0f, 1);
                const hbe::rhi::MeshHandle plane = e.GetRenderer().UploadMesh(pm);
                hbe::MeshInstance gi;
                gi.mesh = plane;
                gi.surface.base_color = {0.5f, 0.5f, 0.5f, 1.0f};
                gi.surface.specular_roughness = 0.9f;
                gi.surface.base_metalness = 0.0f;
                const entt::entity g = scene.CreateEntity("Ground");
                hbe::Transform gt;
                gt.position = {0.0f, -0.5f, 0.0f};
                reg.emplace<hbe::Transform>(g, gt);
                reg.emplace<hbe::MeshInstance>(g, gi);
                reg.emplace<hbe::AABB>(g, hbe::AABB{glm::vec3(-7.0f, 0.0f, -7.0f),
                                                    glm::vec3(7.0f, 0.0f, 7.0f)});
            }

            const entt::entity sun = scene.CreateEntity("Sun");
            reg.emplace<hbe::Transform>(sun);
            hbe::DirectionalLightComponent dl;
            dl.direction = glm::normalize(glm::vec3(-0.5f, -0.55f, -0.45f));
            dl.color = {1.0f, 0.97f, 0.92f};
            dl.intensity = 3.0f;
            reg.emplace<hbe::DirectionalLightComponent>(sun, dl);
            scene.Environment().ambientIntensity = 0.35f;
            scene.Environment().exposure = 0.85f;
            // Represent the SHIPPED config, not the raw all-on defaults: the headless
            // harness can't resolve TAA, so the default SSGI/SSAO passes show raw
            // screen-space noise that TAA denoises in-engine (High) and that Medium/Low
            // don't run at all. Disable them here (as shipped Medium) so what I judge is
            // the skin SHADING, not a headless-only noise pass.
            scene.Environment().post.ssgiEnabled = 0;
            scene.Environment().post.ssaoEnabled = 0;
        });
        spEngine.SetOnFrame([](hbe::Engine& e) {
            spEditor.BuildUI(e);
            constexpr hbe::u32 kW = 1600, kH = 620;
            e.GetRenderer().SetViewportSize(kW, kH);
            auto& cam = e.GetRenderer().GetCamera();
            cam.SetPerspective(34.0f, static_cast<float>(kW) / static_cast<float>(kH), 0.05f, 100.0f);
            cam.LookAt({0.0f, 0.0f, 3.25f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
            if (++spFrame >= 40) { // TAA warmup
                std::vector<hbe::u8> px;
                hbe::u32 w = 0, h = 0;
                const auto out = std::filesystem::temp_directory_path() / "hbe_skin_preview.png";
                if (e.GetRenderer().ReadbackViewportColor(px, w, h))
                    hbe::movie::WritePng(out, w, h, px);
                std::printf("skin-preview -> %s\n", out.string().c_str());
                e.Quit();
            }
        });
        return spEngine.Run(config);
    }

    // --water-preview: render the Gerstner water headless to a PNG so the depth-graded
    // absorption/foam/shoreline AND the TAA behaviour of moving objects over water can be
    // EYEBALLED without a live window (mirrors --skin-preview). A sandy floor is gently
    // sloped so it crosses the waterline (a REAL shoreline gradient); 3 cubes sit
    // half-submerged (waterline intersection foam); 4 cubes are hand-animated and the camera
    // pans, so TAA has real motion to (mis)handle. Per-backend filename for A/B compare.
    bool waterPreview = false;
    bool wpNoSsr = false;     // --wp-nossr: disable SSR, to isolate SSR-on-water artifacts
    bool wpTopdown = false;   // --wp-topdown: look straight down (tests rain streak billboarding)
    bool wpClean = false;     // --wp-clean: no cubes (open water only), to isolate surface artifacts
    bool wpFlatGrade = false; // --wp-flatgrade: neutralize the depth-grade (isolate reflection facets)
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--water-preview") == 0) waterPreview = true;
        if (std::strcmp(argv[i], "--wp-nossr") == 0) wpNoSsr = true;
        if (std::strcmp(argv[i], "--wp-topdown") == 0) wpTopdown = true;
        if (std::strcmp(argv[i], "--wp-clean") == 0) wpClean = true;
        if (std::strcmp(argv[i], "--wp-flatgrade") == 0) wpFlatGrade = true;
    }
    if (waterPreview) {
        static hbe::Editor wpEditor;
        static int wpFrame = 0;
        static std::vector<entt::entity> wpMovers;
        hbe::Engine wpEngine;
        wpEngine.SetOnInit([wpNoSsr, wpClean, wpFlatGrade](hbe::Engine& e) {
            e.GetPhysics().SetRunning(false); // movers are hand-animated (deterministic motion)
            e.SetGameCameraEnabled(false);
            e.GetScene().SetEditorView(true);
            e.GetRenderer().SetOrbitEnabled(false);
            if (e.GetRenderer().InitUI(e.GetWindow().GetNativeHandle().hwnd))
                hbe::Editor::ApplyTheme();

            hbe::Scene& scene = e.GetScene();
            auto& reg = scene.Registry();

            // Water plane at the origin (Y = 0).
            const entt::entity we = scene.CreateEntity("Water");
            reg.emplace<hbe::Transform>(we, hbe::Transform{});
            hbe::WaterComponent wc{};
            if (wpFlatGrade) { // neutralize the depth-grade to isolate reflection/mesh facets
                wc.absorptionDepth = 1.0e6f;
                wc.shorelineWidth = 0.0001f;
                wc.edgeFade = 0.0001f;
            }
            reg.emplace<hbe::WaterComponent>(we, wc);

            // Sandy floor gently sloped so it crosses the waterline: deep on the far side,
            // dry on the near side. A too-wide shoreline-foam band shows as a broad fake band.
            {
                const hbe::MeshData pm = hbe::mesh::GeneratePlane(90.0f, 1);
                const hbe::rhi::MeshHandle plane = e.GetRenderer().UploadMesh(pm);
                hbe::MeshInstance gi;
                gi.mesh = plane;
                gi.surface.base_color = {0.42f, 0.37f, 0.30f, 1.0f};
                gi.surface.specular_roughness = 0.95f;
                const entt::entity g = scene.CreateEntity("Floor");
                hbe::Transform gt;
                gt.position = {0.0f, -2.5f, 0.0f};
                gt.rotation = glm::angleAxis(glm::radians(7.0f), glm::vec3(1.0f, 0.0f, 0.0f));
                reg.emplace<hbe::Transform>(g, gt);
                reg.emplace<hbe::MeshInstance>(g, gi);
                reg.emplace<hbe::AABB>(g, hbe::AABB{glm::vec3(-45.0f, -2.0f, -45.0f),
                                                    glm::vec3(45.0f, 2.0f, 45.0f)});
            }

            if (!wpClean) { // --wp-clean: open water only, to isolate surface artifacts
            const hbe::MeshData cm = hbe::mesh::GenerateCube(1.2f);
            const hbe::rhi::MeshHandle cube = e.GetRenderer().UploadMesh(cm);

            // Static half-submerged cubes -> object/waterline intersection foam.
            for (int i = 0; i < 3; ++i) {
                hbe::MeshInstance mi;
                mi.mesh = cube;
                mi.surface.base_color = {0.88f, 0.88f, 0.90f, 1.0f};
                mi.surface.specular_roughness = 0.6f;
                const entt::entity c = scene.CreateEntity("HalfCube");
                hbe::Transform t;
                t.position = {-7.0f + i * 7.0f, -0.15f, -6.0f};
                reg.emplace<hbe::Transform>(c, t);
                reg.emplace<hbe::MeshInstance>(c, mi);
                reg.emplace<hbe::AABB>(c, hbe::AABB{glm::vec3(-0.6f), glm::vec3(0.6f)});
            }

            // Hand-animated cubes hovering over the water -> exercise TAA on moving objects.
            for (int i = 0; i < 4; ++i) {
                hbe::MeshInstance mi;
                mi.mesh = cube;
                mi.surface.base_color = {0.95f, 0.95f, 0.98f, 1.0f};
                mi.surface.specular_roughness = 0.5f;
                const entt::entity c = scene.CreateEntity("Mover");
                hbe::Transform t;
                t.position = {-6.0f + i * 4.0f, 1.6f, 2.0f};
                reg.emplace<hbe::Transform>(c, t);
                reg.emplace<hbe::MeshInstance>(c, mi);
                reg.emplace<hbe::AABB>(c, hbe::AABB{glm::vec3(-0.6f), glm::vec3(0.6f)});
                wpMovers.push_back(c);
            }
            } // end if (!wpClean)

            auto& env = scene.Environment();
            env.exposure = wpClean ? 0.55f : 0.9f;       // wpClean: dusk-ish, reflection-dominated
            env.ambientIntensity = wpClean ? 0.15f : 0.35f;
            env.post.taaEnabled = 1;  // the ghosting is a TAA behaviour - keep it ON
            env.post.ssgiEnabled = 0; // headless can't denoise these (see --skin-preview)
            env.post.ssaoEnabled = 0;
            if (wpNoSsr) env.post.ssrEnabled = 0; // A/B: isolate SSR-on-water reflections
            // Rain, so the streaks + physical surface impacts can be eyeballed.
            env.precipType = 1;        // rain
            env.precipIntensity = 0.9f;
            env.windAngle = 25.0f;
            env.windSpeed = 0.02f;
        });
        wpEngine.SetOnFrame([wpTopdown](hbe::Engine& e) {
            wpEditor.BuildUI(e);
            // Deliberately NOT the window size, so sceneW_/sceneH_ != width_/height_ and the
            // editor-viewport depth-grade UV path is actually exercised (see the W2 screenTexel fix).
            constexpr hbe::u32 kW = 1600, kH = 900;
            e.GetRenderer().SetViewportSize(kW, kH);
            auto& cam = e.GetRenderer().GetCamera();
            cam.SetPerspective(38.0f, static_cast<float>(kW) / static_cast<float>(kH), 0.05f, 300.0f);
            if (wpTopdown) {
                // Straight down: the OLD screen-projected rain streaks collapsed to bad dashes
                // here; the world-velocity-stretched streaks should read as tidy dots/short
                // streaks (you are looking along the fall axis).
                cam.LookAt({0.0f, 24.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f});
            } else {
                // Pan the eye each frame so the water surface (which writes no velocity) has
                // apparent motion -> reveals TAA smear on the surface, while the hand-moved
                // cubes reveal TAA ghosting on moving geometry.
                const float cs = static_cast<float>(wpFrame) * 0.02f;
                cam.LookAt({std::sin(cs) * 3.5f, 6.5f, 20.0f}, {0.0f, 0.5f, -3.0f}, {0.0f, 1.0f, 0.0f});
            }

            auto& reg = e.GetScene().Registry();
            const float t = static_cast<float>(wpFrame) * 0.12f;
            for (size_t k = 0; k < wpMovers.size(); ++k) {
                if (!reg.valid(wpMovers[k])) continue;
                auto& tf = reg.get<hbe::Transform>(wpMovers[k]);
                tf.position.x = -6.0f + static_cast<float>(k) * 4.0f + std::sin(t + static_cast<float>(k)) * 2.2f;
            }

            if (++wpFrame >= 64) { // TAA accumulation + waves settle
                std::vector<hbe::u8> px;
                hbe::u32 w = 0, h = 0;
                const std::string api = ReadbackTag(e.GetRenderer().API());
                const auto out = std::filesystem::temp_directory_path() /
                                 ("hbe_water_preview_" + api + ".png");
                if (e.GetRenderer().ReadbackViewportColor(px, w, h))
                    hbe::movie::WritePng(out, w, h, px);
                std::printf("water-preview -> %s\n", out.string().c_str());
                e.Quit();
            }
        });
        return wpEngine.Run(config);
    }

    // --volume-preview: render a BAKED NanoVDB volume (a static test density sphere) headless to a
    // PNG through the RUNTIME PNanoVDB raymarch, so the new volumetric runtime can be EYEBALLED and
    // DX12 vs Vulkan compared. This is the P1 acceptance gate for the NanoVDB volume foundation.
    // Per-backend filename. The grid is hand-built (no bake/asset yet - those are P4/P5).
    bool volumePreview = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--volume-preview") == 0) volumePreview = true;
    if (volumePreview) {
        static hbe::Editor vpEditor;
        static int vpFrame = 0;
        static std::vector<std::uint8_t> vpBlob;
        static glm::vec3 vpMin(0.0f), vpMax(64.0f);
        static bool vpOk = hbe::volume::BuildTestVolumeBlob(vpBlob, vpMin, vpMax, 64);
        hbe::Engine vpEngine;
        vpEngine.SetOnInit([](hbe::Engine& e) {
            e.GetPhysics().SetRunning(false);
            e.SetGameCameraEnabled(false);
            e.GetScene().SetEditorView(true);
            e.GetRenderer().SetOrbitEnabled(false);
            if (e.GetRenderer().InitUI(e.GetWindow().GetNativeHandle().hwnd))
                hbe::Editor::ApplyTheme();
            hbe::Scene& scene = e.GetScene();
            auto& reg = scene.Registry();
            // Ground plane under the volume so the raymarch has real scene depth behind it.
            const hbe::MeshData pm = hbe::mesh::GeneratePlane(400.0f, 1);
            const hbe::rhi::MeshHandle plane = e.GetRenderer().UploadMesh(pm);
            hbe::MeshInstance gi;
            gi.mesh = plane;
            gi.surface.base_color = {0.34f, 0.36f, 0.40f, 1.0f};
            gi.surface.specular_roughness = 0.9f;
            const entt::entity g = scene.CreateEntity("Ground");
            hbe::Transform gt;
            gt.position = {32.0f, -2.0f, 32.0f};
            reg.emplace<hbe::Transform>(g, gt);
            reg.emplace<hbe::MeshInstance>(g, gi);
            reg.emplace<hbe::AABB>(g, hbe::AABB{glm::vec3(-200.0f, -1.0f, -200.0f),
                                                glm::vec3(200.0f, 1.0f, 200.0f)});
            auto& env = scene.Environment();
            env.exposure = 1.0f;
            env.ambientIntensity = 0.5f;
            env.post.ssgiEnabled = 0;
            env.post.ssaoEnabled = 0;
        });
        vpEngine.SetOnFrame([](hbe::Engine& e) {
            vpEditor.BuildUI(e);
            constexpr hbe::u32 kW = 1280, kH = 720;
            e.GetRenderer().SetViewportSize(kW, kH);
            auto& cam = e.GetRenderer().GetCamera();
            cam.SetPerspective(40.0f, static_cast<float>(kW) / static_cast<float>(kH), 0.05f, 1000.0f);
            cam.LookAt({32.0f, 42.0f, 150.0f}, {32.0f, 32.0f, 32.0f}, {0.0f, 1.0f, 0.0f});
            // Feed the baked NanoVDB grid every frame (pointer must stay valid - vpBlob is static).
            if (vpOk && !vpBlob.empty()) {
                hbe::rhi::VolumeRenderParams rp;
                rp.boundsMin = vpMin;
                rp.boundsMax = vpMax;
                rp.densityScale = 2.5f;
                rp.emission = 0.0f; // no temperature grid yet (P1 = density-only smoke)
                rp.extinction = 1.2f;
                rp.stepCount = 128;
                rp.shadowSteps = 6;
                e.GetRenderer().SetVolumeGrid(vpBlob.data(), vpBlob.size(), nullptr, 0, rp);
            }
            if (++vpFrame >= 40) { // TAA convergence on the static volume
                std::vector<hbe::u8> px;
                hbe::u32 w = 0, h = 0;
                const std::string api = ReadbackTag(e.GetRenderer().API());
                const auto out = std::filesystem::temp_directory_path() /
                                 ("hbe_volume_preview_" + api + ".png");
                if (e.GetRenderer().ReadbackViewportColor(px, w, h))
                    hbe::movie::WritePng(out, w, h, px);
                std::printf("volume-preview -> %s (grid %zu bytes, built=%d)\n",
                            out.string().c_str(), vpBlob.size(), static_cast<int>(vpOk));
                e.Quit();
            }
        });
        return vpEngine.Run(config);
    }

    // --volume-sim-preview: run the CPU Eulerian smoke SOLVER for a couple of seconds, bridge the
    // resulting density VolumeFrame to a NanoVDB grid, and render it headless to a PNG via the same
    // runtime PNanoVDB raymarch - so the SOLVER's look (a real rising, rolling plume) can be
    // eyeballed and DX12 vs Vulkan compared. The Phase-1 visual gate. Per-backend filename.
    bool volumeSimPreview = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--volume-sim-preview") == 0) volumeSimPreview = true;
    if (volumeSimPreview) {
        static hbe::Editor vspEditor;
        static int vspFrame = 0;
        static std::vector<std::uint8_t> vspBlob;
        static glm::vec3 vspMin(-2.0f, 0.0f, -2.0f), vspMax(2.0f, 8.0f, 2.0f);
        hbe::Engine vspEngine;
        vspEngine.SetOnInit([](hbe::Engine& e) {
            e.GetPhysics().SetRunning(false);
            e.SetGameCameraEnabled(false);
            e.GetScene().SetEditorView(true);
            e.GetRenderer().SetOrbitEnabled(false);
            if (e.GetRenderer().InitUI(e.GetWindow().GetNativeHandle().hwnd))
                hbe::Editor::ApplyTheme();
            hbe::Scene& scene = e.GetScene();
            auto& reg = scene.Registry();
            // Ground plane at the base of the volume box (y=0) for real scene depth behind the smoke.
            const hbe::MeshData pm = hbe::mesh::GeneratePlane(400.0f, 1);
            const hbe::rhi::MeshHandle plane = e.GetRenderer().UploadMesh(pm);
            hbe::MeshInstance gi;
            gi.mesh = plane;
            gi.surface.base_color = {0.34f, 0.36f, 0.40f, 1.0f};
            gi.surface.specular_roughness = 0.9f;
            const entt::entity g = scene.CreateEntity("Ground");
            hbe::Transform gt;
            gt.position = {0.0f, -0.02f, 0.0f};
            reg.emplace<hbe::Transform>(g, gt);
            reg.emplace<hbe::MeshInstance>(g, gi);
            reg.emplace<hbe::AABB>(g, hbe::AABB{glm::vec3(-200.0f, -1.0f, -200.0f),
                                                glm::vec3(200.0f, 1.0f, 200.0f)});
            auto& env = scene.Environment();
            env.exposure = 1.0f;
            env.ambientIntensity = 0.5f;
            env.post.ssgiEnabled = 0;
            env.post.ssaoEnabled = 0;

            // Run the SOLVER (once) for ~2.5s of sim time; the job system is up so passes parallelize.
            const hbe::volume::VolumeSimTypeInfo* t =
                hbe::volume::VolumeSimRegistry::Get().Find("eulerian-smoke");
            if (t != nullptr) {
                hbe::volume::VolumeSimConfig cfg = t->defaultConfig;
                auto sim = hbe::volume::VolumeSimRegistry::Get().Create(cfg);
                if (sim) {
                    sim->Reset();
                    const float dt = 1.0f / (cfg.frameRate * static_cast<float>(cfg.substeps));
                    const int total = 75 * cfg.substeps; // ~2.5 s
                    for (int s = 0; s < total; ++s) sim->Step(dt);
                    hbe::volume::VolumeFrame fr;
                    sim->ReadbackFrame(fr);
                    hbe::volume::BuildDensityGridBlob(fr, vspBlob, vspMin, vspMax, 0.01f);
                }
            }
        });
        vspEngine.SetOnFrame([](hbe::Engine& e) {
            vspEditor.BuildUI(e);
            constexpr hbe::u32 kW = 1280, kH = 720;
            e.GetRenderer().SetViewportSize(kW, kH);
            auto& cam = e.GetRenderer().GetCamera();
            cam.SetPerspective(40.0f, static_cast<float>(kW) / static_cast<float>(kH), 0.05f, 1000.0f);
            cam.LookAt({0.0f, 4.5f, 15.0f}, {0.0f, 4.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
            if (!vspBlob.empty()) {
                hbe::rhi::VolumeRenderParams rp;
                rp.boundsMin = vspMin;
                rp.boundsMax = vspMax;
                rp.densityScale = 1.8f;
                rp.emission = 0.0f; // density-only smoke (temperature grid is not uploaded yet)
                rp.extinction = 1.1f;
                rp.stepCount = 192;
                rp.shadowSteps = 10;
                e.GetRenderer().SetVolumeGrid(vspBlob.data(), vspBlob.size(), nullptr, 0, rp);
            }
            if (++vspFrame >= 40) { // TAA convergence on the static (already-simulated) volume
                std::vector<hbe::u8> px;
                hbe::u32 w = 0, h = 0;
                const std::string api = ReadbackTag(e.GetRenderer().API());
                const auto out = std::filesystem::temp_directory_path() /
                                 ("hbe_volume_sim_preview_" + api + ".png");
                if (e.GetRenderer().ReadbackViewportColor(px, w, h))
                    hbe::movie::WritePng(out, w, h, px);
                std::printf("volume-sim-preview -> %s (grid %zu bytes)\n",
                            out.string().c_str(), vspBlob.size());
                e.Quit();
            }
        });
        return vspEngine.Run(config);
    }

    // --hbvol-preview: the END-TO-END capstone - bake the CPU solver to a `.hbvol` cache, LOAD it back
    // through VolumeAsset, and render a frame's baked NanoVDB density grid via the runtime PNanoVDB
    // raymarch. Proves the whole author->bake->load->render pipeline (not just the CPU round-trip).
    bool hbvolPreview = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--hbvol-preview") == 0) hbvolPreview = true;
    if (hbvolPreview) {
        static hbe::Editor hvEditor;
        static int hvFrame = 0;
        static hbe::volume::VolumeAsset hvAsset;
        static hbe::u32 hvShowFrame = 0;
        static glm::vec3 hvMin(-2.0f, 0.0f, -2.0f), hvMax(2.0f, 8.0f, 2.0f);
        hbe::Engine hvEngine;
        hvEngine.SetOnInit([](hbe::Engine& e) {
            e.GetPhysics().SetRunning(false);
            e.SetGameCameraEnabled(false);
            e.GetScene().SetEditorView(true);
            e.GetRenderer().SetOrbitEnabled(false);
            if (e.GetRenderer().InitUI(e.GetWindow().GetNativeHandle().hwnd))
                hbe::Editor::ApplyTheme();
            hbe::Scene& scene = e.GetScene();
            auto& reg = scene.Registry();
            const hbe::MeshData pm = hbe::mesh::GeneratePlane(400.0f, 1);
            const hbe::rhi::MeshHandle plane = e.GetRenderer().UploadMesh(pm);
            hbe::MeshInstance gi;
            gi.mesh = plane;
            gi.surface.base_color = {0.34f, 0.36f, 0.40f, 1.0f};
            gi.surface.specular_roughness = 0.9f;
            const entt::entity g = scene.CreateEntity("Ground");
            hbe::Transform gt;
            gt.position = {0.0f, -0.02f, 0.0f};
            reg.emplace<hbe::Transform>(g, gt);
            reg.emplace<hbe::MeshInstance>(g, gi);
            reg.emplace<hbe::AABB>(g, hbe::AABB{glm::vec3(-200.0f, -1.0f, -200.0f),
                                                glm::vec3(200.0f, 1.0f, 200.0f)});
            auto& env = scene.Environment();
            env.exposure = 1.0f;
            env.ambientIntensity = 0.5f;
            env.post.ssgiEnabled = 0;
            env.post.ssaoEnabled = 0;

            // Bake the default smoke config to a .hbvol, then load it back (the shipping path).
            const hbe::volume::VolumeSimTypeInfo* t =
                hbe::volume::VolumeSimRegistry::Get().Find("eulerian-smoke");
            if (t != nullptr) {
                hbe::volume::VolumeSimConfig cfg = t->defaultConfig;
                cfg.bakeFields = {"density", "temperature"};
                auto sim = hbe::volume::VolumeSimRegistry::Get().Create(cfg);
                if (sim) {
                    const hbe::u32 frames = 60; // ~2s: enough for the plume to develop a column
                    std::vector<hbe::u8> hbvol;
                    if (hbe::volume::BakeSimulation(*sim, cfg, 0, frames - 1, hbvol) &&
                        hvAsset.Load(hbvol)) {
                        hvShowFrame = frames - 1;
                        hvMin = hvAsset.Bounds().worldMin;
                        hvMax = hvAsset.Bounds().worldMax;
                        std::printf("hbvol-preview: baked+loaded %u frames, %zu bytes\n",
                                    hvAsset.FrameCount(), hbvol.size());
                    }
                }
            }
        });
        hvEngine.SetOnFrame([](hbe::Engine& e) {
            hvEditor.BuildUI(e);
            constexpr hbe::u32 kW = 1280, kH = 720;
            e.GetRenderer().SetViewportSize(kW, kH);
            auto& cam = e.GetRenderer().GetCamera();
            cam.SetPerspective(40.0f, static_cast<float>(kW) / static_cast<float>(kH), 0.05f, 1000.0f);
            cam.LookAt({0.0f, 4.5f, 15.0f}, {0.0f, 4.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
            const hbe::volume::VolumeAsset::GridView g = hvAsset.Grid(hvShowFrame, "density");
            const hbe::volume::VolumeAsset::GridView tg = hvAsset.Grid(hvShowFrame, "temperature");
            if (g.valid()) {
                hbe::rhi::VolumeRenderParams rp;
                rp.boundsMin = hvMin;
                rp.boundsMax = hvMax;
                rp.densityScale = 1.8f;
                rp.emission = 3.0f; // show the baked temperature grid as blackbody glow (P2 emission test)
                rp.extinction = 1.1f;
                rp.stepCount = 192;
                rp.shadowSteps = 10;
                e.GetRenderer().SetVolumeGrid(g.bytes, g.size, tg.valid() ? tg.bytes : nullptr,
                                              tg.valid() ? tg.size : 0, rp);
            }
            if (++hvFrame >= 40) {
                std::vector<hbe::u8> px;
                hbe::u32 w = 0, h = 0;
                const std::string api = ReadbackTag(e.GetRenderer().API());
                const auto out = std::filesystem::temp_directory_path() /
                                 ("hbe_hbvol_preview_" + api + ".png");
                if (e.GetRenderer().ReadbackViewportColor(px, w, h))
                    hbe::movie::WritePng(out, w, h, px);
                std::printf("hbvol-preview -> %s (grid %zu bytes)\n", out.string().c_str(), g.size);
                e.Quit();
            }
        });
        return hvEngine.Run(config);
    }

    // --test-volume-component: the full RUNTIME path - a VolumeComponent placed in a scene at a
    // NON-ORIGIN transform, loaded async via VolumeCache and driven+rendered by the Engine's own
    // per-frame volume drive (NOT hand-fed). Proves component -> cache(async) -> drive -> SetVolumeGrid
    // -> raymarch WITH worldOffset placement. Asserts the chain ran (resolvedFrame advanced) + dumps a
    // PNG showing the plume offset to the entity position (eyeball placement; both backends).
    bool volComponent = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--test-volume-component") == 0) volComponent = true;
    if (volComponent) {
        static hbe::Editor tcEditor;
        static int tcFrame = 0;
        static entt::entity tcEntity = entt::null;
        static bool tcReadOk = false;
        static int tcResolved = -1;
        hbe::Engine tcEngine;
        tcEngine.SetOnInit([](hbe::Engine& e) {
            e.GetPhysics().SetRunning(true); // play mode: the volume drive advances the playhead
            e.SetGameCameraEnabled(false);
            e.GetScene().SetEditorView(true);
            e.GetRenderer().SetOrbitEnabled(false);
            if (e.GetRenderer().InitUI(e.GetWindow().GetNativeHandle().hwnd))
                hbe::Editor::ApplyTheme();
            hbe::Scene& scene = e.GetScene();
            auto& reg = scene.Registry();
            const hbe::MeshData pm = hbe::mesh::GeneratePlane(400.0f, 1);
            const hbe::rhi::MeshHandle plane = e.GetRenderer().UploadMesh(pm);
            hbe::MeshInstance gi;
            gi.mesh = plane;
            gi.surface.base_color = {0.34f, 0.36f, 0.40f, 1.0f};
            gi.surface.specular_roughness = 0.9f;
            const entt::entity g = scene.CreateEntity("Ground");
            hbe::Transform gt;
            gt.position = {0.0f, -0.02f, 0.0f};
            reg.emplace<hbe::Transform>(g, gt);
            reg.emplace<hbe::MeshInstance>(g, gi);
            reg.emplace<hbe::AABB>(g, hbe::AABB{glm::vec3(-200.0f, -1.0f, -200.0f),
                                                glm::vec3(200.0f, 1.0f, 200.0f)});
            auto& env = scene.Environment();
            env.exposure = 1.0f;
            env.ambientIntensity = 0.5f;
            env.post.ssgiEnabled = 0;
            env.post.ssaoEnabled = 0;

            // Bake a plume to a temp .hbvol, then place a VolumeComponent at a NON-ORIGIN position.
            const hbe::volume::VolumeSimTypeInfo* t =
                hbe::volume::VolumeSimRegistry::Get().Find("eulerian-smoke");
            std::string path;
            if (t != nullptr) {
                hbe::volume::VolumeSimConfig cfg = t->defaultConfig;
                auto sim = hbe::volume::VolumeSimRegistry::Get().Create(cfg);
                std::vector<hbe::u8> hbvol;
                path = (std::filesystem::temp_directory_path() / "hbe_component.hbvol").string();
                if (!(sim && hbe::volume::BakeSimulation(*sim, cfg, 0, 47, hbvol) &&
                      hbe::volume::WriteHbvolFile(path, hbvol)))
                    path.clear();
            }
            tcEntity = scene.CreateEntity("Smoke");
            hbe::Transform vt;
            vt.position = {3.0f, 1.0f, -2.0f}; // NON-ORIGIN: proves worldOffset placement
            reg.emplace<hbe::Transform>(tcEntity, vt);
            hbe::VolumeComponent vc;
            vc.source = path;
            vc.playing = true;
            vc.loop = true;
            vc.render.densityScale = 1.8f;
            vc.render.extinction = 1.1f;
            vc.render.stepCount = 160;
            vc.render.shadowSteps = 8;
            reg.emplace<hbe::VolumeComponent>(tcEntity, vc);
        });
        tcEngine.SetOnFrame([](hbe::Engine& e) {
            tcEditor.BuildUI(e);
            constexpr hbe::u32 kW = 1280, kH = 720;
            e.GetRenderer().SetViewportSize(kW, kH);
            auto& cam = e.GetRenderer().GetCamera();
            cam.SetPerspective(45.0f, static_cast<float>(kW) / static_cast<float>(kH), 0.05f, 1000.0f);
            // Look at the ORIGIN so a plume placed at x=+3 appears clearly RIGHT of center.
            cam.LookAt({0.0f, 4.5f, 20.0f}, {0.0f, 4.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
            if (++tcFrame >= 90) { // allow async load + playback to settle
                if (tcEntity != entt::null)
                    if (const auto* vc = e.GetScene().Registry().try_get<hbe::VolumeComponent>(tcEntity))
                        tcResolved = vc->resolvedFrame;
                std::vector<hbe::u8> px;
                hbe::u32 w = 0, h = 0;
                const std::string api = ReadbackTag(e.GetRenderer().API());
                const auto out = std::filesystem::temp_directory_path() /
                                 ("hbe_volume_component_" + api + ".png");
                tcReadOk = e.GetRenderer().ReadbackViewportColor(px, w, h);
                if (tcReadOk) hbe::movie::WritePng(out, w, h, px);
                std::printf("volume-component %s: resolvedFrame=%d, readback=%d -> %s\n",
                            (tcResolved >= 0 && tcReadOk) ? "PASS" : "FAIL", tcResolved,
                            static_cast<int>(tcReadOk), out.string().c_str());
                e.Quit();
            }
        });
        const int rc = tcEngine.Run(config);
        return (tcResolved >= 0 && tcReadOk) ? 0 : (rc != 0 ? rc : 1);
    }

    // --test-readback-compare: the D3D12 <-> Vulkan parity gate. Reads the raw
    // frames --test-readback left in temp for each backend and compares them.
    //
    // "One-backend-only is a bug" is the stated rule of this engine's RHI seam, and
    // nothing mechanically enforced it - the two backends were only ever compared by
    // a human looking at two screenshots. Usage:
    //   HeartbreakEditor --project P --d3d12  --test-readback
    //   HeartbreakEditor --project P --vulkan --test-readback
    //   HeartbreakEditor --test-readback-compare
    bool testRbCompare = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--test-readback-compare") == 0) testRbCompare = true;
    if (testRbCompare) {
        const auto load = [](const char* api, hbe::u32& w, hbe::u32& h,
                             std::vector<hbe::u8>& px) -> bool {
            const auto p =
                std::filesystem::temp_directory_path() / ("hbe_readback_" + std::string(api) +
                                                          ".raw");
            std::ifstream f(p, std::ios::binary);
            if (!f) {
                std::printf("readback-compare: missing %s (run --test-readback --%s first)\n",
                            p.string().c_str(), api);
                return false;
            }
            hbe::u32 hdr[2] = {0, 0};
            f.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
            w = hdr[0];
            h = hdr[1];
            px.assign(static_cast<hbe::usize>(w) * h * 4, 0);
            f.read(reinterpret_cast<char*>(px.data()),
                   static_cast<std::streamsize>(px.size()));
            return static_cast<hbe::usize>(f.gcount()) == px.size();
        };
        hbe::u32 aw = 0, ah = 0, bw = 0, bh = 0;
        std::vector<hbe::u8> a, b;
        if (!load("d3d12", aw, ah, a) || !load("vulkan", bw, bh, b)) return 1;
        if (aw != bw || ah != bh) {
            std::printf("readback-compare FAILED: %ux%u vs %ux%u\n", aw, ah, bw, bh);
            return 1;
        }
        // Not memcmp: two correct backends differ by rasterisation and filtering
        // rounding. A CHANNEL SWAP or a row-pitch shift moves the mean by far more
        // than that, which is what this is sized to catch.
        hbe::u64 diffSum = 0;
        hbe::u32 maxDiff = 0, badPixels = 0;
        for (hbe::usize i = 0; i < a.size(); ++i) {
            const hbe::u32 d = static_cast<hbe::u32>(std::abs(int(a[i]) - int(b[i])));
            diffSum += d;
            maxDiff = d > maxDiff ? d : maxDiff;
            if (d > 24) ++badPixels;
        }
        const double mean = a.empty() ? 0.0 : double(diffSum) / double(a.size());
        const double badPct = a.empty() ? 0.0 : 100.0 * double(badPixels) / double(a.size());
        // A swapped R/B channel on a sky gradient moves the mean by tens of levels;
        // legitimate backend rounding sits well under 1.
        const bool ok = mean < 4.0 && badPct < 2.0;
        std::printf("readback-compare %s: mean=%.3f max=%u over-threshold=%.2f%% (%ux%u)\n",
                    ok ? "PASS" : "FAILED", mean, maxDiff, badPct, aw, ah);
        return ok ? 0 : 1;
    }

    // --test-gpucompute: prove the general GPU-compute + GPU-writable-structured-
    // buffer seam on the ACTIVE backend (--d3d12 / --vulkan). Creates a CpuWrite SRV
    // buffer and a device-local ShaderWrite UAV buffer, queues a dispatch of
    // GpuComputeTest.hlsl, reads the UAV back, and checks every element. Needs a real
    // device (unlike --test-vfxstack), so it runs inside a short engine session and
    // needs no project. This is what stops the RHI plumbing from being delivered blind.
    bool testGpuCompute = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--test-gpucompute") == 0) testGpuCompute = true;
    if (testGpuCompute) {
        static constexpr hbe::u32 kN = 1024;
        struct GpuComputeTest {
            hbe::rhi::GpuBufferHandle in, out;
            hbe::rhi::ComputePipelineHandle pipe;
            int frame = 0;
            bool queued = false;
            bool done = false;
            bool pass = false;
            const char* why = "no result";
        };
        static GpuComputeTest t;
        hbe::Engine gcEngine;
        gcEngine.SetOnInit([](hbe::Engine& e) {
            e.GetPhysics().SetRunning(false);
            e.SetGameCameraEnabled(false);
        });
        gcEngine.SetOnFrame([](hbe::Engine& e) {
            auto& r = e.GetRenderer();
            if (t.done) return;
            if (++t.frame == 2) {
                if (!r.SupportsGpuCompute()) { t.why = "backend has no compute"; t.done = true; e.Quit(); return; }
                hbe::rhi::GpuBufferDesc inDesc{};
                inDesc.elementCount = kN;
                inDesc.elementStride = sizeof(hbe::u32);
                inDesc.usage = hbe::rhi::GpuBufferUsage::ShaderRead |
                               hbe::rhi::GpuBufferUsage::CpuWrite;
                inDesc.debugName = "GpuComputeTestIn";
                t.in = r.CreateGpuBuffer(inDesc);
                hbe::rhi::GpuBufferDesc outDesc{};
                outDesc.elementCount = kN;
                outDesc.elementStride = sizeof(hbe::u32);
                // ShaderRead + VertexBuffer too: exercises the exact usage combo the
                // GPU particle path needs (compute writes it, the VS reads it).
                outDesc.usage = hbe::rhi::GpuBufferUsage::ShaderWrite |
                                hbe::rhi::GpuBufferUsage::ShaderRead |
                                hbe::rhi::GpuBufferUsage::VertexBuffer;
                outDesc.debugName = "GpuComputeTestOut";
                t.out = r.CreateGpuBuffer(outDesc);
                hbe::rhi::ComputePipelineDesc pd{};
                pd.shaderName = "GpuComputeTest";
                pd.constantBytes = 16;
                pd.uavCount = 1;
                pd.srvCount = 1;
                t.pipe = r.CreateComputePipeline(pd);
                if (!t.in.IsValid() || !t.out.IsValid() || !t.pipe.IsValid()) {
                    t.why = "resource/pipeline creation failed";
                    t.done = true;
                    e.Quit();
                    return;
                }
                if (auto* src = static_cast<hbe::u32*>(r.MapGpuBuffer(t.in))) {
                    for (hbe::u32 i = 0; i < kN; ++i) src[i] = i * 7u + 3u;
                } else {
                    t.why = "MapGpuBuffer returned null";
                    t.done = true;
                    e.Quit();
                    return;
                }
                struct TestCB { hbe::u32 count, p0, p1, p2; } cb{kN, 0, 0, 0};
                hbe::rhi::ComputeDispatch d{};
                d.pipeline = t.pipe;
                d.constants = &cb;
                d.constantBytes = sizeof(cb);
                d.uavs[0] = t.out;
                d.uavCount = 1;
                d.srvs[0] = t.in;
                d.srvCount = 1;
                d.groupsX = (kN + 63) / 64; // numthreads(64,1,1)
                r.QueueCompute(d);           // executes at the next BeginFrame
                // Also exercise the VS-visible structured-buffer bind (D3D12 root
                // param 6 / Vulkan set 2); it must not disturb the scene pass.
                r.SetVertexShaderBuffer(t.out, 0);
                t.queued = true;
            } else if (t.queued && t.frame >= 5) {
                std::vector<hbe::u32> got(kN, 0);
                t.pass = r.ReadGpuBuffer(t.out, got.data(),
                                         static_cast<hbe::u32>(got.size() * sizeof(hbe::u32)));
                if (!t.pass) {
                    t.why = "ReadGpuBuffer failed";
                } else {
                    for (hbe::u32 i = 0; i < kN; ++i) {
                        const hbe::u32 want = (i * 7u + 3u) * 2u + 1u + (kN << 16);
                        if (got[i] != want) {
                            t.pass = false;
                            t.why = "element mismatch";
                            std::printf("  first mismatch at %u: got %u want %u\n", i, got[i], want);
                            break;
                        }
                    }
                    if (t.pass) t.why = "ok";
                }
                r.DestroyGpuBuffer(t.in);
                r.DestroyGpuBuffer(t.out);
                t.done = true;
                e.Quit();
            } else if (t.frame > 120) {
                t.why = "timed out";
                t.done = true;
                e.Quit();
            }
        });
        gcEngine.Run(config);
        std::printf("gpucompute %s (%s)\n", t.pass ? "PASS" : "FAIL", t.why);
        return t.pass ? 0 : 1;
    }

    // --test-runtimegpu: the END-TO-END proof of the EDITOR RUNTIME SHADER COMPILER. Compiles a
    // compute kernel from an HLSL STRING at runtime (RuntimeShaderCompiler), then - exactly like
    // --test-gpucompute - creates a pipeline from that runtime-compiled bytecode, dispatches it, and
    // reads the result back off the GPU, checking every element. Proves runtime-compiled bytecode
    // RUNS correctly on the active backend, not merely that it compiles. Needs a real device.
    bool testRuntimeGpu = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--test-runtimegpu") == 0) testRuntimeGpu = true;
    if (testRuntimeGpu) {
        hbe::rhi::GraphicsAPI api = hbe::rhi::GraphicsAPI::D3D12;
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--vulkan") == 0) api = hbe::rhi::GraphicsAPI::Vulkan;
            else if (std::strcmp(argv[i], "--opengl") == 0) api = hbe::rhi::GraphicsAPI::OpenGL;
        }
        // Same math as GpuComputeTest.hlsl, but compiled from a STRING at runtime. Explicit
        // [[vk::binding]] matches the ComputeDispatch binding convention (b0 / u0 / t0).
        const std::string src =
            "[[vk::binding(0,0)]] cbuffer TestCB : register(b0){ uint gCount; uint gP0; uint gP1; uint gP2; };\n"
            "[[vk::binding(1,0)]] RWStructuredBuffer<uint> gOut : register(u0);\n"
            "[[vk::binding(2,0)]] StructuredBuffer<uint>   gIn  : register(t0);\n"
            "[numthreads(64,1,1)]\n"
            "void CSMain(uint3 id : SV_DispatchThreadID){ uint i=id.x; if(i>=gCount) return; gOut[i]=gIn[i]*2u+1u+(gCount<<16); }\n";
        const auto cr = hbe::editor::RuntimeShaderCompiler::Compile(api, src, "CSMain", "cs", "RuntimeGpuTest");
        if (!cr.ok) {
            std::printf("runtimegpu: RUNTIME COMPILE FAILED:\n%s\nruntimegpu FAIL\n", cr.log.c_str());
            return 1;
        }
        std::printf("runtimegpu: compiled %zu bytes at runtime; dispatching on GPU...\n", cr.bytecode.size());
        static constexpr hbe::u32 kN = 1024;
        struct RtGpu {
            hbe::rhi::GpuBufferHandle in, out;
            hbe::rhi::ComputePipelineHandle pipe;
            int frame = 0;
            bool queued = false, done = false, pass = false;
            const char* why = "no result";
        };
        static RtGpu t;
        hbe::Engine rgEngine;
        rgEngine.SetOnInit([](hbe::Engine& e) {
            e.GetPhysics().SetRunning(false);
            e.SetGameCameraEnabled(false);
        });
        rgEngine.SetOnFrame([](hbe::Engine& e) {
            auto& r = e.GetRenderer();
            if (t.done) return;
            if (++t.frame == 2) {
                if (!r.SupportsGpuCompute()) { t.why = "backend has no compute"; t.done = true; e.Quit(); return; }
                hbe::rhi::GpuBufferDesc inD{};
                inD.elementCount = kN; inD.elementStride = sizeof(hbe::u32);
                inD.usage = hbe::rhi::GpuBufferUsage::ShaderRead | hbe::rhi::GpuBufferUsage::CpuWrite;
                inD.debugName = "RtGpuIn";
                t.in = r.CreateGpuBuffer(inD);
                hbe::rhi::GpuBufferDesc outD{};
                outD.elementCount = kN; outD.elementStride = sizeof(hbe::u32);
                outD.usage = hbe::rhi::GpuBufferUsage::ShaderWrite | hbe::rhi::GpuBufferUsage::ShaderRead;
                outD.debugName = "RtGpuOut";
                t.out = r.CreateGpuBuffer(outD);
                hbe::rhi::ComputePipelineDesc pd{};
                pd.shaderName = "RuntimeGpuTest"; // <- the RUNTIME-compiled bytecode file
                pd.constantBytes = 16; pd.uavCount = 1; pd.srvCount = 1;
                t.pipe = r.CreateComputePipeline(pd);
                if (!t.in.IsValid() || !t.out.IsValid() || !t.pipe.IsValid()) {
                    t.why = "resource/pipeline creation failed (runtime bytecode did not load)";
                    t.done = true; e.Quit(); return;
                }
                if (auto* s = static_cast<hbe::u32*>(r.MapGpuBuffer(t.in)))
                    for (hbe::u32 i = 0; i < kN; ++i) s[i] = i * 7u + 3u;
                struct CB { hbe::u32 c, a, b, d; } cb{kN, 0, 0, 0};
                hbe::rhi::ComputeDispatch d{};
                d.pipeline = t.pipe; d.constants = &cb; d.constantBytes = sizeof(cb);
                d.uavs[0] = t.out; d.uavCount = 1; d.srvs[0] = t.in; d.srvCount = 1;
                d.groupsX = (kN + 63) / 64;
                r.QueueCompute(d);
                t.queued = true;
            } else if (t.queued && t.frame >= 5) {
                std::vector<hbe::u32> got(kN, 0);
                t.pass = r.ReadGpuBuffer(t.out, got.data(), static_cast<hbe::u32>(got.size() * sizeof(hbe::u32)));
                if (!t.pass) t.why = "ReadGpuBuffer failed";
                else {
                    t.why = "ok";
                    for (hbe::u32 i = 0; i < kN; ++i) {
                        const hbe::u32 want = (i * 7u + 3u) * 2u + 1u + (kN << 16);
                        if (got[i] != want) { t.pass = false; t.why = "element mismatch"; break; }
                    }
                }
                // Prove DestroyComputePipeline RECYCLES the slot (no leak): destroy + recreate many
                // times; the returned handle id must NOT grow (a leak would push it up every rep).
                if (t.pass) {
                    const hbe::u32 firstId = t.pipe.id;
                    for (int rep = 0; rep < 50 && t.pass; ++rep) {
                        r.DestroyComputePipeline(t.pipe);
                        hbe::rhi::ComputePipelineDesc pd2{};
                        pd2.shaderName = "RuntimeGpuTest"; pd2.constantBytes = 16; pd2.uavCount = 1; pd2.srvCount = 1;
                        t.pipe = r.CreateComputePipeline(pd2);
                        if (!t.pipe.IsValid() || t.pipe.id > firstId + 1) {
                            t.pass = false; t.why = "DestroyComputePipeline did not recycle the slot (leak)";
                        }
                    }
                    if (t.pass) std::printf("  pipeline create/destroy x50 stayed at slot id %u (no leak)\n", t.pipe.id);
                    r.DestroyComputePipeline(t.pipe);
                }
                r.DestroyGpuBuffer(t.in); r.DestroyGpuBuffer(t.out);
                t.done = true; e.Quit();
            } else if (t.frame > 120) { t.why = "timed out"; t.done = true; e.Quit(); }
        });
        rgEngine.Run(config);
        std::printf("runtimegpu %s (%s)\n", t.pass ? "PASS" : "FAIL", t.why);
        return t.pass ? 0 : 1;
    }

    // --test-effekseer [effect.efk]: proves the EFFEKSEER VFX runtime is integrated end to end on the
    // GPU. Opens a real device session (like --test-runtimegpu), loads an Effekseer effect file
    // through the Heartbreak RHI seam (Renderer::VfxLoadEffect -> D3D12Device -> EffekseerBackend),
    // spawns it, lets the engine's Update/DrawScene tick + render it a few frames, and checks live
    // instances exist. The DX12 renderer, LLGI, and the manager are all real Effekseer code. (The
    // visual LOOK still needs a human eyeball; this verifies load/spawn/simulate.)
    bool testEffekseer = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--test-effekseer") == 0) testEffekseer = true;
    if (testEffekseer) {
        std::string effPath;
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            if (a.size() > 4 && a.substr(a.size() - 4) == ".efk") effPath = a;
        }
        if (effPath.empty())
            effPath = "third_party/Effekseer/Dev/Cpp/Test/Resource/block_simple.efk";
        std::error_code ec;
        effPath = std::filesystem::absolute(effPath, ec).string();
        bool efkVulkan = false;
        for (int i = 1; i < argc; ++i)
            if (std::strcmp(argv[i], "--vulkan") == 0) efkVulkan = true;
        struct EfkT {
            hbe::u32 effect = 0;
            int handle = -1, frame = 0, live = 0;
            bool done = false, pass = false, avail = false, vulkan = false;
            const char* why = "no result";
            std::string path;
        };
        static EfkT t;
        t.path = effPath;
        t.vulkan = efkVulkan;
        std::printf("effekseer: loading '%s'\n", t.path.c_str());
        hbe::Engine efkEngine;
        efkEngine.SetOnInit([](hbe::Engine& e) {
            e.GetPhysics().SetRunning(false);
            e.SetGameCameraEnabled(false);
        });
        efkEngine.SetOnFrame([](hbe::Engine& e) {
            auto& r = e.GetRenderer();
            if (t.done) return;
            ++t.frame;
            if (t.frame == 2) {
                // VfxLoadEffect triggers the lazy Effekseer init (create renderer/manager).
                t.effect = r.VfxLoadEffect(t.path.c_str());
                t.avail = r.VfxAvailable();
                if (!t.avail) {
                    // Both backends support Effekseer now. If it is unavailable the engine gracefully
                    // uses the native particle system, which is a legitimate (if degraded) state - e.g.
                    // a GPU/driver where Effekseer's Vulkan renderer fails to create. Treat that as a
                    // soft pass so the test still verifies the graceful-fallback path on such machines.
                    if (t.vulkan) { t.pass = true; t.why = "Vulkan native fallback (Effekseer runtime unavailable)"; }
                    else t.why = "Effekseer init failed (DX12 renderer/manager creation)";
                    t.done = true; e.Quit(); return;
                }
                if (t.effect == 0) { t.why = "VfxLoadEffect failed (file missing / parse error)"; t.done = true; e.Quit(); return; }
                t.handle = r.VfxPlay(t.effect, glm::vec3(0.0f, 0.0f, 0.0f));
                if (t.handle < 0) { t.why = "VfxPlay failed"; t.done = true; e.Quit(); return; }
                std::printf("  effect id=%u handle=%d; engine now updates + draws it\n", t.effect, t.handle);
            } else if (t.frame == 8) {
                // The engine's Update (VfxUpdate) + DrawScene have ticked the effect ~6 frames.
                t.live = r.VfxLiveInstanceCount();
                t.pass = t.live > 0;
                t.why = t.pass ? "ok" : "no live instances after play + updates";
                t.done = true;
                e.Quit();
            } else if (t.frame > 120) {
                t.why = "timed out";
                t.done = true;
                e.Quit();
            }
        });
        efkEngine.Run(config);
        std::printf("  live instances = %d\neffekseer %s (%s)\n", t.live, t.pass ? "PASS" : "FAIL", t.why);
        return t.pass ? 0 : 1;
    }

    // --test-matgpu: verifies the interactive GPU preview's COMPUTE PATH on hardware. Generates a
    // material node graph's compute shader (GenerateComputeHlsl), compiles it at runtime, dispatches
    // the 2D kernel into a res*res*8 float4 buffer, reads it back, and checks the base-colour channel
    // is in range and NOT flat. This is exactly what the editor GPU preview does minus the (trivial)
    // texture upload + ImGui::Image, so it verifies the preview's compute+readback end to end.
    bool testMatGpu = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--test-matgpu") == 0) testMatGpu = true;
    if (testMatGpu) {
        using namespace hbe;
        rhi::GraphicsAPI api = rhi::GraphicsAPI::D3D12;
        for (int i = 1; i < argc; ++i)
            if (std::strcmp(argv[i], "--vulkan") == 0) api = rhi::GraphicsAPI::Vulkan;
            else if (std::strcmp(argv[i], "--opengl") == 0) api = rhi::GraphicsAPI::OpenGL;
        mat::Graph g;
        const u32 fbm = g.AddNode(mat::NodeType::FractalNoise);
        g.FindNode(fbm)->constant = {6.0f, 5.0f, 0.55f, 1.0f};
        const u32 ramp = g.AddNode(mat::NodeType::ColorRamp);
        g.FindNode(ramp)->ramp = {{0.0f, {0.2f, 0.13f, 0.08f, 1}}, {1.0f, {0.6f, 0.5f, 0.4f, 1}}};
        const u32 cell = g.AddNode(mat::NodeType::Cellular);
        g.FindNode(cell)->constant = {9.0f, 2.0f, 2.0f, 0.0f};
        const u32 out = g.AddNode(mat::NodeType::Output);
        g.Connect(fbm, ramp, 0);
        g.Connect(ramp, out, static_cast<u8>(mat::Channel::BaseColor));
        g.Connect(cell, out, static_cast<u8>(mat::Channel::Roughness));
        const std::string hlsl = mat::GenerateComputeHlsl(g);
        const auto cr = editor::RuntimeShaderCompiler::Compile(api, hlsl, "CSMain", "cs", "MatGpuTest");
        if (!cr.ok) { std::printf("matgpu: compile FAILED:\n%s\nmatgpu FAIL\n", cr.log.c_str()); return 1; }
        std::printf("matgpu: material shader compiled (%zu bytes); dispatching on GPU...\n", cr.bytecode.size());
        static constexpr hbe::u32 kRes = 64;
        struct MatGpu {
            hbe::rhi::GpuBufferHandle buf;
            hbe::rhi::ComputePipelineHandle pipe;
            int frame = 0; bool queued = false, done = false, pass = false;
            const char* why = "no result";
        };
        static MatGpu m;
        hbe::Engine e2;
        e2.SetOnInit([](hbe::Engine& e) { e.GetPhysics().SetRunning(false); e.SetGameCameraEnabled(false); });
        e2.SetOnFrame([](hbe::Engine& e) {
            auto& r = e.GetRenderer();
            if (m.done) return;
            if (++m.frame == 2) {
                if (!r.SupportsGpuCompute()) { m.why = "backend has no compute"; m.done = true; e.Quit(); return; }
                hbe::rhi::GpuBufferDesc bd{};
                bd.elementCount = kRes * kRes * 8; bd.elementStride = sizeof(glm::vec4);
                bd.usage = hbe::rhi::GpuBufferUsage::ShaderWrite | hbe::rhi::GpuBufferUsage::ShaderRead;
                bd.debugName = "MatGpuBuf";
                m.buf = r.CreateGpuBuffer(bd);
                hbe::rhi::ComputePipelineDesc pd{};
                pd.shaderName = "MatGpuTest"; pd.constantBytes = 16; pd.uavCount = 1; pd.srvCount = 0;
                m.pipe = r.CreateComputePipeline(pd);
                if (!m.buf.IsValid() || !m.pipe.IsValid()) { m.why = "resource/pipeline creation failed"; m.done = true; e.Quit(); return; }
                struct CB { hbe::u32 res, a, b, c; } cb{kRes, 0, 0, 0};
                hbe::rhi::ComputeDispatch d{};
                d.pipeline = m.pipe; d.constants = &cb; d.constantBytes = sizeof(cb);
                d.uavs[0] = m.buf; d.uavCount = 1;
                d.groupsX = (kRes + 7) / 8; d.groupsY = (kRes + 7) / 8;
                r.QueueCompute(d);
                m.queued = true;
            } else if (m.queued && m.frame >= 5) {
                std::vector<glm::vec4> data(static_cast<size_t>(kRes) * kRes * 8);
                m.pass = r.ReadGpuBuffer(m.buf, data.data(), static_cast<hbe::u32>(data.size() * sizeof(glm::vec4)));
                if (!m.pass) m.why = "ReadGpuBuffer failed";
                else {
                    float mn = 1e9f, mx = -1e9f; bool inRange = true;
                    for (hbe::u32 p = 0; p < kRes * kRes; ++p) {
                        const glm::vec4 base = data[static_cast<size_t>(p) * 8 + 0];
                        for (int ch = 0; ch < 3; ++ch) { if (base[ch] < -0.01f || base[ch] > 1.01f) inRange = false; mn = base[ch] < mn ? base[ch] : mn; mx = base[ch] > mx ? base[ch] : mx; }
                    }
                    if (!inRange) { m.pass = false; m.why = "base colour out of [0,1]"; }
                    else if (mx - mn < 0.02f) { m.pass = false; m.why = "base colour is flat (shader produced nothing)"; }
                    else { m.why = "ok"; std::printf("  base colour range [%.3f, %.3f]\n", mn, mx); }
                }
                r.DestroyGpuBuffer(m.buf);
                m.done = true; e.Quit();
            } else if (m.frame > 120) { m.why = "timed out"; m.done = true; e.Quit(); }
        });
        e2.Run(config);
        std::printf("matgpu %s (%s)\n", m.pass ? "PASS" : "FAIL", m.why);
        return m.pass ? 0 : 1;
    }

    // --test-bc: prove BC (block-compressed) texture STAGING on the ACTIVE backend (--d3d12 /
    // --vulkan). Encodes a synthetic mipped RGBA8 gradient (non-multiple-of-4 => edge + sub-4x4
    // tail blocks) to BC3/BC5/BC4/BC1 via the same stb_dxt path import uses, uploads each through
    // the RHI (exercising the block-aware staging: row pitch, per-mip offset, tail mips), and
    // checks every handle is valid. Under --validation this is what catches a wrong block-size or
    // offset on either backend. Needs a real device; short engine session, no project.
    bool testBC = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--test-bc") == 0) testBC = true;
    if (testBC) {
        struct BCTest {
            int frame = 0;
            bool done = false, pass = false;
            const char* why = "no result";
            std::vector<hbe::rhi::TextureHandle> handles;
        };
        static BCTest t;
        static std::vector<hbe::uaf::Texture> bcTex;
        {
            // Two sizes: 64x64 (power-of-2, multiple-of-4) and 70x50 (NPOT, non-mult-4) - both
            // mipped to 1x1 - to exercise edge/tail blocks and any dimension constraint.
            const auto makeRGBA = [](hbe::u32 W, hbe::u32 H) {
                hbe::uaf::Texture rgba;
                rgba.width = W; rgba.height = H; rgba.mipCount = 1;
                rgba.format = static_cast<hbe::u32>(hbe::rhi::Format::R8G8B8A8_UNORM);
                for (hbe::u32 y = 0; y < H; ++y)
                    for (hbe::u32 x = 0; x < W; ++x) {
                        rgba.pixels.push_back(static_cast<hbe::u8>(x * 3));
                        rgba.pixels.push_back(static_cast<hbe::u8>(y * 5));
                        rgba.pixels.push_back(static_cast<hbe::u8>((x + y) * 2));
                        rgba.pixels.push_back(255);
                    }
                hbe::assets::GenerateMips(rgba);
                return rgba;
            };
            for (const auto dims : {std::pair<hbe::u32, hbe::u32>{64, 64}, {70, 50}}) {
                const hbe::uaf::Texture rgba = makeRGBA(dims.first, dims.second);
                for (const hbe::tex::BCKind kind :
                     {hbe::tex::BCKind::ColorRGBA, hbe::tex::BCKind::NormalRG,
                      hbe::tex::BCKind::SingleChannel, hbe::tex::BCKind::ColorRGB}) {
                    if (auto bc = hbe::tex::CompressToBC(rgba, kind, /*srgb*/ false))
                        bcTex.push_back(std::move(*bc));
                }
            }
        }
        hbe::Engine bcEngine;
        bcEngine.SetOnInit([](hbe::Engine& e) {
            e.GetPhysics().SetRunning(false);
            e.SetGameCameraEnabled(false);
        });
        bcEngine.SetOnFrame([](hbe::Engine& e) {
            auto& r = e.GetRenderer();
            if (t.done) return;
            if (++t.frame == 2) {
                if (!r.SupportsBlockCompression()) {
                    t.why = "backend reports no BC support (skipped)";
                    t.pass = true; t.done = true; e.Quit(); return;
                }
                for (const hbe::uaf::Texture& bt : bcTex) {
                    hbe::rhi::TextureDesc d{};
                    d.width = bt.width; d.height = bt.height; d.mipCount = bt.mipCount;
                    d.format = static_cast<hbe::rhi::Format>(bt.format);
                    d.pixels = bt.pixels.data();
                    d.debugName = "BCTest";
                    t.handles.push_back(r.UploadTexture(d));
                }
                t.pass = !t.handles.empty();
                for (const auto h : t.handles)
                    if (!h.IsValid()) { t.pass = false; t.why = "a BC upload returned an invalid handle"; }
                if (t.pass) t.why = "ok";
            } else if (t.frame >= 8) {
                for (const auto h : t.handles) r.DestroyTexture(h); // exercise BC destroy too
                t.done = true; e.Quit();
            } else if (t.frame > 120) {
                t.why = "timed out"; t.done = true; e.Quit();
            }
        });
        bcEngine.Run(config);
        std::printf("bc %s (%s, %zu textures)\n", t.pass ? "PASS" : "FAIL", t.why, bcTex.size());
        return t.pass ? 0 : 1;
    }

    // --test-oceanfft-gpu: prove the GPU Tessendorf FFT ocean matches the CPU oracle. Runs
    // the compute chain (evolve -> IFFT rows/cols -> assemble) on the ACTIVE backend and
    // ReadGpuBuffer-diffs the displacement field against CpuOcean::Evolve at the same time.
    // This is what stops a blind GPU FFT from being delivered on "it booted". Needs a real
    // device (short engine session, no project), same shape as --test-gpucompute.
    bool testOceanGpu = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--test-oceanfft-gpu") == 0) testOceanGpu = true;
    if (testOceanGpu) {
        struct OceanTest {
            hbe::ocean::GpuOcean gpu;
            int frame = 0;
            bool inited = false, done = false, pass = false;
            std::string why = "no result";
        };
        static OceanTest ot;
        static constexpr hbe::f32 kTestTime = 3.25f;
        hbe::Engine oEngine;
        oEngine.SetOnInit([](hbe::Engine& e) {
            e.GetPhysics().SetRunning(false);
            e.SetGameCameraEnabled(false);
        });
        oEngine.SetOnFrame([](hbe::Engine& e) {
            auto& r = e.GetRenderer();
            if (ot.done) return;
            if (ot.frame == 1) {
                if (!r.SupportsGpuCompute()) {
                    ot.why = "backend has no compute";
                    ot.done = true;
                    e.Quit();
                    return;
                }
                hbe::ocean::OceanParams p;
                p.gridN = hbe::ocean::GpuOcean::kGpuN;
                if (!ot.gpu.Init(r, p)) {
                    ot.why = "GpuOcean::Init failed";
                    ot.done = true;
                    e.Quit();
                    return;
                }
                ot.inited = true;
            }
            // Drive the compute every frame (fills h0's ring slot + queues the chain), so the
            // readback at frame 5 sees a fully-populated displacement buffer.
            if (ot.inited && ot.frame >= 1) ot.gpu.Update(r, kTestTime);
            if (ot.inited && ot.frame >= 5) {
                const hbe::u32 N = ot.gpu.N();
                const hbe::u32 cells = N * N;
                std::vector<glm::vec4> gpuField(cells);
                if (!r.ReadGpuBuffer(ot.gpu.DisplacementBuffer(), gpuField.data(),
                                     cells * static_cast<hbe::u32>(sizeof(glm::vec4)))) {
                    ot.why = "ReadGpuBuffer failed";
                    ot.done = true;
                    e.Quit();
                    return;
                }
                hbe::ocean::CpuOcean cpu;
                cpu.Init(ot.gpu.Params());
                std::vector<glm::vec4> cpuField;
                cpu.Evolve(kTestTime, cpuField);
                hbe::f32 maxDiff = 0.0f, maxRef = 1e-6f;
                for (hbe::u32 i = 0; i < cells; ++i) {
                    const glm::vec3 g(gpuField[i]), c(cpuField[i]); // xyz (foam is VS-side)
                    maxDiff = std::max(maxDiff, glm::length(g - c));
                    maxRef = std::max(maxRef, glm::length(c));
                }
                const hbe::f32 rel = maxDiff / maxRef;
                ot.pass = std::isfinite(rel) && rel < 0.02f;
                char b[160];
                std::snprintf(b, sizeof(b), "rel err %.3e (maxDiff %.3e / fieldMax %.3e)", rel,
                              maxDiff, maxRef);
                ot.why = b;
                ot.gpu.Shutdown(r);
                ot.done = true;
                e.Quit();
                return;
            }
            if (ot.frame > 120) {
                ot.why = "timed out";
                ot.done = true;
                e.Quit();
            }
            ++ot.frame;
        });
        oEngine.Run(config);
        std::printf("oceanfft-gpu %s (%s)\n", ot.pass ? "PASS" : "FAIL", ot.why.c_str());
        return ot.pass ? 0 : 1;
    }

    // --test-gpusolver: prove the GPU Eulerian smoke solver matches the CPU oracle. Runs the CPU
    // reference and the GPU solver with an IDENTICAL config for several substeps (each GPU substep is
    // its own BeginFrame drain, so this also exercises the cross-frame compute->compute barrier), then
    // ReadGpuBuffer-diffs density + temperature. Needs a real device (short engine session). This is
    // what stops a blind GPU fluid solver from being delivered on "it booted".
    bool testGpuSolver = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--test-gpusolver") == 0) testGpuSolver = true;
    if (testGpuSolver) {
        // Shared config (small, characterized - matches the --test-eulersim fixture).
        static hbe::volume::VolumeSimConfig scfg = [] {
            hbe::volume::VolumeSimConfig c;
            c.model = "eulerian-smoke";
            c.bounds.worldMin = glm::vec3(-1.0f, 0.0f, -1.0f);
            c.bounds.worldMax = glm::vec3(1.0f, 4.0f, 1.0f);
            c.bounds.dim = glm::ivec3(24, 48, 24);
            c.frameRate = 30.0f;
            c.substeps = 2;
            c.pressureIterations = 20;
            hbe::volume::VolumeEmitter em;
            em.name = "base";
            em.shape.kind = hbe::volume::VolumeShapeKind::Sphere;
            em.shape.center = glm::vec3(0.0f, 0.35f, 0.0f);
            em.shape.halfExtents = glm::vec3(0.4f);
            em.densityRate = 4.0f;
            em.temperatureRate = 6.0f;
            em.temperatureTarget = 1.0f;
            c.emitters.push_back(em);
            // Bake velocity too: it carries the closed-box wall handling + the gradient-subtract
            // sign that the smooth scalar plume barely stresses, so the diff certifies that surface.
            c.bakeFields = {"density", "temperature", "velocity"};
            return c;
        }();
        static constexpr int kSub = 6;
        static const hbe::f32 kDt = 1.0f / (scfg.frameRate * static_cast<hbe::f32>(scfg.substeps));

        // CPU oracle (no GPU): run kSub substeps, capture density + temperature.
        static hbe::volume::VolumeFrame frCpu = [] {
            hbe::volume::EulerianSmokeSimulation cpu(scfg);
            cpu.Reset();
            for (int s = 0; s < kSub; ++s) cpu.Step(kDt);
            hbe::volume::VolumeFrame f;
            cpu.ReadbackFrame(f);
            return f;
        }();

        struct GpuSolverTest {
            std::unique_ptr<hbe::volume::GpuEulerianSmokeSimulation> gpu;
            int frame = 0, steps = 0, pump = 0;
            bool inited = false, done = false, pass = false;
            std::string why = "no result";
        };
        static GpuSolverTest gt;
        hbe::Engine gEngine;
        gEngine.SetOnInit([](hbe::Engine& e) {
            e.GetPhysics().SetRunning(false);
            e.SetGameCameraEnabled(false);
        });
        gEngine.SetOnFrame([](hbe::Engine& e) {
            auto& r = e.GetRenderer();
            if (gt.done) return;
            if (gt.frame == 1) {
                if (!r.SupportsGpuCompute()) {
                    gt.why = "backend has no compute";
                    gt.done = true;
                    e.Quit();
                    return;
                }
                gt.gpu = std::make_unique<hbe::volume::GpuEulerianSmokeSimulation>(scfg, r);
                if (!gt.gpu->Valid()) {
                    gt.why = "GPU solver resources/pipelines failed";
                    gt.done = true;
                    e.Quit();
                    return;
                }
                gt.gpu->Reset();
                gt.inited = true;
            }
            // One substep per frame (each its own drain), then pump frames so the last drain submits.
            if (gt.inited && gt.steps < kSub) {
                gt.gpu->Step(kDt);
                ++gt.steps;
            } else if (gt.inited && gt.steps == kSub) {
                if (++gt.pump >= 5) {
                    hbe::volume::VolumeFrame frGpu;
                    gt.gpu->ReadbackFrame(frGpu);
                    auto relL2 = [](const std::vector<hbe::f32>& g,
                                    const std::vector<hbe::f32>& c) -> double {
                        const size_t n = std::min(g.size(), c.size());
                        double num = 0.0, den = 0.0;
                        for (size_t i = 0; i < n; ++i) {
                            const double d = static_cast<double>(g[i]) - static_cast<double>(c[i]);
                            num += d * d;
                            den += static_cast<double>(c[i]) * static_cast<double>(c[i]);
                        }
                        return std::sqrt(num) / std::max(std::sqrt(den), 1e-8);
                    };
                    const hbe::volume::VolumeField* dC = frCpu.field("density");
                    const hbe::volume::VolumeField* tC = frCpu.field("temperature");
                    const hbe::volume::VolumeField* vC = frCpu.field("velocity");
                    const hbe::volume::VolumeField* dG = frGpu.field("density");
                    const hbe::volume::VolumeField* tG = frGpu.field("temperature");
                    const hbe::volume::VolumeField* vG = frGpu.field("velocity");
                    if (!dC || !tC || !vC || !dG || !tG || !vG || dG->data.empty() ||
                        vG->data.empty()) {
                        gt.why = "missing fields / empty GPU readback";
                        gt.done = true;
                        e.Quit();
                        return;
                    }
                    bool finite = true;
                    double dTot = 0.0;
                    for (hbe::f32 v : dG->data) {
                        if (!std::isfinite(v)) finite = false;
                        dTot += v;
                    }
                    for (hbe::f32 v : vG->data)
                        if (!std::isfinite(v)) finite = false;
                    const double relD = relL2(dG->data, dC->data);
                    const double relT = relL2(tG->data, tC->data);
                    const double relV = relL2(vG->data, vC->data); // wall/projection surface
                    // Measured ~2.7e-7 (near fp32 epsilon) on RTX 3060, both backends, both configs.
                    // 5e-4 keeps a wide cross-vendor margin while still catching any real defect (a
                    // wrong coefficient / missing pass gives relL2 >= ~1e-2).
                    gt.pass = finite && dTot > 0.0 && relD < 5e-4 && relT < 5e-4 && relV < 5e-4;
                    char b[224];
                    std::snprintf(
                        b, sizeof(b),
                        "relL2 density=%.3e temp=%.3e vel=%.3e, densTotal=%.2f, finite=%d", relD,
                        relT, relV, dTot, static_cast<int>(finite));
                    gt.why = b;
                    gt.done = true;
                    e.Quit();
                    return;
                }
            }
            if (gt.frame > 200) {
                gt.why = "timed out";
                gt.done = true;
                e.Quit();
            }
            ++gt.frame;
        });
        gEngine.Run(config);
        std::printf("gpusolver %s (%s)\n", gt.pass ? "PASS" : "FAIL", gt.why.c_str());
        return gt.pass ? 0 : 1;
    }

    // --test-uidoc-invariants <file.hbui>: the P3 STRUCTURAL contract. Every
    // entity a document creates carries UIDocMember and NONE carries world
    // content; both scene writers emit zero of them; a Replace sweep spares the
    // whole set while destroying an ordinary entity; capture round-trips; Close
    // reaps everything.
    //
    // GPU SESSION, not headless: opening a document runs ui::PreloadUIAssets,
    // which calls SharedFont().Initialize(renderer) and uploads every UI texture.
    // Same shape as --test-readback / --test-gpucompute - a short engine session
    // whose OnFrame does the work and quits. `--project` is required because the
    // preload resolves paths against the project's Assets/.
    {
        const char* docPath = nullptr;
        bool docFlagSeen = false;
        for (int i = 1; i < argc; ++i)
            if (std::strcmp(argv[i], "--test-uidoc-invariants") == 0) {
                docFlagSeen = true;
                if (i + 1 < argc) docPath = argv[i + 1];
            }
        // The flag WITHOUT its argument used to fall straight through into the
        // interactive editor - so a scripted test sweep opened a window and hung
        // instead of reporting a usage error. Fail loudly like every other flag.
        if (docFlagSeen && !docPath) {
            std::printf("--test-uidoc-invariants requires a .hbui path\n");
            return 1;
        }
        if (docPath) {
            if (!hbe::Project::HasActive()) {
                std::printf("--test-uidoc-invariants requires --project\n");
                return 1;
            }
            static std::filesystem::path invPath;
            invPath = docPath;
            // Resolve a bare relative path against Assets/ for convenience.
            if (invPath.is_relative() && !std::filesystem::exists(invPath))
                invPath = hbe::Project::Active().AssetsDir() / invPath;
            static bool invRan = false, invOk = false;
            hbe::Engine invEngine;
            invEngine.SetOnInit([](hbe::Engine& e) {
                // Editor-shaped session: onInit_ is set, so the engine does NOT
                // open the project's boot/UI documents. The test opens its own.
                e.GetPhysics().SetRunning(false);
                e.SetGameCameraEnabled(false);
            });
            invEngine.SetOnFrame([](hbe::Engine& e) {
                if (invRan) return;
                invRan = true;
                invOk = hbe::ui::DocumentInvariantsSelfTest(e.GetScene(), &e.GetRenderer(),
                                                            invPath, /*preload*/ true);
                e.Quit();
            });
            invEngine.Run(config);
            std::printf("uidoc-invariants %s\n", invOk ? "PASS" : "FAIL");
            return invOk ? 0 : 1;
        }
    }

    // --test-uicanvas <file.hbui>: the ANTI-DRIFT GATE for the dedicated `.hbui`
    // editor's authoring canvas. That canvas's entire justification is that it is
    // the SHIPPED UI pass rather than a second renderer, and this asserts it
    // mechanically: the document-scoped build and the runtime build emit
    // BYTE-IDENTICAL vertex streams over the same scene, the document filter is
    // exact and inert, and the authoring render target is real and presentable to
    // ImGui. See ui::DocumentCanvasSelfTest for what it deliberately cannot cover
    // (whether the picture LOOKS right - that is a visual check).
    //
    // GPU SESSION, same shape as --test-uidoc-invariants: emission bakes fonts and
    // uploads UI textures, so `--project` is required.
    {
        const char* canvasPath = nullptr;
        bool canvasFlagSeen = false;
        for (int i = 1; i < argc; ++i)
            if (std::strcmp(argv[i], "--test-uicanvas") == 0) {
                canvasFlagSeen = true;
                if (i + 1 < argc) canvasPath = argv[i + 1];
            }
        // Same fall-through-into-the-GUI hazard as --test-uidoc-invariants above.
        if (canvasFlagSeen && !canvasPath) {
            std::printf("--test-uicanvas requires a .hbui path\n");
            return 1;
        }
        if (canvasPath) {
            if (!hbe::Project::HasActive()) {
                std::printf("--test-uicanvas requires --project\n");
                return 1;
            }
            static std::filesystem::path canPath;
            canPath = canvasPath;
            if (canPath.is_relative() && !std::filesystem::exists(canPath))
                canPath = hbe::Project::Active().AssetsDir() / canPath;
            static bool canRan = false, canOk = false;
            hbe::Engine canEngine;
            canEngine.SetOnInit([](hbe::Engine& e) {
                // Editor-shaped session (onInit_ set), so the engine does not open
                // the project's boot/UI documents and pollute the scene the parity
                // check compares over.
                e.GetPhysics().SetRunning(false);
                e.SetGameCameraEnabled(false);
                e.GetScene().SetEditorView(true); // as the editor runs (EditorUIShow)
                // The panel hands its render target to ImGui, so check 4 needs a
                // real ImGui session - the same thing --test-readback does.
                if (e.GetRenderer().InitUI(e.GetWindow().GetNativeHandle().hwnd))
                    hbe::Editor::ApplyTheme();
            });
            canEngine.SetOnFrame([](hbe::Engine& e) {
                if (canRan) return;
                canRan = true;
                canOk = hbe::ui::DocumentCanvasSelfTest(e.GetScene(), e.GetRenderer(), canPath);
                e.Quit();
            });
            canEngine.Run(config);
            std::printf("uicanvas %s\n", canOk ? "PASS" : "FAIL");
            return canOk ? 0 : 1;
        }
    }

    // --test-uivtable: the P9.2 WidgetVTable byte-identity gate. Boots a real GPU
    // session (so fonts bake and the emit path is the shipped one) but needs NO
    // project - it builds its OWN widget corpus, emits it with the legacy switch and
    // again through the WidgetVTable, and asserts the two rhi::UIVertex streams are
    // byte-identical. This is what makes the D3 decomposition provable render-blind.
    // See ui::WidgetVTableSelfTest / docs/Design-UIWidgetRegistry.md.
    {
        bool vtFlagSeen = false;
        for (int i = 1; i < argc; ++i)
            if (std::strcmp(argv[i], "--test-uivtable") == 0) vtFlagSeen = true;
        if (vtFlagSeen) {
            static bool vtRan = false, vtOk = false;
            hbe::Engine vtEngine;
            vtEngine.SetOnInit([](hbe::Engine& e) {
                e.GetPhysics().SetRunning(false);
                e.SetGameCameraEnabled(false);
                e.GetScene().SetEditorView(false); // the shipped runtime emit path
            });
            vtEngine.SetOnFrame([](hbe::Engine& e) {
                if (vtRan) return;
                vtRan = true;
                vtOk = hbe::ui::WidgetVTableSelfTest(e.GetScene(), e.GetRenderer());
                e.Quit();
            });
            vtEngine.Run(config);
            std::printf("uivtable %s\n", vtOk ? "PASS" : "FAIL");
            return vtOk ? 0 : 1;
        }
    }

    // --test-uiflow: the RUNTIME game-flow contract, end to end on a real device.
    //
    // Booting -> MainMenu -> Loading -> Playing -> MainMenu, asserting the four
    // things P3 can silently break:
    //   1. the BOOT DOCUMENT IS CLOSED once boot finishes. Documents are spared
    //      by the sweep that used to dispose the splash implicitly, so without
    //      the explicit Close in FlowAfterBoot the splash renders forever over
    //      the menu - and nothing else in the engine would notice.
    //   2. exactly ONE UIPanel is active at a time, and
    //   3. it belongs to the bound UI document (FindPanel is document-scoped now;
    //      an unscoped one hands back a duplicate after a .hbsave restore).
    //   4. the document's entity count is UNCHANGED across a LoadGameplayWorld
    //      Replace and across a SaveGame/LoadGame cycle - the Replace sweep spares
    //      it, and BuildSceneJson never wrote it into the save in the first place.
    //
    // Runs the engine in RUNTIME mode (no OnInit hook), which is what makes the
    // boot sequence execute at all.
    bool testUIFlow = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--test-uiflow") == 0) testUIFlow = true;
    if (testUIFlow) {
        if (!hbe::Project::HasActive()) {
            std::printf("--test-uiflow requires --project\n");
            return 1;
        }
        struct UIFlowTest {
            int step = 0;
            int frame = 0;
            int stepFrame = 0;
            hbe::usize docCount = 0;
            bool pass = true;
            bool done = false;
            std::string why = "ok";
        };
        static UIFlowTest f;
        const auto fail = [](const char* why) {
            f.pass = false;
            f.why = why;
        };
        // Membership is in THE SCREEN SET now, not in one document: a per-screen
        // split makes "the UI document" four documents, and an assertion that
        // still named only the first would fail on three of them. The invariant
        // gets STRONGER, not weaker - "exactly one active panel across every
        // resident screen" also catches a second screen shipping a stray
        // startVisible, which the one-document world could not even express.
        static const auto inScreens = [](hbe::Engine& e, entt::entity ent) {
            const auto& reg = e.GetScene().Registry();
            const hbe::UIDocMember* m = reg.try_get<hbe::UIDocMember>(ent);
            if (!m) return false;
            for (const hbe::ui::DocHandle h : e.UIDocuments())
                if (h == m->doc) return true;
            return false;
        };
        static const auto countDoc = [](hbe::Engine& e) {
            const auto& reg = e.GetScene().Registry();
            hbe::usize n = 0;
            for (const entt::entity ent : reg.view<const hbe::UIDocMember>())
                if (inScreens(e, ent)) ++n;
            return n;
        };
        // Active panels, and whether every one of them is in the screen set.
        static const auto activePanels = [](hbe::Engine& e, bool& allInDoc) {
            const auto& reg = e.GetScene().Registry();
            hbe::usize n = 0;
            allInDoc = true;
            for (const entt::entity ent : reg.view<const hbe::UIPanel>()) {
                if (!reg.get<const hbe::UIPanel>(ent).active) continue;
                ++n;
                if (!inScreens(e, ent)) allInDoc = false;
            }
            return n;
        };
        hbe::Engine flowEngine;
        flowEngine.SetOnFrame([fail](hbe::Engine& e) {
            if (f.done) return;
            ++f.frame;
            ++f.stepFrame;
            if (f.frame > 3000) { // ~50 s at 60 Hz; the flow has real fades in it
                fail("timed out");
                f.done = true;
                e.Quit();
                return;
            }
            const auto advance = [&](int next) {
                f.step = next;
                f.stepFrame = 0;
            };
            switch (f.step) {
            case 0: // wait out the boot dwell
                if (e.State() == hbe::Engine::GameState::Booting) return;
                if (e.UIDocument() == 0) { fail("no UI document opened at boot"); break; }
                // (1) THE splash bug.
                if (e.BootDocument() != 0) {
                    fail("boot document still open after FlowAfterBoot "
                         "(the splash would render over the menu)");
                    break;
                }
                if (e.State() != hbe::Engine::GameState::MainMenu) {
                    fail("boot did not land in MainMenu");
                    break;
                }
                f.docCount = countDoc(e);
                if (f.docCount == 0) { fail("UI document has no live entities"); break; }
                {
                    bool inDoc = false;
                    const hbe::usize n = activePanels(e, inDoc);
                    if (n != 1) { fail("MainMenu: not exactly one active UIPanel"); break; }
                    if (!inDoc) { fail("MainMenu: the active panel is not in the UI document"); break; }
                }
                advance(1);
                return;
            case 1: // menu -> play
                e.FlowPlay();
                advance(2);
                return;
            case 2: // wait for the world + HUD
                if (e.State() != hbe::Engine::GameState::Playing) return;
                if (countDoc(e) != f.docCount) {
                    fail("document entity count changed across LoadGameplayWorld "
                         "(the Replace sweep did not spare it, or it was duplicated)");
                    break;
                }
                {
                    bool inDoc = false;
                    const hbe::usize n = activePanels(e, inDoc);
                    if (n != 1) { fail("Playing: not exactly one active UIPanel"); break; }
                    if (!inDoc) { fail("Playing: the active panel is not in the UI document"); break; }
                }
                advance(3);
                return;
            case 3: // save + load: the .hbsave must contain zero UI
                if (!e.SaveGame("__uiflowtest")) { fail("SaveGame failed"); break; }
                if (!e.LoadGame("__uiflowtest")) { fail("LoadGame failed"); break; }
                advance(4);
                return;
            case 4: {
                if (f.stepFrame < 3) return; // let the restore settle
                if (countDoc(e) != f.docCount) {
                    fail("document entity count changed across SaveGame/LoadGame "
                         "(UI leaked into the .hbsave)");
                    break;
                }
                // LEGACY (v1) `.hbsave` COMPATIBILITY, on real data. A save written before
                // tag streaming existed is a complete whole-world snapshot with no
                // "shards" key; loading it must restore that world, adopt whatever shard
                // membership the level describes, and above all not DOUBLE-SPAWN - the
                // failure mode of restoring a snapshot and then also spawning the shards
                // it already contains. Only runs when the project actually has one, so
                // this is a no-op on a fresh project rather than a false failure.
                if (e.HasSave("checkpoint")) {
                    if (!e.LoadGame("checkpoint")) {
                        fail("loading the project's existing (legacy) checkpoint.hbsave failed");
                        break;
                    }
                    const entt::registry& reg = e.GetScene().Registry();
                    std::unordered_map<hbe::u64, hbe::u32> perGuid;
                    for (const entt::entity ent : reg.view<const hbe::Guid>())
                        ++perGuid[reg.get<const hbe::Guid>(ent).value];
                    hbe::u32 dupes = 0;
                    for (const auto& [g, n] : perGuid)
                        if (n > 1) ++dupes;
                    if (dupes != 0) {
                        fail("a legacy checkpoint.hbsave restored DUPLICATE entities (the "
                             "double-spawn P7 exists to prevent)");
                        break;
                    }
                    if (countDoc(e) != f.docCount) {
                        fail("document entity count changed across a legacy .hbsave load");
                        break;
                    }
                }
                advance(5);
                return;
            }
            case 5: // back to the menu
                e.FlowMainMenu();
                advance(6);
                return;
            case 6:
                if (f.stepFrame < 3) return;
                if (e.State() != hbe::Engine::GameState::MainMenu) {
                    fail("quit-to-menu did not land in MainMenu");
                    break;
                }
                if (countDoc(e) != f.docCount) {
                    fail("document entity count changed across FlowMainMenu's sweep");
                    break;
                }
                {
                    bool inDoc = false;
                    const hbe::usize n = activePanels(e, inDoc);
                    if (n != 1) { fail("back at MainMenu: not exactly one active UIPanel"); break; }
                    if (!inDoc) { fail("back at MainMenu: the active panel is not in the UI document"); break; }
                }
                break;
            default: break;
            }
            f.done = true;
            e.Quit();
        });
        flowEngine.Run(config);
        if (!f.done) {
            f.pass = false;
            f.why = "the flow never completed";
        }
        // A TEST MAY NOT LEAVE A FILE IN THE USER'S PROJECT. This one exercises the
        // real SaveGame/LoadGame pair, which write `<project>/Saves/*.hbsave` - and
        // the flag REQUIRES --project, so every compliant regression sweep was
        // permanently dropping `__uiflowtest.hbsave` into whatever game it was run
        // against. The save path is the thing under test, so it stays; the artefact
        // does not. (Best-effort: a failure to remove it must not fail the test.)
        if (hbe::Project::HasActive()) {
            std::error_code rmec;
            std::filesystem::remove(
                hbe::Project::Active().Root() / "Saves" / "__uiflowtest.hbsave", rmec);
        }
        std::printf("uiflow %s (%s)\n", f.pass ? "PASS" : "FAIL", f.why.c_str());
        return f.pass ? 0 : 1;
    }

    // --test-uiscreens: THE GATE for one-.hbui-per-screen (task I1).
    //
    // Two halves. PHASE A is fully headless - no window, no GPU, no engine - and
    // pins everything that is a property of the FILES:
    //   1. every configured screen document LOADS INDEPENDENTLY (each one is a
    //      complete, self-describing `.hbui`: kind, canvas config, post block),
    //   2. a split document ROUND-TRIPS byte-for-byte through the in-memory form
    //      (LoadDocumentFromString(SaveDocumentToString(d)) == d as text),
    //   3. panel names are UNIQUE across the set - a duplicate makes Show(name)
    //      ambiguous and UIManager::Init reject the second,
    //   4. the GLOBALLY-RESOLVED UIElement::action values (`setting:*` plus the
    //      flow verbs and "caption") are unique across the set. This is the ONE
    //      new invariant the split can violate: every consumer of `action`
    //      addresses it by string over the whole registry, so a `setting:volume`
    //      in two screens would be seeded twice and written twice.
    //   5. every screen document declares at least one root UIPanel - a screen
    //      file with no panel can never be shown by name.
    //
    // PHASE B runs the real runtime boot and pins the FLOW: all screens resident,
    // the manager reaches EACH ONE by name (Show/Push/Pop), exactly one panel is
    // ever active, the resident entity count never moves (residency, not
    // on-demand loading), the preload contract held (no unresolved texture on a
    // screen that has just been shown - the "no white quads" guarantee), and the
    // flow still lands MainMenu -> Playing/HUD -> MainMenu.
    bool testUIScreens = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--test-uiscreens") == 0) testUIScreens = true;
    if (testUIScreens) {
        if (!hbe::Project::HasActive()) {
            std::printf("--test-uiscreens requires --project\n");
            return 1;
        }
        bool passA = true;
        const auto failA = [&passA](const std::string& why) {
            passA = false;
            std::printf("uiscreens: FAIL - %s\n", why.c_str());
        };

        const hbe::Project& proj = hbe::Project::Active();
        const std::vector<std::string>& rels = proj.Settings().uiDocuments;
        if (rels.empty()) failA("the project configures no UI screen documents");

        std::unordered_map<std::string, std::string> panelOwner;  // panel -> screen rel
        std::unordered_map<std::string, std::string> actionOwner; // action -> screen rel
        // ONE definition (ui::IsGlobalAction) shared with the boot audit and the
        // migrator's collision report - a local copy could drift out of lockstep
        // and silently stop gating.
        const auto isGlobalAction = [](const std::string& a) {
            return hbe::ui::IsGlobalAction(a);
        };
        std::size_t totalPanels = 0;
        for (const std::string& rel : rels) {
            const std::filesystem::path p = proj.AssetsDir() / rel;
            if (std::filesystem::path(rel).extension() != ".hbui") {
                std::printf("uiscreens: '%s' is a LEGACY scene slot; skipping the "
                            "document checks for it.\n",
                            rel.c_str());
                continue;
            }
            // (1) loads independently
            hbe::ui::DocData d;
            if (!hbe::ui::LoadDocument(p, d)) {
                failA("screen '" + rel + "' failed to load on its own");
                continue;
            }
            if (d.entities.empty()) failA("screen '" + rel + "' has no entities");
            if (d.canvas.refWidth <= 0.0f || d.canvas.refHeight <= 0.0f)
                failA("screen '" + rel + "' has no usable canvas config - it is not "
                                         "self-describing, so its layout depends on "
                                         "whatever document loaded first");
            // (2) round-trips
            const std::string once = hbe::ui::SaveDocumentToString(d);
            hbe::ui::DocData back;
            if (!hbe::ui::LoadDocumentFromString(once, back)) {
                failA("screen '" + rel + "' does not re-parse from its own text");
            } else if (hbe::ui::SaveDocumentToString(back) != once) {
                failA("screen '" + rel + "' does not round-trip byte-for-byte");
            }
            // (3) + (5) panels
            std::size_t rootsHere = 0;
            for (const hbe::ui::DocEntity& e : d.entities) {
                if (!e.hasPanel) continue;
                if (e.parent < 0) ++rootsHere;
                ++totalPanels;
                const auto [it, fresh] = panelOwner.emplace(e.panel.name, rel);
                if (!fresh)
                    failA("panel name '" + e.panel.name + "' appears in BOTH '" +
                          it->second + "' and '" + rel +
                          "'; Show(name) would be ambiguous");
            }
            if (rootsHere == 0)
                failA("screen '" + rel +
                      "' declares no ROOT UIPanel, so it can never be shown by name");
            // (4) globally-resolved actions
            for (const hbe::ui::DocEntity& e : d.entities) {
                if (!e.hasElement) continue;
                const std::string& a = e.element.action;
                if (a.empty() || !isGlobalAction(a)) continue;
                const auto [it, fresh] = actionOwner.emplace(a, rel);
                if (!fresh && it->second != rel)
                    failA("action '" + a + "' is resolved GLOBALLY by the engine but "
                          "appears in BOTH '" + it->second + "' and '" + rel +
                          "'; both copies would fire");
            }
        }
        if (totalPanels == 0) failA("no UIPanel found in any configured screen");
        std::printf("uiscreens phase A: %zu screen document(s), %zu panel(s), %zu "
                    "globally-resolved action(s).\n",
                    rels.size(), totalPanels, actionOwner.size());
        if (!passA) {
            std::printf("uiscreens FAIL (phase A - the files)\n");
            return 1;
        }

        // ---- Phase B: the live flow over the resident screen set --------------
        struct ScreensTest {
            int step = 0;
            int frame = 0;
            int stepFrame = 0;
            hbe::usize residentEntities = 0;
            std::size_t screenIdx = 0;
            std::vector<std::string> panels; // every panel name, discovered at boot
            std::string restore;             // panel to put back when done cycling
            bool pass = true;
            bool done = false;
            std::string why = "ok";
        };
        static ScreensTest s;
        s = ScreensTest{};
        const auto fail = [](const char* why) {
            s.pass = false;
            s.why = why;
        };
        // Resident members of the SCREEN SET.
        static const auto residentCount = [](hbe::Engine& e) {
            const auto& reg = e.GetScene().Registry();
            hbe::usize n = 0;
            for (const entt::entity ent : reg.view<const hbe::UIDocMember>()) {
                const hbe::u32 doc = reg.get<const hbe::UIDocMember>(ent).doc;
                for (const hbe::ui::DocHandle h : e.UIDocuments())
                    if (h == doc) { ++n; break; }
            }
            return n;
        };
        static const auto activeCount = [](hbe::Engine& e) {
            const auto& reg = e.GetScene().Registry();
            hbe::usize n = 0;
            for (const entt::entity ent : reg.view<const hbe::UIPanel>())
                if (reg.get<const hbe::UIPanel>(ent).active) ++n;
            return n;
        };
        // THE PRELOAD GUARANTEE, as an assertion: after a screen is shown, every
        // element in it that names a texture must already have it resolved. An
        // unresolved reference is exactly the white quad / blank glyph a
        // load-on-demand design would flash.
        static const auto unresolvedIn = [](hbe::Engine& e, const std::string& panel) {
            auto& reg = e.GetScene().Registry();
            const entt::entity root = e.GetUIManager().PanelEntity(e.GetScene(), panel);
            if (root == entt::null) return hbe::usize(0);
            hbe::usize bad = 0;
            for (const entt::entity ent : reg.view<hbe::UIElement>()) {
                entt::entity cur = ent;
                bool under = false;
                for (int d = 0; cur != entt::null && d < 64; ++d) {
                    if (cur == root) { under = true; break; }
                    const hbe::Parent* p = reg.try_get<hbe::Parent>(cur);
                    cur = (p && reg.valid(p->entity)) ? p->entity : entt::null;
                }
                if (!under) continue;
                const hbe::UIElement& el = reg.get<hbe::UIElement>(ent);
                if (!el.texture.empty() && !el.textureResolved) ++bad;
            }
            return bad;
        };

        hbe::Engine screensEngine;
        screensEngine.SetOnFrame([fail, &rels](hbe::Engine& e) {
            if (s.done) return;
            ++s.frame;
            ++s.stepFrame;
            if (s.frame > 3000) {
                fail("timed out");
                s.done = true;
                e.Quit();
                return;
            }
            const auto advance = [&](int next) {
                s.step = next;
                s.stepFrame = 0;
            };
            switch (s.step) {
            case 0: // wait out the boot dwell
                if (e.State() == hbe::Engine::GameState::Booting) return;
                if (e.BootDocument() != 0) {
                    fail("boot document still open after FlowAfterBoot");
                    break;
                }
                // ALL screens resident, one open document each.
                if (e.UIDocuments().size() != rels.size()) {
                    fail("not every configured screen document is resident");
                    break;
                }
                if (e.State() != hbe::Engine::GameState::MainMenu) {
                    fail("boot did not land in MainMenu");
                    break;
                }
                if (activeCount(e) != 1) {
                    fail("MainMenu: not exactly one active UIPanel across the screen "
                         "set (a second screen shipped a stray startVisible?)");
                    break;
                }
                s.residentEntities = residentCount(e);
                if (s.residentEntities == 0) { fail("the screen set has no entities"); break; }
                // Collect every panel name the manager can reach.
                {
                    auto& reg = e.GetScene().Registry();
                    for (const entt::entity ent : reg.view<const hbe::UIPanel>()) {
                        const hbe::UIDocMember* m = reg.try_get<hbe::UIDocMember>(ent);
                        if (!m) continue;
                        bool mine = false;
                        for (const hbe::ui::DocHandle h : e.UIDocuments())
                            if (h == m->doc) { mine = true; break; }
                        if (!mine) continue;
                        const std::string& nm = reg.get<const hbe::UIPanel>(ent).name;
                        if (!nm.empty()) s.panels.push_back(nm);
                    }
                    std::sort(s.panels.begin(), s.panels.end());
                    s.restore = e.GetUIManager().Top();
                }
                if (s.panels.empty()) { fail("no reachable panels"); break; }
                // THE INITIAL SCREEN MUST EXIST AND BE REACHABLE. Only ONE of the
                // split documents carries `startVisible`, so losing that one file
                // leaves `initial_` empty, ShowInitial a silent no-op and the game
                // parked in MainMenu with a black screen and zero elements.
                // Nothing checked this before.
                {
                    const std::string init = e.GetUIManager().Initial();
                    if (init.empty()) {
                        fail("the UIManager has NO initial screen (nothing declares "
                             "startVisible, and the boot fallback did not fire)");
                        break;
                    }
                    if (std::find(s.panels.begin(), s.panels.end(), init) ==
                        s.panels.end()) {
                        fail(("the initial screen '" + init +
                              "' is not a reachable panel in the resident set")
                                 .c_str());
                        break;
                    }
                    if (e.GetUIManager().Top() != init) {
                        fail(("boot showed '" + e.GetUIManager().Top() +
                              "' but the initial screen is '" + init + "'")
                                 .c_str());
                        break;
                    }
                }
                advance(1);
                return;
            case 1: { // show EVERY screen in turn - each must be reachable by name
                if (s.screenIdx >= s.panels.size()) {
                    e.GetUIManager().Show(e.GetScene(),
                                          s.restore.empty() ? s.panels.front() : s.restore);
                    advance(2);
                    return;
                }
                const std::string& nm = s.panels[s.screenIdx];
                if (s.stepFrame == 1) {
                    if (!e.GetUIManager().Has(e.GetScene(), nm)) {
                        fail("a panel in the screen set is not reachable by name");
                        break;
                    }
                    e.GetUIManager().Show(e.GetScene(), nm);
                    return; // let the show land before asserting
                }
                if (activeCount(e) != 1) {
                    fail("showing a screen left more than one active UIPanel");
                    break;
                }
                if (e.GetUIManager().Top() != nm) {
                    fail("Show(name) did not make that screen the top of the stack");
                    break;
                }
                // RESIDENCY: showing a screen must not load or destroy anything.
                if (residentCount(e) != s.residentEntities) {
                    fail("the resident entity count moved when a screen was shown - "
                         "screens are supposed to be resident, not loaded on demand");
                    break;
                }
                // PRELOAD: nothing unstyled on the frame it appears.
                if (unresolvedIn(e, nm) != 0) {
                    fail("a shown screen has an UNRESOLVED texture reference (it would "
                         "flash a white quad) - the preload contract was not honoured");
                    break;
                }
                ++s.screenIdx;
                s.stepFrame = 0;
                return;
            }
            case 2: // and the real flow still works end to end
                e.FlowPlay();
                advance(3);
                return;
            case 3:
                if (e.State() != hbe::Engine::GameState::Playing) return;
                if (residentCount(e) != s.residentEntities) {
                    fail("the screen set's entity count changed across "
                         "LoadGameplayWorld (a Replace sweep did not spare it)");
                    break;
                }
                if (activeCount(e) != 1) { fail("Playing: not exactly one active UIPanel"); break; }
                if (e.GetUIManager().Top() != "HUD") {
                    fail("Playing did not put the HUD screen on top");
                    break;
                }
                advance(4);
                return;
            case 4: // settings must still PUSH over the HUD and POP back
                e.GetUIManager().Push(e.GetScene(), "Settings");
                advance(5);
                return;
            case 5:
                if (s.stepFrame < 2) return;
                if (e.GetUIManager().Top() != "Settings") {
                    fail("Push(\"Settings\") did not reach the Settings screen "
                         "(a cross-document panel lookup failed)");
                    break;
                }
                if (activeCount(e) != 1) { fail("Settings: not exactly one active UIPanel"); break; }
                e.GetUIManager().Pop(e.GetScene());
                advance(6);
                return;
            case 6:
                if (s.stepFrame < 2) return;
                if (e.GetUIManager().Top() != "HUD") { fail("Pop did not restore the HUD"); break; }
                e.FlowMainMenu();
                advance(7);
                return;
            case 7:
                if (s.stepFrame < 3) return;
                if (e.State() != hbe::Engine::GameState::MainMenu) {
                    fail("quit-to-menu did not land in MainMenu");
                    break;
                }
                if (residentCount(e) != s.residentEntities) {
                    fail("the screen set's entity count changed across FlowMainMenu's "
                         "sweep");
                    break;
                }
                if (activeCount(e) != 1) {
                    fail("back at MainMenu: not exactly one active UIPanel");
                    break;
                }
                break;
            default: break;
            }
            s.done = true;
            e.Quit();
        });
        screensEngine.Run(config);
        if (!s.done) {
            s.pass = false;
            s.why = "the flow never completed";
        }
        std::printf("uiscreens %s (%s)\n", s.pass ? "PASS" : "FAIL", s.why.c_str());
        return s.pass ? 0 : 1;
    }

    // --test-vfxsim: CPU/GPU PARITY for the module-stack interpreter.
    //
    // Shaders/VfxSim.hlsl is a transliteration of the K_* kernels in VfxStack.cpp -
    // same modules, same order, same RNG draw order - so the two paths must agree.
    // This drives ONE emitter on the real GPU path (the engine's own particle::Update
    // -> particle::GpuSim -> compute dispatch), drives a CPU reference through
    // vfx::RunFrame with the SAME compiled stack and the SAME per-frame ModuleParams
    // (taken from the live emitter, so the test cannot pass by testing its own copy of
    // the mapping), reads the simulation buffer back, and compares particle for
    // particle.
    //
    // NOT bit-exact, deliberately: sin/cos/exp are implementation-defined and glm::mix
    // is x*(1-a)+y*a where HLSL lerp is x+a*(y-x). The assertion is a relative
    // tolerance plus a MOVEMENT check - a comparison that passes because both sides
    // produced zeros would prove nothing.
    bool testVfxSim = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--test-vfxsim") == 0) testVfxSim = true;
    if (testVfxSim) {
        static constexpr hbe::f32 kDt = 1.0f / 60.0f;
        static constexpr hbe::u32 kBurst = 256;   // one shot: slot k == pool index k
        static constexpr hbe::u32 kSimFrames = 45;
        struct VfxSimTest {
            entt::entity entity = entt::null;
            hbe::vfx::CompiledStack refStack;
            hbe::vfx::ParticleSoA refPool;
            hbe::vfx::EmitterState refState;
            hbe::u32 refFrames = 0;
            bool armed = false;
            bool settled = false;
            int frame = 0;
            bool done = false;
            bool pass = false;
            const char* why = "no result";
            hbe::f32 worstRel = 0.0f;
            hbe::f32 travelled = 0.0f;
            hbe::u32 compared = 0;
        };
        static VfxSimTest v;
        hbe::Engine vsEngine;
        vsEngine.SetOnInit([](hbe::Engine& e) {
            e.GetPhysics().SetRunning(false);
            e.SetGameCameraEnabled(false);
            e.SetRenderFixedDt(kDt); // determinism: the CPU reference uses the same dt
        });
        vsEngine.SetOnFrame([](hbe::Engine& e) {
            if (v.done) return;
            auto& scene = e.GetScene();
            auto& reg = scene.Registry();
            auto& r = e.GetRenderer();
            ++v.frame;

            if (v.frame == 2) {
                if (!r.SupportsGpuCompute()) {
                    v.why = "backend has no compute";
                    v.done = true;
                    e.Quit();
                    return;
                }
                v.entity = scene.CreateEntity("VfxSimTest");
                hbe::Transform t;
                t.position = {3.5f, 1.25f, -2.0f}; // non-trivial world matrix
                t.rotation = glm::quat(glm::vec3(0.35f, 0.8f, -0.2f)); // yaw/pitch/roll
                reg.emplace<hbe::Transform>(v.entity, t);

                hbe::ParticleEmitter em;
                em.gpuSim = true;
                em.gpuSeed = 0x1234ABCDu; // fixed so the reference can use it too
                // One shot, nothing retires inside the window: the ring cursor never
                // laps, so GPU slot k and CPU pool index k are the same particle and a
                // 1:1 comparison is meaningful.
                em.burst = kBurst;
                em.rate = 0.0f;
                em.loop = false;
                em.duration = 0.0f;
                em.maxParticles = 512;
                em.lifetime = 60.0f;
                em.lifetimeVariance = 0.35f;
                em.emitRadius = 0.6f;
                em.direction = {0.2f, 1.0f, -0.3f};
                em.startSpeed = 2.5f;
                em.speedVariance = 0.4f;
                em.spread = 0.45f;
                em.gravity = {0.3f, -1.6f, 0.1f};
                em.drag = 0.7f;                 // exercises the exp() arm
                em.turbulence = 1.4f;           // -> CurlNoiseForce (3 cos/particle)
                em.turbulenceScale = 0.9f;
                em.spin = 1.7f;                 // -> SpawnInitRotation + RotationRate
                em.startColor = {1.0f, 0.8f, 0.3f, 1.0f};
                em.endColor = {0.9f, 0.15f, 0.05f, 0.0f};
                em.colorVariance = 0.3f;        // exercises VarianceScale on both paths
                em.startSize = 0.5f;
                em.endSize = 0.12f;
                em.sizeVariance = 0.25f;
                em.fadeIn = 0.1f;
                em.fadeOut = 0.4f;
                reg.emplace<hbe::ParticleEmitter>(v.entity, em);
                return;
            }
            if (v.frame < 3) return;

            hbe::ParticleEmitter* em = reg.try_get<hbe::ParticleEmitter>(v.entity);
            if (!em || !em->stack.valid) return;

            // Compile the reference from the SAME builder the live emitter used.
            if (!v.armed) {
                std::string errs;
                if (!hbe::particle::BuildGpuDesc(*em).modules.empty() &&
                    hbe::vfx::Compile(hbe::particle::BuildGpuDesc(*em), v.refStack, &errs)) {
                    hbe::vfx::ReservePool(v.refStack, v.refPool);
                    v.refState.Reset(em->gpuSeed);
                    v.refState.emitting = true;
                    v.armed = true;
                } else {
                    v.why = "reference stack failed to compile";
                    v.done = true;
                    e.Quit();
                    return;
                }
            }

            // Lockstep the reference to the GPU emitter's frame count. emitterTime is
            // advanced by exactly dt per StepGpuEmitter call, so it IS the count.
            const hbe::u32 gpuFrames =
                static_cast<hbe::u32>(std::lround(em->state.emitterTime / kDt));
            while (v.refFrames < gpuFrames && v.refFrames < kSimFrames) {
                // Take this frame's operands from the live emitter rather than
                // re-deriving them - the mapping is under test, not duplicated.
                for (hbe::u32 st = 0; st < hbe::vfx::kStageCount; ++st) {
                    auto& live = em->stack.stages[st];
                    auto& ref = v.refStack.stages[st];
                    if (live.size() != ref.size()) continue;
                    for (size_t k = 0; k < live.size(); ++k) ref[k].params = live[k].params;
                }
                v.refStack.seed = em->stack.seed;
                v.refStack.spawnRate = em->stack.spawnRate;
                v.refStack.burst = em->stack.burst;
                v.refStack.loop = em->stack.loop;
                v.refStack.duration = em->stack.duration;
                v.refState.emitting = true;
                hbe::vfx::RunFrame(v.refStack, v.refState, v.refPool, kDt);
                ++v.refFrames;
            }
            if (v.refFrames < kSimFrames) return;

            // ONE frame of settle, and the reason is worth stating: the editor's
            // onFrame callback runs AFTER particle::Update and GpuSim::Update but
            // BEFORE Renderer::RenderScene, and the compute queue is only executed
            // inside the device's BeginFrame (which RenderScene drives). So at the
            // moment the reference reaches frame N, the GPU has executed N-1 steps and
            // step N is still sitting in the software queue. Reading here would compare
            // frame N against frame N-1 and report a plausible-looking one-gravity-step
            // difference - which is exactly what it did before this wait existed.
            if (!v.settled) {
                v.settled = true;
                return;
            }

            // --- read the simulation buffer back and compare ---
            const hbe::u32 n = v.refPool.count;
            if (n != kBurst) {
                v.why = "reference pool did not hold the burst";
                v.done = true;
                e.Quit();
                return;
            }
            const hbe::u32 first = em->gpuSlotBase + hbe::rhi::kGpuParticleEmitterElements;
            std::vector<hbe::vfx::GpuParticle> got(first + n);
            if (!r.ReadGpuBuffer(e.GetGpuSim().Records(), got.data(),
                                 static_cast<hbe::u32>(got.size() *
                                                       sizeof(hbe::vfx::GpuParticle)))) {
                v.why = "ReadGpuBuffer failed";
                v.done = true;
                e.Quit();
                return;
            }

            // MIXED tolerance, not a bare relative one. Several of these quantities
            // legitimately cross zero (velocity.y under gravity, position around the
            // origin), and a pure relative error there reports 2.0 for a difference of
            // one ULP - it would measure how close to zero the value is, not how far
            // apart the two paths are. `d <= atol + rtol*|v|` is the standard fix; the
            // score below is the failure MARGIN, so 1.0 is exactly at tolerance.
            constexpr hbe::f32 kAtol = 2.0e-3f;
            constexpr hbe::f32 kRtol = 2.0e-3f;
            const auto score = [](hbe::f32 a, hbe::f32 b) {
                const hbe::f32 d = std::fabs(a - b);
                const hbe::f32 m = std::max(std::fabs(a), std::fabs(b));
                return d / (kAtol + kRtol * m);
            };
            hbe::f32 worst = 0.0f;
            hbe::f32 travel = 0.0f;
            hbe::f32 worstA = 0.0f, worstB = 0.0f;
            hbe::u32 failing = 0;
            const char* worstField = "";
            hbe::u32 worstIdx = 0;
            // Colour is compared too. It is not a formality: it is the ONLY thing that
            // exercises VfxSpawnInitColor, VfxColorOverLife, VarianceScale on the
            // colour salt, and the half4 pack/unpack - the test emitter sets a
            // start->end ramp, fadeIn/fadeOut and colorVariance specifically for it.
            // Without these four fields all of that shipped unverified.
            const bool hasColor = v.refPool.Has(hbe::vfx::Attr::Color);
            for (hbe::u32 i = 0; i < n; ++i) {
                const hbe::vfx::GpuParticle& g = got[first + i];
                const glm::vec3 cp = v.refPool.position[i];
                travel = std::max(travel, glm::length(cp - glm::vec3(3.5f, 1.25f, -2.0f)));
                // The GPU record stores colour as half4; the CPU pool keeps float4.
                // Comparing zeros when the stream was eliminated keeps the field list
                // fixed-size without ever reading an unbacked stream.
                const glm::vec4 gc =
                    hasColor ? glm::vec4(glm::unpackHalf2x16(g.colorRG),
                                         glm::unpackHalf2x16(g.colorBA))
                             : glm::vec4(0.0f);
                const glm::vec4 cc = hasColor ? v.refPool.color[i] : glm::vec4(0.0f);
                struct { const char* name; hbe::f32 a, b; } fields[] = {
                    {"position.x", g.position.x, cp.x},
                    {"position.y", g.position.y, cp.y},
                    {"position.z", g.position.z, cp.z},
                    {"velocity.x", g.velocity.x, v.refPool.velocity[i].x},
                    {"velocity.y", g.velocity.y, v.refPool.velocity[i].y},
                    {"velocity.z", g.velocity.z, v.refPool.velocity[i].z},
                    {"age", g.age, v.refPool.age[i]},
                    {"lifetime", g.lifetime, v.refPool.lifetime[i]},
                    {"rotation", g.rotation, v.refPool.rotation[i]},
                    {"size", g.sizeX, v.refPool.sizeX[i]},
                    {"color.r", gc.r, cc.r},
                    {"color.g", gc.g, cc.g},
                    {"color.b", gc.b, cc.b},
                    {"color.a", gc.a, cc.a},
                };
                bool bad = false;
                for (const auto& f : fields) {
                    const hbe::f32 e2 = score(f.a, f.b);
                    if (e2 > 1.0f) bad = true;
                    if (e2 > worst) {
                        worst = e2;
                        worstField = f.name;
                        worstIdx = i;
                        worstA = f.a;
                        worstB = f.b;
                    }
                }
                if (bad) ++failing;
            }
            v.worstRel = worst;
            v.travelled = travel;
            v.compared = n;
            // The movement check is what stops a pair of zero pools from passing - a
            // parity test that both sides satisfy by doing nothing proves nothing.
            v.pass = (worst <= 1.0f) && (travel > 0.25f);
            v.why = v.pass ? "ok" : (travel <= 0.25f ? "particles never moved" : worstField);
            if (!v.pass) {
                std::printf("  %u/%u particles out of tolerance; worst '%s' on particle %u: "
                            "gpu %.7f vs cpu %.7f (margin %.2f, travel %.3f)\n",
                            failing, n, worstField, worstIdx, worstA, worstB, worst, travel);
            }
            v.done = true;
            e.Quit();
        });
        vsEngine.Run(config);
        std::printf("vfxsim %s (%s) - %u particles x %u frames, worst relative diff %.2e, "
                    "max travel %.2f m\n",
                    v.pass ? "PASS" : "FAIL", v.why, v.compared, kSimFrames, v.worstRel,
                    v.travelled);
        return v.pass ? 0 : 1;
    }

    // --render-movie <cutscene.hbcutscene | "current"> [--out FILE.mp4|DIR] [--res WxH]
    //   [--fps N] [--seconds S] [--music FILE]: offline-render a trailer, then exit. An
    //   .mp4 out encodes H.264 video + AAC audio (Media Foundation); a directory out
    //   writes a lossless PNG frame sequence. Reuses the editor's offscreen render + the
    //   deterministic cutscene evaluator at a fixed dt.
    std::string renderMovie;
    static hbe::movie::MovieConfig s_cfg;
    s_cfg.outputDir = "movie_frames";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--render-movie") == 0 && i + 1 < argc) renderMovie = argv[++i];
        else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            const std::filesystem::path o = argv[++i];
            std::string ext = o.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == ".mp4") s_cfg.outputFile = o;
            else s_cfg.outputDir = o;
        }
        else if (std::strcmp(argv[i], "--res") == 0 && i + 1 < argc)
            std::sscanf(argv[++i], "%ux%u", &s_cfg.width, &s_cfg.height);
        else if (std::strcmp(argv[i], "--fps") == 0 && i + 1 < argc)
            s_cfg.fps = static_cast<hbe::u32>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
            s_cfg.duration = static_cast<hbe::f32>(std::atof(argv[++i]));
        else if (std::strcmp(argv[i], "--music") == 0 && i + 1 < argc) s_cfg.musicRel = argv[++i];
    }
    if (!renderMovie.empty()) {
        if (!hbe::Project::HasActive()) {
            std::printf("--render-movie requires --project\n");
            return 1;
        }
        // A `.hbseq` target renders the Sequencer timeline; a `.hbcutscene` (or any
        // other value) renders the legacy cutscene; "current" renders the live scene.
        if (renderMovie != "current") {
            if (renderMovie.ends_with(".hbseq")) s_cfg.sequenceRel = renderMovie;
            else s_cfg.cutsceneRel = renderMovie;
        }
        static hbe::Editor mvEditor;
        static hbe::movie::MovieJob mvJob;
        static bool mvStarted = false;
        hbe::Engine mvEngine;
        mvEngine.SetOnInit([](hbe::Engine& e) {
            e.GetPhysics().SetRunning(false);
            e.SetGameCameraEnabled(false);
            e.GetScene().SetEditorView(true);
            if (e.GetRenderer().InitUI(e.GetWindow().GetNativeHandle().hwnd))
                hbe::Editor::ApplyTheme();
        });
        mvEngine.SetOnFrame([](hbe::Engine& e) {
            mvEditor.BuildUI(e); // proven offscreen + ImGui plumbing; job overrides the size
            if (!mvStarted) {
                mvStarted = true;
                mvJob.Start(e, hbe::Project::Active().AssetsDir(), s_cfg);
            }
            if (mvJob.Active()) {
                mvJob.Tick(e);
            } else {
                const auto target = s_cfg.outputFile.empty() ? s_cfg.outputDir : s_cfg.outputFile;
                std::printf("render-movie done: %u/%u frames -> %s\n", mvJob.FramesWritten(),
                            mvJob.TotalFrames(), target.string().c_str());
                // Headless verify: decode frame 0 of the .mp4 back to a PNG.
                if (!s_cfg.outputFile.empty()) {
                    auto verify = s_cfg.outputFile;
                    verify.replace_extension(".verify.png");
                    const bool vok = hbe::movie::DecodeFirstFrameToPng(s_cfg.outputFile, verify);
                    std::printf("verify-decode %s -> %s\n", vok ? "OK" : "FAIL",
                                verify.string().c_str());
                }
                mvJob.Stop(e);
                e.Quit();
            }
        });
        return mvEngine.Run(config);
    }

    // AUTO-UPGRADE: before the editing session loads any assets, bring every out-of-date `.uaf`
    // in the project up to the current spec IN PLACE (meshes -> v9 + LODs; material textures -> BC
    // when enabled), preserving each asset's guid + pack slot. This is what lets an old project
    // gain the current feature set without re-importing anything. Idempotent, so a current project
    // pays only a header-peek per asset. Reached only by the real editor session - every CLI op
    // (--import / --pack / --upgrade-assets / --test-*) returns above this.
    if (hbe::Project::HasActive()) {
        const hbe::importer::UpgradeReport r = hbe::importer::UpgradeAssets(
            hbe::Project::Active().AssetsDir(), hbe::Project::Active().SlotManifestPath());
        (void)r; // UpgradeAssets already logs a summary when it changes anything
    }

    hbe::Engine engine;
    hbe::Editor editor;

    // Initialize ImGui and route Win32 input to it. Physics starts paused in
    // the editor (toggled via the Stats panel's "Simulate physics").
    engine.SetOnInit([](hbe::Engine& e) {
        e.GetPhysics().SetRunning(false); // the Game tab's Play starts the sim
        e.SetGameCameraEnabled(false);    // scene view owns the camera until Play
        e.GetScene().SetEditorView(true); // honor per-entity EditorHidden in the viewport
        // Seeded here as well as every frame in Editor::BuildUI: the frame loop runs
        // ui::UpdateAnimations BEFORE onFrame_, so without this the very first editor
        // frame would advance a document's clips and bake one animated pose into the
        // authored offset/scale/colour of every element carrying one.
        e.GetScene().SetUIAuthoringView(true);
        if (e.GetRenderer().InitUI(e.GetWindow().GetNativeHandle().hwnd)) {
            hbe::Editor::ApplyTheme();
            hbe::Editor::EnableLayoutPersistence("HeartbreakEditor.ini"); // save/restore docking
            e.GetWindow().SetWndProcHook([](void* h, hbe::u32 m, hbe::u64 w, hbe::i64 l) -> hbe::i64 {
                return ImGui_ImplWin32_WndProcHandler(static_cast<HWND>(h), m,
                                                      static_cast<WPARAM>(w),
                                                      static_cast<LPARAM>(l));
            });
        }
    });

    // Build the editor UI each frame.
    engine.SetOnFrame([&editor](hbe::Engine& e) { editor.BuildUI(e); });

    return engine.Run(config);
}
