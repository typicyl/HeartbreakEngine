// Audio/SpatializerResonance.h - Heartbreak-side binaural spatializer backed by the
// HDS Resonance Audio fork (github.com/hollowdstudios/HDS-resonance-audio, vendored at
// <repo>/HDSResonance-Audio).
//
// WHAT IT IS. Resonance is a spatial-audio RENDERER, not an audio engine: it takes mono
// source buffers plus a listener pose and returns one binaural (HRTF) stereo mix, with an
// optional shoebox room model. This class wraps it as ONE node inside the AudioSystem's
// existing miniaudio graph - N mono spatial voices in, one HRTF stereo bus out - so
// miniaudio keeps owning decoding, the device, the bus tree and music, while Resonance
// replaces miniaudio's amplitude-panning for 3D voices. All the Resonance/vraudio and
// miniaudio detail stays in the .cpp (pimpl); this header pulls in neither.
//
// THE HARD PART is that a miniaudio node's process callback runs with a VARIABLE frame
// count on the audio thread, while Resonance renders in FIXED-size blocks. The node
// re-blocks internally with a ring buffer. See the .cpp.
//
// This is Heartbreak-specific glue and lives entirely on the engine side. The fork knows
// nothing about it. When the fork is not present the whole class compiles to no-ops
// (HBE_HAVE_RESONANCE=0) so the engine still builds and runs on plain miniaudio.
#pragma once

#include "Audio/AcousticRoom.h"
#include "Core/Types.h"

#include <glm/glm.hpp>

#include <memory>

namespace hbe {

class ResonanceSpatializer : public NonCopyable {
public:
    ResonanceSpatializer();
    ~ResonanceSpatializer();

    // Creates the Resonance renderer and the miniaudio node, attaching the node's stereo
    // output to `destNode` (typically the Master group, so master volume still applies).
    // `maEngine` is the ma_engine* and `destNode` a ma_node* (opaque here). `sampleRate` is
    // the device rate. Returns false when the backend is unavailable or init fails; the
    // caller then keeps miniaudio's built-in spatialization.
    bool Init(void* maEngine, void* destNode, u32 sampleRate);
    void Shutdown();
    bool IsReady() const;

    // The ma_node* (opaque) that a spatial voice attaches its output to, on the input bus
    // index returned by AcquireSource. Null until Init succeeds.
    void* InputNode() const;

    // Claims a spatial source slot. The returned value is BOTH the miniaudio input-bus index
    // the voice attaches to AND the id passed to SetSource/ReleaseSource. Returns -1 when the
    // pool is exhausted (the caller leaves that voice on miniaudio panning).
    int AcquireSource();
    void ReleaseSource(int slot);

    // Per-frame parameter updates. Safe to call from the main thread while the node renders
    // on the audio thread (the Resonance API is built for exactly this split).
    void SetListener(const glm::vec3& pos, const glm::vec3& forward, const glm::vec3& up);
    // `occlusion01` is Heartbreak's own multi-ray occlusion result in [0,1]; it is fed to
    // Resonance's per-source occlusion intensity so the bespoke geometry probing is kept.
    // `minDist`/`maxDist` forward the engine's authored distance range so Resonance's
    // distance attenuation matches the source's falloff instead of its own built-in defaults.
    void SetSource(int slot, const glm::vec3& pos, f32 volume, f32 occlusion01,
                   f32 minDist, f32 maxDist);

    // Loudspeaker mode: HRTF is a headphone technique, so over stereo speakers Resonance
    // switches to amplitude panning (which also avoids HRTF coloring and is cheaper). Maps
    // straight to vraudio SetStereoSpeakerMode.
    void SetSpeakerMode(bool speakers);

    // Applies (or disables when `enabled` is false) shoebox room acoustics - early reflections +
    // late reverb heard by all spatialized sources. `room` is engine POD; it is translated to the
    // backend's ReflectionProperties/ReverbProperties internally (no vraudio type in this header).
    void SetRoom(const AcousticRoom& room, bool enabled);

    // Per-source static parameters, set once at voice creation: `reverbSend` scales how much this
    // source feeds the room reverb (a distant tail is wetter than a close mechanical click);
    // `spreadDeg` widens the source from a point (0) toward diffuse. Maps to vraudio
    // SetSourceRoomEffectsGain + SetSoundObjectSpread.
    void SetSourceParams(int slot, f32 reverbSend, f32 spreadDeg);

    // Max number of simultaneous spatial voices the pool supports.
    static int Capacity();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hbe
