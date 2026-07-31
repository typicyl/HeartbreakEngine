// Scene/StreamingSalvage.h - preserved logic from the level/cell-streaming code that
// tag streaming replaces.
//
// WHY THIS FILE EXISTS
// --------------------
// The `.hbworld` cell streamer (StreamingWorld.{h,cpp}), the static+dynamic Level
// loader (Level.{h,cpp}) and the build-time level split (SceneSerializer::
// SplitSceneFile) were DELETED by P1 of the tag-streaming plan
// (docs/Design-TagStreaming.md section 4). Four pieces of that code are NOT
// level machinery - they are hard-won, measured behaviours that the tag streamer
// and the `.hbui` migrator will need again, and that would be silently
// re-derived (wrongly) if they went out with the rest.
//
// Everything below is a VERBATIM copy of the code as it stood immediately before
// P1 deleted it, plus its original comments, with the source file:line it came
// from recorded. Those files are gone, so this is the only copy. This header is
// documentation-with-executable-fragments: SALVAGE 1 and SALVAGE 4 are real,
// compilable, dependency-free helpers you can call; SALVAGE 2 and SALVAGE 3 are
// preserved as reference blocks because they are inseparable from the
// StreamingWorld::Cell type that went away with it - re-apply them by shape, not
// by call.
//
// STATUS: SALVAGE 1 is LIVE - UI/UIDocument.cpp's ConvertSceneToDocument (the
// `.hbui` migrator, P2) calls PartitionEntitiesRemappingParents to split UI
// entities out of a `.hbscene` and renumber their parent links. SALVAGE 2/3/4
// are still parked for the tag streamer.
#pragma once

#include "Core/Types.h"

#include <nlohmann/json.hpp>

#include <unordered_map>
#include <vector>

