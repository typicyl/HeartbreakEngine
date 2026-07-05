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
#include <vector>

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
    std::memset(pressedEdge_, 0, sizeof(pressedEdge_));
    mouseDeltaX_ = mouseDeltaY_ = 0.0f;
    wheel_ = 0.0f;
    textEventCount_ = 0;
    PollGamepads();
}

void Input::PushEditKey(Key k) {
    TextEditEvent::Kind kind;
    switch (k) {
        case Key::Backspace: kind = TextEditEvent::Backspace; break;
        case Key::Delete:    kind = TextEditEvent::Delete; break;
        case Key::Left:      kind = TextEditEvent::CaretLeft; break;
        case Key::Right:     kind = TextEditEvent::CaretRight; break;
        case Key::Home:      kind = TextEditEvent::CaretHome; break;
        case Key::End:       kind = TextEditEvent::CaretEnd; break;
        default: return; // not an editing key
    }
    if (textEventCount_ < kMaxTextEvents)
        textEvents_[textEventCount_++] = TextEditEvent{kind, 0};
}

void Input::OnKeyVK(u32 nativeKey, bool down) {
    const Key k = TranslateVK(nativeKey);
    if (k == Key::Unknown) return;
    if (down) lastGamepad_ = false; // deliberate keyboard input -> keyboard/mouse prompts
    if (down) {
        // Down-transition = a fresh press edge (survives a same-frame release).
        if (!keys_[Index(k)]) pressedEdge_[Index(k)] = true;
        // Editing keys feed the ordered text-event stream, one per WM_KEYDOWN -
        // so OS auto-repeat yields repeated edits and message order relative to
        // WM_CHAR insertions is preserved. Cleared each frame; only drained by
        // an active text-field edit session.
        PushEditKey(k);
    }
    keys_[Index(k)] = down;
}

void Input::OnChar(u32 codepoint) {
    if (textEventCount_ < kMaxTextEvents)
        textEvents_[textEventCount_++] = TextEditEvent{TextEditEvent::InsertChar, codepoint};
}

void Input::OnMouseButton(MouseButton b, bool down) {
    if (down) lastGamepad_ = false;
    mouse_[Index(b)] = down;
}

void Input::OnMouseMove(f32 x, f32 y) {
    if (hasMousePos_) {
        mouseDeltaX_ += x - mouseX_;
        mouseDeltaY_ += y - mouseY_;
        // Notable mouse movement -> the player is on mouse (not a resting cursor
        // while playing on a pad); flip prompts back to keyboard/mouse.
        if (std::fabs(x - mouseX_) + std::fabs(y - mouseY_) > 2.0f) lastGamepad_ = false;
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
    std::memset(pressedEdge_, 0, sizeof(pressedEdge_));
    textEventCount_ = 0;
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

        // Deliberate gamepad activity flips the active device to the pad, so prompts
        // swap to controller glyphs. (Buttons, a stick push, or a trigger pull -
        // not idle drift, which the deadzone already zeroed.)
        if (pad.buttons != 0 || std::fabs(pad.leftX) > 0.5f || std::fabs(pad.leftY) > 0.5f ||
            std::fabs(pad.rightX) > 0.5f || std::fabs(pad.rightY) > 0.5f ||
            pad.leftTrigger > 0.5f || pad.rightTrigger > 0.5f)
            lastGamepad_ = true;
    }

    // Periodically re-identify the controller family (cheap; handles hot-plug).
    if (padBrandCooldown_ == 0) {
        RefreshGamepadBrand();
        padBrandCooldown_ = 120; // ~2s at 60 FPS
    } else {
        --padBrandCooldown_;
    }
}

void Input::RefreshGamepadBrand() {
    // XInput reports every pad as an Xbox pad, so identify the brand from the
    // connected HID game controllers' USB vendor ids via Raw Input (no window /
    // registration needed). Prefer Sony/Nintendo over Microsoft when several are
    // present; keep the default (Xbox) if nothing identifiable is found.
    UINT count = 0;
    if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != 0 || count == 0)
        return;
    std::vector<RAWINPUTDEVICELIST> list(count);
    if (GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1))
        return;

    bool sony = false, nintendo = false, microsoft = false, anyPad = false;
    for (UINT i = 0; i < count; ++i) {
        if (list[i].dwType != RIM_TYPEHID) continue;
        RID_DEVICE_INFO info{};
        info.cbSize = sizeof(info);
        UINT sz = sizeof(info);
        if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICEINFO, &info, &sz) ==
            static_cast<UINT>(-1))
            continue;
        // HID usage page 0x01 (generic desktop), usage 0x04 joystick / 0x05 gamepad.
        if (info.hid.usUsagePage != 0x01 || (info.hid.usUsage != 0x04 && info.hid.usUsage != 0x05))
            continue;
        anyPad = true;
        switch (info.hid.dwVendorId) {
            case 0x054C: sony = true; break;      // Sony
            case 0x057E: nintendo = true; break;  // Nintendo
            case 0x045E: microsoft = true; break; // Microsoft
            default: break;
        }
    }
    if (sony) padBrand_ = PadBrand::PlayStation;
    else if (nintendo) padBrand_ = PadBrand::Nintendo;
    else if (microsoft) padBrand_ = PadBrand::Xbox;
    else if (anyPad) padBrand_ = PadBrand::Generic;
    // else: no HID game controller found -> leave the previous/default brand.
}

} // namespace hbe
