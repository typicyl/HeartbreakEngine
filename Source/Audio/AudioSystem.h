// Audio/AudioSystem.h - audio playback + mixing (miniaudio) behind a plain API.
//
// FMOD/Wwise-style middleware layer:
//   * a MIXER BUS tree (Master -> Music/SFX/Ambience by default, user buses
//     from the project settings) with per-bus volume and mute,
//   * AUDIO EVENTS (.hbevent assets): weighted random sound pools with
//     volume/pitch randomization, posted by name from gameplay or the editor,
//   * fire-and-forget voices and persistent spatial AudioSource voices, all
//     routed through their bus.
// miniaudio stays an implementation detail (pimpl); engine code never
// includes it.
#pragma once

#include "Core/Types.h"

#include <glm/glm.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace hbe {

class Scene;
struct AudioEvent;

// One mixer bus. Parentage forms the routing tree under the implicit "Master"
// root (an empty/unknown parent attaches to Master).
struct AudioBusDesc {
    std::string name;
    std::string parent = "Master";
    f32 volume = 1.0f;
    bool muted = false;
};

class AudioSystem : public NonCopyable {
public:
    AudioSystem();
    ~AudioSystem();

    // -- Mixer -----------------------------------------------------------------
    // (Re)builds the bus tree ("Master" is implicit and always exists). Stops
    // all playing voices first: call at startup / project switches, not mid-game.
    void ConfigureBuses(const std::vector<AudioBusDesc>& buses);
    std::vector<std::string> BusNames() const; // Master first, then tree order
    void SetBusVolume(const std::string& bus, f32 volume);
    f32  BusVolume(const std::string& bus) const;
    void SetBusMuted(const std::string& bus, bool muted);
    bool BusMuted(const std::string& bus) const;

    // Active playback device description for boot/diagnostics UI (device name +
    // sample rate, e.g. "Speakers (Realtek) - 48000 Hz"). "No audio device" when
    // playback is unavailable.
    std::string DeviceName() const;

    // -- Events ------------------------------------------------------------------
    // Posts an audio event: picks a weighted-random sound from the pool,
    // applies volume/pitch randomization, and routes it to the event's bus.
    // Spatial events play at `position` (when given). Returns a voice id for
    // StopEvent (0 = nothing played).
    u32 PostEvent(const AudioEvent& ev, const std::filesystem::path& assetsDir,
                  const glm::vec3* position = nullptr);
    void StopEvent(u32 voiceId);
    void StopAllVoices(); // every fire-and-forget/event voice (spatial stay)

    // -- Direct playback ----------------------------------------------------------
    // Plays interleaved PCM (8-bit unsigned, 16-bit signed, or 32-bit float).
    // The data is copied; returns false if the device/format is unavailable.
    bool PlayPCM(const void* pcm, usize bytes, u32 channels, u32 sampleRate,
                 u32 bitsPerSample, const std::string& bus = {});

    // Convenience: loads a .uaf Audio asset and plays it.
    bool PlayUAF(const std::filesystem::path& uafPath, const std::string& bus = {});

    // Reaps finished voices; call once per frame.
    void Update();

    // 3D audio: moves the listener to the camera and drives every AudioSource
    // entity (creates positional voices from .uaf assets under `assetsDir`,
    // follows world transforms, honors playing/loop/volume/attenuation/bus).
    // `gamePlaying` is the simulation state: autoplay sources arm only when the
    // GAME runs (play mode / runtime), so merely viewing a scene in the editor
    // stays silent. The inspector's manual Play button still previews anytime.
    void UpdateScene(Scene& scene, const std::filesystem::path& assetsDir,
                     const glm::vec3& listenerPos, const glm::vec3& listenerForward,
                     bool gamePlaying);

    bool IsAvailable() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool prevScenePlaying_ = false; // edge-detect the game start for autoplay
};

// The default bus layout used when a project defines none.
std::vector<AudioBusDesc> DefaultAudioBuses();

} // namespace hbe
