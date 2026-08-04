// Core/Window_Win32.cpp - Win32 implementation of the platform window.
#include "Core/Window.h"
#include "Core/Input.h"
#include "Core/Log.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h> // DragAcceptFiles / WM_DROPFILES

namespace hbe {

// Defined below; the window class registration needs it before then.
i64 __stdcall WndProcThunk(void* hwnd, u32 msg, u64 wparam, i64 lparam);

namespace {
constexpr const wchar_t* kWindowClassName = L"HeartbreakEngineWindowClass";

// UTF-8 -> UTF-16 for the title bar. The interface carries UTF-8 because a window title is
// user-facing text; this is the one place that has to speak the OS's encoding.
std::wstring Widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
    if (need <= 0) return {};
    std::wstring out(static_cast<usize>(need), 0);
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                          out.data(), need);
    return out;
}
} // namespace

Window::Window(const WindowDesc& desc) {
    HINSTANCE hinstance = ::GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc   = reinterpret_cast<WNDPROC>(&WndProcThunk);
    wc.hInstance     = hinstance;
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClassName;
    ::RegisterClassExW(&wc); // idempotent enough for a single-window foundation

    DWORD style;
    int x, y, w, h;
    if (desc.fullscreen) {
        // Borderless windowed fullscreen: a chromeless popup covering the
        // primary monitor. No exclusive mode / display-mode change, so it plays
        // nicely with the swapchain and alt-tab.
        style = WS_POPUP;
        const int sw = ::GetSystemMetrics(SM_CXSCREEN);
        const int sh = ::GetSystemMetrics(SM_CYSCREEN);
        x = 0;
        y = 0;
        w = sw;
        h = sh;
        width_ = static_cast<u32>(sw);
        height_ = static_cast<u32>(sh);
    } else {
        // Convert desired client size to an outer window rect.
        style = WS_OVERLAPPEDWINDOW;
        RECT rect{0, 0, static_cast<LONG>(desc.width), static_cast<LONG>(desc.height)};
        ::AdjustWindowRect(&rect, style, FALSE);
        x = desc.posX >= 0 ? desc.posX : CW_USEDEFAULT;
        y = desc.posY >= 0 ? desc.posY : CW_USEDEFAULT;
        w = rect.right - rect.left;
        h = rect.bottom - rect.top;
        width_ = desc.width;
        height_ = desc.height;
    }

    const std::wstring wideTitle = Widen(desc.title);
    HWND hwnd = ::CreateWindowExW(0, kWindowClassName, wideTitle.c_str(), style, x, y, w, h,
                                  nullptr, nullptr, hinstance, this);

    if (!hwnd) {
        HBE_ERROR("CreateWindowExW failed (GetLastError={})", ::GetLastError());
        return;
    }

    native_.hwnd = hwnd;
    native_.hinstance = hinstance;

    ::DragAcceptFiles(hwnd, TRUE); // accept OS file drops (WM_DROPFILES)

    ::ShowWindow(hwnd, SW_SHOW); // already screen-sized when fullscreen
    ::UpdateWindow(hwnd);
    HBE_INFO("Window created ({}x{}{})", width_, height_,
             desc.fullscreen ? ", fullscreen" : "");
}

Window::~Window() {
    if (native_.hwnd) {
        ::DestroyWindow(static_cast<HWND>(native_.hwnd));
        native_.hwnd = nullptr;
    }
    ::UnregisterClassW(kWindowClassName, ::GetModuleHandleW(nullptr));
}

bool Window::PumpMessages() {
    MSG msg{};
    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
        if (msg.message == WM_QUIT) {
            closing_ = true;
        }
    }
    return !closing_;
}

// Grants the free-function window procedure access to Window::HandleMessage without
// putting an MSVC calling convention in the shared header.
struct WindowPlatformAccess {
    static i64 Handle(Window& w, void* hwnd, u32 msg, u64 wparam, i64 lparam) {
        return w.HandleMessage(hwnd, msg, wparam, lparam);
    }
};

