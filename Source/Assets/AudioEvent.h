// Assets/AudioEvent.h - .hbevent audio event assets (FMOD/Wwise-style).
//
// An audio event is a small JSON file under the project's Assets/ directory
// describing *how* something should sound, decoupled from gameplay code:
// a weighted random pool of `.uaf` audio assets, the mixer bus it routes to,
// volume/pitch randomization, and 3D attenuation settings. Gameplay (or the
// editor's Play button) posts events through AudioSystem::PostEvent.
#pragma once

#include "Core/Types.h"

#include <glm/glm.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace hbe {

struct AudioEventSound {
    std::string asset;  // `.uaf` Audio path relative to Assets/
    f32 weight = 1.0f;  // random-pick weight within the event
};

// One COMPONENT of a composite event: a distinct spatial source spawned alongside the others when
// the event fires, with its OWN sound pool, spatial offset, start delay, attenuation, bus routing
// and per-source DSP. This is what lets e.g. Rifle.Fire be {Mechanical, Muzzle Blast, Shell Casing,
// Near Reflection, Distant Tail} as five independent 3D sources rather than one clip.
struct AudioEventComponent {
    std::string name = "Component";       // label (e.g. "Muzzle Blast")
    std::vector<AudioEventSound> sounds;  // weighted random pool for this component
    glm::vec3 offset{0.0f};               // spatial offset from the event origin (world space)
    f32 delaySeconds = 0.0f;              // start delay after the event fires (e.g. shell casing)
    f32 volume = 1.0f;
    f32 volumeVariance = 0.0f;
    f32 pitch = 1.0f;
    f32 pitchVariance = 0.0f;
    std::string bus;                      // routing bus ("" = inherit the event's bus)
    bool loop = false;
    bool spatial = true;                  // components are 3D by default
    f32 minDistance = 1.0f;
    f32 maxDistance = 30.0f;
    f32 reverbSend = 1.0f;                // per-source room-effects gain (distant tail = wetter)
    f32 spread = 0.0f;                    // source spread in degrees (0 = point, wider = diffuse)
    bool enabled = true;
};

struct AudioEvent {
    std::string bus = "SFX";              // mixer bus (empty/unknown = Master)
    std::vector<AudioEventSound> sounds;  // weighted random container (flat/legacy event)

    f32 volume = 1.0f;          // base gain
    f32 volumeVariance = 0.0f;  // +- random gain per post
    f32 pitch = 1.0f;           // base playback rate
    f32 pitchVariance = 0.0f;   // +- random rate per post
    bool loop = false;

    // 3D: posts with a position attenuate between min and max distance.
    bool spatial = false;
    f32 minDistance = 1.0f;
    f32 maxDistance = 30.0f;

    // COMPOSITE events: when non-empty, PostEvent spawns one voice per component (each at its own
    // offset/attenuation/bus/DSP) instead of the flat weighted pool above. Empty = legacy behavior.
    std::vector<AudioEventComponent> components;
};

namespace assets {

inline constexpr const char* kAudioEventExtension = ".hbevent";

bool SaveAudioEvent(const std::filesystem::path& path, const AudioEvent& ev);
// Pack-aware (reads through the VFS like every other asset load).
std::optional<AudioEvent> LoadAudioEvent(const std::filesystem::path& path);

// Headless self-test (part of --test-acoustics): composite-event serialization round-trip.
bool AudioEventSelfTest();

} // namespace assets
} // namespace hbe
