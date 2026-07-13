// main_editor.cpp - Heartbreak *Editor* entry point.
//
// This build links the engine runtime PLUS the editor (Dear ImGui + ImGuizmo,
// asset browser, gizmos). It is a tool, not shipped with the game. It wires the
// editor UI into the engine via the engine's per-frame hook.
//
// Usage: HeartbreakEditor [--d3d12 | --vulkan] [--width N] [--height N]
//                         [--validation] [--model <path>]
#include "Assets/SeamWeld.h"
#include "Core/JobSystem.h"
#include "Core/Window.h"
#include "Editor/Editor.h"
#include "Editor/Importer.h"
#include "Editor/MovieRender.h"
#include "Engine/Engine.h"
#include "Physics/PhysicsWorld.h"
#include "Project/Project.h"
#include "Renderer/Renderer.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
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

int main(int argc, char** argv) {
    // --test-seamweld: run the modular-rig seam-weld bit-identity proof (headless,
    // no GPU/window) and exit. Used by CI / the build discipline.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--test-seamweld") == 0) {
            const bool ok = hbe::weld::SelfTest();
            std::printf("seamweld %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
    }

    hbe::EngineConfig config = hbe::ParseCommandLine(argc, argv);
    config.title = L"Heartbreak Editor";

    // Normally no project is opened here - the editor's Project Manager modal
    // handles creating/opening projects (and remembers recent ones). The
    // --project flag opens one directly (automation / file association).
    if (!config.projectPath.empty()) {
        hbe::Project::Active().Open(std::filesystem::path(config.projectPath));
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
            e.GetRenderer().SetViewportSize(641, 361); // odd -> exercises 256B row pitch
            if (++rbFrame >= 30) {
                std::vector<hbe::u8> px;
                hbe::u32 w = 0, h = 0;
                const bool ok = e.GetRenderer().ReadbackViewportColor(px, w, h);
                const auto out = std::filesystem::temp_directory_path() / "hbe_readback.png";
                if (ok) hbe::movie::WritePng(out, w, h, px);
                std::printf("readback %s %ux%u -> %s\n", ok ? "OK" : "FAIL", w, h,
                            out.string().c_str());
                e.Quit();
            }
        });
        return rbEngine.Run(config);
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
        s_cfg.cutsceneRel = (renderMovie == "current") ? std::string() : renderMovie;
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

    hbe::Engine engine;
    hbe::Editor editor;

    // Initialize ImGui and route Win32 input to it. Physics starts paused in
    // the editor (toggled via the Stats panel's "Simulate physics").
    engine.SetOnInit([](hbe::Engine& e) {
        e.GetPhysics().SetRunning(false); // the Game tab's Play starts the sim
        e.SetGameCameraEnabled(false);    // scene view owns the camera until Play
        e.GetScene().SetEditorView(true); // honor per-entity EditorHidden in the viewport
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
