// Core/InputActions.cpp
#include "Core/InputActions.h"

namespace hbe::input {

const char* KeyName(Key k) {
    switch (k) {
        case Key::A: return "A"; case Key::B: return "B"; case Key::C: return "C";
        case Key::D: return "D"; case Key::E: return "E"; case Key::F: return "F";
        case Key::G: return "G"; case Key::H: return "H"; case Key::I: return "I";
        case Key::J: return "J"; case Key::K: return "K"; case Key::L: return "L";
        case Key::M: return "M"; case Key::N: return "N"; case Key::O: return "O";
        case Key::P: return "P"; case Key::Q: return "Q"; case Key::R: return "R";
        case Key::S: return "S"; case Key::T: return "T"; case Key::U: return "U";
        case Key::V: return "V"; case Key::W: return "W"; case Key::X: return "X";
        case Key::Y: return "Y"; case Key::Z: return "Z";
        case Key::Num0: return "0"; case Key::Num1: return "1"; case Key::Num2: return "2";
        case Key::Num3: return "3"; case Key::Num4: return "4"; case Key::Num5: return "5";
        case Key::Num6: return "6"; case Key::Num7: return "7"; case Key::Num8: return "8";
        case Key::Num9: return "9";
        case Key::F1: return "F1"; case Key::F2: return "F2"; case Key::F3: return "F3";
        case Key::F4: return "F4"; case Key::F5: return "F5"; case Key::F6: return "F6";
        case Key::F7: return "F7"; case Key::F8: return "F8"; case Key::F9: return "F9";
        case Key::F10: return "F10"; case Key::F11: return "F11"; case Key::F12: return "F12";
        case Key::Escape: return "Esc"; case Key::Tab: return "Tab"; case Key::Space: return "Space";
        case Key::Enter: return "Enter"; case Key::Backspace: return "Backspace";
        case Key::Delete: return "Del"; case Key::Insert: return "Ins";
        case Key::Home: return "Home"; case Key::End: return "End";
        case Key::PageUp: return "PgUp"; case Key::PageDown: return "PgDn";
        case Key::Left: return "Left"; case Key::Right: return "Right";
        case Key::Up: return "Up"; case Key::Down: return "Down";
        case Key::Shift: return "Shift"; case Key::Ctrl: return "Ctrl"; case Key::Alt: return "Alt";
        case Key::Grave: return "`";
        default: return "";
    }
}

const std::vector<PadButtonInfo>& PadButtons() {
    static const std::vector<PadButtonInfo> kButtons = {
        {Gamepad_A, "A"},         {Gamepad_B, "B"},         {Gamepad_X, "X"},
        {Gamepad_Y, "Y"},         {Gamepad_LShoulder, "LB"}, {Gamepad_RShoulder, "RB"},
        {Gamepad_LThumb, "L3"},   {Gamepad_RThumb, "R3"},   {Gamepad_Start, "Start"},
        {Gamepad_Back, "Back"},   {Gamepad_DPadUp, "D-Up"}, {Gamepad_DPadDown, "D-Down"},
        {Gamepad_DPadLeft, "D-Left"}, {Gamepad_DPadRight, "D-Right"},
    };
    return kButtons;
}

Binding ActionMap::Current(const std::string& name) const {
    if (const auto it = overrides_.find(name); it != overrides_.end()) return it->second;
    for (const ActionDef& d : defs_)
        if (d.name == name) return d.defaults;
    return {};
}

bool ActionMap::Pressed(const Input& in, const std::string& name) const {
    const Binding b = Current(name);
    if (b.key != Key::Unknown && in.WasKeyPressed(b.key)) return true;
    if (b.pad != 0 && in.Gamepad().WasPressed(static_cast<GamepadButton>(b.pad))) return true;
    return false;
}

bool ActionMap::Down(const Input& in, const std::string& name) const {
    const Binding b = Current(name);
    if (b.key != Key::Unknown && in.IsKeyDown(b.key)) return true;
    if (b.pad != 0 && in.Gamepad().IsDown(static_cast<GamepadButton>(b.pad))) return true;
    return false;
}

bool ActionMap::UpdateRebind(const Input& in) {
    if (!rebinding_) return false;
    // Skip the frame BeginRebind was called: the Enter / Gamepad_A edge that activated
    // the "rebind" UI button (keyboard/gamepad menu nav) is still live this frame and
    // would otherwise be captured as the new binding. Arm now, capture the NEXT frame.
    if (!armed_) {
        armed_ = true;
        return false;
    }
    // Escape cancels (raw, so it works even if some field claims text capture).
    if (in.WasKeyPressedRaw(Key::Escape)) {
        CancelRebind();
        return false;
    }
    // First key edge this frame wins; else first gamepad button. Modifiers and the
    // dev-console key are skipped: a bare modifier is an ambiguous binding, and Grave
    // collides with the Ctrl+` dev-menu toggle.
    for (u32 k = 1; k < static_cast<u32>(Key::Count); ++k) {
        const Key key = static_cast<Key>(k);
        if (key == Key::Escape || key == Key::Shift || key == Key::Ctrl ||
            key == Key::Alt || key == Key::Grave)
            continue;
        if (in.WasKeyPressedRaw(key)) {
            overrides_[rebindName_] = Binding{key, 0};
            CancelRebind();
            return true;
        }
    }
    for (const PadButtonInfo& pb : PadButtons()) {
        if (in.Gamepad().WasPressed(static_cast<GamepadButton>(pb.bit))) {
            overrides_[rebindName_] = Binding{Key::Unknown, pb.bit};
            CancelRebind();
            return true;
        }
    }
    return false;
}

void ActionMap::ClearOverride(const std::string& name) {
    overrides_.erase(name);
}

} // namespace hbe::input
