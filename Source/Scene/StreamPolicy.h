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
// THE SIX RULES, and why each is not negotiable:
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
//
// 6. AN ASSOCIATED SHARD LOADS OUT OF RANGE, AND DOES NOT UNLOAD WHILE IT IS
//    ASSOCIATED. `PolicyShard::associated` means "some tag the author said pulls
//    this one in is resident by DISTANCE". It has to touch BOTH sites: `pinned`
//    only blocks the unload, so a non-resident pinned shard still falls through to
//    the load test and still needs d <= loadRadius - which is exactly the half an
//    association cannot supply, since the whole point is that the driven content is
//    out of range. The motivating case is a hill you can see a distant city from:
//    the low-poly city is separate content with its own tag, associated with the
//    hill's, and it must appear while the player is nowhere near it.
//
//    THE FLAG IS DERIVED, NEVER REFERENCE-COUNTED. It is recomputed from scratch on
//    every evaluation (AssocPass below) from a seed set that is purely distance-
//    derived, so a shard resident for two reasons at once - in range AND driven -
//    survives losing either one with no bookkeeping, and a cycle cannot leak: an
//    association is never seeded from association-derived residency, so a mutual
//    pair collapses the moment neither member is within its own unload radius.
//    A refcount would need matched increment/decrement across bind, despawn, stage
//    failure, level transition and rebind - five leak sites for a flag that has no
//    lifetime at all.
//
//    IT IS DELIBERATELY NOT `pinned`. Two bools are the "why is this resident?"
//    answer the codebase otherwise lacks: `pinned` = a live member walked out of the
//    box, `associated` = a driver is holding it. The editor readout and the Tags
//    panel's State column both depend on being able to tell them apart.
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
    // RULE 6: a tag the author associated with this shard's tag is resident BY
    // DISTANCE, so this shard loads however far away it is and does not unload while
    // that stays true. Derived fresh every evaluation by AssocPass; never stored.
    bool associated = false;
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

// --- RULE 6: tag association ---------------------------------------------------
// How many hops are followed from a seeded tag. Hill -> Vista -> Lake is honoured
// because the author's mental model is "loading the hill loads the vista, and the
// vista is whatever the vista tag says it is"; refusing the second hop would be
// arbitrary. Four is deep enough that no sane authoring reaches it and shallow
// enough that the cost is a constant. The bake warns about a longer chain.
inline constexpr u32 kMaxAssocDepth = 4;
// "This shard's tag is not a node in the graph" - the project does not list it. Such
// a tag still STREAMS on default radii (see TagStreaming.cpp DefForTag); it simply
// neither drives nor is driven.
inline constexpr u32 kNoAssocTag = 0xFFFFFFFFu;

// The association graph as INTEGER ADJACENCY - one entry per tag, holding the tags
// that tag pulls in. Names are resolved to indices by the caller (tags::
// BuildAssocGraph), which is what keeps this file free of strings, of the project
// and of any notion of what a tag is.
struct AssocGraph {
    std::vector<std::vector<u32>> edges; // edges[t] = tags t pulls in
    void Clear() { edges.clear(); }
    usize TagCount() const { return edges.size(); }
};

// ONE implementation of the propagation, called from BOTH the runtime streamer and
// the editor's Tags-panel prediction. Duplicating it would be a lockstep hazard of
// the same class as the panel-enum rule: the prediction would silently disagree with
// the runtime. The lockstep here reduces to "both call Run()", which greps.
//
// `seed[t]` is set by the caller for every tag that DRIVES - one of its shards is
// resident on its own terms: distance (inside the load radius, or inside its own
// hysteresis band having been inside the load radius) or alwaysLoaded. `marked[t]`
// comes back true for every tag REACHED from a seed in 1..kMaxAssocDepth hops.
//
// SEEDING AND BEING MARKED ARE INDEPENDENT, AND THAT IS THE POINT. They answer
// different questions - "does t drive?" and "is t driven?" - and a tag can be both
// at once. They used to share one array (a seeded tag could never be marked), which
// made `marked` read as "held by a driver AND not in range". That is wrong the
// moment a tag has more than one shard: the seed is OR'd over the tag's shards, so
// ONE shard walking into its own radius cleared the mark for ALL of them and the
// tag's other shards unloaded while their driver was still driving. The mark is now
// set on every reached tag, seeded or not, and "which REASON is holding this
// particular shard" is answered per shard by its own distance (see
// Streamer::IsSelfHeld) rather than by squeezing two facts into one bit.
//
// CYCLES TERMINATE BY CONSTRUCTION: a SEPARATE `visited` set makes the walk monotone,
// so A -> B -> A adds nothing on the second visit and the whole pass ends in at most
// kMaxAssocDepth rounds whatever the graph shape. (The guard never depended on being
// the same array as `marked`; sharing them was only ever the "a seed is not marked"
// convention.) Reused scratch, so a warm pass allocates nothing.
struct AssocPass {
    AssocGraph graph;
    std::vector<u8> seed;   // IN, parallel to graph.edges
    std::vector<u8> marked; // OUT, parallel to graph.edges
    std::vector<u8> visited; // scratch: the termination guard, NOT the output
    std::vector<u32> frontier, next; // scratch
    // Resizes seed/marked to the graph and clears the seed. Call before filling it.
    void BeginSeed();
    void Run();
};

// Distance from `p` to the axis-aligned box, 0 when inside. (tagshard::
// DistanceToShard is this function applied to a scene::ShardDesc; this overload
// exists so the policy has no dependency on the scene format.)
f32 DistanceToBox(const glm::vec3& mn, const glm::vec3& mx, const glm::vec3& p);

// Applies the six rules. Pure: same input, same output. Allocation-free once warm -
// every vector it uses (the two outputs and the two ranking scratches) lives in
// PolicyOut and is cleared, not reallocated, on reuse. Pass the SAME PolicyOut each
// frame, which is what the streamer does.
void Evaluate(const PolicyIn& in, PolicyOut& out);

// Headless proof of all six rules (--test-tagpolicy): a focus oscillating exactly
// on the boundary does not thrash; an elongated shard is measured to its box and not
// its centre; two foci union for loading and intersect for unloading; priority beats
// distance and distance breaks priority ties; the concurrency throttle and the unload
// cap both report moreWork; a focus inside the box can never unload it; an empty
// focus list changes nothing; pinned and failed shards are respected; disabled
// means everything loads and nothing unloads; and (rule 6) an associated shard loads
// out of range, never unloads while associated, survives losing either of two
// reasons, and a cycle terminates instead of leaking residency. No GPU, no window,
// no registry.
bool PolicySelfTest();

} // namespace hbe::stream
