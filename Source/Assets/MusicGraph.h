// Assets/MusicGraph.h - .hbmusic adaptive-music assets (FMOD/Wwise-style).
//
// A music graph describes interactive, adaptive music decoupled from gameplay:
//   * STATES (sections) - "Explore", "Combat", "Boss" - each a set of LAYERS
//     (stems) that loop together in sync. Switching state crossfades.
//   * LAYERS - one looping `.uaf` stem each, with a base volume and an optional
//     binding to a PARAMETER: the layer fades in as the parameter rises through
//     [paramLo, paramHi] (e.g. add drums/strings as "intensity" climbs).
//   * PARAMETERS - named runtime floats (e.g. "intensity") gameplay drives to
//     reshape the mix without switching state.
// Authored in the editor's Music panel, played through AudioSystem's music
// director (SetMusicGraph / PlayMusicState / SetMusicParameter / PostStinger).
#pragma once

#include "Core/Types.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace hbe {

// One looping stem within a state. When `parameter` is set, the layer's gain is
// `volume * smoothstep(paramLo, paramHi, parameterValue)`; otherwise just `volume`.
struct MusicLayer {
    std::string name = "Layer";
    std::string asset;          // `.uaf` Audio (a loop), relative to Assets/
    f32 volume = 1.0f;          // base gain
    std::string parameter;      // optional parameter that fades this layer in/out
    f32 paramLo = 0.0f;         // fade-in starts here
    f32 paramHi = 1.0f;         // fully in here
};

// When a queued state change is allowed to actually happen. Switching the instant
// gameplay asks lands mid-phrase and sounds like a mistake; waiting for the next
// musical boundary is what makes an adaptive score feel composed rather than
// switched. Serialized - append only.
enum class MusicSync : u8 {
    Immediate = 0, // switch on the spot (stingers, hard cuts, deaths)
    Beat,          // wait for the next beat
    Bar,           // wait for the next bar (the usual choice)
    TwoBars,
    FourBars,      // phrase-level; the most musical, the least responsive
};

const char* SyncName(MusicSync s);
// The sync interval in seconds for a state's tempo. Immediate returns 0.
f32 SyncInterval(MusicSync s, f32 bpm, int beatsPerBar);

// A music section: layers that loop together; entering the state crossfades them in.
struct MusicState {
    std::string name = "State";
    f32 bpm = 120.0f;           // drives beat/bar-quantized transitions
    int beatsPerBar = 4;
    // When LEAVING this state, wait for this musical boundary before the switch.
    // The outgoing state owns the rule because the boundary being waited for is
    // the one currently playing.
    MusicSync sync = MusicSync::Immediate;
    std::vector<MusicLayer> layers;
};

// A runtime float gameplay drives (e.g. "intensity" 0..1) to reshape the live mix.
struct MusicParameter {
    std::string name = "intensity";
    f32 defaultValue = 0.0f;
    f32 min = 0.0f;
    f32 max = 1.0f;
};

struct MusicGraph {
    f32 defaultFade = 2.0f;            // crossfade seconds between states
    std::string initialState;         // state played when the game starts
    // Dialogue ducking: while a voice line plays, the music bus drops by this many
    // dB so speech stays intelligible without the player reaching for the mixer.
    // 0 = off. Standard practice in any game with spoken dialogue.
    f32 duckDecibels = 0.0f;
    f32 duckAttack = 0.15f;   // seconds to duck down (fast: speech starts abruptly)
    f32 duckRelease = 0.60f;  // seconds to come back up (slow: avoids pumping)
    std::vector<MusicParameter> parameters;
    std::vector<MusicState> states;

    const MusicState* FindState(const std::string& name) const {
        for (const MusicState& s : states)
            if (s.name == name) return &s;
        return nullptr;
    }
};

namespace assets {

inline constexpr const char* kMusicGraphExtension = ".hbmusic";

bool SaveMusicGraph(const std::filesystem::path& path, const MusicGraph& g);
// Pack-aware (reads through the VFS like every other asset load).
std::optional<MusicGraph> LoadMusicGraph(const std::filesystem::path& path);

} // namespace assets
} // namespace hbe
