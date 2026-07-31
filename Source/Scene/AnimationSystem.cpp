// Scene/AnimationSystem.cpp
#include "Scene/AnimationSystem.h"

#include "Assets/Animation.h"
#include "Assets/UAF.h"
#include "Core/JobSystem.h"
#include "Core/Log.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace hbe::anim {

void Sample(const AnimationTrack& track, Transform& out) { SampleAt(track, track.time, out); }

void SampleAt(const AnimationTrack& track, f32 t, Transform& out) {
    if (track.keys.empty()) return;

    const auto& keys = track.keys;
    if (t <= keys.front().time) {
        out.position = keys.front().position;
        out.rotation = keys.front().rotation;
        out.scale = keys.front().scale;
        return;
    }
    if (t >= keys.back().time) {
        out.position = keys.back().position;
        out.rotation = keys.back().rotation;
        out.scale = keys.back().scale;
        return;
    }

    usize next = 1;
    while (next < keys.size() && keys[next].time < t) ++next;
    const AnimationTrack::Key& a = keys[next - 1];
    const AnimationTrack::Key& b = keys[next];
    const f32 span = glm::max(b.time - a.time, 1e-6f);
    const f32 f = glm::clamp((t - a.time) / span, 0.0f, 1.0f);

    out.position = glm::mix(a.position, b.position, f);
    out.rotation = glm::normalize(glm::slerp(a.rotation, b.rotation, f));
    out.scale = glm::mix(a.scale, b.scale, f);
}

void Update(Scene& scene, f32 dt, bool simulating) {
    auto& reg = scene.Registry();
    for (const entt::entity e : reg.view<Transform, AnimationTrack>()) {
        AnimationTrack& track = reg.get<AnimationTrack>(e);
        if (!track.playing || track.duration <= 0.0f) continue;

        track.time += dt * track.speed;
        if (track.time > track.duration) {
            if (track.loop) {
                track.time = std::fmod(track.time, track.duration);
            } else {
                track.time = track.duration;
                // `playing` is AUTHORED and SERIALIZED. Clearing it in the editor at
                // rest wrote a runtime value over the author's - see anim::Update's
                // header comment. Holding at the end looks identical and touches
                // nothing the save writes.
                if (simulating) track.playing = false;
            }
        } else if (track.time < 0.0f) { // negative speed
            track.time = track.loop ? track.time + track.duration : 0.0f;
        }

        Sample(track, reg.get<Transform>(e));
    }
}

// --- Skeletal animation --------------------------------------------------------