namespace hbe::salvage {

// =============================================================================
// SALVAGE 1 - parent-index REMAP loop
// Source: Scene/SceneSerializer.cpp:1815-1837 (the `buildLayer` lambda inside
//         SplitSceneFile), as of 2026-07-29 - BEFORE P1 deleted SplitSceneFile.
//         This header is now the only surviving copy; there is nothing left in
//         the tree to diff it against.
//
// WHY IT MATTERS LATER: `.hbscene` stores `parent` as an INDEX into the file's
// own `entities` array. Any operation that writes a SUBSET of a scene's entities
// to a new document - the `.hbui` migrator splitting UI entities out into their
// own asset, a spatial sharder writing one shard per file - has to renumber
// every surviving `parent` link, and has to have an answer for a parent that did
// NOT survive into this partition. Get it wrong and you get either an index that
// points at an unrelated entity (silent hierarchy corruption) or an out-of-range
// index (which SceneSerializer::Instantiate's parent pass turns into an
// emplace on entt::null - blocker B1 in the plan).
//
// The rule, preserved from SplitSceneFile's own comment: a cross-partition
// parent gracefully becomes a ROOT (-1). That is only safe because the authoring
// tools tag whole subtrees, so in practice a parent is always in the same
// partition; the -1 is the defensive fallback, not the expected path.
//
// The original's surrounding comment, verbatim:
//   The editor authors ONE merged scene; the build splits it into the two layer
//   files the runtime's level loader expects. Done at the JSON level (no GPU /
//   instantiate): partition entities by their "sceneLayer" tag (default static) and
//   REMAP parent indices per partition. A tagged subtree shares one layer (the
//   Inspector toggle tags whole subtrees), so a parent is always in the same file;
//   any cross-layer parent gracefully becomes a root.
//
// Generalized here to "any predicate", which is exactly what the callers above
// need; `SplitSceneFile`'s predicate was `sceneLayer == "dynamic"`.
// =============================================================================

// Returns a copy of `entities` containing only the elements `keepPred` accepts,
// with each survivor's "parent" index remapped into the new array. A parent that
// was not kept becomes -1 (root).
template <typename KeepPred>
inline nlohmann::json PartitionEntitiesRemappingParents(const nlohmann::json& ents,
                                                        KeepPred&& keepPred) {
    std::unordered_map<int, int> remap; // old index -> new index in this partition
    std::vector<int> keep;
    for (int i = 0; i < static_cast<int>(ents.size()); ++i)
        if (keepPred(ents[static_cast<usize>(i)])) {
            remap[i] = static_cast<int>(keep.size());
            keep.push_back(i);
        }
    nlohmann::json arr = nlohmann::json::array();
    for (int oldIdx : keep) {
        nlohmann::json e = ents[static_cast<usize>(oldIdx)];
        if (auto it = e.find("parent"); it != e.end() && it->is_number_integer()) {
            const int p = it->get<int>();
            const auto rit = remap.find(p);
            *it = (rit != remap.end()) ? rit->second : -1; // cross-layer parent -> root
        }
        arr.push_back(std::move(e));
    }
    return arr;
}

// =============================================================================
// SALVAGE 2 - load/unload HYSTERESIS validation
// Source: Scene/StreamingWorld.cpp:94-96 (inside the `.hbworld` manifest parse),
//         as of 2026-07-29. Verbatim:
//
//     c.unloadRadius = jc.value("unloadRadius", c.loadRadius * 1.35f);
//     if (c.unloadRadius <= c.loadRadius)
//         c.unloadRadius = c.loadRadius * 1.25f + 1.0f; // enforce hysteresis
//
// WHY IT MATTERS LATER: a distance streamer with unloadRadius <= loadRadius
// THRASHES. The player standing on the boundary loads and unloads the same
// region every frame, and because a load runs asynchronously and a Finalize is a
// synchronous scene::Instantiate, that is not a small cost - it is a permanent
// hitch that looks like a memory or GPU problem. The default (1.35x) and, more
// importantly, the CLAMP (any authored value that does not separate the two
// radii is silently corrected to 1.25x + 1) must survive into whatever the tag
// streamer uses for its per-tag/per-shard ranges. Do not make this a warning
// the author can ignore; the shipping code corrects it unconditionally.
//
// The "+ 1.0f" matters as much as the 1.25x: it keeps the band non-degenerate
// when loadRadius is 0 (an author disabling a shard by zeroing its range).
// =============================================================================

// Enforces the hysteresis band on a load/unload radius pair, exactly as the
// `.hbworld` manifest parser did. `unload` is corrected in place.
inline void EnforceHysteresis(f32 load, f32& unload) {
    if (unload <= load) unload = load * 1.25f + 1.0f; // enforce hysteresis
}
inline f32 DefaultUnloadRadius(f32 load) { return load * 1.35f; }

// =============================================================================
// SALVAGE 3 - ONE-FINALIZE-PER-FRAME budget, and the measured jank it fixes
// Source: Scene/StreamingWorld.cpp:167-183 (StreamingWorld::Update step 1),
//         as of 2026-07-29. Verbatim, comment included:
//
//     // 1) Finalize completed loads / clear failures (main thread). BUDGET: finalize at
//     // most ONE ready cell per frame - Finalize runs scene::Instantiate synchronously
//     // (hundreds/thousands of entities), so finalizing several cells that became ready in
//     // the same frame stacks into one big main-thread stall (the ~1-2s streaming jank).
//     // Loads still trickle in via maxConcurrentLoads_, so one finalize/frame keeps up.
//     bool finalizedThisFrame = false;
//     for (std::unique_ptr<Cell>& up : cells_) {
//         const State s = static_cast<State>(up->state.load(std::memory_order_acquire));
//         if (s == State::Ready) {
//             if (finalizedThisFrame) continue; // defer the rest to later frames
//             Finalize(*up, scene, renderer);
//             finalizedThisFrame = true;
//         } else if (s == State::Failed) {
//             HBE_WARN("StreamingWorld: cell '{}' failed to load.", up->desc.name);
//             up->state.store(static_cast<int>(State::Unloaded), std::memory_order_relaxed);
//         }
//     }
//
// WHY IT MATTERS LATER: the parenthetical "(the ~1-2s streaming jank)" is a
// MEASURED number from this engine, not an estimate. Parse+stage is on a worker;
// instantiate is not and cannot be (GPU + registry, main thread only). Several
// regions crossing their load radius in the same frame - which is the normal
// case when the player turns a corner or a loading screen ends - stacks their
// instantiate cost into ONE frame. The fix is a hard budget of one finalize per
// frame; the throttle on concurrent LOADS is a separate, insufficient control
// (it bounds worker IO, not main-thread instantiate).
//
// A tag streamer that spawns per-tag has exactly this shape and must carry
// exactly this budget. If a future version needs finer granularity, budget by
// ENTITY COUNT within a finalize rather than raising the cell/shard count.
//
// Also preserved: a Failed region is reset to Unloaded and merely warned about,
// rather than left in Failed forever (see SALVAGE 4 for why Failed must not
// block settling).
//
// CORRECTION, and it is the one part of the original NOT to copy: resetting to
// Unloaded while the region is still inside its load radius means step 3 of the
// same Update re-enters Loading immediately. A region that fails for a durable
// reason (missing file, corrupt JSON) therefore retries EVERY FRAME, spamming
// the warn and re-running the parse, for as long as the player stands near it.
// Worse, the original re-parsed into a REUSED, never-cleared scratch buffer, so
// a partially-populated retry appended to the previous attempt's rows. A tag
// streamer must (a) clear the region's parse scratch when resetting Failed, and
// (b) hold a Failed region in a terminal state (or back off) instead of
// immediately requeueing it.
// =============================================================================

// The budget as a reusable predicate: call once per candidate, in state order.
// Returns true at most once per frame.
struct FinalizeBudget {
    bool spent = false;
    bool Take() {
        if (spent) return false; // defer the rest to later frames
        spent = true;
        return true;
    }
    void NextFrame() { spent = false; }
};

// =============================================================================
// SALVAGE 4 - IsSettled semantics (what "the world is ready" means)
// Source: Scene/StreamingWorld.cpp:263-276, as of 2026-07-29. Verbatim:
//
//     bool StreamingWorld::IsSettled(const glm::vec3& focus) const {
//         for (const std::unique_ptr<Cell>& up : cells_) {
//             const State s = static_cast<State>(up->state.load(std::memory_order_acquire));
//             // A load in flight (or finished-but-not-yet-instantiated) is never settled.
//             if (s == State::Loading || s == State::Ready) return false;
//             // An in-range cell that hasn't started loading will pop in once revealed.
//             // (State::Failed is left as-is: a broken cell won't appear, so waiting on it
//             // would hang the loading screen forever.)
//             if (s == State::Unloaded &&
//                 glm::distance(focus, up->desc.center) <= up->desc.loadRadius)
//                 return false;
//         }
//         return true;
//     }
//
// WHY IT MATTERS LATER: this is the predicate the LOADING SCREEN waits on, and
// all three of its clauses are load-bearing:
//   * Loading  -> not settled (obvious).
//   * Ready    -> not settled. Ready means the worker finished but the main
//                 thread has not instantiated yet. Treating Ready as settled
//                 drops the loading screen one frame before the geometry exists,
//                 which is precisely the pop the screen is there to hide. This
//                 clause is what makes SALVAGE 3's deferral safe: deferred
//                 finalizes still hold the screen up.
//   * Unloaded AND in range -> not settled. A region that qualifies but has not
//                 started (throttled by maxConcurrentLoads) would otherwise pop
//                 in after the screen lifts.
//   * Failed    -> IGNORED, deliberately. A broken region will never load, so
//                 waiting on it hangs the loading screen FOREVER. The comment
//                 says so; keep the exemption.
//
// Reproduce all four clauses in any tag-streaming "is the world ready" gate.
// =============================================================================

// Region lifecycle states, mirroring StreamingWorld::State VALUE FOR VALUE
// (`enum class State : int { Unloaded, Loading, Ready, Failed, Loaded };`), so
// the predicate below can be reused verbatim by the tag streamer AND so a
// streamer that publishes its state as an `std::atomic<int>` (which is what the
// original did, and what any worker-thread streamer will do) can static_cast
// between the two without silently swapping Failed and Loaded. Do not reorder.
enum class RegionState : int { Unloaded, Loading, Ready, Failed, Loaded };

// The settle test for ONE region. `inRange` is the caller's distance test.
// AND these together across all regions to get IsSettled().
inline bool RegionSettled(RegionState s, bool inRange) {
    // A load in flight (or finished-but-not-yet-instantiated) is never settled.
    if (s == RegionState::Loading || s == RegionState::Ready) return false;
    // An in-range region that hasn't started loading will pop in once revealed.
    // (Failed is left as-is: a broken region won't appear, so waiting on it
    // would hang the loading screen forever.)
    if (s == RegionState::Unloaded && inRange) return false;
    return true;
}

} // namespace hbe::salvage