i64 __stdcall WndProcThunk(void* hwnd, u32 msg, u64 wparam, i64 lparam) {
    HWND h = static_cast<HWND>(hwnd);

    if (msg == WM_NCCREATE) {
        // Stash the Window* passed via CreateWindowExW's lpParam.
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        auto* self = static_cast<Window*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }

    auto* self = reinterpret_cast<Window*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self) {
        return WindowPlatformAccess::Handle(*self, hwnd, msg, wparam, lparam);
    }
    return ::DefWindowProcW(h, msg, static_cast<WPARAM>(wparam), static_cast<LPARAM>(lparam));
}

void Window::FeedInput(u32 msg, u64 wparam, i64 lparam) {
    if (!input_) return;
    HWND h = static_cast<HWND>(native_.hwnd);

    auto button = [&](MouseButton b, bool down) {
        input_->OnMouseButton(b, down);
        // Keep receiving mouse moves while any button is held (drag past edge).
        if (down) {
            if (mouseCapture_++ == 0) ::SetCapture(h);
        } else {
            if (mouseCapture_ > 0 && --mouseCapture_ == 0) ::ReleaseCapture();
        }
    };

    switch (msg) {
        case WM_KEYDOWN: case WM_SYSKEYDOWN:
            input_->OnKeyVK(static_cast<u32>(wparam), true);
            break;
        case WM_KEYUP: case WM_SYSKEYUP:
            input_->OnKeyVK(static_cast<u32>(wparam), false);
            break;
        case WM_CHAR:
            // Translated text input (TranslateMessage in PumpMessages emits
            // these). Only printable ASCII 32..126 is accepted, matching the
            // font atlas's baked range (FontAtlas bakes ASCII 32..126 and lays
            // text out byte-wise): admitting higher code points would store
            // characters that render as nothing and freeze the caret. Controls
            // (<32) and DEL (127) arrive as WM_KEYDOWN edits instead. Widen this
            // (and the atlas + a UTF-8-aware Layout) when extended glyphs land.
            if (wparam >= 32 && wparam < 127) {
                input_->OnChar(static_cast<u32>(wparam));
            }
            break;
        case WM_LBUTTONDOWN: button(MouseButton::Left, true);    break;
        case WM_LBUTTONUP:   button(MouseButton::Left, false);   break;
        case WM_RBUTTONDOWN: button(MouseButton::Right, true);   break;
        case WM_RBUTTONUP:   button(MouseButton::Right, false);  break;
        case WM_MBUTTONDOWN: button(MouseButton::Middle, true);  break;
        case WM_MBUTTONUP:   button(MouseButton::Middle, false); break;
        case WM_XBUTTONDOWN:
            button(GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? MouseButton::X1
                                                          : MouseButton::X2, true);
            break;
        case WM_XBUTTONUP:
            button(GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? MouseButton::X1
                                                          : MouseButton::X2, false);
            break;
        case WM_MOUSEMOVE:
            if (cursorLocked_) {
                // Mouse-look: report the move as a delta from the window centre,
                // then snap the cursor back so it never reaches a screen edge.
                HWND mh = static_cast<HWND>(native_.hwnd);
                RECT rc{};
                ::GetClientRect(mh, &rc);
                const i32 cx = (rc.right - rc.left) / 2, cy = (rc.bottom - rc.top) / 2;
                const i32 dx = GET_X_LPARAM(lparam) - cx, dy = GET_Y_LPARAM(lparam) - cy;
                if (dx != 0 || dy != 0) {
                    input_->OnMouseLockedDelta(static_cast<f32>(dx), static_cast<f32>(dy));
                    POINT p{cx, cy};
                    ::ClientToScreen(mh, &p);
                    ::SetCursorPos(p.x, p.y);
                }
            } else {
                input_->OnMouseMove(static_cast<f32>(GET_X_LPARAM(lparam)),
                                    static_cast<f32>(GET_Y_LPARAM(lparam)));
            }
            break;
        case WM_MOUSEWHEEL:
            input_->OnMouseWheel(static_cast<f32>(GET_WHEEL_DELTA_WPARAM(wparam)) /
                                 static_cast<f32>(WHEEL_DELTA));
            break;
        case WM_KILLFOCUS:
            input_->OnFocusLost();
            if (mouseCapture_ > 0) {
                mouseCapture_ = 0;
                ::ReleaseCapture();
            }
            break;
        default:
            break;
    }
}

