// Core/InputActions.h - data-driven input actions + rebindable bindings.
//
// A game defines named ACTIONS ("Interact", "Attack", ...) in the project, each with
// a default keyboard key + gamepad button. Players can REBIND them at runtime (stored
// per-user); the editor authors the defaults. Gameplay queries actions by name
// (Pressed/Down) instead of raw keys, and the interaction-prompt icon is looked up
// from an action's CURRENT binding on the active device. See [[heartbreak-interaction]].
#pragma once

#include "Core/Input.h" // Key, GamepadButton, Input
#include "Core/Types.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace hbe::input {

// One control binding: a keyboard key and/or a gamepad button (either may be unset).
struct Binding {
    Key key = Key::Unknown; // keyboard key (Unknown = none)
    u32 pad = 0;            // GamepadButton bit (0 = none)
};

// An authored action: a name + its default binding. Defaults live in the project;
// per-user rebinds override them.
struct ActionDef {
    std::string name;
    Binding defaults;
};

// Human-readable name for a key ("E", "Space", "F1", ...); "" for Key::Unknown.
const char* KeyName(Key k);

// The gamepad buttons that can be bound / shown, with a generic (Xbox-style) label.
struct PadButtonInfo {
    u32 bit;             // GamepadButton bit
    const char* name;    // generic label ("A", "X", "LB", "D-Pad Up", ...)
};
const std::vector<PadButtonInfo>& PadButtons();

// Runtime action registry: resolves each action to its current binding (per-user
// override, else the project default) and answers input queries. Also drives
// interactive rebinding (listen for the next key/button and assign it).
class ActionMap {
public:
    void SetDefinitions(std::vector<ActionDef> defs) { defs_ = std::move(defs); }
    void SetOverrides(std::unordered_map<std::string, Binding> ov) { overrides_ = std::move(ov); }
    const std::vector<ActionDef>& Definitions() const { return defs_; }
    const std::unordered_map<std::string, Binding>& Overrides() const { return overrides_; }

    // Current binding for `name`: the user override if present, else the authored
    // default, else an empty binding.
    Binding Current(const std::string& name) const;

    bool Pressed(const Input& in, const std::string& name) const; // down-edge this frame
    bool Down(const Input& in, const std::string& name) const;    // held

    // --- Rebinding ----------------------------------------------------------
    // armed_ = false means UpdateRebind will SKIP the frame BeginRebind was called:
    // the key/button edge that activated the "rebind" UI button (Enter / Gamepad_A on
    // keyboard/gamepad menu nav) is still live that frame, so capturing immediately
    // would bind that trigger. We arm on the first UpdateRebind and capture the NEXT.
    void BeginRebind(const std::string& name) { rebinding_ = true; rebindName_ = name; armed_ = false; }
    bool Rebinding() const { return rebinding_; }
    const std::string& RebindingAction() const { return rebindName_; }
    void CancelRebind() { rebinding_ = false; rebindName_.clear(); armed_ = false; }
    // While rebinding, capture the first key/button pressed this frame and store it
    // as an override (Escape cancels; modifiers/Grave are ignored). Returns true the
    // frame a bind was COMMITTED (so the caller can persist), false otherwise.
    bool UpdateRebind(const Input& in);

    void ClearOverride(const std::string& name); // revert one action to its default
    void ClearAllOverrides() { overrides_.clear(); }

private:
    std::vector<ActionDef> defs_;
    std::unordered_map<std::string, Binding> overrides_;
    bool rebinding_ = false;
    bool armed_ = false; // false = skip the frame BeginRebind was called (see BeginRebind)
    std::string rebindName_;
};

} // namespace hbe::input
