// main_hub.cpp - Heartbreak *Hub* entry point.
//
// A small launcher: shows the Project Manager (recent projects, create, open)
// over the live demo scene, then hands the chosen project to a freshly
// spawned HeartbreakEditor process and exits.
//
// Usage: HeartbreakHub [--d3d12 | --vulkan]
#include "Core/Window.h"
#include "Editor/Editor.h"
#include "Engine/Engine.h"
#include "Renderer/Renderer.h"

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
    config.title = L"Heartbreak Hub";
    config.width = 960;
    config.height = 600;

    hbe::Engine engine;
    hbe::Editor editor;
    editor.SetHubMode(true);

    engine.SetOnInit([](hbe::Engine& e) {
        if (e.GetRenderer().InitUI(e.GetWindow().GetNativeHandle().hwnd)) {
            hbe::Editor::ApplyTheme();
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
