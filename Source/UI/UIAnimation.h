// UI/UIAnimation.h - reusable keyframe animation clips for UI elements.
//
// A clip (`.hbuianim`) is a set of scalar keyframe tracks that drive a UIElement's
// animatable properties over time (offset/scale/rotation/color/opacity/sprite-frame).
// Clips are authored once and referenced by a UIAnimator component (Components.h),
// which ui::UpdateAnimations advances each frame per its trigger. Deliberately NOT the
// entity Animator (that one is hardwired to Transform); UI needs its own light model.
#pragma once

#include "Core/Easing.h"
#include "Core/Types.h"

#include <glm/glm.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace hbe {
class Scene;
struct UIElement;
} // namespace hbe

namespace hbe::ui {

// One animatable scalar channel. Vec/quat props are split into scalar tracks so
// sampling is uniform (each track is a sorted list of {time, value, curve}).
enum class AnimTarget : u8 {
    OffsetX = 0,
    OffsetY,
    ScaleX,
    ScaleY,
    Rotation,
    ColorR,
    ColorG,
    ColorB,
    Opacity,      // color alpha
    SpriteFrame,  // stepped index into UIElement::frames
    Count
};

struct AnimKey {
    f32 time = 0.0f;                        // seconds
    f32 value = 0.0f;
    ease::Curve curve = ease::Curve::Linear; // easing INTO this key from the previous
};

struct AnimTrack {
    AnimTarget target = AnimTarget::Opacity;
    std::vector<AnimKey> keys; // sorted by time (LoadClip sorts)
};

struct UIClip {
    f32 duration = 1.0f;
    bool loop = false;
    std::vector<AnimTrack> tracks;
};

// Load/save a `.hbuianim` (JSON). LoadClip sorts each track's keys by time.
bool LoadClip(const std::filesystem::path& path, UIClip& out);
bool SaveClip(const std::filesystem::path& path, const UIClip& clip);

// Drops the loaded-clip cache (call after re-authoring a clip or switching projects
// so the next play picks up the new data).
void ClearClipCache();

// Sample one track at time t (linear interp + the destination key's easing curve).
f32 SampleTrack(const AnimTrack& track, f32 t);

// Apply the clip at time t to `el`. Offset tracks are ADDITIVE to `baseOffset` (a 0
// key = the element's authored position); scale/rotation/color/opacity/spriteFrame are
// absolute overrides for the tracks present (untouched properties keep their authored
// values). Only tracks that exist in the clip are written.
void Apply(const UIClip& clip, f32 t, UIElement& el, const glm::vec2& baseOffset);

// Per-frame driver: advance every UIAnimator, fire triggers, sample + apply its clip.
// Call once a frame BEFORE ui::BuildVertices. `assetsDir` resolves clip paths.
void UpdateAnimations(Scene& scene, f32 dt, const std::filesystem::path& assetsDir);

} // namespace hbe::ui
