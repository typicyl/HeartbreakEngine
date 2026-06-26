// Scene/AnimationSystem.h - keyframe tracks + skeletal animation.
//
// Two layers: AnimationTrack samples TRS keys into entity Transforms (simple
// object animation), and Animator drives SKELETAL animation - it samples a
// clip's joint-name channels onto the entity's skeleton (retargeting across
// skeletons happens here too) and produces the skinning palette the renderer
// uploads to the GPU.
#pragma once

#include "Core/Types.h"

#include <filesystem>
#include <memory>
#include <string>

namespace hbe {

class Scene;
struct AnimationTrack;
struct Transform;
struct Rig;

namespace anim {

// Evaluates the track at `track.time` and writes the pose into `out`.
// No-op when the track has no keys.
void Sample(const AnimationTrack& track, Transform& out);

// Advances every playing track by `dt` (looping or stopping at the end) and
// applies the sampled pose to the entity's Transform.
void Update(Scene& scene, f32 dt);

// -- Skeletal ------------------------------------------------------------------
// Cached rig loads (pack-aware through the UAF/VFS path). `relUaf` is the
// mesh asset path relative to Assets/. Returns nullptr when the asset has no
// rig. Clear the cache when assets change on disk or a project switches.
std::shared_ptr<const Rig> LoadRigCached(const std::filesystem::path& assetsDir,
                                         const std::string& relUaf);
void ClearRigCache();

// Advances every Animator and rebuilds its joint palette: target skeleton
// from the entity's mesh asset, clips from the Animator's source asset (or
// the mesh's own), channels matched to joints BY NAME (real-time retarget).
void UpdateSkeletal(Scene& scene, const std::filesystem::path& assetsDir, f32 dt);

// -- Motion matching -----------------------------------------------------------
// Data-driven locomotion: builds a feature database from a MotionMatching
// entity's clips (root speed / turn rate sampled over time) and, each search
// interval, retargets the entity's Animator to the clip+time best matching the
// desired velocity. Run BEFORE UpdateSkeletal (it sets the Animator's clip/time
// which UpdateSkeletal then poses). The database is cached per source rig.
void UpdateMotionMatching(Scene& scene, const std::filesystem::path& assetsDir, f32 dt);

// -- Rotators ------------------------------------------------------------------
// Applies each Rotator's constant angular velocity to its entity's Transform.
// Call while the simulation runs (like physics/scripts), not in the editor at
// rest, so it doesn't drift authored transforms.
void UpdateRotators(Scene& scene, f32 dt);

} // namespace anim
} // namespace hbe
