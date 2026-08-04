// Core/Input_Win32.cpp - the Windows half of Input: virtual-key translation and gamepads.
//
// Everything that is not OS-shaped lives in Core/Input.cpp. What remains here is the part a
// second platform genuinely has to reimplement: turning native key codes into hbe::Key, and
// asking XInput / Raw Input about controllers. The frame logic, edge detection, text stream
// and deadzone curve are shared and tested independently.
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

} // namespace

// Translate, then hand to the shared logic. This function is the ONLY thing standing between
// a Win32 message pump and platform-independent input state.
void Input::OnKeyVK(u32 nativeKey, bool down) {
    OnKey(TranslateVK(nativeKey), down);
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
        input_detail::NormalizeStick(g.sThumbLX, g.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE,
                                     pad.leftX, pad.leftY);
        input_detail::NormalizeStick(g.sThumbRX, g.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE,
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
