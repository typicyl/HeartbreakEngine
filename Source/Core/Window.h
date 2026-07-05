// Core/Window.h - platform window abstraction (Win32 implementation for now).
#pragma once

#include "Core/Types.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace hbe {

class Input;

struct WindowDesc {
    std::wstring title = L"Heartbreak Engine";
    u32 width  = 1280;
    u32 height = 720;
    // Outer window position; -1 lets the OS choose (useful fixed values make
    // UI automation and multi-instance layouts deterministic).
    i32 posX = -1;
    i32 posY = -1;
    // Borderless fullscreen: a chromeless popup covering the primary monitor
    // (width/height are ignored - the client area is the screen size).
    bool fullscreen = false;
};

// Opaque native handle. On Win32 this carries an HWND; backends reinterpret it.
struct NativeWindowHandle {
    void* hwnd = nullptr;       // HWND
    void* hinstance = nullptr;  // HINSTANCE
};

class Window : public NonCopyable {
public:
    explicit Window(const WindowDesc& desc);
    ~Window();

    // Pumps the OS message queue. Returns false once the window is closing.
    bool PumpMessages();

    // Asks the main loop to end (same as the user closing the window).
    void RequestClose() { closing_ = true; }

    NativeWindowHandle GetNativeHandle() const { return native_; }
    u32  Width()  const { return width_; }
    u32  Height() const { return height_; }
    bool IsMinimized() const { return width_ == 0 || height_ == 0; }

    // Invoked when the client area changes size (new width, height).
    using ResizeCallback = std::function<void(u32, u32)>;
    void SetResizeCallback(ResizeCallback cb) { onResize_ = std::move(cb); }

    // Optional pre-handler (e.g. Dear ImGui). Receives (hwnd, msg, wparam,
    // lparam); a non-zero return value consumes the message. Used to feed input
    // to overlays without coupling the window to them.
    using WndProcHook = std::function<i64(void*, u32, u64, i64)>;
    void SetWndProcHook(WndProcHook hook) { wndHook_ = std::move(hook); }

    // Routes keyboard/mouse messages into the engine input state. The window
    // translates native events; `input` just stores platform-agnostic state.
    void SetInputSink(Input* input) { input_ = input; }

    // Files dropped onto the window from the OS since the last call (native
    // paths, encoding-safe). Moves them out and clears the queue - the editor
    // drains this each frame to import; unread drops are bounded so a runtime
    // that never drains can't leak.
    std::vector<std::filesystem::path> TakeDroppedFiles();

    // Locks the OS cursor to the window centre and hides it (FPS/mouse-look mode):
    // each mouse-move is fed to Input as a raw delta and the cursor is re-centred,
    // so it never reaches a screen edge. Unlocking shows the cursor again.
    void SetCursorLocked(bool locked);
    bool IsCursorLocked() const { return cursorLocked_; }

private:
    // Translates an input-related message into Input calls (no-op otherwise).
    void FeedInput(u32 msg, u64 wparam, i64 lparam);

    NativeWindowHandle native_{};
    u32  width_  = 0;
    u32  height_ = 0;
    bool closing_ = false;
    ResizeCallback onResize_;
    WndProcHook wndHook_;
    Input* input_ = nullptr;
    int mouseCapture_ = 0; // held-button count driving SetCapture/ReleaseCapture
    bool cursorLocked_ = false; // mouse-look mode: hidden + recentred each move
    std::vector<std::filesystem::path> droppedFiles_; // OS drag-drop, drained by the editor

    // Win32 window procedure dispatches into this instance.
    static i64 __stdcall WndProcThunk(void* hwnd, u32 msg, u64 wparam, i64 lparam);
    i64 HandleMessage(void* hwnd, u32 msg, u64 wparam, i64 lparam);
};

} // namespace hbe
