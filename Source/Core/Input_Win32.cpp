// Core/Input_Win32.cpp - Win32 virtual-key translation + XInput gamepad poll.
#include "Core/Input.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <xinput.h>

#include <cmath>
#include <cstring>

namespace hbe {

namespace {

Key TranslateVK(u32 vk) {
    if (vk >= 'A' && vk <= 'Z') return static_cast<Key>(static_cast<u32>(Key::A) + (vk - 'A'));
    if (vk >= '0' && vk <= '9') return static_cast<Key>(static_cast<u32>(Key::Num0) + (vk - '0'));
    if (vk >= VK_F1 && vk <= VK_F12) return static_cast<Key>(static_cast<u32>(Key::F1) + (vk - VK_F1));
    switch (vk) {
        case VK_ESCAPE:  return Key::Escape;
        case VK_TAB:     return Key::Tab;
        case VK_SPACE:   return Key::Space;
        case VK_RETURN:  return Key::Enter;
        case VK_BACK:    return Key::Backspace;
        case VK_DELETE:  return Key::Delete;
        case VK_INSERT:  return Key::Insert;
        case VK_HOME:    return Key::Home;
        case VK_END:     return Key::End;
        case VK_PRIOR:   return Key::PageUp;
        case VK_NEXT:    return Key::PageDown;
        case VK_LEFT:    return Key::Left;
        case VK_RIGHT:   return Key::Right;
        case VK_UP:      return Key::Up;
        case VK_DOWN:    return Key::Down;
        case VK_SHIFT:   return Key::Shift;
        case VK_CONTROL: return Key::Ctrl;
        case VK_MENU:    return Key::Alt;
        case VK_OEM_3:   return Key::Grave; // backtick/tilde (US layout)
        default:         return Key::Unknown;
    }
}

// Applies a radial deadzone and normalizes a raw thumbstick axis pair.
void NormalizeStick(i16 rawX, i16 rawY, i32 deadzone, f32& outX, f32& outY) {
    const f32 x = static_cast<f32>(rawX);
    const f32 y = static_cast<f32>(rawY);
    const f32 mag = std::sqrt(x * x + y * y);
    if (mag <= static_cast<f32>(deadzone)) {
        outX = outY = 0.0f;
        return;
    }
    const f32 norm = (mag - deadzone) / (32767.0f - deadzone);
    const f32 scale = (norm > 1.0f ? 1.0f : norm) / mag;
    outX = x * scale;
    outY = y * scale;
}

} // namespace

void Input::NewFrame() {
    std::memcpy(prevKeys_, keys_, sizeof(keys_));
    std::memcpy(prevMouse_, mouse_, sizeof(mouse_));
    mouseDeltaX_ = mouseDeltaY_ = 0.0f;
    wheel_ = 0.0f;
    PollGamepads();
}

void Input::OnKeyVK(u32 nativeKey, bool down) {
    const Key k = TranslateVK(nativeKey);
    if (k != Key::Unknown) keys_[Index(k)] = down;
}

void Input::OnMouseButton(MouseButton b, bool down) {
    mouse_[Index(b)] = down;
}

void Input::OnMouseMove(f32 x, f32 y) {
    if (hasMousePos_) {
        mouseDeltaX_ += x - mouseX_;
        mouseDeltaY_ += y - mouseY_;
    }
    mouseX_ = x;
    mouseY_ = y;
    hasMousePos_ = true;
}

void Input::OnMouseLockedDelta(f32 dx, f32 dy) {
    mouseDeltaX_ += dx;
    mouseDeltaY_ += dy;
}

void Input::ResetMousePos() {
    hasMousePos_ = false; // the next OnMouseMove seeds position without a delta
}

void Input::OnMouseWheel(f32 delta) {
    wheel_ += delta;
}

void Input::OnFocusLost() {
    std::memset(keys_, 0, sizeof(keys_));
    std::memset(mouse_, 0, sizeof(mouse_));
    hasMousePos_ = false;
}

void Input::PollGamepads() {
    for (u32 i = 0; i < kMaxGamepads; ++i) {
        GamepadState& pad = pads_[i];
        pad.prevButtons = pad.buttons;

        // Probing a disconnected pad stalls XInput for a while; back off.
        if (!pad.connected && padRetryCooldown_[i] > 0) {
            --padRetryCooldown_[i];
            continue;
        }

        XINPUT_STATE state{};
        if (XInputGetState(i, &state) != ERROR_SUCCESS) {
            pad = GamepadState{};
            padRetryCooldown_[i] = 120; // ~2s at 60 FPS
            continue;
        }

        pad.connected = true;
        const XINPUT_GAMEPAD& g = state.Gamepad;
        NormalizeStick(g.sThumbLX, g.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE,
                       pad.leftX, pad.leftY);
        NormalizeStick(g.sThumbRX, g.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE,
                       pad.rightX, pad.rightY);
        pad.leftTrigger = g.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD
                              ? (g.bLeftTrigger - XINPUT_GAMEPAD_TRIGGER_THRESHOLD) /
                                    (255.0f - XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
                              : 0.0f;
        pad.rightTrigger = g.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD
                               ? (g.bRightTrigger - XINPUT_GAMEPAD_TRIGGER_THRESHOLD) /
                                     (255.0f - XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
                               : 0.0f;
        pad.buttons = g.wButtons;
    }
}

} // namespace hbe
