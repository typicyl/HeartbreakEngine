// Assets/CutsceneAsset.h - .hbcutscene cinematic timeline assets.
//
// A cutscene is a JSON file under Assets/: a fixed-duration timeline with a
// camera track (keyframed position/aim/fov + hard cuts), any number of
// per-entity animation tracks (transform keyframes + skeletal-clip triggers,
// entities addressed by Name), and dialogue markers (fire a .hbdialogue or a
// voiceline at a time). A schematic Play Cutscene node runs it: the engine
// takes over the camera, evaluates the tracks each frame, and restores the
// game camera when it ends. Entity references are by Name (stable across load).
#pragma once

#include "Core/Types.h"
#include "Scene/CameraRig.h" // cam::CinematicSettings (rig layered over the keys)

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace hbe {

// How a segment interpolates INTO a key. Linear reads mechanical on a camera
// move; the ease variants are what make a push-in or a whip feel authored.
// Values are serialized, so this list is append-only.
enum class CutsceneEase : u8 {
    Linear = 0,
    EaseIn,     // slow start, arrives at speed
    EaseOut,    // arrives gently (the usual choice for a settle)
    EaseInOut,  // smoothstep: the default cinematic move
    Hold,       // no interpolation - the previous key holds until this one
};

const char* EaseName(CutsceneEase e);
// Remaps a normalized 0..1 segment parameter by the curve.
f32 ApplyEase(CutsceneEase e, f32 u);

// Camera keyframe: eye position, aim, vertical FOV, roll and focus. `cut` jumps
// straight to this key (a hard cut) instead of interpolating into it.
struct CutsceneCameraKey {
    f32 time = 0.0f;
    glm::vec3 position{0.0f, 2.0f, 5.0f};
    glm::vec3 aim{0.0f};      // world point the camera looks at
    f32 fov = 60.0f;
    bool cut = false;
    // Curve used to reach THIS key from the previous one.
    CutsceneEase ease = CutsceneEase::EaseInOut;
    // Dutch angle / horizon tilt, degrees. Roll cannot be expressed by an aim
    // point alone, so it is its own channel.
    f32 roll = 0.0f;
    // Depth of field for this key. focusDistance <= 0 means "auto": focus on the
    // aim point, which is what a focus puller does by default. The player writes
    // these into PostSettings so a cutscene can rack focus.
    f32 focusDistance = -1.0f;
    f32 focusRange = 3.0f;    // sharp band around the focus plane (world units)
    f32 aperture = 0.0f;      // 0 = leave the scene's DoF blur alone; >0 = max blur
    // When set, the camera AIMS AT THIS ENTITY (by Name) instead of the static
    // `aim` point, so a shot tracks a moving actor without keyframing every frame.
    // Empty = use `aim`.
    std::string aimTarget;
};

// One camera-shake impulse. Fired when the playhead crosses `time`, then decays -
// the same trauma model the live camera rig uses, so an explosion in a cutscene
// and one in gameplay shake identically.
struct CutsceneShakeMarker {
    f32 time = 0.0f;
    f32 trauma = 0.5f; // 0..1
};

// A subtitle shown for a fixed window, independent of any audio. Cutscenes often
// need narration or a translated sign with no voice clip behind it.
struct CutsceneSubtitleMarker {
    f32 time = 0.0f;
    f32 duration = 0.0f; // 0 = auto from text length
    std::string speaker; // "" = no speaker prefix
    std::string text;
};

// Transform keyframe for an animation track's target entity.
struct CutsceneTransformKey {
    f32 time = 0.0f;
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
};

// Skeletal-clip trigger: when the playhead passes `time`, set the target's
// Animator to clip index `clip` (restarts it).
struct CutsceneClipMarker {
    f32 time = 0.0f;
    int clip = 0;
};

// One animation track, bound to a scene entity by Name. Either/both of the
// transform keyframes (moves the entity) and clip markers (drives its skeleton)
// may be used.
struct CutsceneAnimTrack {
    std::string target;                        // entity Name
    std::vector<CutsceneTransformKey> keys;    // transform keyframes (optional)
    std::vector<CutsceneClipMarker> clips;     // skeletal-clip triggers (optional)
};

// Dialogue marker: at `time`, play a .hbdialogue (`dialogue`) or a single
// voiceline `.uaf` (`voiceline`). Whichever is set fires (dialogue wins).
struct CutsceneDialogueMarker {
    f32 time = 0.0f;
    std::string dialogue;   // `.hbdialogue` (rel. Assets)
    std::string voiceline;  // `.uaf` Voiceline (rel. Assets)
};

struct CutsceneAsset {
    f32 duration = 5.0f;                          // total length in seconds
    std::vector<CutsceneCameraKey> camera;        // empty = leave the camera alone
    std::vector<CutsceneAnimTrack> animTracks;
    std::vector<CutsceneDialogueMarker> dialogue;
    std::vector<CutsceneShakeMarker> shakes;
    std::vector<CutsceneSubtitleMarker> subtitles;
    // Cinematic rig applied on top of the keyframed camera: handheld/breathing
    // make a keyframed move feel operated rather than motion-controlled. Off by
    // default, so existing cutscenes play back exactly as before.
    cam::CinematicSettings cinematic;
};

namespace assets {

inline constexpr const char* kCutsceneExtension = ".hbcutscene";

bool SaveCutscene(const std::filesystem::path& path, const CutsceneAsset& c);
std::optional<CutsceneAsset> LoadCutscene(const std::filesystem::path& path);

} // namespace assets
} // namespace hbe
