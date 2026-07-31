// Scene/StreamPolicy.h - the DECISION half of distance streaming, on purpose
// separated from the doing half.
//
// This file has no registry, no GPU, no filesystem and no clock. It answers one
// question - "given where the player/camera is, which shards should start loading and
// which should be unloaded" - from plain numbers, which is what makes the rules below
// testable without a world (--test-tagpolicy) instead of only observable as emergent
// behaviour in a running game. Scene/TagStreaming.{h,cpp} owns everything with a side
// effect.
//
// THE FIVE RULES, and why each is not negotiable:
//
// 1. DISTANCE IS TO THE SHARD'S AABB, NEVER TO ITS CENTRE. The deleted `.hbworld`
//    streamer measured to the centre. For a 300 m wall or a long street that is
//    wrong by up to half the shard: the player stands against the geometry while the
//    "distance" is 150 m, so the shard unloads under their feet. tagshard::
//    DistanceToShard is the one implementation and it returns 0 inside the box.
//
// 2. HYSTERESIS. Load inside loadRadius, unload only OUTSIDE unloadRadius, with
//    unload strictly greater (salvage::EnforceHysteresis, applied at parse in
//    tags::Normalize). With unload <= load a player standing on the boundary spawns
//    and despawns the same shard EVERY FRAME, and a spawn is a synchronous
//    Instantiate - a permanent hitch that looks like a GPU or memory problem. This
//    file additionally refuses to unload a shard a focus is INSIDE, which the
//    corrected radii already imply; it is stated as code because the cost of the
//    band being wrong is so high.
//
// 3. THE UNION LOADS, THE INTERSECTION UNLOADS. There is more than one focus - the
//    player AND the active camera, because a cutscene camera flies ahead of a
//    stationary player, and a camera-zone/spline shot can look at a place the player
//    will never stand. A shard loads if ANY focus is within its load radius, and
//    unloads only if EVERY focus is beyond its unload radius. With no focus at all
//    (no player, no camera) NOTHING changes: an empty focus list must not be read as
//    "everything is infinitely far away, unload the world".
//
// 4. THROTTLE, AND PRIORITY DECIDES WHO GOES FIRST. At most `maxConcurrent` loads
//    are in flight, so a focus crossing into six shards at once does not start six
//    parses. Candidates sort by priority (high first), then by distance (near
//    first) - so the thing the player is about to walk into wins over the thing
//    behind them. Unloads are capped per evaluation for the same reason a finalize
//    is: a despawn is a synchronous capture + destroy on the main thread.
//
// 5. WHEN A CANDIDATE IS THROTTLED OUT, SAY SO (`moreWork`). The caller evaluates on
//    a cadence, not every frame; without this flag a backlog would drain at one item
//    per cadence period instead of one per frame, which is exactly the case (a
//    loading screen ending, a corner turned) that the budget exists to smooth.
#pragma once

#include "Core/Types.h"

#include <glm/glm.hpp>

#include <vector>

namespace hbe::stream {

// One shard as the policy sees it: a box, a band, a priority and its current state.
// Deliberately NOT the streamer's ShardRuntime - the policy must not be able to
// touch a registry, a job or a staged asset even by accident.
struct PolicyShard {
    glm::vec3 min{0.0f}, max{0.0f}; // the BAKED world AABB (scene::ShardDesc)
    f32 loadRadius = 0.0f;
    f32 unloadRadius = 0.0f;
    i32 priority = 0;
    bool resident = false; // spawned right now
    // A load is in flight or has finished staging and is waiting for its main-thread
    // finalize. Not a load candidate (it is already becoming one) and not a resident
    // (nothing to unload yet).
    bool busy = false;
    bool pinned = false; // never unload (an alwaysLoaded tag, or a shard the runtime
                         // has pinned for the duration of a cutscene/conversation)
    bool failed = false; // terminal: never a load candidate again (see SALVAGE 3's
                         // correction - a durably broken shard must not retry forever)
};

struct PolicyIn {
    const PolicyShard* shards = nullptr;
    u32 count = 0;
    // Every streaming focus, in world space. See rule 3.
    const glm::vec3* foci = nullptr;
    u32 fociCount = 0;
    // Loads allowed in flight at once, and how many already are.
    u32 maxConcurrent = 4;
    u32 inFlight = 0;
    // Unloads this evaluation may order. 1 keeps the per-frame main-thread cost of a
    // despawn (capture + closure + destroy) bounded exactly the way the salvaged
    // one-finalize-per-frame budget bounds a spawn.
    u32 maxUnloads = 1;
    // false = PIN EVERYTHING LOADED: load every shard, unload none. This is the
    // semantics the deleted StreamingWorld::LoadAll had, and it is what an editor
    // "streaming off" switch and a debug key need - "off" must mean "the whole level
    // is there", never "the level is half missing and nothing will fix it".
    bool enabled = true;
};

// A ranked load/unload candidate. In the header only so PolicyOut can own the scratch
// the ranking needs; callers never touch it.
struct PolicyCandidate {
    u32 index = 0;
    i32 priority = 0;
    f32 dist = 0.0f;
};

struct PolicyOut {
    std::vector<u32> load;   // shard indices to START loading, best first
    std::vector<u32> unload; // shard indices to despawn, furthest first
    // Ranking scratch, owned here so Evaluate really is allocation-free once warm (it
    // used to declare these as function locals, which allocated on EVERY call while the
    // contract below claimed otherwise). Cleared, never reallocated, on reuse.
    std::vector<PolicyCandidate> scratchLoad, scratchUnload;
    u32 inRange = 0;         // shards within load radius of some focus (diagnostic)
    u32 residentCount = 0;
    // A load or unload candidate was dropped by a throttle/cap. The caller should
    // evaluate again NEXT frame rather than waiting for its normal cadence.
    bool moreWork = false;
};

// Distance from `p` to the axis-aligned box, 0 when inside. (tagshard::
// DistanceToShard is this function applied to a scene::ShardDesc; this overload
// exists so the policy has no dependency on the scene format.)
f32 DistanceToBox(const glm::vec3& mn, const glm::vec3& mx, const glm::vec3& p);

// Applies the five rules. Pure: same input, same output. Allocation-free once warm -
// every vector it uses (the two outputs and the two ranking scratches) lives in
// PolicyOut and is cleared, not reallocated, on reuse. Pass the SAME PolicyOut each
// frame, which is what the streamer does.
void Evaluate(const PolicyIn& in, PolicyOut& out);

// Headless proof of all five rules (--test-tagpolicy): a focus oscillating exactly
// on the boundary does not thrash; an elongated shard is measured to its box and not
// its centre; two foci union for loading and intersect for unloading; priority beats
// distance and distance breaks priority ties; the concurrency throttle and the unload
// cap both report moreWork; a focus inside the box can never unload it; an empty
// focus list changes nothing; pinned and failed shards are respected; and disabled
// means everything loads and nothing unloads. No GPU, no window, no registry.
bool PolicySelfTest();

} // namespace hbe::stream