namespace {

// Canonicalizes a joint/channel name so it matches across skeletons and
// across DCC quirks:
//   * drops a namespace prefix ("mixamorig:Hips" / "Armature|Hips" -> "Hips"),
//   * drops Assimp's FBX pivot suffix ("Hips_$AssimpFbx$_Rotation" -> "Hips"),
//     which appears when pivot nodes aren't fully collapsed and otherwise
//     makes animation channels silently fail to bind to their joint,
//   * lowercases.
std::string CanonicalJointName(const std::string& name) {
    std::string s = name;
    // Namespace prefix: take the text after the last ':' or '|'.
    const usize sep = s.find_last_of(":|");
    if (sep != std::string::npos) s = s.substr(sep + 1);
    // Assimp FBX pivot marker: everything from "$AssimpFbx$" onward is noise.
    if (const usize fbx = s.find("$AssimpFbx$"); fbx != std::string::npos) {
        s = s.substr(0, fbx);
        while (!s.empty() && s.back() == '_') s.pop_back(); // trailing separator
    }
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Process-wide rig cache: "rel path" -> rig (nullptr = asset has none).
// Guarded because UpdateSkeletal resolves rigs from parallel jobs; contention
// is limited to cold misses (every hit after the first is a plain lookup).
std::unordered_map<std::string, std::shared_ptr<const Rig>>& RigCache() {
    static std::unordered_map<std::string, std::shared_ptr<const Rig>> cache;
    return cache;
}
std::mutex& RigCacheMutex() {
    static std::mutex m;
    return m;
}

template <typename Key, typename Value, typename Mix>
Value SampleKeys(const std::vector<Key>& keys, f32 t, const Value& fallback, Mix mix) {
    if (keys.empty()) return fallback;
    if (t <= keys.front().time) return keys.front().value;
    if (t >= keys.back().time) return keys.back().value;
    usize next = 1;
    while (next < keys.size() && keys[next].time < t) ++next;
    const Key& a = keys[next - 1];
    const Key& b = keys[next];
    const f32 span = glm::max(b.time - a.time, 1e-6f);
    return mix(a.value, b.value, glm::clamp((t - a.time) / span, 0.0f, 1.0f));
}

// Extracts "<rel>.uaf" from a MeshRef source ("uaf:<rel>#<n>"); empty for prims.
std::string RelFromMeshRef(const std::string& source) {
    if (source.rfind("uaf:", 0) != 0) return {};
    const std::string rest = source.substr(4);
    const usize hash = rest.find_last_of('#');
    return hash == std::string::npos ? rest : rest.substr(0, hash);
}

// Backed by Scene's hashed name index (was a per-call linear scan over every
// named entity, run per animated entity per frame).
entt::entity FindEntityByName(const Scene& scene, const std::string& name) {
    return scene.FindByName(name);
}

// First joint in `skeleton` whose canonical name matches `name`; -1 if none.
i32 FindJointCanonical(const Skeleton& skeleton, const std::string& name) {
    const std::string want = CanonicalJointName(name);
    for (usize j = 0; j < skeleton.joints.size(); ++j) {
        if (CanonicalJointName(skeleton.joints[j].name) == want) return static_cast<i32>(j);
    }
    return -1;
}

// Rotation part of a (possibly scaled) global matrix as a unit quaternion.
glm::quat GlobalRotation(const glm::mat4& m) {
    glm::mat3 r(m);
    for (int c = 0; c < 3; ++c) {
        const f32 len = glm::length(r[c]);
        if (len > 1e-6f) r[c] /= len;
    }
    return glm::normalize(glm::quat_cast(r));
}

} // namespace

std::shared_ptr<const Rig> LoadRigCached(const std::filesystem::path& assetsDir,
                                         const std::string& relUaf) {
    if (relUaf.empty()) return nullptr;
    {
        std::lock_guard<std::mutex> lk(RigCacheMutex());
        auto& cache = RigCache();
        if (const auto it = cache.find(relUaf); it != cache.end()) return it->second;
    }
    // Load outside the lock (file IO); cache the result, tolerating a racing
    // duplicate load (the map insert is what must be serialized, not the read).
    std::shared_ptr<const Rig> rig;
    if (auto loaded = uaf::ReadRig(assetsDir / relUaf)) {
        rig = std::make_shared<const Rig>(std::move(*loaded));
    }
    std::lock_guard<std::mutex> lk(RigCacheMutex());
    auto& cache = RigCache();
    if (const auto it = cache.find(relUaf); it != cache.end()) return it->second;
    cache[relUaf] = rig; // negative results cache too (no rig in the asset)
    return rig;
}

// --- Motion matching -----------------------------------------------------------

namespace {

// One sampled pose's locomotion features: where in which clip, plus the root's
// horizontal speed and turn rate there.
struct MMSample {
    i32 clip = 0;
    f32 time = 0.0f;
    f32 speed = 0.0f; // m/s along the ground
    f32 turn = 0.0f;  // rad/s
};
using MMDatabase = std::vector<MMSample>;

std::unordered_map<const Rig*, std::shared_ptr<const MMDatabase>>& MMCache() {
    static std::unordered_map<const Rig*, std::shared_ptr<const MMDatabase>> cache;
    return cache;
}

f32 YawOf(const glm::quat& q) {
    const glm::vec3 fwd = q * glm::vec3(0.0f, 0.0f, -1.0f);
    return std::atan2(fwd.x, -fwd.z);
}

// Builds (and caches) the locomotion feature database for a rig: every clip is
// sampled at ~12 Hz, reading the root joint's channel for ground speed + turn.
std::shared_ptr<const MMDatabase> GetMMDatabase(const Rig& rig) {
    auto& cache = MMCache();
    if (const auto it = cache.find(&rig); it != cache.end()) return it->second;

    auto db = std::make_shared<MMDatabase>();
    // Canonical joint-name -> index, for finding the root mover channel.
    std::unordered_map<std::string, i32> byName;
    for (usize j = 0; j < rig.skeleton.joints.size(); ++j) {
        byName.emplace(CanonicalJointName(rig.skeleton.joints[j].name), static_cast<i32>(j));
    }
    constexpr f32 kRate = 1.0f / 12.0f;
    constexpr f32 kH = 0.1f;
    for (usize ci = 0; ci < rig.clips.size(); ++ci) {
        const AnimationClip& clip = rig.clips[ci];
        // Root channel: the position-animated channel bound to the lowest joint.
        const AnimChannel* rc = nullptr;
        i32 bestJoint = static_cast<i32>(rig.skeleton.joints.size()) + 1;
        for (const AnimChannel& ch : clip.channels) {
            if (ch.positions.size() < 2) continue;
            const auto it = byName.find(CanonicalJointName(ch.jointName));
            if (it != byName.end() && it->second < bestJoint) {
                bestJoint = it->second;
                rc = &ch;
            }
        }
        const f32 dur = glm::max(clip.duration, kRate);
        for (f32 t = 0.0f; t < dur; t += kRate) {
            MMSample s;
            s.clip = static_cast<i32>(ci);
            s.time = t;
            if (rc) {
                const f32 t1 = glm::min(t + kH, clip.duration);
                const f32 dtt = glm::max(t1 - t, 1e-3f);
                const glm::vec3 p0 = SampleKeys(rc->positions, t, glm::vec3(0.0f),
                                                [](const glm::vec3& a, const glm::vec3& b,
                                                   f32 f) { return glm::mix(a, b, f); });
                const glm::vec3 p1 = SampleKeys(rc->positions, t1, glm::vec3(0.0f),
                                                [](const glm::vec3& a, const glm::vec3& b,
                                                   f32 f) { return glm::mix(a, b, f); });
                glm::vec3 vel = (p1 - p0) / dtt;
                vel.y = 0.0f;
                s.speed = glm::length(vel);
                if (!rc->rotations.empty()) {
                    const glm::quat q0 =
                        SampleKeys(rc->rotations, t, glm::quat(1, 0, 0, 0),
                                   [](const glm::quat& a, const glm::quat& b, f32 f) {
                                       return glm::normalize(glm::slerp(a, b, f));
                                   });
                    const glm::quat q1 =
                        SampleKeys(rc->rotations, t1, glm::quat(1, 0, 0, 0),
                                   [](const glm::quat& a, const glm::quat& b, f32 f) {
                                       return glm::normalize(glm::slerp(a, b, f));
                                   });
                    f32 dy = YawOf(q1) - YawOf(q0);
                    while (dy > glm::pi<f32>()) dy -= glm::two_pi<f32>();
                    while (dy < -glm::pi<f32>()) dy += glm::two_pi<f32>();
                    s.turn = dy / dtt;
                }
            }
            db->push_back(s);
        }
    }
    HBE_INFO("MotionMatching: built database of {} pose samples from {} clip(s).",
             static_cast<u32>(db->size()), static_cast<u32>(rig.clips.size()));
    cache[&rig] = db;
    return db;
}

} // namespace

void UpdateMotionMatching(Scene& scene, const std::filesystem::path& assetsDir, f32 dt) {
    auto& reg = scene.Registry();
    for (const entt::entity e : reg.view<MotionMatching, Animator, MeshRef>()) {
        MotionMatching& mm = reg.get<MotionMatching>(e);
        if (!mm.enabled) continue;
        Animator& an = reg.get<Animator>(e);

        // Keep the Animator's clip source in sync so clip indices line up with
        // the database we build below.
        if (!mm.sourceAsset.empty() && an.sourceAsset != mm.sourceAsset) {
            an.sourceAsset = mm.sourceAsset;
        }
        const std::string rel =
            an.sourceAsset.empty() ? RelFromMeshRef(reg.get<MeshRef>(e).source)
                                   : an.sourceAsset;
        const std::shared_ptr<const Rig> rig = LoadRigCached(assetsDir, rel);
        if (!rig || rig->clips.empty()) continue;

        const std::shared_ptr<const MMDatabase> db = GetMMDatabase(*rig);
        if (db->empty()) continue;
        mm.dbKey = reinterpret_cast<u64>(rig.get());

        // Desired ground velocity drives the locomotion match. Auto-source it from
        // whatever moves this entity: a NavigationAgent (AI) or a CharacterController
        // (the player) - so a skinned player animates straight from its movement.
        // Else use the field a script/inspector set.
        glm::vec3 dv = mm.desiredVelocity;
        if (mm.useNavVelocity) {
            if (const NavigationAgent* na = reg.try_get<NavigationAgent>(e)) dv = na->velocity;
            else if (const CharacterController* cc = reg.try_get<CharacterController>(e)) dv = cc->desiredVelocity;
        }
        const f32 desiredSpeed = glm::length(glm::vec2(dv.x, dv.z));

        mm.sinceSearch += dt;
        if (mm.sinceSearch >= mm.searchInterval) {
            mm.sinceSearch = 0.0f;
            f32 best = 1e30f;
            i32 bestClip = an.clip;
            f32 bestTime = an.time;
            for (const MMSample& s : *db) {
                const f32 cs = s.speed * glm::max(mm.speedScale, 1e-3f);
                f32 cost = (cs - desiredSpeed) * (cs - desiredSpeed);
                // Bias toward the currently playing clip to avoid jitter.
                if (s.clip == an.clip) cost *= 0.8f;
                if (cost < best) {
                    best = cost;
                    bestClip = s.clip;
                    bestTime = s.time;
                }
            }
            if (bestClip != an.clip) {
                an.clip = bestClip;
                an.time = bestTime; // jump to the matched pose (UpdateSkeletal blends via remap)
            }
            an.playing = true;
        }
    }
}

void UpdateRotators(Scene& scene, f32 dt) {
    auto& reg = scene.Registry();
    for (const entt::entity e : reg.view<Transform, Rotator>()) {
        const Rotator& r = reg.get<Rotator>(e);
        if (!r.enabled || r.speed == 0.0f) continue;
        const glm::vec3 axis =
            glm::length(r.axis) > 1e-6f ? glm::normalize(r.axis) : glm::vec3(0, 1, 0);
        const glm::quat dq = glm::angleAxis(glm::radians(r.speed * dt), axis);
        Transform& t = reg.get<Transform>(e);
        t.rotation = glm::normalize(t.rotation * dq);
    }
}

void ClearRigCache() {
    std::lock_guard<std::mutex> lk(RigCacheMutex());
    RigCache().clear();
    MMCache().clear();
}

void UpdateSkeletal(Scene& scene, const std::filesystem::path& assetsDir, f32 dt,
                    bool simulating) {
    auto& reg = scene.Registry();

    // Each animator's work is independent (it writes only its own Animator /
    // Transform and reads a shared, locked rig cache), so gather them with a
    // single-threaded view iteration and then pose them in parallel. The pose
    // scratch buffers below are thread_local for exactly this reason.
    std::vector<entt::entity> ents;
    for (const entt::entity e : reg.view<Animator, MeshRef>()) ents.push_back(e);
    if (ents.empty()) return;

    // Resolve IK target-entity references to world positions up front so the
    // parallel pose loop reads only plain data (no registry lookups in a job).
    for (const entt::entity e : reg.view<IKConstraint>()) {
        IKConstraint& ik = reg.get<IKConstraint>(e);
        for (IKChain& chain : ik.chains) {
            if (chain.targetEntity.empty()) continue;
            const entt::entity te = FindEntityByName(scene, chain.targetEntity);
            chain.targetResolved = te != entt::null;
            if (chain.targetResolved) {
                chain.target = glm::vec3(scene.WorldMatrix(te)[3]);
                chain.warnedUnresolved = false; // it came back
            } else if (!chain.warnedUnresolved) {
                // ONCE per chain per break. The `.hbscene` deliberately omits
                // `target` while `targetEntity` is set (see IKChain), so an
                // unresolved binding has no authored vec3 to fall back on - the
                // solver below skips the chain rather than aim it at the world
                // origin, and this is the only thing that says why.
                chain.warnedUnresolved = true;
                HBE_WARN("IK: chain '{}' targets entity '{}', which is not in the "
                         "scene. The chain is DISABLED until it resolves (renamed, "
                         "deleted, or in a shard that is not resident?).",
                         chain.endJoint, chain.targetEntity);
            }
        }
    }

    const u32 n = static_cast<u32>(ents.size());
    const u32 workers = std::max(1u, jobs::WorkerCount());
    const u32 group = std::max(1u, n / (workers * 4)); // ~4 jobs per worker
    jobs::ParallelFor(n, group, [&](u32 jobBegin, u32 jobEnd) {
    for (u32 i = jobBegin; i < jobEnd; ++i) {
        const entt::entity e = ents[i];
        Animator& an = reg.get<Animator>(e);

        // Target skeleton = the entity's own mesh asset.
        const std::string ownRel = RelFromMeshRef(reg.get<MeshRef>(e).source);
        const std::shared_ptr<const Rig> target = LoadRigCached(assetsDir, ownRel);
        if (!target || !target->Valid()) {
            an.palette.clear();
            continue;
        }

        // Clip source: another asset's rig when retargeting, else our own.
        std::shared_ptr<const Rig> source =
            an.sourceAsset.empty() ? target : LoadRigCached(assetsDir, an.sourceAsset);
        if (!source || source->clips.empty()) source = target;

        const Skeleton& skeleton = target->skeleton;
        const usize jointCount = glm::min<usize>(skeleton.joints.size(), kMaxJoints);

        const AnimationClip* clip = nullptr;
        if (!source->clips.empty()) {
            an.clip = glm::clamp(an.clip, 0, static_cast<i32>(source->clips.size()) - 1);
            clip = &source->clips[static_cast<usize>(an.clip)];
        }

        // Advance the playhead.
        if (clip && an.playing && clip->duration > 0.0f) {
            an.time += dt * an.speed;
            if (an.time > clip->duration) {
                if (an.loop) an.time = std::fmod(an.time, clip->duration);
                // Same rule as anim::Update: hold, do not clear the authored flag,
                // when the editor is merely previewing.
                else { an.time = clip->duration; if (simulating) an.playing = false; }
            } else if (an.time < 0.0f) {
                an.time = an.loop ? an.time + clip->duration : 0.0f;
            }
        }

        // (Re)build the channel -> joint mapping when the clip/skeletons
        // change. This IS the retarget: source channels bind to target joints
        // by canonical name; the miss value -1 leaves the joint in bind pose.
        const u64 mapKey = reinterpret_cast<u64>(clip) ^
                           (reinterpret_cast<u64>(target.get()) << 1);
        if (clip && an.mapKey != mapKey) {
            an.mapKey = mapKey;
            std::unordered_map<std::string, i32> byName;
            for (usize j = 0; j < skeleton.joints.size(); ++j) {
                byName.emplace(CanonicalJointName(skeleton.joints[j].name),
                               static_cast<i32>(j));
            }
            an.channelToJoint.assign(clip->channels.size(), -1);
            for (usize c = 0; c < clip->channels.size(); ++c) {
                const auto it = byName.find(CanonicalJointName(clip->channels[c].jointName));
                if (it != byName.end()) an.channelToJoint[c] = it->second;
            }
            // Motion root: the translating channel bound to the joint nearest
            // the skeleton root (Mixamo's "Hips" and friends).
            an.rootChannel = -1;
            an.rootTrackValid = false;
            i32 bestJoint = static_cast<i32>(skeleton.joints.size());
            for (usize c = 0; c < clip->channels.size(); ++c) {
                const i32 j = an.channelToJoint[c];
                if (j >= 0 && j < bestJoint && clip->channels[c].positions.size() >= 2) {
                    bestJoint = j;
                    an.rootChannel = static_cast<i32>(c);
                }
            }
            // Different skeleton sizes move different distances: scale
            // translation channels by the bone-length ratio.
            an.translationScale =
                (source.get() != target.get() &&
                 source->skeleton.AverageBoneLength() > 1e-4f)
                    ? skeleton.AverageBoneLength() / source->skeleton.AverageBoneLength()
                    : 1.0f;

            u32 matched = 0;
            for (i32 j : an.channelToJoint) if (j >= 0) ++matched;
            if (matched == 0 && !clip->channels.empty()) {
                // The clip animates joints that don't exist in this skeleton by
                // name - the usual cause of "imported but won't move". Surface
                // the first mismatch so the naming gap is obvious.
                HBE_WARN("Animator: clip '{}' matched 0/{} channels to the "
                         "skeleton - joint names don't line up (e.g. channel "
                         "'{}' vs skeleton '{}'). The mesh stays in bind pose.",
                         clip->name, static_cast<u32>(clip->channels.size()),
                         clip->channels.empty() ? "" : clip->channels[0].jointName,
                         skeleton.joints.empty() ? "" : skeleton.joints[0].name);
            } else {
                HBE_INFO("Animator: clip '{}' dur={:.2f}s, {}/{} channels matched "
                         "({} skeleton joints).",
                         clip->name, clip->duration, matched,
                         static_cast<u32>(clip->channels.size()),
                         static_cast<u32>(skeleton.joints.size()));
            }
        }

        // Local poses start at bind, then matched channels override.
        struct Local { glm::vec3 p; glm::quat r; glm::vec3 s; };
        static thread_local std::vector<Local> locals;
        locals.resize(jointCount);
        for (usize j = 0; j < jointCount; ++j) {
            const Joint& joint = skeleton.joints[j];
            locals[j] = {joint.bindPosition, joint.bindRotation, joint.bindScale};
        }
        if (clip) {
            for (usize c = 0; c < clip->channels.size() && c < an.channelToJoint.size(); ++c) {
                const i32 j = an.channelToJoint[c];
                if (j < 0 || static_cast<usize>(j) >= jointCount) continue;
                const AnimChannel& ch = clip->channels[c];
                Local& l = locals[static_cast<usize>(j)];
                if (!ch.positions.empty()) {
                    // Scale only ANIMATED translations (bind stays as-is).
                    l.p = SampleKeys(ch.positions, an.time, l.p,
                                     [](const glm::vec3& a, const glm::vec3& b, f32 f) {
                                         return glm::mix(a, b, f);
                                     }) *
                          an.translationScale;
                }
                l.r = SampleKeys(ch.rotations, an.time, l.r,
                                 [](const glm::quat& a, const glm::quat& b, f32 f) {
                                     return glm::normalize(glm::slerp(a, b, f));
                                 });
                l.s = SampleKeys(ch.scales, an.time, l.s,
                                 [](const glm::vec3& a, const glm::vec3& b, f32 f) {
                                     return glm::mix(a, b, f);
                                 });
            }
        }

        // Root motion: hand the motion root's horizontal travel to the entity
        // and pin the pose so the character animates in place. Assumes the
        // motion root's LOCAL translation is model-space travel (true for
        // Mixamo-style rigs whose hips sit under identity armature nodes).
        if (an.rootMotion && clip && an.rootChannel >= 0 &&
            static_cast<usize>(an.rootChannel) < clip->channels.size()) {
            const AnimChannel& rc = clip->channels[static_cast<usize>(an.rootChannel)];
            const i32 j = an.channelToJoint[static_cast<usize>(an.rootChannel)];
            if (j >= 0 && static_cast<usize>(j) < jointCount && !rc.positions.empty()) {
                const glm::vec3 P = locals[static_cast<usize>(j)].p; // sampled+scaled
                const glm::vec3 P0 = rc.positions.front().value * an.translationScale;
                const glm::vec3 Pend = rc.positions.back().value * an.translationScale;

                glm::vec3 delta(0.0f);
                if (an.rootTrackValid) {
                    delta = P - an.lastRootPos;
                    // Loop wraps: add the distance covered across the seam.
                    if (an.loop && an.speed >= 0.0f && an.time < an.lastRootTime) {
                        delta += Pend - P0;
                    } else if (an.loop && an.speed < 0.0f && an.time > an.lastRootTime) {
                        delta -= Pend - P0;
                    }
                }
                an.lastRootPos = P;
                an.lastRootTime = an.time;
                an.rootTrackValid = true;

                // Horizontal travel only; vertical bounce stays in the pose.
                delta.y = 0.0f;
                if (Transform* t = reg.try_get<Transform>(e);
                    t && glm::dot(delta, delta) > 0.0f) {
                    t->position += t->rotation * (delta * t->scale);
                }

                // Pin the pose's horizontal position to the clip's start.
                locals[static_cast<usize>(j)].p.x = P0.x;
                locals[static_cast<usize>(j)].p.z = P0.z;
            }
        } else {
            an.rootTrackValid = false;
        }

        // Compose model-space globals (parents precede children).
        static thread_local std::vector<glm::mat4> globals;
        globals.resize(jointCount);
        const auto compose = [&]() {
            for (usize j = 0; j < jointCount; ++j) {
                glm::mat4 local = glm::translate(glm::mat4(1.0f), locals[j].p) *
                                  glm::mat4_cast(locals[j].r);
                local = glm::scale(local, locals[j].s);
                const i32 parent = skeleton.joints[j].parent;
                globals[j] = (parent >= 0 && static_cast<usize>(parent) < j)
                                 ? globals[static_cast<usize>(parent)] * local
                                 : local;
            }
        };
        compose();

        // Inverse kinematics: solve each two-bone chain on the posed skeleton,
        // writing corrective LOCAL rotations, then recompose so descendants
        // follow. Targets are in world space -> transform into model space.
        if (const IKConstraint* ik = reg.try_get<IKConstraint>(e); ik && !ik->chains.empty()) {
            const glm::mat4 invEntity = glm::inverse(scene.WorldMatrix(e));
            bool any = false;
            for (const IKChain& chain : ik->chains) {
                if (!chain.enabled || chain.weight <= 0.0f) continue;
                // A chain bound to an entity that is not in the scene has no
                // meaningful target (see IKChain::targetResolved): keep the animated
                // pose rather than reach for {0,0,0}.
                if (!chain.targetEntity.empty() && !chain.targetResolved) continue;
                const i32 end = FindJointCanonical(skeleton, chain.endJoint);
                if (end < 0 || static_cast<usize>(end) >= jointCount) continue;
                const i32 mid = skeleton.joints[static_cast<usize>(end)].parent;
                if (mid < 0) continue;
                const i32 root = skeleton.joints[static_cast<usize>(mid)].parent;
                if (root < 0) continue;

                const glm::vec3 a(globals[root][3]);
                const glm::vec3 b(globals[mid][3]);
                const glm::vec3 c(globals[end][3]);
                const glm::vec3 t = glm::vec3(invEntity * glm::vec4(chain.target, 1.0f));
                const glm::vec3 pole =
                    chain.hasPole ? glm::vec3(invEntity * glm::vec4(chain.pole, 1.0f)) : a;

                const f32 eps = 1e-4f;
                const f32 lab = glm::length(b - a);
                const f32 lcb = glm::length(c - b);
                if (lab < eps || lcb < eps) continue;
                const f32 lat = glm::clamp(glm::length(t - a), eps, lab + lcb - eps);
                const glm::vec3 ca = c - a;
                if (glm::length(ca) < eps) continue;

                const auto safeAcos = [](f32 x) { return std::acos(glm::clamp(x, -1.0f, 1.0f)); };
                const f32 ac_ab_0 = safeAcos(glm::dot(glm::normalize(ca), glm::normalize(b - a)));
                const f32 ba_bc_0 = safeAcos(glm::dot(glm::normalize(a - b), glm::normalize(c - b)));
                const f32 ac_at_0 = safeAcos(glm::dot(glm::normalize(ca), glm::normalize(t - a)));
                const f32 ac_ab_1 =
                    safeAcos((lcb * lcb - lab * lab - lat * lat) / (-2.0f * lab * lat));
                const f32 ba_bc_1 =
                    safeAcos((lat * lat - lab * lab - lcb * lcb) / (-2.0f * lab * lcb));

                glm::vec3 axis0 = chain.hasPole ? glm::cross(ca, pole - a) : glm::cross(ca, b - a);
                if (glm::length(axis0) < eps) axis0 = glm::cross(ca, glm::vec3(0, 0, 1));
                if (glm::length(axis0) < eps) axis0 = glm::cross(ca, glm::vec3(1, 0, 0));
                glm::vec3 axis1 = glm::cross(ca, t - a);
                if (glm::length(axis1) < eps) axis1 = axis0;
                axis0 = glm::normalize(axis0);
                axis1 = glm::normalize(axis1);

                const glm::quat aGr = GlobalRotation(globals[root]);
                const glm::quat bGr = GlobalRotation(globals[mid]);
                const glm::vec3 axis0a = glm::normalize(glm::inverse(aGr) * axis0);
                const glm::vec3 axis1a = glm::normalize(glm::inverse(aGr) * axis1);
                const glm::vec3 axis0b = glm::normalize(glm::inverse(bGr) * axis0);

                const glm::quat r0 = glm::angleAxis(ac_ab_1 - ac_ab_0, axis0a);
                const glm::quat r2 = glm::angleAxis(ac_at_0, axis1a);
                const glm::quat r1 = glm::angleAxis(ba_bc_1 - ba_bc_0, axis0b);

                const f32 w = glm::clamp(chain.weight, 0.0f, 1.0f);
                glm::quat& lr = locals[static_cast<usize>(root)].r;
                glm::quat& lm = locals[static_cast<usize>(mid)].r;
                lr = glm::normalize(glm::slerp(lr, glm::normalize(lr * r0 * r2), w));
                lm = glm::normalize(glm::slerp(lm, glm::normalize(lm * r1), w));
                any = true;
            }
            if (any) compose();
        }

        // Build the skinning palette from the final globals.
        an.palette.resize(jointCount);
        for (usize j = 0; j < jointCount; ++j) {
            an.palette[j] = globals[j] * skeleton.joints[j].inverseBind;
        }
    }
    });
}

} // namespace hbe::anim
