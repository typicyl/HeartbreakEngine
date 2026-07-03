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

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace hbe {

// Camera keyframe: eye position, world-space aim point, vertical FOV. `cut`
// jumps straight to this key (a hard cut) instead of interpolating into it.
struct CutsceneCameraKey {
    f32 time = 0.0f;
    glm::vec3 position{0.0f, 2.0f, 5.0f};
    glm::vec3 aim{0.0f};      // world point the camera looks at
    f32 fov = 60.0f;
    bool cut = false;
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
};

namespace assets {

inline constexpr const char* kCutsceneExtension = ".hbcutscene";

bool SaveCutscene(const std::filesystem::path& path, const CutsceneAsset& c);
std::optional<CutsceneAsset> LoadCutscene(const std::filesystem::path& path);

} // namespace assets
} // namespace hbe