void Window::SetCursorLocked(bool locked) {
    if (locked == cursorLocked_) return;
    cursorLocked_ = locked;
    HWND h = static_cast<HWND>(native_.hwnd);
    if (locked) {
        while (::ShowCursor(FALSE) >= 0) {} // hide (counter-based)
        ::SetCapture(h);
        RECT rc{};
        ::GetClientRect(h, &rc);
        POINT p{(rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2};
        ::ClientToScreen(h, &p);
        ::SetCursorPos(p.x, p.y);
    } else {
        while (::ShowCursor(TRUE) < 0) {} // show
        if (mouseCapture_ == 0) ::ReleaseCapture();
        if (input_) input_->ResetMousePos(); // avoid a delta spike on the next move
    }
}

i64 Window::HandleMessage(void* hwnd, u32 msg, u64 wparam, i64 lparam) {
    HWND h = static_cast<HWND>(hwnd);

    // The engine input state sees every event, even ones an overlay consumes;
    // higher layers (editor) decide what counts as "game" input.
    FeedInput(msg, wparam, lparam);

    // Give an overlay (ImGui) first chance at the message.
    if (wndHook_) {
        const i64 consumed = wndHook_(hwnd, msg, wparam, lparam);
        if (consumed) return consumed;
    }

    switch (msg) {
        case WM_SIZE: {
            const u32 w = LOWORD(lparam);
            const u32 hgt = HIWORD(lparam);
            width_  = w;
            height_ = hgt;
            if (onResize_ && wparam != SIZE_MINIMIZED) {
                onResize_(w, hgt);
            }
            return 0;
        }
        // Escape no longer closes the window: games own the Escape key (pause
        // menus etc.) and the editor must not exit on a stray press. Keyboard
        // state is already captured by FeedInput above; let WM_KEYDOWN fall
        // through to DefWindowProc.
        case WM_CLOSE:
            closing_ = true;
            ::PostQuitMessage(0);
            return 0;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        case WM_DROPFILES: {
            // Files dropped from Explorer. Queue their native paths; the editor
            // drains TakeDroppedFiles() each frame and imports the supported ones.
            HDROP drop = reinterpret_cast<HDROP>(static_cast<uintptr_t>(wparam));
            const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
            for (UINT idx = 0; idx < count; ++idx) {
                const UINT len = ::DragQueryFileW(drop, idx, nullptr, 0);
                if (len == 0) continue;
                std::wstring wpath(len, L'\0');
                ::DragQueryFileW(drop, idx, wpath.data(), len + 1); // +1: room for the null
                droppedFiles_.emplace_back(std::move(wpath));       // path from native wstring
            }
            ::DragFinish(drop);
            // Bound growth if nobody drains (e.g. a shipped runtime): keep newest.
            if (droppedFiles_.size() > 256)
                droppedFiles_.erase(droppedFiles_.begin(), droppedFiles_.end() - 128);
            return 0;
        }
        default:
            break;
    }
    return ::DefWindowProcW(h, msg, static_cast<WPARAM>(wparam), static_cast<LPARAM>(lparam));
}

std::vector<std::filesystem::path> Window::TakeDroppedFiles() {
    std::vector<std::filesystem::path> out = std::move(droppedFiles_);
    droppedFiles_.clear(); // moved-from vector is valid-but-unspecified; force empty
    return out;
}

} // namespace hbe
