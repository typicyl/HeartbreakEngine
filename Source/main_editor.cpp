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
#include "Engine/Engine.h"
#include "Physics/PhysicsWorld.h"
#include "Project/Project.h"
#include "Renderer/Renderer.h"
#include "Scene/Scene.h"

#include <cstring>
#include <filesystem>

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
