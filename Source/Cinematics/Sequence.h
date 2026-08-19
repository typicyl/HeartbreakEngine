// Cinematics/Sequence.h - the hierarchical cinematic sequence data model.
//
// This is the pure, value-semantic asset model for the Heartbreak Cinematic
// System (the ".hbseq" Sequencer). It holds NO runtime pointers and NO engine
// handles - actors are referenced by stable EntityGuid (with a Name fallback),
// assets by path, so a sequence serializes cleanly and stays reusable across
// compatible scenes (spec §1, §12, §20).
//
// Shape:
//   Sequence            an .hbseq asset (a Master when it owns Shots, else a leaf)
//   ├─ Binding[]        stable actor references (possessable or spawnable)
//   ├─ Track[]          extensible lanes; the `kind` string selects a registered
//   │  ├─ Section[]     evaluator (Cinematics/TrackRegistry.h). Tracks nest
//   │  └─ Track[]       (groups / animation layers).
//   ├─ Marker[]         named timeline points (§15)
//   └─ Shot[]           a master's shot list (§14); each references a sub-Sequence
//
// A Section (clip) either drives properties via keyframe Channels (reusing the
// Core/Curve engine), references an asset (clip/audio/dialogue/sub-sequence),
// fires an Event, or cuts to a camera. Track kinds interpret sections; the core
// evaluator (Cinematics/Evaluator.h) never hard-codes a track type.
#pragma once

#include "Core/Types.h"
#include "Core/Curve.h"   // curve::Curve (keyframe channels)
#include "Core/Easing.h"  // ease::Curve (section blend + quaternion key easing)

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <utility>
#include <vector>

namespace hbe::cine {

// A named scalar channel: an animatable property path plus its curve. The path is
// interpreted by the owning track's kind, e.g. a Transform track reads
// "location.x/y/z" and "scale.x/y/z"; a Light track reads "intensity","color.r";
// a generic Property track reads whatever path it was pointed at (§2, §11).
struct Channel {
    std::string target;
    curve::Curve curve;
};

// Gimbal-free rotation keyframe (slerped), for Transform / Camera sections that
// need real quaternion rotation instead of euler channels.
struct QuatKey {
    f32 time = 0.0f;
    glm::quat value{1.0f, 0.0f, 0.0f, 0.0f};
    ease::Curve ease = ease::Curve::InOutCubic; // curve INTO this key
};

// What a section does. Serialized as an int; append-only.
enum class SectionKind : u8 {
    Keyframe = 0, // `channels` (+ optional `rotationKeys`) drive bound properties
    Asset,        // `assetRef` names a clip / audio / dialogue to play over the range
    Event,        // fires when the playhead crosses `start` (one-shot); `assetRef`/params = payload
    CameraCut,    // makes `bindingRef`'s camera the active render camera for the range
    SubSequence,  // `assetRef` is a nested .hbseq evaluated within this range
};

// One clip on a track's local timeline.
struct Section {
    u64 id = 0;             // stable local id (editor selection / undo)
    f32 start = 0.0f;       // start on the track's local timeline (seconds)
    f32 duration = 1.0f;
    f32 timeScale = 1.0f;   // content playback rate within the clip (retime, §5)
    f32 innerStart = 0.0f;  // offset into the clip / sub-sequence content
    bool loop = false;

    // Section weight ramp: crossfade in/out with neighbouring sections (or with the
    // pre-existing state). Drives camera/transform/property blending (§4, §5).
    f32 blendIn = 0.0f;     // seconds to ramp weight 0->1 at the start
    f32 blendOut = 0.0f;    // seconds to ramp weight 1->0 at the end
    ease::Curve blendEase = ease::Curve::InOutCubic;
    int row = 0;            // lane within the track (for overlapping sections)

    SectionKind kind = SectionKind::Keyframe;
    std::vector<Channel> channels;      // Keyframe
    std::vector<QuatKey> rotationKeys;  // optional gimbal-free rotation
    std::string assetRef;               // Asset/SubSequence path, or Event id
    int bindingRef = -1;                // CameraCut: which Binding is the camera

    // Free-form typed params: event arguments, per-section knobs. String-keyed and
    // sparse so any track kind carries data without a schema/format change.
    std::vector<std::pair<std::string, f32>> floatParams;
    std::vector<std::pair<std::string, std::string>> stringParams;

