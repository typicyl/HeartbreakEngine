// Core/UserSettings.h - per-user, per-game options that persist across runs.
//
// Distinct from .hbsave game saves and the project file: these are the player's own
// preferences (volume / graphics / brightness / captions), stored in the OS user data
// dir so they survive reinstalls of the game folder. Loaded on boot, saved on change.
#pragma once

#include "Core/Types.h"
#include "Core/InputActions.h" // input::Binding (per-user control rebinds)

#include <filesystem>
#include <string>
#include <unordered_map>

namespace hbe {

struct UserSettings {
    f32 masterVolume = 1.0f;      // [0,1] -> AudioSystem Master bus
    int graphicsPreset = 1;       // 0 High (as authored), 1 Medium (default; lean shipped stack), 2 Low
    f32 brightness = 0.5f;        // [0,1] -> +/-1-stop VIEW exposure multiplier (0.5 = neutral)
    // Two INDEPENDENT accessibility toggles, following the usual convention:
    //   subtitles = spoken dialogue only        (on by default - story legibility)
    //   captions  = non-speech sounds too       (off by default - opt-in verbosity)
    // Keeping them separate means a player can read the story without a running
    // commentary of every footstep and door.
    bool subtitlesEnabled = true; // speech subtitles on/off
    bool captionsEnabled = false; // closed captions (non-speech sounds) on/off
    bool speakerNames = true;     // show "Speaker:" in front of dialogue lines
    // Per-user control rebinds: action name -> binding, overriding the project's
    // default for that action. Empty = use every action's authored default.
    std::unordered_map<std::string, input::Binding> inputBindings;

    bool Save(const std::filesystem::path& dir) const; // <dir>/usersettings.json
    bool Load(const std::filesystem::path& dir);        // false if absent/invalid

    // Per-user settings directory for `gameName` (Windows: %LOCALAPPDATA%/<gameName>,
    // falling back to the working dir). Sanitizes the name for use as a folder.
    static std::filesystem::path ResolveDir(const std::string& gameName);
};

} // namespace hbe
