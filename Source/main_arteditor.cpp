// main_arteditor.cpp - Heartbreak *Art Editor* entry point.
//
// A focused build of the editor for 2D artists: it boots straight into the
// painting workflow (a wide Art Editor panel + viewport with paint mode on) and
// hides the engineering panels. It is the full editor underneath, so scenes and
// paint canvases save/load identically (Editor::SetArtMode just picks the
// painting-focused default layout).
//
// Usage: HeartbreakArtEditor [--d3d12 | --vulkan] [--project <hbproj>]
#include "Core/Window.h"
#include "Editor/Editor.h"
#include "Engine/Engine.h"
#include "Physics/PhysicsWorld.h"
#include "Project/Project.h"
#include "Renderer/Renderer.h"
#include "Scene/Scene.h"

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
    hbe::EngineConfig config = hbe::ParseCommandLine(argc, argv);
    config.title = L"Heartbreak Art Editor";

    // --project opens a project directly; otherwise the Project Manager modal
    // lets the artist pick one (recents remembered).
    if (!config.projectPath.empty()) {
        hbe::Project::Active().Open(std::filesystem::path(config.projectPath));
    }

    hbe::Engine engine;
    hbe::Editor editor;
    editor.SetArtMode(true);

    engine.SetOnInit([](hbe::Engine& e) {
        e.GetPhysics().SetRunning(false); // no simulation in the painting tool
        e.SetGameCameraEnabled(false);    // the scene view owns the camera
        e.GetScene().SetEditorView(true); // honor per-entity EditorHidden
        if (e.GetRenderer().InitUI(e.GetWindow().GetNativeHandle().hwnd)) {
            hbe::Editor::ApplyTheme();
            hbe::Editor::EnableLayoutPersistence("HeartbreakArtEditor.ini"); // own layout
            e.GetWindow().SetWndProcHook([](void* h, hbe::u32 m, hbe::u64 w, hbe::i64 l) -> hbe::i64 {
                return ImGui_ImplWin32_WndProcHandler(static_cast<HWND>(h), m,
                                                      static_cast<WPARAM>(w),
                                                      static_cast<LPARAM>(l));
            });
        }
    });

    engine.SetOnFrame([&editor](hbe::Engine& e) { editor.BuildUI(e); });

    return engine.Run(config);
}
