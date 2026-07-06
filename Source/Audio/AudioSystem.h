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
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hbe {

class Scene;
struct AudioEvent;
struct MusicGraph;

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

    // Closed captions: when enabled, starting playback of an audio asset that carries
    // a caption enqueues its text; the engine drains it (PopCaption) to an on-screen
    // caption element. No-op / nothing enqueued when disabled.
    void SetCaptionsEnabled(bool on);
    bool PopCaption(std::string& out); // FIFO; returns false when the queue is empty

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

    // Convenience: loads a .uaf Audio asset and plays it. `caption` surfaces the
    // clip's baked "Speaker: caption" into the caption queue (when captions are
    // enabled) - pass false when the caller shows its own text (dialogue lines).
    bool PlayUAF(const std::filesystem::path& uafPath, const std::string& bus = {},
                 bool caption = true);
    // Like PlayUAF but plays the clip as a 3D one-shot at a WORLD position (distance
    // attenuation + occlusion). Used for dialogue ACTORS so a voice line emits from
    // the speaking character instead of flat 2D. Non-looping.
    bool PlayUAFAt(const std::filesystem::path& uafPath, const glm::vec3& position,
                   const std::string& bus = {}, f32 minDist = 1.0f, f32 maxDist = 35.0f,
                   bool caption = true);
    // Drops PlayUAF's decoded-PCM cache (call after a .uaf is re-imported or its
    // tags edited, so the next play reflects the new PCM / caption / speaker).
    void ClearUAFCache();

    // -- Spatial occlusion -------------------------------------------------------
    // True positional occlusion for spatial voices: geometry between a source and
    // the listener both ATTENUATES and MUFFLES (per-voice low-pass) the sound, and
    // multiple rays let it leak through gaps/openings instead of dead-stopping at a
    // wall. Off by default; the engine enables it per project + feeds a world
    // segment-blocked test (physics raycast) into UpdateScene.
    struct OcclusionConfig {
        bool enabled = false;
        int rays = 4;               // 1 = straight line; more = gap leakage
        f32 attenuation = 0.35f;    // volume floor at full occlusion (0..1)
        f32 cutoffHz = 700.0f;      // low-pass cutoff at full occlusion
        f32 spread = 0.7f;          // offset-ray ring radius in metres
    };
    void SetOcclusion(const OcclusionConfig& cfg);

    // -- Adaptive music director -------------------------------------------------
    // Interactive music: a graph of STATES (each a set of synced looping LAYERS)
    // that crossfade, with runtime PARAMETERS that fade layers in/out for a smooth,
    // gameplay-reactive score. See MusicGraph (.hbmusic).
    //
    // Installs the graph (copies it) and resets parameters to their defaults. Stops
    // any music currently playing. `assetsDir` resolves the layers' `.uaf` stems.
    void SetMusicGraph(const MusicGraph& graph, const std::filesystem::path& assetsDir);
    // Crossfades to `state` (its layers loop in sync). `fadeSeconds` < 0 uses the
    // graph's default fade. An unknown state is ignored (a warning is logged).
    void PlayMusicState(const std::string& state, f32 fadeSeconds = -1.0f);
    void StopMusic(f32 fadeSeconds = -1.0f);
    // Sets a runtime parameter (e.g. "intensity"): layers bound to it fade between
    // their paramLo/paramHi range. Clamped to the parameter's declared min/max.
    void SetMusicParameter(const std::string& name, f32 value);
    f32  MusicParameterValue(const std::string& name) const;
    std::string CurrentMusicState() const; // "" when stopped
    std::vector<std::string> MusicStateNames() const; // states in the installed graph
    bool HasMusicGraph() const;
    // One-shot musical accent over the current music (fire-and-forget on `bus`).
    void PostStinger(const std::filesystem::path& uafPath, const std::string& bus = "Music",
                     f32 volume = 1.0f);
    // Advances crossfades + applies parameter-driven layer gains; call once a frame.
    void UpdateMusic(f32 dt);

    // Reaps finished voices; call once per frame.
    void Update();

    // 3D audio: moves the listener to the camera and drives every AudioSource
    // entity (creates positional voices from .uaf assets under `assetsDir`,
    // follows world transforms, honors playing/loop/volume/attenuation/bus).
    // `gamePlaying` is the simulation state: autoplay sources arm only when the
    // GAME runs (play mode / runtime), so merely viewing a scene in the editor
    // stays silent. The inspector's manual Play button still previews anytime.
    // `segmentBlocked(a, b)` returns true when world geometry blocks the segment a->b
    // (the engine wraps a physics raycast). When set and occlusion is enabled, every
    // spatial voice is attenuated/muffled by obstruction each frame. `dt` drives the
    // smooth glide of the occlusion amount (0 = snap).
    void UpdateScene(Scene& scene, const std::filesystem::path& assetsDir,
                     const glm::vec3& listenerPos, const glm::vec3& listenerForward,
                     bool gamePlaying,
                     const std::function<bool(const glm::vec3&, const glm::vec3&)>& segmentBlocked = {},
                     f32 dt = 0.0f);

    bool IsAvailable() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool prevScenePlaying_ = false; // edge-detect the game start for autoplay
};

// The default bus layout used when a project defines none.
std::vector<AudioBusDesc> DefaultAudioBuses();

} // namespace hbe
