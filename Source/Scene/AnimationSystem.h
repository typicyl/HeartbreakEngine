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
// Same, at an EXPLICIT time. The serializer uses it to write the pose an
// auto-playing track will hold on the frame after the file loads (t = 0) rather
// than wherever the editor's preview playhead happened to be - see EntityToJson.
void SampleAt(const AnimationTrack& track, f32 t, Transform& out);

// Advances every playing track by `dt` (looping or stopping at the end) and
// applies the sampled pose to the entity's Transform.
//
// `simulating` = Play mode / the shipped runtime. It exists because BOTH of these
// run in the EDITOR at rest (the Timeline panel's transport is a live preview), and
// `AnimationTrack::playing` / `Animator::playing` are AUTHORED, SERIALIZED fields.
// When a non-looping clip ran out in edit mode the systems wrote `playing = false`
// onto the component, and the next save wrote that over the author's "plays on
// load" - a runtime value overwriting an authored one, exactly the UIAnimator bake.
// With `simulating` false the playhead still advances and still poses (so the
// preview is unchanged on screen) but it HOLDS at the end instead of stopping, so
// no serialized field is touched. Only an explicit Pause/Stop - authoring - clears
// `playing`.
void Update(Scene& scene, f32 dt, bool simulating);

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
// `simulating`: see anim::Update - a non-looping clip reaching its end holds
// instead of clearing the authored `Animator::playing` when the editor is at rest.
void UpdateSkeletal(Scene& scene, const std::filesystem::path& assetsDir, f32 dt,
                    bool simulating);

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
