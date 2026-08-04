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
    // UTF-8, NOT a native wide string. A window title is user-facing text - a game called
    // "Café" or one with a Japanese name has to survive reaching the title bar - and
    // wchar_t is not even the same width on every platform. The Win32 backend converts
    // properly on the way out; the previous std::wstring here pushed that conversion onto
    // callers, one of which "converted" by widening bytes one-for-one and produced mojibake
    // for any non-ASCII project name.
    std::string title = "Heartbreak Engine";
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

    // AN ACKNOWLEDGED PLATFORM ESCAPE HATCH, not an abstraction leak by accident. Dear
    // ImGui's platform backend needs the raw native message - that is its interface, and
    // wrapping it would only mean unwrapping it again inside the hook. The signature is
    // deliberately spelled in plain types (void*, u32, u64, i64) so this header still
    // compiles without any OS header; a second platform passes ITS native message here.
    // Everything the engine itself needs from a message goes through SetInputSink instead.
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

    // The per-instance message handler. The static native window procedure that dispatches
    // into it lives entirely in the platform .cpp: it needs __stdcall, which is an MSVC
    // spelling that does not exist on other compilers and therefore must not appear in a
    // header every translation unit includes.
    i64 HandleMessage(void* hwnd, u32 msg, u64 wparam, i64 lparam);
    friend struct WindowPlatformAccess;
};

} // namespace hbe
