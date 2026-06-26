// Assets/AudioEvent.h - .hbevent audio event assets (FMOD/Wwise-style).
//
// An audio event is a small JSON file under the project's Assets/ directory
// describing *how* something should sound, decoupled from gameplay code:
// a weighted random pool of `.uaf` audio assets, the mixer bus it routes to,
// volume/pitch randomization, and 3D attenuation settings. Gameplay (or the
// editor's Play button) posts events through AudioSystem::PostEvent.
#pragma once

#include "Core/Types.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace hbe {

struct AudioEventSound {
    std::string asset;  // `.uaf` Audio path relative to Assets/
    f32 weight = 1.0f;  // random-pick weight within the event
};

struct AudioEvent {
    std::string bus = "SFX";              // mixer bus (empty/unknown = Master)
    std::vector<AudioEventSound> sounds;  // weighted random container

    f32 volume = 1.0f;          // base gain
    f32 volumeVariance = 0.0f;  // +- random gain per post
    f32 pitch = 1.0f;           // base playback rate
    f32 pitchVariance = 0.0f;   // +- random rate per post
    bool loop = false;

    // 3D: posts with a position attenuate between min and max distance.
    bool spatial = false;
    f32 minDistance = 1.0f;
    f32 maxDistance = 30.0f;
};

namespace assets {

inline constexpr const char* kAudioEventExtension = ".hbevent";

bool SaveAudioEvent(const std::filesystem::path& path, const AudioEvent& ev);
// Pack-aware (reads through the VFS like every other asset load).
std::optional<AudioEvent> LoadAudioEvent(const std::filesystem::path& path);

} // namespace assets
} // namespace hbe