    // Absolute end on the track's local timeline.
    f32 End() const { return start + duration; }
    // Maps an absolute track-local time into the clip's own content time.
    f32 ContentTime(f32 localT) const { return innerStart + (localT - start) * timeScale; }
    // Section weight at a track-local time (0 outside, blended at the edges).
    f32 Weight(f32 localT) const;
};

// A lane. `kind` is a registry key ("camera","cameraCut","transform","animation",
// "facial","lookAt","ik","visibility","material","light","environment","vfx",
// "particle","audio","dialogue","music","subtitle","event","signal","gameplay",
// "post","property.float","property.color","spawn","layer",...). Unknown kinds are
// skipped with a warning, never a crash, so a file authored against a plugin track
// still loads (§2, §20).
struct Track {
    u64 id = 0;
    std::string kind;
    std::string name;   // display label
    int binding = -1;   // Binding this track targets (-1 = global / none)
    bool mute = false;
    bool solo = false;
    bool locked = false;

    std::vector<Section> sections;
    std::vector<Track> children; // track groups / animation layers (§2)

    std::vector<std::pair<std::string, f32>> floatParams;
    std::vector<std::pair<std::string, std::string>> stringParams;

    // Convenience param access (linear scan; tracks hold a handful of params).
    f32 FloatParam(const std::string& key, f32 fallback) const;
    const std::string& StringParam(const std::string& key, const std::string& fallback) const;
};

// How a Binding resolves to a live entity.
enum class BindingKind : u8 {
    Possessable = 0, // an EXISTING scene actor (resolved by guid, then name)
    Spawnable,       // the sequence SPAWNS it for its lifetime (§12)
};

// A stable actor reference. Tracks/sections reference a binding by `id`; the
// resolver maps it to an entt::entity at evaluation time and caches the result.
struct Binding {
    int id = 0;
    std::string label; // display
    BindingKind kind = BindingKind::Possessable;
    u64 guid = 0;      // primary stable reference (Scene EntityGuid); 0 = use name
    std::string name;  // fallback / search key (entity Name)
    std::string spawnAsset; // Spawnable: mesh(.uaf)/prefab(.hbprefab)/character(.hbchar)
};

// A named timeline point (§15). Categories/colours are metadata for the editor's
// marker list + navigation; a marker fires no events by itself.
struct Marker {
    f32 time = 0.0f;
    std::string name;
    std::string category; // e.g. "MUSIC_CUE","ACTION","DIALOGUE_START"
    std::string note;
    u32 color = 0xFFFFFFFFu;
    bool jump = true;     // shown in the jump-to list
};

// A shot in a Master sequence's shot list (§14). A shot is a placed reference to a
// sub-Sequence with first-class production metadata (id / notes / take). Shots may
// be rearranged on the master timeline without touching the referenced sequence's
// internal animation.
struct Shot {
    u64 id = 0;
    std::string shotId;   // production id, e.g. "SHOT_0010"
    std::string sequence; // sub-sequence .hbseq path (relative to Assets/)
    f32 start = 0.0f;     // placement on the master timeline (seconds)
    f32 duration = 5.0f;
    f32 innerStart = 0.0f;
    f32 timeScale = 1.0f;
    std::string note;
    std::string take;     // take label / version
    bool enabled = true;

    f32 End() const { return start + duration; }
};

// The asset root.
struct Sequence {
    std::string name;
    u64 guid = 0;
    f64 frameRate = 30.0;  // authoring frame rate (frame snapping / offline render)
    f32 playStart = 0.0f;  // playback range start (seconds)
    f32 duration = 10.0f;  // playback range end (seconds)

    std::vector<Binding> bindings;
    std::vector<Track> tracks;
    std::vector<Marker> markers;
    std::vector<Shot> shots; // non-empty => this is a Master sequence

    // Cinematic mode: while this sequence plays, freeze player movement + AI + player
    // fire (the two gameplay bands), restoring them on end (spec §17). Off = an
    // "environmental" or interactive sequence that plays over live gameplay.
    bool suppressGameplay = true;

    int version = 1;

    bool IsMaster() const { return !shots.empty(); }
    // Total timeline length: the playback range end, extended to cover any shot.
    f32 Length() const;
    const Binding* FindBinding(int id) const;
    Binding* FindBinding(int id);
};

// True if the sequence drives the render camera anywhere (a camera or cameraCut
// track, at any nesting level of its own tracks). Used to decide camera takeover.
bool HasCameraTrack(const Sequence& seq);

} // namespace hbe::cine
